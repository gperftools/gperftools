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

#include <google/perftools/config.h>

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
#include <google/malloc_hook.h>
#include <google/perftools/basictypes.h>

#include "heap-profiler-inl.h"
#include "internal_spinlock.h"
#include "addressmap-inl.h"

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

#define LOGF  STL_NAMESPACE::cout   // where we log to; LOGF is a historical name

using HASH_NAMESPACE::hash_set;
using std::string;
using std::sort;

//----------------------------------------------------------------------
// Flags that control heap-profiling
//----------------------------------------------------------------------

DEFINE_string(heap_profile, "",
              "If non-empty, turn heap-profiling on, and dump heap "
              "profiles to a sequence of files prefixed with the "
              "specified --heap_profile string.");
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

// Prefix to which we dump heap profiles.  If empty, we do not dump.
// Default: empty
void HeapProfilerSetDumpPath(const char* path) {
  if (HeapProfiler::IsOn()) {
    HeapProfiler::MESSAGE(-1,
      "Cannot set dump path to %s, heap profiler is already running!\n",
      path);
  } else {
    FLAGS_heap_profile = path;
  }
}

// Level of logging used by the heap profiler and heap checker (if applicable)
// Default: 0
void HeapProfilerSetLogLevel(int level) {
  FLAGS_heap_profile_log = level;
}

// Dump heap profiling information once every specified number of bytes
// allocated by the program.  Default: 1GB
void HeapProfilerSetAllocationInterval(int64 interval) {
  FLAGS_heap_profile_allocation_interval = interval;
}

// Dump heap profiling information whenever the high-water 
// memory usage mark increases by the specified number of
// bytes.  Default: 100MB
void HeapProfilerSetInuseInterval(int64 interval) {
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
  char buf[500];
  vsnprintf(buf, sizeof(buf), format, ap);
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

    HeapProfiler::MESSAGE(0, "HeapProfiler: allocating %"PRIuS
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
static struct timespec delay = { 0, 5000000 };  // Five milliseconds

void HeapProfiler::Lock() {
  heap_lock.Lock();
}

void HeapProfiler::Unlock() {
  heap_lock.Unlock();
}


//----------------------------------------------------------------------
// Profile-maintenance code
//----------------------------------------------------------------------

typedef HeapProfiler::Bucket Bucket;

bool HeapProfiler::is_on_ = false;
bool HeapProfiler::temp_disable_ = false;
pthread_t HeapProfiler::temp_disabled_tid_;
HeapProfiler::DisabledAddressesSet* HeapProfiler::disabled_addresses_ = NULL;
HeapProfiler::DisabledRangeMap* HeapProfiler::disabled_ranges_ = NULL;
bool HeapProfiler::dump_for_leaks_ = false;
bool HeapProfiler::dumping_ = false;
Bucket HeapProfiler::total_;
Bucket HeapProfiler::disabled_;
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

// We return the amount of space in buf that we use.  We start printing
// at buf + buflen, and promise not to go beyond buf + bufsize.
int HeapProfiler::UnparseBucket(char* buf, int buflen, int bufsize, Bucket* b) {
  // do not dump the address-disabled allocations
  if (dump_for_leaks_  &&  (disabled_addresses_ || disabled_ranges_)) {
    bool disable = false;
    for (int depth = 0; !disable && depth < b->depth_; depth++) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(b->stack_[depth]);
      if (disabled_addresses_  &&
          disabled_addresses_->find(addr) != disabled_addresses_->end()) {
        disable = true;  // found; dropping
      }
      if (disabled_ranges_) {
        DisabledRangeMap::const_iterator iter
          = disabled_ranges_->lower_bound(addr);
        if (iter != disabled_ranges_->end()) {
          assert(iter->first > addr);
          if (iter->second.start_address < addr  &&
              iter->second.max_depth > depth) {
            disable = true;  // in range; dropping
          }
        }
      }
    }
    if (disable) {
      disabled_.allocs_ += b->allocs_;
      disabled_.alloc_size_ += b->alloc_size_;
      disabled_.frees_ += b->frees_;
      disabled_.free_size_ += b->free_size_;
      return buflen;
    }
  }
  // count non-disabled allocations for leaks checking
  profile_.allocs_ += b->allocs_;
  profile_.alloc_size_ += b->alloc_size_;
  profile_.frees_ += b->frees_;
  profile_.free_size_ += b->free_size_;
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

char* GetHeapProfile() {
  // We used to be smarter about estimating the required memory and
  // then capping it to 1MB and generating the profile into that.
  // However it should not cost us much to allocate 1MB every time.
  static const int size = 1 << 20;
  char* buf = reinterpret_cast<char*>(malloc(size));
  if (buf == NULL) {
    return NULL;
  }

  // Grab the lock and generate the profile
  // (for leak checking the lock is acquired higher up).
  if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Lock();
  if (HeapProfiler::is_on_) {
    // Get all buckets and sort
    assert(table != NULL);
    Bucket* list[num_buckets];
    int n = 0;
    for (int b = 0; b < kHashTableSize; b++) {
      for (Bucket* x = table[b]; x != 0; x = x->next_) {
        list[n++] = x;
      }
    }
    assert(n == num_buckets);
    sort(list, list + num_buckets, ByAllocatedSpace);

    int buflen = snprintf(buf, size-1, "heap profile: ");
    buflen =
      HeapProfiler::UnparseBucket(buf, buflen, size-1, &HeapProfiler::total_);
    memset(&HeapProfiler::profile_, 0, sizeof(HeapProfiler::profile_));
    memset(&HeapProfiler::disabled_, 0, sizeof(HeapProfiler::disabled_));
    for (int i = 0; i < num_buckets; i++) {
      Bucket* b = list[i];
      buflen = HeapProfiler::UnparseBucket(buf, buflen, size-1, b);
    }
    assert(buflen < size);
    buf[buflen] = '\0';
  }
  if (!HeapProfiler::dump_for_leaks_)  HeapProfiler::Unlock();

  return buf;
}

// We keep HeapProfile() as a backwards-compatible name for GetHeapProfile(),
// but don't export the symbol, so you probably won't be able to call this.
extern char* HeapProfile() {
  return GetHeapProfile();
}

// second_prefix is not NULL when the dumped profile
// is to be named differently for leaks checking
void HeapProfiler::DumpLocked(const char *reason, const char* second_prefix) {
  assert(is_on_);

  if (filename_prefix_ == NULL)  return;
    // we are not yet ready for dumping

  dumping_ = true;

  // Make file name
  char fname[1000];
  if (second_prefix == NULL) {
    dump_count++;
    snprintf(fname, sizeof(fname), "%s.%04d.heap",
             filename_prefix_, dump_count);
  } else {
    snprintf(fname, sizeof(fname), "%s.%s.heap",
             filename_prefix_, second_prefix);
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
                          fname, reason);
    FILE* f = fopen(fname, "w");
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
                            fname, reason);
      if (dump_for_leaks_)  abort();  // no sense to continue
    }
  }

  if (!dump_for_leaks_)  HeapProfiler::Lock();

  dumping_ = false;
}

void HeapProfilerDump(const char *reason) {
  if (HeapProfiler::is_on_ && (num_buckets > 0)) {

    HeapProfiler::Lock();
    if(!HeapProfiler::dumping_) {
      HeapProfiler::DumpLocked(reason, NULL);
    }
    HeapProfiler::Unlock();
  }
}

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
        MESSAGE(-1, "Determined strip_frames_ to be %d\n", i - 1);
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

  // Uncomment for debugging:
  // HeapProfiler::MESSAGE(7, "HeapProfiler: Alloc %p : %"PRIuS"\n",
  //                       ptr, bytes);

  if (temp_disable_  &&  temp_disabled_tid_ == pthread_self())  return;
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

    const int64 inuse_bytes = total_.alloc_size_ - total_.free_size_;
    if (!dumping_) {
      bool need_dump = false;
      char buf[128];
      if(total_.alloc_size_ >=
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

void HeapProfiler::RecordFreeLocked(void* ptr) {
  assert(is_on_);
  AllocValue v;
  if (allocation_->FindAndRemove(ptr, &v)) {
    Bucket* b = v.bucket;
    b->frees_++;
    b->free_size_ += v.bytes;
    total_.frees_++;
    total_.free_size_ += v.bytes;
  }
}

void HeapProfiler::RecordFree(void* ptr) {
  // All activity before if (is_on_)
  // is not an overhead because with profiling turned off this hook
  // is not called at all.

  // Uncomment for debugging:
  // HeapProfiler::MESSAGE(7, "HeapProfiler: Free %p\n", ptr);

  if (temp_disable_  &&  temp_disabled_tid_ == pthread_self())  return;
  HeapProfiler::Lock();
  if (is_on_)  RecordFreeLocked(ptr);
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
  } else if (allocation_->Find(reinterpret_cast<char*>(*ptr)
                               - kStringOffset,
                               alloc_value)  &&
             alloc_value->bytes > kStringOffset) {
    // this case is to account for basic_string<> representation in
    // newer C++ library versions when the kept pointer points to inside of
    // the allocated region
    *ptr = reinterpret_cast<char*>(*ptr) - kStringOffset;
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

  // GNU libc++ versions 3.3 and 3.4 obey the environment variables
  // GLIBCPP_FORCE_NEW and GLIBCXX_FORCE_NEW respectively.  Setting one of
  // these variables forces the STL default allocator to call new() or delete()
  // for each allocation or deletion.  Otherwise the STL allocator tries to
  // avoid the high cost of doing allocations by pooling memory internally.
  // This STL pool makes it impossible to get an accurate heap profile.
  // Luckily, our tcmalloc implementation gives us similar performance
  // characteristics *and* allows to to profile accurately.
  setenv("GLIBCPP_FORCE_NEW", "1", false /* no overwrite*/);
  setenv("GLIBCXX_FORCE_NEW", "1", false /* no overwrite*/);

  heap_profiler_memory.Init();

  is_on_ = true;
  if (temp_disable_) abort();
  filename_prefix_ = NULL;

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
  void* here[1];
  GetStackTrace(here, 1, 0);
  // This actually records the frame above this one.  We take this into account
  // in RecordAlloc.
  recordalloc_reference_stack_position_ = here[0];
  done_first_alloc_ = false; // Initialization has not occured yet
  void* first_alloc = malloc(kFirstAllocationNumBytes);
  free(first_alloc);

  MallocHook::SetDeleteHook(DeleteHook);

  HeapProfiler::MESSAGE(0, "HeapProfiler: Starting heap tracking\n");
}

void HeapProfiler::StartLocked(const char* prefix) {
  assert(filename_prefix_ == NULL);

  if (!is_on_) EarlyStartLocked();

  // Copy filename prefix
  const int prefix_length = strlen(prefix);
  filename_prefix_ = reinterpret_cast<char*>(Malloc(prefix_length + 1));
  memcpy(filename_prefix_, prefix, prefix_length);
  filename_prefix_[prefix_length] = '\0';
}

void HeapProfiler::StopLocked() {
  assert(is_on_);
  MallocHook::SetNewHook(NULL);
  MallocHook::SetDeleteHook(NULL);

  // Get rid of all memory we allocated
  heap_profiler_memory.Clear();

  table             = NULL;
  filename_prefix_  = NULL;
  allocation_       = NULL;
  is_on_            = false;
}

void HeapProfilerStart(const char* prefix) {
  HeapProfiler::Lock();
  if (HeapProfiler::filename_prefix_ == NULL) {
    HeapProfiler::StartLocked(prefix);
  }
  HeapProfiler::Unlock();
}

void HeapProfilerStop() {
  HeapProfiler::Lock();
  if (HeapProfiler::is_on_) HeapProfiler::StopLocked();
  HeapProfiler::Unlock();
}

//----------------------------------------------------------------------
// Initialization/finalization code
//----------------------------------------------------------------------

// helper function for HeapProfiler::Init()
inline static bool GlobOk(int r) {
  return r == 0 || r == GLOB_NOMATCH;
}

// Initialization code
void HeapProfiler::Init() {
  if (FLAGS_mmap_profile || FLAGS_mmap_log) {
    MallocHook::SetMmapHook(MmapHook);
    MallocHook::SetMunmapHook(MunmapHook);
  }

  if (FLAGS_heap_profile.empty()) return;

  // Cleanup any old profile files
  string pattern = FLAGS_heap_profile + ".[0-9][0-9][0-9][0-9].heap";
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  pattern = FLAGS_heap_profile + ".*-beg.heap";
  const int r2 = glob(pattern.c_str(), GLOB_ERR|GLOB_APPEND, NULL, &g);
  pattern = FLAGS_heap_profile + ".*-end.heap";
  const int r3 = glob(pattern.c_str(), GLOB_ERR|GLOB_APPEND, NULL, &g);
  if (GlobOk(r) && GlobOk(r2) && GlobOk(r3)) {
    const int prefix_length = FLAGS_heap_profile.size();
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, FLAGS_heap_profile.data(), prefix_length) == 0)) {
        HeapProfiler::MESSAGE(0, "HeapProfiler: "
                              "Removing old profile %s\n", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);

  HeapProfilerStart(FLAGS_heap_profile.c_str());
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
