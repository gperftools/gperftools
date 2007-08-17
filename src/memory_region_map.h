/* Copyright (c) 2006, Google Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Maxim Lifantsev
 */

#ifndef BASE_MEMORY_REGION_MAP_H__
#define BASE_MEMORY_REGION_MAP_H__

#include "config.h"

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#include <set>
#include "base/stl_allocator.h"
#include "base/spinlock.h"
#include "base/low_level_alloc.h"

/// TODO(maxim): add a unittest

// Class to collect and query the map of all memory regions
// in a process that have been created with mmap, munmap, mremap, sbrk.
// After initialization with Init()
// (which can happened even before global object constructor execution)
// we collect the map by installing and monitoring MallocHook-s
// to mmap, munmap, mremap, sbrk.
// At any time one can query this map via provided interface.
class MemoryRegionMap {
 public:  // interface

  // Start up this module (can be called more than once w/o harm).
  // Will install mmap, munmap, mremap, sbrk hooks
  // and initialize arena_ and our hook and locks, hence one can use
  // MemoryRegionMap::Lock()/Unlock() to manage the locks.
  // Uses Lock/Unlock inside.
  static void Init();

  // Try to shutdown this module undoing what Init() did.
  // Returns iff could do full shutdown.
  static bool Shutdown();

  // Check that our hooks are still in place and crash if not.
  // No need for locking.
  static void CheckMallocHooks();

  // Locks to protect our internal data structures.
  // These also protect use of arena_
  // if our Init() has been done.
  // The lock is recursive.
  static void Lock();
  static void Unlock();
  // Whether the lock is held by this thread.
  static bool LockIsHeldByThisThread();

  // A memory region that we know about through malloc_hook-s.
  struct Region {
    uintptr_t start_addr;  // region start address
    uintptr_t end_addr;  // region end address
    uintptr_t caller;  // who called this region's allocation function
                       // (return address in the stack of the immediate caller)
                       // NULL if could not get it
    bool is_stack;  // does this region contain a thread's stack

    bool Overlaps(const Region& x) const {
      return start_addr < x.end_addr  &&  end_addr > x.start_addr;
    }
  };

  // Find the region that contains stack_top, mark that region as
  // a stack region, and write its data into *result.
  // Returns success. Uses Lock/Unlock inside.
  static bool FindStackRegion(uintptr_t stack_top, Region* result);

 private:  // our internal types

  // Region comparator for sorting with STL
  struct RegionCmp {
    bool operator()(const Region& x, const Region& y) const {
      return x.end_addr < y.end_addr;
    }
  };

  // We allocate STL objects in our own arena.
  struct MyAllocator {
    static void *Allocate(size_t n) {
      return LowLevelAlloc::AllocWithArena(n, arena_);
    }
    static void Free(void *p) { LowLevelAlloc::Free(p); }
  };

  // Set of the memory regions
  typedef std::set<Region, RegionCmp,
              STL_Allocator<Region, MyAllocator> > RegionSet;

 public:  // more in-depth interface

  // STL iterator with values of Region
  typedef RegionSet::const_iterator RegionIterator;

  // Return the begin/end iterators to all the regions.
  // Ideally these need Lock/Unlock protection around their whole usage (loop),
  // but LockOther/UnlockOther is usually sufficient:
  // in this case of single-threaded mutability (achieved by (Un)lockOther)
  // via region additions/modifications
  // the loop iterator will still be valid as log as its region
  // has not been deleted and EndRegionLocked should be
  // re-evaluated whenever the set of regions has changed.
  static RegionIterator BeginRegionLocked();
  static RegionIterator EndRegionLocked();

 public:  // effectively private type

  union RegionSetRep;  // in .cc

 private:  // representation

  // If have initialized this module
  static bool have_initialized_;

  // Arena used for our allocations in regions_.
  static LowLevelAlloc::Arena* arena_;

  // Set of the mmap/sbrk/mremap-ed memory regions
  // To be accessed *only* when Lock() is held.
  // Hence we protect the non-recursive lock used inside of arena_
  // with our recursive Lock(). This lets a user prevent deadlocks
  // when threads are stopped by ListAllProcessThreads at random spots
  // simply by acquiring our recursive Lock() before that.
  static RegionSet* regions_;

  // Lock to protect regions_ variable and the data behind.
  static SpinLock lock_;

  // Recursion count for the recursive lock.
  static int recursion_count_;
  // The thread id of the thread that's inside the recursive lock.
  static pthread_t self_tid_;

 private:  // helpers

  // Verifying wrapper around regions_->insert(region)
  // To be called for InsertRegionLocked only!
  inline static void DoInsertRegionLocked(const Region& region);
  // Handle regions saved by InsertRegionLocked into a tmp static array
  // by calling insert_func on them.
  inline static void HandleSavedRegionsLocked(
                       void (*insert_func)(const Region& region));
  // Wrapper around DoInsertRegionLocked
  // that handles the case of recursive allocator calls.
  inline static void InsertRegionLocked(const Region& region);

  // Record addition of a memory region at address "start" of size "size"
  // (called from our mmap/mremap/sbrk hooks).
  static void RecordRegionAddition(const void* start, size_t size);
  // Record deletion of a memory region at address "start" of size "size"
  // (called from our munmap/mremap/sbrk hooks).
  static void RecordRegionRemoval(const void* start, size_t size);

  // Hooks for MallocHook
  static void MmapHook(const void* result,
                       const void* start, size_t size,
                       int prot, int flags,
                       int fd, off_t offset);
  static void MunmapHook(const void* ptr, size_t size);
  static void MremapHook(const void* result, const void* old_addr,
                         size_t old_size, size_t new_size, int flags,
                         const void* new_addr);
  static void SbrkHook(const void* result, ptrdiff_t increment);

  // Log all memory regions; Useful for debugging only.
  // Assumes Lock() is held
  static void LogAllLocked();

  DISALLOW_EVIL_CONSTRUCTORS(MemoryRegionMap);
};

#endif  // BASE_MEMORY_REGION_MAP_H__
