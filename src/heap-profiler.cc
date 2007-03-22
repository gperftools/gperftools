// Copyright (c) 2005, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat
//
// TODO: Log large allocations

#include "config.h"

#include <malloc.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>

#include <algorithm>
#include <string>
#include <iostream>
#include <map>
#include <google/perftools/hash_set.h>

#include <google/heap-profiler.h>
#include <google/stacktrace.h>
#include <google/malloc_extension.h>
#include <google/malloc_hook.h>

#include "heap-profiler-inl.h"
#include "internal_spinlock.h"
#include "addressmap-inl.h"

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/googleinit.h"
#include "base/commandlineflags.h"

#ifdef HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#define LLD    PRId64               // how to write 64-bit numbers
#else
#define LLD    "lld"                // hope for the best
#endif

#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

#define LOGF  STL_NAMESPACE::cout   // where we log to; LOGF is a historical name

using HASH_NAMESPACE::hash_set;
using STL_NAMESPACE::string;
using STL_NAMESPACE::sort;

//----------------------------------------------------------------------
// Flags that control heap-profiling
//----------------------------------------------------------------------

DEFINE_bool(cleanup_old_heap_profiles, true,
            "At initialization time, delete old heap profiles.");
DEFINE_int64(heap_profile_allocation_interval, 1 << 30 /*1GB*/,
             "Dump heap profiling information once every specified "
             "number of bytes allocated by the program.");
DEFINE_int64(heap_profile_inuse_interval, 100 << 20 /*100MB*/,
             "Dump heap profiling information whenever the high-water "
             "memory usage mark increases by the specified number of "
             "bytes.");
DEFINE_bool(mmap_log, false, "Should mmap/munmap calls be logged?");
DEFINE_bool(mmap_profile, false, "If heap-profiling on, also profile mmaps");
DEFINE_int32(heap_profile_log, 0,
             "Logging level for heap profiler/checker messages");

// Level of logging used by the heap profiler and heap checker (if applicable)
// Default: 0
void HeapProfilerSetLogLevel(int level) {
  FLAGS_heap_profile_log = level;
}

// Dump heap profiling information once every specified number of bytes
// allocated by the program.  Default: 1GB
void HeapProfilerSetAllocationInterval(size_t interval) {
  FLAGS_heap_profile_allocation_interval = interval;
}

// Dump heap profiling information whenever the high-water 
// memory usage mark increases by the specified number of
// bytes.  Default: 100MB
void HeapProfilerSetInuseInterval(size_t interval) {
  FLAGS_heap_profile_inuse_interval = interval;
}

//----------------------------------------------------------------------
// For printing messages without using malloc
//----------------------------------------------------------------------

void HeapProfiler::MESSAGE(int level, const char* format, ...) {
  if (FLAGS_heap_profile_log < level) return;

  // We write directly to the stderr file descriptor and avoid FILE
  // buffering because that may invoke malloc()
  va_list ap;
  va_start(ap, format);
  char buf[600];
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  write(STDERR_FILENO, buf, strlen(buf));
}

//----------------------------------------------------------------------
// Simple allocator
//----------------------------------------------------------------------

class HeapProfilerMemory {
 private:
  // Default unit of allocation from system
  static const int kBlockSize = 1 << 20;

  // Maximum number of blocks we can allocate
  static const int kMaxBlocks = 1024;

  // Info kept per allocated block
  struct Block {
    void*       ptr;
    size_t      size;
  };

  // Alignment
  union AlignUnion { double d; void* p; int64 i; size_t s; };
  static const int kAlignment = sizeof(AlignUnion);

  Block         blocks_[kMaxBlocks];    // List of allocated blocks
  int           nblocks_;               // # of allocated blocks
  char*         current_;               // Current block
  int           pos_;                   // Position in current block

  // Allocate a block with the specified size
  void* AllocBlock(size_t size) {
    // Round size upto a multiple of the page size
    const size_t pagesize = getpagesize();
    size = ((size + pagesize -1 ) / pagesize) * pagesize;

    HeapProfiler::MESSAGE(1, "HeapProfiler: allocating %"PRIuS
                          " bytes for internal use\n", size);
    if (nblocks_ == kMaxBlocks) {
      HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: Alloc out of memory\n");
      abort();
    }

    // Disable mmap hooks while calling mmap here to avoid recursive calls
    MallocHook::MmapHook saved = MallocHook::SetMmapHook(NULL);
    void* ptr = mmap(NULL, size,
                     PROT_READ|PROT_WRITE,
                     MAP_PRIVATE|MAP_ANONYMOUS,
                     -1, 0);
    MallocHook::SetMmapHook(saved);

    if (ptr == reinterpret_cast<void*>(MAP_FAILED)) {
      HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: mmap %"PRIuS": %s\n",
                            size, strerror(errno));
      abort();
    }
    blocks_[nblocks_].ptr = ptr;
    blocks_[nblocks_].size = size;
    return ptr;
  }

 public:
  void Init() {
    nblocks_ = 0;
    current_ = NULL;
    pos_ = kBlockSize;
  }

  void Clear() {
    // Disable munmap hooks while calling mmap here to avoid recursive calls
    MallocHook::MunmapHook saved = MallocHook::SetMunmapHook(NULL);
    for (int i = 0; i < nblocks_; ++i) {
      if (munmap(blocks_[i].ptr, blocks_[i].size) != 0) {
        HeapProfiler::MESSAGE(-1, "HeapProfilerMemory: munmap: %s\n",
                              strerror(errno));
        abort();
      }
    }
    MallocHook::SetMunmapHook(saved);

    nblocks_ = 0;
    current_ = NULL;
    pos_ = kBlockSize;
  }

  void* Alloc(size_t bytes) {
    if (bytes >= kBlockSize / 8) {
      // Too big for piecemeal allocation
      return AllocBlock(bytes);
    } else {
      if (pos_ + bytes > kBlockSize) {
        current_ = reinterpret_cast<char*>(AllocBlock(kBlockSize));
        pos_ = 0;
      }
      void* result = current_ + pos_;
      pos_ = (pos_ + bytes + kAlignment - 1) & ~(kAlignment-1);
      return result;
    }
  }
};
static HeapProfilerMemory heap_profiler_memory;
void* HeapProfiler::Malloc(size_t bytes) {
  return heap_profiler_memory.Alloc(bytes);
}
void HeapProfiler::Free(void* p) {
  // Do nothing -- all memory is released in one shot
}

//----------------------------------------------------------------------
// Locking code
//----------------------------------------------------------------------

// A pthread_mutex has way too much lock contention to be used here.
// In some applications we've run, pthread_mutex took >75% of the running
// time.
// I would like to roll our own mutex wrapper, but the obvious
// solutions can call malloc(), which can lead to infinite recursion.
//
// So we use a simple spinlock (just like the spinlocks used in tcmalloc)

static TCMalloc_SpinLock heap_lock;

void HeapProfiler::Lock() {
  if (kMaxLogging) {
    // for debugging deadlocks
    HeapProfiler::MESSAGE(10, "HeapProfiler: Lock from %d\n",
                          int(pthread_self()));
  }

  heap_lock.Lock();
}

void HeapProfiler::Unlock() {
  if (kMaxLogging) {
    HeapProfiler::MESSAGE(10, "HeapProfiler: Unlock from %d\n",
                          int(pthread_self()));
  }

  heap_lock.Unlock();
}


//----------------------------------------------------------------------
// Profile-maintenance code
//----------------------------------------------------------------------

typedef HeapProfiler::Bucket Bucket;

bool HeapProfiler::is_on_ = false;
bool HeapProfiler::init_has_been_called_ = false;
bool HeapProfiler::need_for_leaks_ = false;
bool HeapProfiler::self_disable_ = false;
pthread_t HeapProfiler::self_disabled_tid_;
HeapProfiler::IgnoredObjectSet* HeapProfiler::ignored_objects_ = NULL;
bool HeapProfiler::dump_for_leaks_ = false;
bool HeapProfiler::dumping_ = false;
Bucket HeapProfiler::total_;
Bucket HeapProfiler::self_disabled_;
Bucket HeapProfiler::profile_;
char* HeapProfiler::filename_prefix_ = NULL;

// Hash-table: we hand-craft one instead of using one of the pre-written
// ones because we do not want to use malloc when operating on the table.
// It is only five lines of code, so no big deal.
static const int kHashTableSize = 179999;
static Bucket** table = NULL;
HeapProfiler::AllocationMap* HeapProfiler::allocation_ = NULL;

static int     num_buckets = 0;
static int     total_stack_depth = 0;
static int     dump_count = 0;      // How many dumps so far
static int64   last_dump = 0;       // When did we last dump
static int64   high_water_mark = 0; // In-use-bytes at last high-water dump

int HeapProfiler::strip_frames_ = 0;
bool HeapProfiler::done_first_alloc_ = false;
void* HeapProfiler::recordalloc_reference_stack_position_ = NULL;

// For sorting buckets by in-use space
static bool ByAllocatedSpace(Bucket* a, Bucket* b) {
  // Return true iff "a" has more allocated space than "b"
  return (a->alloc_size_ - a->free_size_) > (b->alloc_size_ - b->free_size_);
}

int HeapProfiler::UnparseBucket(char* buf, int buflen, int bufsize,
                                const Bucket* b) {
  profile_.allocs_ += b->allocs_;
  profile_.alloc_size_ += b->alloc_size_;
  profile_.frees_ += b->frees_;
  profile_.free_size_ += b->free_size_;
  if (dump_for_leaks_  &&
      b->allocs_ - b->frees_ == 0  &&
      b->alloc_size_ - b->free_size_ == 0) {
    // don't waste the profile space on buckets that do not matter
    return buflen;
  }
  int printed =
    snprintf(buf + buflen, bufsize - buflen, "%6d: %8"LLD" [%6d: %8"LLD"] @",
             b->allocs_ - b->frees_,
             b->alloc_size_ - b->free_size_,
             b->allocs_,
             b->alloc_size_);
  // If it looks like the snprintf failed, ignore the fact we printed anything
  if (printed < 0 || printed >= bufsize - buflen)  return buflen;
  buflen += printed;
  for (int d = 0; d < b->depth_; d++) {
    printed = snprintf(buf + buflen, bufsize - buflen, " 0x%08lx",
                       (unsigned long)b->stack_[d]);
    if (printed < 0 || printed >= bufsize - buflen)  return buflen;
    buflen += printed;
  }
  printed = snprintf(buf + buflen, bufsize - buflen, "\n");
  if (printed < 0 || printed >= bufsize - buflen)  return buflen;
  buflen += printed;
  return buflen;
}

void HeapProfiler::AdjustByIgnoredObjects(int adjust) {
  if (ignored_objects_) {
    assert(dump_for_leaks_);
    for (IgnoredObjectSet::const_iterator i = ignored_objects_->begin();
         i != ignored_objects_->end(); ++i) {
      AllocValue v;
      if (!allocation_->Find(reinterpret_cast<void*>(*i), &v))  abort();
         // must be in
      v.bucket->allocs_ += adjust;
      v.bucket->alloc_size_ += adjust * int64(v.bytes);
        // need explicit size_t to int64 conversion before multiplication
        // in case size_t is unsigned and adjust is negative
      assert(v.bucket->allocs_ >= 0  &&  v.bucket->alloc_size_ >= 0);
      if (kMaxLogging  &&  adjust < 0) {
        HeapProfiler::MESSAGE(4, "HeapChecker: "
                              "Ignoring object of %"PRIuS" bytes\n", v.bytes);
      }
    }
  }
}

char* GetHeapProfile() {
  // We used to be smarter about estimating the required memory and
  // then capping it to 1MB and generating the profile into that.
  // However it should not cost us much to allocate 1MB every time.
  static const int size = 1 << 20;
  int buflen = 0;
  char* buf = reinterpret_cast<char*>(malloc(size));
  if (buf == NULL) {
    return NULL;
  }

  Bucket **list = NULL;

  // We can't allocate list on the stack, as this would overflow on threads
  // running with a small stack size.  We can't allocate it under the lock
  // either, as this would cause a deadlock.  But num_buckets is only valid
  // while holding the lock- new buckets can be created at any time otherwise.
  // So we'll read num_buckets dirtily, allocate room for all the current
  // buckets + a few more, and check the count when we get the lock; if we
  // don't have enough, we release the lock and try again.
  while (true) {
    int nb = num_buckets + num_buckets / 16 + 8;

    if (list)
      delete[] list;

    list = new Bucket *[nb];

    // Grab the lock and generate the profile
    // (for leak checking the lock is acquired higher up).
    if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Lock();
    if (!HeapProfiler::is_on_) {
      if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Unlock();
      break;
    }

    // Get all buckets and sort
    assert(table != NULL);

    // If we have allocated some extra buckets while waiting for the lock, we
    // may have to reallocate list
    if (num_buckets > nb) {
      if (!HeapProfiler::dump_for_leaks_) HeapProfiler::Unlock();
      continue;
    }

    int n = 0;
    for (int b = 0; b < kHashTableSize; b++) {
      for (Bucket* x = table[b]; x != 0; x = x->next_) {
        list[n++] = x;
      }
    }
    assert(n == num_buckets);
    sort(list, list + num_buckets, ByAllocatedSpace);

    buflen = snprintf(buf, size-1, "heap profile: ");
    buflen = HeapProfiler::UnparseBucket(buf, buflen, size-1,
                                         &HeapProfiler::total_);
    memset(&HeapProfiler::profile_, 0, sizeof(HeapProfiler::profile_));
    HeapProfiler::AdjustByIgnoredObjects(-1);  // drop from profile
    for (int i = 0; i < num_buckets; i++) {
      Bucket* b = list[i];
      buflen = HeapProfiler::UnparseBucket(buf, buflen, size-1, b);
    }
    HeapProfiler::AdjustByIgnoredObjects(1);  // add back to profile
    assert(buflen < size);
    if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Unlock();
    break;
  }

  buf[buflen] = '\0';
  delete[] list;

  return buf;
}

// We keep HeapProfile() as a backwards-compatible name for GetHeapProfile(),
// but don't export the symbol, so you probably won't be able to call this.
extern char* HeapProfile() {
  return GetHeapProfile();
}

void HeapProfiler::DumpLocked(const char *reason, const char* file_name) {
  assert(is_on_);

  if (filename_prefix_ == NULL  &&  file_name == NULL)  return;
    // we do not yet need dumping

  dumping_ = true;

  // Make file name
  char fname[1000];
  if (file_name == NULL) {
    dump_count++;
    snprintf(fname, sizeof(fname), "%s.%04d.heap",
             filename_prefix_, dump_count);
    file_name = fname;
  }

  // Release allocation lock around the meat of this routine
  // when not leak checking thus not blocking other threads too much,
  // but for leak checking we want to hold the lock to prevent heap activity.
  if (!dump_for_leaks_)  HeapProfiler::Unlock();
  {
    // Dump the profile
    HeapProfiler::MESSAGE(dump_for_leaks_ ? 1 : 0,
                          "HeapProfiler: "
                          "Dumping heap profile to %s (%s)\n",
                          file_name, reason);
    FILE* f = fopen(file_name, "w");
    if (f != NULL) {
      const char* profile = HeapProfile();
      fputs(profile, f);
      free(const_cast<char*>(profile));

      // Dump "/proc/self/maps" so we get list of mapped shared libraries
      fputs("\nMAPPED_LIBRARIES:\n", f);
      int maps = open("/proc/self/maps", O_RDONLY);
      if (maps >= 0) {
        char buf[100];
        ssize_t r;
        while ((r = read(maps, buf, sizeof(buf))) > 0) {
          fwrite(buf, 1, r, f);
        }
        close(maps);
      }

      fclose(f);
      f = NULL;
    } else {
      HeapProfiler::MESSAGE(0, "HeapProfiler: "
                            "FAILED Dumping heap profile to %s (%s)\n",
                            file_name, reason);
      if (dump_for_leaks_)  abort();  // no sense to continue
    }
  }

  if (!dump_for_leaks_)  HeapProfiler::Lock();

  dumping_ = false;
}

void HeapProfilerDump(const char *reason) {
  if (HeapProfiler::is_on_ && (num_buckets > 0)) {

    HeapProfiler::Lock();
    if (!HeapProfiler::dumping_) {
      HeapProfiler::DumpLocked(reason, NULL);
    }
    HeapProfiler::Unlock();
  }
}

// Allocation map for heap objects (de)allocated
// while HeapProfiler::self_disable_ is true.
// We use it to test if heap leak checking itself changed the heap state.
// An own map seems cleaner than trying to keep everything
// in HeapProfiler::allocation_.
HeapProfiler::AllocationMap* self_disabled_allocation = NULL;

// This is the number of bytes allocated by the first call to malloc() after
// registering this handler.  We want to sanity check that our first call is
// actually for this number of bytes.
static const int kFirstAllocationNumBytes = 23;

void HeapProfiler::RecordAlloc(void* ptr, size_t bytes, int skip_count) {
  // Our first allocation is triggered in EarlyStartLocked and is intended
  // solely to calibrate strip_frames_, which may be greater or smaller
  // depending on the degree of optimization with which we were compiled.
  if (!done_first_alloc_) {
    done_first_alloc_ = true;
    assert(bytes == kFirstAllocationNumBytes);
    assert(strip_frames_ == 0);

    static const int kMaxStackTrace = 32;
    void* stack[kMaxStackTrace];
    // We skip one frame here so that it's as if we are running from NewHook,
    // which is where strip_frames_ is used.
    int depth = GetStackTrace(stack, kMaxStackTrace, 1);

    int i;
    for (i = 0; i < depth; i++) {
      if (stack[i] == recordalloc_reference_stack_position_) {
        MESSAGE(1, "Determined strip_frames_ to be %d\n", i - 1);
        // Subtract one to offset the fact that
        // recordalloc_reference_stack_position_ actually records the stack
        // position one frame above the spot in EarlyStartLocked where we are
        // called from.
        strip_frames_ = i - 1;
      }
    }
    // Fell through the loop without finding our parent
    if (strip_frames_ == 0) {
      MESSAGE(0, "Could not determine strip_frames_, aborting");
      abort();
    }

    // Return without recording the allocation.  We will free the memory before
    // registering a DeleteHook.
    return;
  }

  // this locking before if (is_on_ ...)
  // is not an overhead because with profiling off
  // this hook is not called at all.

  if (kMaxLogging) {
    HeapProfiler::MESSAGE(7, "HeapProfiler: Alloc: %p of %"PRIuS" from %d\n",
                          ptr, bytes, int(pthread_self()));
  }

  if (self_disable_  &&  self_disabled_tid_ == pthread_self()) {
    self_disabled_.allocs_++;
    self_disabled_.alloc_size_ += bytes;
    AllocValue v;
    v.bucket = NULL;  // initialize just to make smart tools happy
                      // (no one will read it)
    v.bytes = bytes;
    self_disabled_allocation->Insert(ptr, v);
    return;
  }

  HeapProfiler::Lock();
  if (is_on_) {
    Bucket* b = GetBucket(skip_count+1);
    b->allocs_++;
    b->alloc_size_ += bytes;
    total_.allocs_++;
    total_.alloc_size_ += bytes;

    AllocValue v;
    v.bucket = b;
    v.bytes = bytes;
    allocation_->Insert(ptr, v);

    if (kMaxLogging) {
      HeapProfiler::MESSAGE(8, "HeapProfiler: Alloc Recorded: %p of %"PRIuS"\n",
                            ptr, bytes);
    }

    const int64 inuse_bytes = total_.alloc_size_ - total_.free_size_;
    if (!dumping_) {
      bool need_dump = false;
      char buf[128];
      if (total_.alloc_size_ >=
          last_dump + FLAGS_heap_profile_allocation_interval) {
        snprintf(buf, sizeof(buf), "%"LLD" MB allocated",
                 total_.alloc_size_ >> 20);
        // Track that we made a "total allocation size" dump
        last_dump = total_.alloc_size_;
        need_dump = true;
      } else if(inuse_bytes >
                high_water_mark + FLAGS_heap_profile_inuse_interval) {
        sprintf(buf, "%"LLD" MB in use", inuse_bytes >> 20);
        // Track that we made a "high water mark" dump
        high_water_mark = inuse_bytes;
        need_dump = true;
      }

      if (need_dump) {
        // Dump profile
        DumpLocked(buf, NULL);
      }
    }
  }
  HeapProfiler::Unlock();
}

void HeapProfiler::RecordFree(void* ptr) {
  // All activity before if (is_on_)
  // is not an overhead because with profiling turned off this hook
  // is not called at all.

  if (kMaxLogging) {
    HeapProfiler::MESSAGE(7, "HeapProfiler: Free %p from %d\n",
                          ptr, int(pthread_self()));
  }

  if (self_disable_  &&  self_disabled_tid_ == pthread_self()) {
    AllocValue v;
    if (self_disabled_allocation->FindAndRemove(ptr, &v)) {
      self_disabled_.free_size_ += v.bytes;
      self_disabled_.frees_++;
    } else {
      // Try to mess the counters up and fail later in
      // HeapLeakChecker::DumpProfileLocked instead of failing right now:
      // presently execution gets here only from within the guts
      // of pthread library and only when being in an address space
      // that is about to disappear completely.
      // I.e. failing right here is wrong, but failing later if
      // this happens in the course of normal execution is needed.
      self_disabled_.free_size_ += 100000000;
      self_disabled_.frees_ += 100000000;
    }
    return;
  }

  HeapProfiler::Lock();
  if (is_on_) {
    AllocValue v;
    if (allocation_->FindAndRemove(ptr, &v)) {
      Bucket* b = v.bucket;
      b->frees_++;
      b->free_size_ += v.bytes;
      total_.frees_++;
      total_.free_size_ += v.bytes;

      if (kMaxLogging) {
        HeapProfiler::MESSAGE(8, "HeapProfiler: Free Recorded: %p\n", ptr);
      }
    }
  }
  HeapProfiler::Unlock();
}

bool HeapProfiler::HaveOnHeapLocked(void** ptr, AllocValue* alloc_value) {
  assert(is_on_);
  // Size of the C++ object array size integer
  // (potentially compiler/dependent; 4 on i386 and gcc)
  const int kArraySizeOffset = sizeof(int);
  // sizeof(basic_string<...>::_Rep) for C++ library of gcc 3.4
  // (basically three integer counters;
  // library/compiler dependent; 12 on i386 and gcc)
  const int kStringOffset = sizeof(int) * 3;
  // NOTE: One can add more similar offset cases below
  //       even when they do not happen for the used compiler/library;
  //       all that's impacted is
  //       - HeapLeakChecker's performace during live heap walking
  //       - and a slightly greater chance to mistake random memory bytes
  //         for a pointer and miss a leak in a particular run of a binary.
  bool result = true;
  if (allocation_->Find(*ptr, alloc_value)) {
    // done
  } else if (allocation_->Find(reinterpret_cast<char*>(*ptr)
                               - kArraySizeOffset,
                               alloc_value)  &&
             alloc_value->bytes > kArraySizeOffset) {
    // this case is to account for the array size stored inside of
    // the memory allocated by new FooClass[size] for classes with destructors
    *ptr = reinterpret_cast<char*>(*ptr) - kArraySizeOffset;
    if (kMaxLogging) {
      HeapProfiler::MESSAGE(7, "HeapProfiler: Got poiter into %p at +%d\n",
                            ptr, kArraySizeOffset);
    }
  } else if (allocation_->Find(reinterpret_cast<char*>(*ptr)
                               - kStringOffset,
                               alloc_value)  &&
             alloc_value->bytes > kStringOffset) {
    // this case is to account for basic_string<> representation in
    // newer C++ library versions when the kept pointer points to inside of
    // the allocated region
    *ptr = reinterpret_cast<char*>(*ptr) - kStringOffset;
    if (kMaxLogging) {
      HeapProfiler::MESSAGE(7, "HeapProfiler: Got poiter into %p at +%d\n",
                            ptr, kStringOffset);
    }
  } else {
    result = false;
  }
  return result;
}

bool HeapProfiler::HaveOnHeap(void** ptr, AllocValue* alloc_value) {
  HeapProfiler::Lock();
  bool result = is_on_  &&  HaveOnHeapLocked(ptr, alloc_value);
  HeapProfiler::Unlock();
  return result;
}

//----------------------------------------------------------------------
// Allocation/deallocation hooks
//----------------------------------------------------------------------

void HeapProfiler::NewHook(void* ptr, size_t size) {
  if (ptr != NULL) RecordAlloc(ptr, size, strip_frames_);
}

void HeapProfiler::DeleteHook(void* ptr) {
  if (ptr != NULL) RecordFree(ptr);
}

void HeapProfiler::MmapHook(void* result,
                            void* start, size_t size,
                            int prot, int flags,
                            int fd, off_t offset) {
  // Log the mmap if necessary
  if (FLAGS_mmap_log) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "mmap(start=%p, len=%"PRIuS", prot=0x%x, flags=0x%x, "
             "fd=%d, offset=0x%x) = %p",
             start, size, prot, flags, fd, (unsigned int) offset,
             result);
    LOGF << buf;
    // TODO(jandrews): Re-enable stack tracing
    //DumpStackTrace(1, DebugWriteToStream, &LOG(INFO));
  }

  // Record mmap in profile if appropriate
  if (result != (void*) MAP_FAILED &&
      FLAGS_mmap_profile &&
      is_on_) {

    RecordAlloc(result, size, strip_frames_);
  }
}

void HeapProfiler::MunmapHook(void* ptr, size_t size) {
  if (FLAGS_mmap_profile && is_on_) {
    RecordFree(ptr);
  }
  if (FLAGS_mmap_log) {
    char buf[200];
    snprintf(buf, sizeof(buf), "munmap(start=%p, len=%"PRIuS")", ptr, size);
    LOGF << buf;
  }
}

//----------------------------------------------------------------------
// Profiler maintenance
//----------------------------------------------------------------------

Bucket* HeapProfiler::GetBucket(int skip_count) {
  // Get raw stack trace
  static const int kMaxStackTrace = 32;
  void* key[kMaxStackTrace];
  int depth = GetStackTrace(key, kMaxStackTrace, skip_count+1);

  // Make hash-value
  uintptr_t h = 0;
  for (int i = 0; i < depth; i++) {
    uintptr_t pc = reinterpret_cast<uintptr_t>(key[i]);
    h = (h << 8) | (h >> (8*(sizeof(h)-1)));
    h += (pc * 31) + (pc * 7) + (pc * 3);
  }

  // Lookup stack trace in table
  const size_t key_size = sizeof(key[0]) * depth;
  unsigned int buck = ((unsigned int) h) % kHashTableSize;
  for (Bucket* b = table[buck]; b != 0; b = b->next_) {
    if ((b->hash_ == h) &&
        (b->depth_ == depth) &&
        (memcmp(b->stack_, key, key_size) == 0)) {
      return b;
    }
  }

  // Create new bucket
  void** kcopy = reinterpret_cast<void**>(Malloc(key_size));
  memcpy(kcopy, key, key_size);
  Bucket* b = reinterpret_cast<Bucket*>(Malloc(sizeof(Bucket)));
  memset(b, 0, sizeof(*b));
  b->hash_      = h;
  b->depth_     = depth;
  b->stack_     = kcopy;
  b->next_      = table[buck];
  table[buck] = b;
  num_buckets++;
  total_stack_depth += depth;
  return b;
}

void HeapProfiler::EarlyStartLocked() {
  assert(!is_on_);

  heap_profiler_memory.Init();

  is_on_ = true;
  // we should be really turned off:
  if (need_for_leaks_)  abort();
  if (self_disable_)  abort();
  if (filename_prefix_ != NULL)  abort();

  // Make the table
  const int table_bytes = kHashTableSize * sizeof(Bucket*);
  table = reinterpret_cast<Bucket**>(Malloc(table_bytes));
  memset(table, 0, table_bytes);

  // Make allocation map
  void* aptr = Malloc(sizeof(AllocationMap));
  allocation_ = new (aptr) AllocationMap(Malloc, Free);

  memset(&total_, 0, sizeof(total_));
  num_buckets = 0;
  total_stack_depth = 0;
  last_dump = 0;
  // We do not reset dump_count so if the user does a sequence of
  // HeapProfilerStart/HeapProfileStop, we will get a continuous
  // sequence of profiles.

  // Now set the hooks that capture mallocs/frees
  MallocHook::SetNewHook(NewHook);

  // Our first allocation after registering our hook is treated specially by
  // RecordAlloc();  It looks at the stack and counts how many frames up we
  // are.  First we record the current stack pointer.
  // Note: The stacktrace implementations differ about how many args they
  // fill when skip is non-zero.  Safest just to reserve maxdepth space.
  void* here[2];
  GetStackTrace(here, 2, 1);
  // Skip the first frame.  It points to the current offset within this
  // function, which will have changed by the time we get to the malloc()
  // call which triggers.  Instead, we store our parent function's offset,
  // which is shared by both us and by the malloc() call below.
  recordalloc_reference_stack_position_ = here[0];
  done_first_alloc_ = false; // Initialization has not occured yet
  void* first_alloc = malloc(kFirstAllocationNumBytes);
  free(first_alloc);

  MallocHook::SetDeleteHook(DeleteHook);

  HeapProfiler::MESSAGE(1, "HeapProfiler: Starting heap tracking\n");
}

void HeapProfiler::StartLocked(const char* prefix) {
  if (filename_prefix_ != NULL) return;

  if (!is_on_) {
    EarlyStartLocked();
  }

  // Copy filename prefix
  const int prefix_length = strlen(prefix);
  filename_prefix_ = reinterpret_cast<char*>(Malloc(prefix_length + 1));
  memcpy(filename_prefix_, prefix, prefix_length);
  filename_prefix_[prefix_length] = '\0';
}

void HeapProfiler::StopLocked() {
  if (!is_on_) return;

  filename_prefix_ = NULL;

  if (need_for_leaks_)  return;

  // Turn us off completely:

  MallocHook::SetNewHook(NULL);
  MallocHook::SetDeleteHook(NULL);

  // Get rid of all memory we allocated
  heap_profiler_memory.Clear();

  table             = NULL;
  allocation_       = NULL;
  is_on_            = false;
}

void HeapProfiler::StartForLeaks() {
  Lock();

  if (!is_on_) {
    EarlyStartLocked();  // fire-up HeapProfiler hooks
  }
  need_for_leaks_ = true;

  memset(&self_disabled_, 0, sizeof(self_disabled_));  // zero the counters

  // Make allocation map for self-disabled allocations
  void* aptr = Malloc(sizeof(AllocationMap));
  self_disabled_allocation = new (aptr) AllocationMap(Malloc, Free);

  Unlock();
}

void HeapProfiler::StopForLeaks() {
  Lock();
  need_for_leaks_ = false;
  if (filename_prefix_ == NULL) StopLocked();
  Unlock();
}

void HeapProfilerStart(const char* prefix) {
  HeapProfiler::Lock();
  HeapProfiler::StartLocked(prefix);
  HeapProfiler::Unlock();
}

void HeapProfilerStop() {
  HeapProfiler::Lock();
  HeapProfiler::StopLocked();
  HeapProfiler::Unlock();
}

//----------------------------------------------------------------------
// Initialization/finalization code
//----------------------------------------------------------------------

// Initialization code
void HeapProfiler::Init() {
  // depending on the ordering of the global constructors (undefined
  // according to the C++ spec, HeapProfiler::Init() can either be
  // called from this file directly, or from heap-checker.cc's global
  // constructor if it gets run first.  Either way is fine by us; we
  // just want to be sure not to run twice.
  if (init_has_been_called_)  return;  // we were already run, I guess
  init_has_been_called_ = true;

  // We want to make sure tcmalloc is set up properly, in order to
  // profile as much as we can.
  MallocExtension::Initialize();

  if (FLAGS_mmap_profile || FLAGS_mmap_log) {
    MallocHook::SetMmapHook(MmapHook);
    MallocHook::SetMunmapHook(MunmapHook);
  }

  // Everything after this point is for setting up the profiler based on envvar

  char* heapprofile = getenv("HEAPPROFILE");
  if (!heapprofile || heapprofile[0] == '\0') {
    return;
  }
  // We do a uid check so we don't write out files in a setuid executable.
  if (getuid() != geteuid()) {
    HeapProfiler::MESSAGE(0, ("HeapProfiler: ignoring HEAPPROFILE because "
                              "program seems to be setuid\n"));
    return;
  }

  // If we're a child process of the 'main' process, we can't just use
  // the name HEAPPROFILE -- the parent process will be using that.
  // Instead we append our pid to the name.  How do we tell if we're a
  // child process?  Ideally we'd set an environment variable that all
  // our children would inherit.  But -- and perhaps this is a bug in
  // gcc -- if you do a setenv() in a shared libarary in a global
  // constructor, the environment setting is lost by the time main()
  // is called.  The only safe thing we can do in such a situation is
  // to modify the existing envvar.  So we do a hack: in the parent,
  // we set the high bit of the 1st char of HEAPPROFILE.  In the child,
  // we notice the high bit is set and append the pid().  This works
  // assuming cpuprofile filenames don't normally have the high bit
  // set in their first character!  If that assumption is violated,
  // we'll still get a profile, but one with an unexpected name.
  // TODO(csilvers): set an envvar instead when we can do it reliably.
  char fname[PATH_MAX];
  if (heapprofile[0] & 128) {                   // high bit is set
    snprintf(fname, sizeof(fname), "%c%s_%u",   // add pid and clear high bit
             heapprofile[0] & 127, heapprofile+1, (unsigned int)(getpid()));
  } else {
    snprintf(fname, sizeof(fname), "%s", heapprofile);
    heapprofile[0] |= 128;                      // set high bit for kids to see
  }

  CleanupProfiles(fname);

  HeapProfilerStart(fname);
}

void HeapProfiler::CleanupProfiles(const char* prefix) {
  if (!FLAGS_cleanup_old_heap_profiles)
    return;
  string pattern(prefix);
  pattern += ".*.heap";
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  if (r == 0 || r == GLOB_NOMATCH) {
    const int prefix_length = strlen(prefix);
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        HeapProfiler::MESSAGE(0, "HeapProfiler: "
                              "Removing old profile %s\n", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);
}

// class used for finalization -- dumps the heap-profile at program exit
class HeapProfileEndWriter {
 public:
  ~HeapProfileEndWriter() {
    HeapProfilerDump("Exiting");
  }
};

REGISTER_MODULE_INITIALIZER(heapprofile, HeapProfiler::Init());
static HeapProfileEndWriter heap_profile_end_writer;
