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

#include "config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif
#ifdef HAVE_PTHREAD
#include <pthread.h>   // for pthread_t, pthread_self()
#endif

#include <set>

#include "memory_region_map.h"

#include "base/linux_syscall_support.h"
#include "base/logging.h"
#include "base/low_level_alloc.h"

#include <google/stacktrace.h>
#include <google/malloc_hook.h>

// MREMAP_FIXED is a linux extension.  How it's used in this file,
// setting it to 0 is equivalent to saying, "This feature isn't
// supported", which is right.
#ifndef MREMAP_FIXED
# define MREMAP_FIXED  0
#endif

// ========================================================================= //

bool MemoryRegionMap::have_initialized_ = false;
MemoryRegionMap::RegionSet* MemoryRegionMap::regions_ = NULL;
LowLevelAlloc::Arena* MemoryRegionMap::arena_ = NULL;
SpinLock MemoryRegionMap::lock_(SpinLock::LINKER_INITIALIZED);
int MemoryRegionMap::recursion_count_ = 0;
pthread_t MemoryRegionMap::self_tid_;

// ========================================================================= //

// Simple hook into execution of global object constructors,
// so that we do not call pthread_self() when it does not yet work.
static bool libpthread_initialized = false;
static bool initializer = (libpthread_initialized = true, true);

static inline bool current_thread_is(pthread_t should_be) {
  // Before main() runs, there's only one thread, so we're always that thread
  if (!libpthread_initialized) return true;
  // this starts working only sometime well into global constructor execution:
  return pthread_equal(pthread_self(), should_be);
}

// ========================================================================= //

union MemoryRegionMap::RegionSetRep {
  char rep[sizeof(RegionSet)];
  void* align_it;
};

// The bytes where MemoryRegionMap::regions_ will point to
static MemoryRegionMap::RegionSetRep regions_rep;

// ========================================================================= //

// Has InsertRegionLocked been called recursively
// (or rather should we *not* use regions_ to record a hooked mmap).
static bool recursive_insert = false;

void MemoryRegionMap::Init() {
  RAW_VLOG(2, "MemoryRegionMap Init");
  Lock();
  if (have_initialized_) {
    Unlock();
    return;
  }
  MallocHook::SetMmapHook(MmapHook);
  MallocHook::SetMremapHook(MremapHook);
  MallocHook::SetSbrkHook(SbrkHook);
  recursive_insert = true;  // to buffer the mmap info caused by NewArena
  arena_ = LowLevelAlloc::NewArena(0, LowLevelAlloc::DefaultArena());
  recursive_insert = false;
  HandleSavedRegionsLocked(&InsertRegionLocked);  // flush the buffered ones
  MallocHook::SetMunmapHook(MunmapHook);
  have_initialized_ = true;
  Unlock();
  RAW_VLOG(2, "MemoryRegionMap Init done");
}

bool MemoryRegionMap::Shutdown() {
  RAW_VLOG(2, "MemoryRegionMap Shutdown");
  Lock();
  RAW_CHECK(have_initialized_, "");
  CheckMallocHooks();
  MallocHook::SetMmapHook(NULL);
  MallocHook::SetMremapHook(NULL);
  MallocHook::SetSbrkHook(NULL);
  MallocHook::SetMunmapHook(NULL);
  if (regions_) regions_->~RegionSet();
  regions_ = NULL;
  bool deleted_arena = LowLevelAlloc::DeleteArena(arena_);
  if (deleted_arena) {
    arena_ = 0;
  } else {
    RAW_LOG(WARNING, "Can't delete LowLevelAlloc arena: it's being used");
  }
  have_initialized_ = false;
  Unlock();
  RAW_VLOG(2, "MemoryRegionMap Shutdown done");
  return deleted_arena;
}

void MemoryRegionMap::CheckMallocHooks() {
  if (MallocHook::GetMmapHook() != MmapHook  ||
      MallocHook::GetMunmapHook() != MunmapHook  ||
      MallocHook::GetMremapHook() != MremapHook  ||
      MallocHook::GetSbrkHook() != SbrkHook) {
    RAW_LOG(FATAL, "Some malloc hooks got changed");
  }
}

void MemoryRegionMap::Lock() {
  if (recursion_count_ == 0  ||  !current_thread_is(self_tid_)) {
    lock_.Lock();
    if (libpthread_initialized)
      self_tid_ = pthread_self();
  }
  recursion_count_++;
  RAW_CHECK(recursion_count_ <= 5, "recursive lock nesting unexpectedly deep");
}

void MemoryRegionMap::Unlock() {
  RAW_CHECK(recursion_count_ >  0, "unlock when not held");
  RAW_CHECK(current_thread_is(self_tid_), "unlock by non-holder");
  recursion_count_--;
  if (recursion_count_ == 0) {
    lock_.Unlock();
  }
}

bool MemoryRegionMap::LockIsHeldByThisThread() {
  return lock_.IsHeld()  &&  current_thread_is(self_tid_);
}

bool MemoryRegionMap::FindStackRegion(uintptr_t stack_top, Region* result) {
  bool found = false;
  Lock();
  if (regions_ != NULL) {
    Region sample;
    sample.end_addr = stack_top;
    RegionSet::iterator region = regions_->lower_bound(sample);
    if (region != regions_->end()) {
      RAW_CHECK(stack_top <= region->end_addr, "");
      if (region->start_addr <= stack_top  &&  stack_top < region->end_addr) {
        RAW_VLOG(2, "Stack at %p is inside region %p..%p",
                    reinterpret_cast<void*>(stack_top),
                    reinterpret_cast<void*>(region->start_addr),
                    reinterpret_cast<void*>(region->end_addr));
        const_cast<Region*>(&*region)->is_stack = true;  // now we know
        *result = *region;
        found = true;
      }
    }
  }
  Unlock();
  return found;
}

MemoryRegionMap::RegionIterator MemoryRegionMap::BeginRegionLocked() {
  RAW_CHECK(LockIsHeldByThisThread(), "should be held (by this thread)");
  RAW_CHECK(regions_ != NULL, "");
  return regions_->begin();
}

MemoryRegionMap::RegionIterator MemoryRegionMap::EndRegionLocked() {
  RAW_CHECK(LockIsHeldByThisThread(), "should be held (by this thread)");
  RAW_CHECK(regions_ != NULL, "");
  return regions_->end();
}

inline void MemoryRegionMap::DoInsertRegionLocked(const Region& region) {
  if (DEBUG_MODE) {
    RegionSet::const_iterator i = regions_->lower_bound(region);
    RAW_CHECK(i == regions_->end()  ||  !region.Overlaps(*i),
              "Wow, overlapping memory regions");
    Region sample;
    sample.end_addr = region.start_addr;
    i = regions_->lower_bound(sample);
    RAW_CHECK(i == regions_->end()  ||  !region.Overlaps(*i),
              "Wow, overlapping memory regions");
  }
  RAW_VLOG(4, "Inserting region %p..%p from %p",
              reinterpret_cast<void*>(region.start_addr),
              reinterpret_cast<void*>(region.end_addr),
              reinterpret_cast<void*>(region.caller));
  regions_->insert(region);
  RAW_VLOG(4, "Inserted region %p..%p :",
              reinterpret_cast<void*>(region.start_addr),
              reinterpret_cast<void*>(region.end_addr));
  if (VLOG_IS_ON(4))  LogAllLocked();
}

// These variables are local to MemoryRegionMap::InsertRegionLocked()
// and MemoryRegionMap::HandleSavedRegionsLocked()
// and are file-level to ensure that they are initialized at load time.

// No. of unprocessed inserts
static int saved_regions_count = 0;
// Unprocessed inserts (must be big enough to hold all allocations that can
// be caused by a InsertRegionLocked call).
static MemoryRegionMap::Region saved_regions[10];

inline void MemoryRegionMap::HandleSavedRegionsLocked(
              void (*insert_func)(const Region& region)) {
  while (saved_regions_count > 0) {
    // Making a copy of the region argument is important:
    // in many cases the memory in saved_regions
    // will get written-to during the (*insert_func)(r) call below.
    Region r(saved_regions[--saved_regions_count]);
    (*insert_func)(r);
  }
}

inline void MemoryRegionMap::InsertRegionLocked(const Region& region) {
  RAW_CHECK(LockIsHeldByThisThread(), "should be held (by this thread)");
  // We can be called recursively, because RegionSet constructor
  // and DoInsertRegionLocked() (called below) can call the allocator.
  // recursive_insert tells us if that's the case. When this happens,
  // region insertion information is recorded in saved_regions[],
  // and taken into account when the recursion unwinds.
  // Do the insert:
  if (recursive_insert) {  // recursion
    RAW_VLOG(4, "Saving recursive insert of region %p..%p from %p",
                reinterpret_cast<void*>(region.start_addr),
                reinterpret_cast<void*>(region.end_addr),
                reinterpret_cast<void*>(region.caller));
    RAW_CHECK(saved_regions_count < arraysize(saved_regions), "");
    saved_regions[saved_regions_count++] = region;
  } else {  // not a recusrive call
    if (regions_ == NULL) {  // init regions_
      RAW_VLOG(4, "Initializing region set");
      regions_ = reinterpret_cast<RegionSet*>(&regions_rep.rep);
      recursive_insert = true;
      new(regions_) RegionSet();
      HandleSavedRegionsLocked(&DoInsertRegionLocked);
      recursive_insert = false;
    }
    recursive_insert = true;
    DoInsertRegionLocked(region);
    HandleSavedRegionsLocked(&DoInsertRegionLocked);
    recursive_insert = false;
  }
}

// We strip out different number of stack frames in debug mode
// because less inlining happens in that case
#ifdef NDEBUG
static const int kStripFrames = 1;
#else
static const int kStripFrames = 3;
#endif

void MemoryRegionMap::RecordRegionAddition(const void* start, size_t size) {
  Region region;
  // Record data about this memory acquisition call:
  region.start_addr = reinterpret_cast<uintptr_t>(start);
  region.end_addr = region.start_addr + size;
  region.is_stack = false;
  void* caller;
  const int depth =
    MallocHook::GetCallerStackTrace(&caller, 1, kStripFrames + 1);
  if (depth != 1) {
    // If we weren't able to get the stack frame, that's ok.  This
    // usually happens in recursive calls, when the stack-unwinder
    // calls mmap() which in turn calls the stack-unwinder.
    caller = NULL;
  }
  region.caller = reinterpret_cast<uintptr_t>(caller);
  RAW_VLOG(2, "New global region %p..%p from %p",
              reinterpret_cast<void*>(region.start_addr),
              reinterpret_cast<void*>(region.end_addr),
              reinterpret_cast<void*>(region.caller));
  Lock();  // recursively lock
  InsertRegionLocked(region);
  Unlock();
}

void MemoryRegionMap::RecordRegionRemoval(const void* start, size_t size) {
  Lock();
  HandleSavedRegionsLocked(&InsertRegionLocked);
    // first handle saved regions if any
  uintptr_t start_addr = reinterpret_cast<uintptr_t>(start);
  uintptr_t end_addr = start_addr + size;
  // subtract start_addr, end_addr from all the regions
  RAW_VLOG(2, "Removing global region %p..%p; have %"PRIuS" regions",
              reinterpret_cast<void*>(start_addr),
              reinterpret_cast<void*>(end_addr),
              regions_->size());
  Region start_point;
  memset(&start_point, 0, sizeof(start_point));  // zero out don't-care fields
  start_point.start_addr = start_point.end_addr = start_addr;
  for (RegionSet::iterator region = regions_->lower_bound(start_point);
       region != regions_->end() && region->start_addr < end_addr;
       /*noop*/) {
    RAW_VLOG(5, "Looking at region %p..%p",
                reinterpret_cast<void*>(region->start_addr),
                reinterpret_cast<void*>(region->end_addr));
    if (start_addr <= region->start_addr  &&
        region->end_addr <= end_addr) {  // full deletion
      RAW_VLOG(4, "Deleting region %p..%p",
                  reinterpret_cast<void*>(region->start_addr),
                  reinterpret_cast<void*>(region->end_addr));
      RegionSet::iterator d = region;
      ++region;
      regions_->erase(d);
      continue;
    } else if (region->start_addr < start_addr  &&
               end_addr < region->end_addr) {  // cutting-out split
      RAW_VLOG(4, "Splitting region %p..%p in two",
                  reinterpret_cast<void*>(region->start_addr),
                  reinterpret_cast<void*>(region->end_addr));
      // Make another region for the start portion:
      // The new region has to be the start portion because we can't
      // just modify region->end_addr as it's the sorting key.
      Region r;
      r.start_addr = region->start_addr;
      r.end_addr = start_addr;
      r.caller = region->caller;
      r.is_stack = region->is_stack;
      InsertRegionLocked(r);
      // cut region from start
      const_cast<Region*>(&*region)->start_addr = end_addr;
    } else if (end_addr > region->start_addr  &&
               start_addr <= region->start_addr) {  // cut from start
      RAW_VLOG(4, "Start-chopping region %p..%p",
                  reinterpret_cast<void*>(region->start_addr),
                  reinterpret_cast<void*>(region->end_addr));
      const_cast<Region*>(&*region)->start_addr = end_addr;
    } else if (start_addr > region->start_addr  &&
               start_addr < region->end_addr) {  // cut from end
      RAW_VLOG(4, "End-chopping region %p..%p",
                  reinterpret_cast<void*>(region->start_addr),
                  reinterpret_cast<void*>(region->end_addr));
      // Can't just modify region->end_addr (it's the sorting key):
      Region r = *region;
      r.end_addr = start_addr;
      RegionSet::iterator d = region;
      ++region;
      regions_->erase(d);
      InsertRegionLocked(r);
      continue;
    }
    ++region;
  }
  RAW_VLOG(4, "Removed region %p..%p; have %"PRIuS" regions",
              reinterpret_cast<void*>(start_addr),
              reinterpret_cast<void*>(end_addr),
              regions_->size());
  if (VLOG_IS_ON(4))  LogAllLocked();
  Unlock();
}

void MemoryRegionMap::MmapHook(const void* result,
                               const void* start, size_t size,
                               int prot, int flags,
                               int fd, off_t offset) {
  // TODO(maxim): replace all 0x%"PRIxS" by %p when RAW_VLOG uses a safe
  // snprintf reimplementation that does not malloc to pretty-print NULL
  RAW_VLOG(2, "MMap = 0x%"PRIxS" of %"PRIuS" at %llu "
              "prot %d flags %d fd %d offs %lld",
              reinterpret_cast<uintptr_t>(result), size,
              reinterpret_cast<uint64>(start), prot, flags, fd,
              static_cast<int64>(offset));
  if (result != reinterpret_cast<void*>(MAP_FAILED)  &&  size != 0) {
    RecordRegionAddition(result, size);
  }
}

void MemoryRegionMap::MunmapHook(const void* ptr, size_t size) {
  RAW_VLOG(2, "MUnmap of %p %"PRIuS"", ptr, size);
  if (size != 0) {
    RecordRegionRemoval(ptr, size);
  }
}

void MemoryRegionMap::MremapHook(const void* result,
                                 const void* old_addr, size_t old_size,
                                 size_t new_size, int flags,
                                 const void* new_addr) {
  RAW_VLOG(2, "MRemap = 0x%"PRIxS" of 0x%"PRIxS" %"PRIuS" "
              "to %"PRIuS" flags %d new_addr=0x%"PRIxS,
              (uintptr_t)result, (uintptr_t)old_addr,
               old_size, new_size, flags,
               flags & MREMAP_FIXED ? (uintptr_t)new_addr : 0);
  if (result != reinterpret_cast<void*>(-1)) {
    RecordRegionRemoval(old_addr, old_size);
    RecordRegionAddition(result, new_size);
  }
}

extern "C" void* __sbrk(ptrdiff_t increment);  // defined in libc

void MemoryRegionMap::SbrkHook(const void* result, ptrdiff_t increment) {
  RAW_VLOG(2, "Sbrk = 0x%"PRIxS" of %"PRIdS"", (uintptr_t)result, increment);
  if (result != reinterpret_cast<void*>(-1)) {
    if (increment > 0) {
      void* new_end = sbrk(0);
      RecordRegionAddition(result, reinterpret_cast<uintptr_t>(new_end) -
                                   reinterpret_cast<uintptr_t>(result));
    } else if (increment < 0) {
      void* new_end = sbrk(0);
      RecordRegionRemoval(new_end, reinterpret_cast<uintptr_t>(result) -
                                   reinterpret_cast<uintptr_t>(new_end));
    }
  }
}

void MemoryRegionMap::LogAllLocked() {
  RAW_CHECK(LockIsHeldByThisThread(), "should be held (by this thread)");
  RAW_LOG(INFO, "List of regions:");
  uintptr_t previous = 0;
  for (RegionSet::const_iterator r = regions_->begin();
       r != regions_->end(); ++r) {
    RAW_LOG(INFO, "Memory region 0x%"PRIxS"..0x%"PRIxS" "
                  "from 0x%"PRIxS" stack=%d",
                  r->start_addr, r->end_addr, r->caller, r->is_stack);
    RAW_CHECK(previous < r->end_addr, "wow, we messed up the set order");
      // this must be caused by uncontrolled recursive operations on regions_
    previous = r->end_addr;
  }
  RAW_LOG(INFO, "End of regions list");
}
