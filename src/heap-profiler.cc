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

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#include <errno.h>
#include <assert.h>

#include <algorithm>
#include <string>

#include <google/heap-profiler.h>

#include "base/logging.h"
#include "base/basictypes.h"   // for PRId64, among other things
#include "base/googleinit.h"
#include "base/commandlineflags.h"
#include <google/malloc_hook.h>
#include <google/malloc_extension.h>
#include "base/spinlock.h"
#include "base/low_level_alloc.h"
#include "base/sysinfo.h"      // for GetUniquePathFromEnv()
#include "heap-profile-table.h"
#include "memory_region_map.h"


#ifndef	PATH_MAX
#ifdef MAXPATHLEN
#define	PATH_MAX	MAXPATHLEN
#else
#define	PATH_MAX	4096         // seems conservative for max filename len!
#endif
#endif

using STL_NAMESPACE::string;
using STL_NAMESPACE::sort;

//----------------------------------------------------------------------
// Flags that control heap-profiling
//----------------------------------------------------------------------

DEFINE_int64(heap_profile_allocation_interval,
             EnvToInt64("HEAP_PROFILE_ALLOCATION_INTERVAL", 1 << 30 /*1GB*/),
             "Dump heap profiling information once every specified "
             "number of bytes allocated by the program.");
DEFINE_int64(heap_profile_inuse_interval,
             EnvToInt64("HEAP_PROFILE_INUSE_INTERVAL", 100 << 20 /*100MB*/),
              "Dump heap profiling information whenever the high-water "
             "memory usage mark increases by the specified number of "
             "bytes.");
DEFINE_bool(mmap_log,
            EnvToBool("HEAP_PROFILE_MMAP_LOG", false),
            "Should mmap/munmap calls be logged?");
DEFINE_bool(mmap_profile,
            EnvToBool("HEAP_PROFILE_MMAP", false),
            "If heap-profiling is on, also profile mmap, mremap, and sbrk)");
DEFINE_bool(only_mmap_profile,
            EnvToBool("HEAP_PROFILE_ONLY_MMAP", false),
            "If heap-profiling is on, only profile mmap, mremap, and sbrk; "
            "do not profile malloc/new/etc");


//----------------------------------------------------------------------
// Locking
//----------------------------------------------------------------------

// A pthread_mutex has way too much lock contention to be used here.
//
// I would like to use Mutex, but it can call malloc(),
// which can cause us to fall into an infinite recursion.
//
// So we use a simple spinlock.
static SpinLock heap_lock(SpinLock::LINKER_INITIALIZED);

//----------------------------------------------------------------------
// Simple allocator for heap profiler's internal memory
//----------------------------------------------------------------------

static LowLevelAlloc::Arena *heap_profiler_memory;

static void* ProfilerMalloc(size_t bytes) {
  return LowLevelAlloc::AllocWithArena(bytes, heap_profiler_memory);
}
static void ProfilerFree(void* p) {
  LowLevelAlloc::Free(p);
}

//----------------------------------------------------------------------
// Profiling control/state data
//----------------------------------------------------------------------

static bool  is_on = false;           // If are on as a subsytem.
static bool  dumping = false;         // Dumping status to prevent recursion
static char* filename_prefix = NULL;  // Prefix used for profile file names
                                      // (NULL if no need for dumping yet)
static int   dump_count = 0;          // How many dumps so far
static int64 last_dump = 0;           // When did we last dump
static int64 high_water_mark = 0;     // In-use-bytes at last high-water dump

static HeapProfileTable* heap_profile = NULL;  // the heap profile table

//----------------------------------------------------------------------
// Profile generation
//----------------------------------------------------------------------

enum AddOrRemove { ADD, REMOVE };

// Add or remove all MMap-allocated regions to/from *heap_profile.
// Assumes heap_lock is held.
static void AddRemoveMMapDataLocked(AddOrRemove mode) {
  RAW_DCHECK(heap_lock.IsHeld(), "");
  if (!FLAGS_mmap_profile || !is_on) return;
  if (!FLAGS_mmap_log) MemoryRegionMap::CheckMallocHooks();
  // MemoryRegionMap maintained all the data we need for all
  // mmap-like allocations, so we just use it here:
  MemoryRegionMap::LockHolder l;
  for (MemoryRegionMap::RegionIterator r = MemoryRegionMap::BeginRegionLocked();
       r != MemoryRegionMap::EndRegionLocked(); ++r) {
    if (mode == ADD) {
      heap_profile->RecordAllocWithStack(
        reinterpret_cast<const void*>(r->start_addr),
        r->end_addr - r->start_addr,
        r->call_stack_depth, r->call_stack);
    } else {
      heap_profile->RecordFree(reinterpret_cast<void*>(r->start_addr));
    }
  }
}

static char* DoGetHeapProfile(void* (*alloc_func)(size_t)) {
  // We used to be smarter about estimating the required memory and
  // then capping it to 1MB and generating the profile into that.
  // However it should not cost us much to allocate 1MB every time.
  static const int size = 1 << 20;
  char* buf = reinterpret_cast<char*>((*alloc_func)(size));
  if (buf == NULL) return NULL;

  // Grab the lock and generate the profile.
  SpinLockHolder l(&heap_lock);
  HeapProfileTable::Stats const stats = heap_profile->total();
  AddRemoveMMapDataLocked(ADD);
  int buflen = is_on ? heap_profile->FillOrderedProfile(buf, size - 1) : 0;
  buf[buflen] = '\0';
  // FillOrderedProfile should not reduce the set of active mmap-ed regions,
  // hence MemoryRegionMap will let us remove everything we've added above:
  AddRemoveMMapDataLocked(REMOVE);
  RAW_DCHECK(stats.Equivalent(heap_profile->total()), "");
    // if this fails, we somehow removed by AddRemoveMMapDataLocked
    // more than we have added.
  RAW_DCHECK(buflen == strlen(buf), "");

  return buf;
}

extern "C" char* GetHeapProfile() {
  // Use normal malloc: we return the profile to the user to free it:
  return DoGetHeapProfile(&malloc);
}

// defined below
static void NewHook(const void* ptr, size_t size);
static void DeleteHook(const void* ptr);

// Helper for HeapProfilerDump.
static void DumpProfileLocked(const char* reason) {
  RAW_DCHECK(heap_lock.IsHeld(), "");
  RAW_DCHECK(is_on, "");
  RAW_DCHECK(!dumping, "");

  if (filename_prefix == NULL) return;  // we do not yet need dumping

  if (FLAGS_only_mmap_profile == false) {
    if (MallocHook::GetNewHook() != NewHook  ||
        MallocHook::GetDeleteHook() != DeleteHook) {
      RAW_LOG(FATAL, "Had our new/delete MallocHook-s replaced. "
                     "Are you using another MallocHook client? "
                     "Do not use --heap_profile=... to avoid this conflict.");
    }
  }

  dumping = true;

  // Make file name
  char file_name[1000];
  dump_count++;
  snprintf(file_name, sizeof(file_name), "%s.%04d%s",
           filename_prefix, dump_count, HeapProfileTable::kFileExt);

  // Release allocation lock around the meat of this routine
  // thus not blocking other threads too much.
  heap_lock.Unlock();
  {
    // Dump the profile
    RAW_VLOG(0, "Dumping heap profile to %s (%s)", file_name, reason);
    FILE* f = fopen(file_name, "w");
    if (f != NULL) {
      // Use ProfilerMalloc not to put info about profile itself into itself:
      char* profile = DoGetHeapProfile(&ProfilerMalloc);
      fputs(profile, f);
      ProfilerFree(profile);
      fclose(f);
    } else {
      RAW_LOG(ERROR, "Failed dumping heap profile to %s", file_name);
    }
  }
  heap_lock.Lock();

  dumping = false;
}

//----------------------------------------------------------------------
// Profile collection
//----------------------------------------------------------------------

// Record an allocation in the profile.
static void RecordAlloc(const void* ptr, size_t bytes, int skip_count) {
  SpinLockHolder l(&heap_lock);
  if (is_on) {
    heap_profile->RecordAlloc(ptr, bytes, skip_count + 1);
    const HeapProfileTable::Stats& total = heap_profile->total();
    const int64 inuse_bytes = total.alloc_size - total.free_size;
    if (!dumping) {
      bool need_to_dump = false;
      char buf[128];
      if (total.alloc_size >=
          last_dump + FLAGS_heap_profile_allocation_interval) {
        snprintf(buf, sizeof(buf), "%"PRId64" MB allocated",
                 total.alloc_size >> 20);
        // Track that we made a "total allocation size" dump
        last_dump = total.alloc_size;
        need_to_dump = true;
      } else if (inuse_bytes >
                 high_water_mark + FLAGS_heap_profile_inuse_interval) {
        snprintf(buf, sizeof(buf), "%"PRId64" MB in use", inuse_bytes >> 20);
        // Track that we made a "high water mark" dump
        high_water_mark = inuse_bytes;
        need_to_dump = true;
      }
      if (need_to_dump) {
        DumpProfileLocked(buf);
      }
    }
  }
}

// Record a deallocation in the profile.
static void RecordFree(const void* ptr) {
  SpinLockHolder l(&heap_lock);
  if (is_on) {
    heap_profile->RecordFree(ptr);
  }
}

//----------------------------------------------------------------------
// Allocation/deallocation hooks for MallocHook
//----------------------------------------------------------------------

// static
void NewHook(const void* ptr, size_t size) {
  if (ptr != NULL) RecordAlloc(ptr, size, 0);
}

// static
void DeleteHook(const void* ptr) {
  if (ptr != NULL) RecordFree(ptr);
}

// TODO(jandrews): Re-enable stack tracing
#ifdef TODO_REENABLE_STACK_TRACING
static void RawInfoStackDumper(const char* message, void*) {
  RAW_LOG(INFO, "%.*s", static_cast<int>(strlen(message) - 1), message);
  // -1 is to chop the \n which will be added by RAW_LOG
}
#endif

// Saved MemoryRegionMap's hooks to daisy-chain calls to.
MallocHook::MmapHook saved_mmap_hook = NULL;
MallocHook::MremapHook saved_mremap_hook = NULL;
MallocHook::MunmapHook saved_munmap_hook = NULL;
MallocHook::SbrkHook saved_sbrk_hook = NULL;

static void MmapHook(const void* result, const void* start, size_t size,
                     int prot, int flags, int fd, off_t offset) {
  if (FLAGS_mmap_log) {  // log it
    // We use PRIxS not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO,
            "mmap(start=0x%"PRIxS", len=%"PRIuS", prot=0x%x, flags=0x%x, "
            "fd=%d, offset=0x%x) = 0x%"PRIxS"",
            (uintptr_t) start, size, prot, flags, fd, (unsigned int) offset,
            (uintptr_t) result);
#ifdef TODO_REENABLE_STACK_TRACING
    DumpStackTrace(1, RawInfoStackDumper, NULL);
#endif
  }
  if (saved_mmap_hook) {
    // Call MemoryRegionMap's hook: it will record needed info about the mmap
    // for us w/o deadlocks:
    (*saved_mmap_hook)(result, start, size, prot, flags, fd, offset);
  }
}

static void MremapHook(const void* result, const void* old_addr,
                       size_t old_size, size_t new_size,
                       int flags, const void* new_addr) {
  if (FLAGS_mmap_log) {  // log it
    // We use PRIxS not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO,
            "mremap(old_addr=0x%"PRIxS", old_size=%"PRIuS", new_size=%"PRIuS", "
            "flags=0x%x, new_addr=0x%"PRIxS") = 0x%"PRIxS"",
            (uintptr_t) old_addr, old_size, new_size, flags,
            (uintptr_t) new_addr, (uintptr_t) result);
#ifdef TODO_REENABLE_STACK_TRACING
    DumpStackTrace(1, RawInfoStackDumper, NULL);
#endif
  }
  if (saved_mremap_hook) {  // call MemoryRegionMap's hook
    (*saved_mremap_hook)(result, old_addr, old_size, new_size, flags, new_addr);
  }
}

static void MunmapHook(const void* ptr, size_t size) {
  if (FLAGS_mmap_log) {  // log it
    // We use PRIxS not just '%p' to avoid deadlocks
    // in pretty-printing of NULL as "nil".
    // TODO(maxim): instead should use a safe snprintf reimplementation
    RAW_LOG(INFO, "munmap(start=0x%"PRIxS", len=%"PRIuS")",
                  (uintptr_t) ptr, size);
#ifdef TODO_REENABLE_STACK_TRACING
    DumpStackTrace(1, RawInfoStackDumper, NULL);
#endif
  }
  if (saved_munmap_hook) {  // call MemoryRegionMap's hook
    (*saved_munmap_hook)(ptr, size);
  }
}

static void SbrkHook(const void* result, ptrdiff_t increment) {
  if (FLAGS_mmap_log) {  // log it
    RAW_LOG(INFO, "sbrk(inc=%"PRIdS") = 0x%"PRIxS"",
                  increment, (uintptr_t) result);
#ifdef TODO_REENABLE_STACK_TRACING
    DumpStackTrace(1, RawInfoStackDumper, NULL);
#endif
  }
  if (saved_sbrk_hook) {  // call MemoryRegionMap's hook
    (*saved_sbrk_hook)(result, increment);
  }
}

//----------------------------------------------------------------------
// Starting/stopping/dumping
//----------------------------------------------------------------------

extern "C" void HeapProfilerStart(const char* prefix) {
  SpinLockHolder l(&heap_lock);

  if (is_on) return;

  is_on = true;

  RAW_VLOG(0, "Starting tracking the heap");

  if (FLAGS_only_mmap_profile) {
    FLAGS_mmap_profile = true;
  }

  if (FLAGS_mmap_profile) {
    // Ask MemoryRegionMap to record all mmap, mremap, and sbrk
    // call stack traces of at least size kMaxStackDepth:
    MemoryRegionMap::Init(HeapProfileTable::kMaxStackDepth);
  }

  if (FLAGS_mmap_log) {
    // Install our hooks to do the logging
    // and maybe save MemoryRegionMap's hooks to call:
    saved_mmap_hook = MallocHook::SetMmapHook(MmapHook);
    saved_mremap_hook = MallocHook::SetMremapHook(MremapHook);
    saved_munmap_hook = MallocHook::SetMunmapHook(MunmapHook);
    saved_sbrk_hook = MallocHook::SetSbrkHook(SbrkHook);
  }

  heap_profiler_memory =
    LowLevelAlloc::NewArena(0, LowLevelAlloc::DefaultArena());

  heap_profile = new(ProfilerMalloc(sizeof(HeapProfileTable)))
                   HeapProfileTable(ProfilerMalloc, ProfilerFree);

  last_dump = 0;

  // We do not reset dump_count so if the user does a sequence of
  // HeapProfilerStart/HeapProfileStop, we will get a continuous
  // sequence of profiles.

  if (FLAGS_only_mmap_profile == false) {
    // Now set the hooks that capture new/delete and malloc/free
    // and check that these are the only hooks:
    if (MallocHook::SetNewHook(NewHook) != NULL  ||
        MallocHook::SetDeleteHook(DeleteHook) != NULL) {
      RAW_LOG(FATAL, "Had other new/delete MallocHook-s set. "
                     "Are you using the heap leak checker? "
                     "Use --heap_check=\"\" to avoid this conflict.");
    }
  }

  // Copy filename prefix
  RAW_DCHECK(filename_prefix == NULL, "");
  const int prefix_length = strlen(prefix);
  filename_prefix = reinterpret_cast<char*>(ProfilerMalloc(prefix_length + 1));
  memcpy(filename_prefix, prefix, prefix_length);
  filename_prefix[prefix_length] = '\0';

  // This should be done before the hooks are set up, since it should
  // call new, and we want that to be accounted for correctly.
  MallocExtension::Initialize();
}

extern "C" void HeapProfilerStop() {
  SpinLockHolder l(&heap_lock);

  if (!is_on) return;

  if (FLAGS_only_mmap_profile == false) {
    // Unset our new/delete hooks, checking they were the ones set:
    if (MallocHook::SetNewHook(NULL) != NewHook  ||
        MallocHook::SetDeleteHook(NULL) != DeleteHook) {
      RAW_LOG(FATAL, "Had our new/delete MallocHook-s replaced. "
                     "Are you using another MallocHook client? "
                     "Do not use --heap_profile=... to avoid this conflict.");
    }
  }
  if (FLAGS_mmap_log) {
    // Restore mmap/sbrk hooks, checking that our hooks were the ones set:
    if (MallocHook::SetMmapHook(saved_mmap_hook) != MmapHook  ||
        MallocHook::SetMremapHook(saved_mremap_hook) != MremapHook  ||
        MallocHook::SetMunmapHook(saved_munmap_hook) != MunmapHook  ||
        MallocHook::SetSbrkHook(saved_sbrk_hook) != SbrkHook) {
      RAW_LOG(FATAL, "Had our mmap/mremap/munmap/sbrk MallocHook-s replaced. "
                     "Are you using another MallocHook client? "
                     "Do not use --heap_profile=... to avoid this conflict.");
    }
  }

  // free profile
  heap_profile->~HeapProfileTable();
  ProfilerFree(heap_profile);
  heap_profile = NULL;

  // free prefix
  ProfilerFree(filename_prefix);
  filename_prefix = NULL;

  if (!LowLevelAlloc::DeleteArena(heap_profiler_memory)) {
    RAW_LOG(FATAL, "Memory leak in HeapProfiler:");
  }

  if (FLAGS_mmap_profile) {
    MemoryRegionMap::Shutdown();
  }

  is_on = false;
}

extern "C" void HeapProfilerDump(const char *reason) {
  SpinLockHolder l(&heap_lock);
  if (is_on && !dumping) {
    DumpProfileLocked(reason);
  }
}

//----------------------------------------------------------------------
// Initialization/finalization code
//----------------------------------------------------------------------

// Initialization code
static void HeapProfilerInit() {
  // Everything after this point is for setting up the profiler based on envvar
  char fname[PATH_MAX];
  if (!GetUniquePathFromEnv("HEAPPROFILE", fname)) {
    return;
  }
  // We do a uid check so we don't write out files in a setuid executable.
#ifdef HAVE_GETEUID
  if (getuid() != geteuid()) {
    RAW_LOG(WARNING, ("HeapProfiler: ignoring HEAPPROFILE because "
                      "program seems to be setuid\n"));
    return;
  }
#endif

  HeapProfileTable::CleanupOldProfiles(fname);

  HeapProfilerStart(fname);
}

// class used for finalization -- dumps the heap-profile at program exit
struct HeapProfileEndWriter {
  ~HeapProfileEndWriter() { HeapProfilerDump("Exiting"); }
};

REGISTER_MODULE_INITIALIZER(heapprofiler, HeapProfilerInit());
static HeapProfileEndWriter heap_profile_end_writer;
