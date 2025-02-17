// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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
// Author: Sanjay Ghemawat <opensource@google.com>

#include <config.h>

#include <gperftools/malloc_hook.h>
#include "malloc_hook-inl.h"

#include <stddef.h>
#include <stdint.h>
#if HAVE_SYS_SYSCALL_H
#include <sys/syscall.h>
#endif

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif

#include <algorithm>
#include "base/logging.h"
#include "base/spinlock.h"
#include "malloc_backtrace.h"
#include "maybe_emergency_malloc.h"

// __THROW is defined in glibc systems.  It means, counter-intuitively,
// "This function will never throw an exception."  It's an optional
// optimization tool, but we may need to use it to match glibc prototypes.
#ifndef __THROW    // I guess we're not on a glibc system
# define __THROW   // __THROW is just an optimization, so ok to make it ""
#endif

namespace base { namespace internal {

// This lock is shared between all implementations of HookList::Add & Remove.
// The potential for contention is very small.  This needs to be a SpinLock and
// not a Mutex since it's possible for Mutex locking to allocate memory (e.g.,
// per-thread allocation in debug builds), which could cause infinite recursion.
static SpinLock hooklist_spinlock;

template <typename T>
bool HookList<T>::Add(T value) {
  if (value == T{}) {
    return false;
  }
  SpinLockHolder l(&hooklist_spinlock);
  // Find the first slot in data that is 0.
  int index = 0;
  while ((index < kHookListMaxValues) &&
         cast_priv_data(index)->load(std::memory_order_relaxed) != T{}) {
    ++index;
  }
  if (index == kHookListMaxValues) {
    return false;
  }
  uintptr_t prev_num_hooks = priv_end.load(std::memory_order_acquire);
  cast_priv_data(index)->store(value, std::memory_order_relaxed);
  if (prev_num_hooks <= index) {
    priv_end.store(index + 1, std::memory_order_relaxed);
  }
  return true;
}

template <typename T>
void HookList<T>::FixupPrivEndLocked() {
  uintptr_t hooks_end = priv_end.load(std::memory_order_relaxed);
  while ((hooks_end > 0) &&
         cast_priv_data(hooks_end-1)->load(std::memory_order_relaxed) == 0) {
    --hooks_end;
  }
  priv_end.store(hooks_end, std::memory_order_relaxed);
}

template <typename T>
bool HookList<T>::Remove(T value) {
  if (value == T{}) {
    return false;
  }
  SpinLockHolder l(&hooklist_spinlock);
  uintptr_t hooks_end = priv_end.load(std::memory_order_relaxed);
  int index = 0;
  while (index < hooks_end
         && value != cast_priv_data(index)->load(std::memory_order_relaxed)) {
    ++index;
  }
  if (index == hooks_end) {
    return false;
  }
  cast_priv_data(index)->store(T{}, std::memory_order_relaxed);
  FixupPrivEndLocked();
  return true;
}

template <typename T>
int HookList<T>::Traverse(T* output_array, int n) const {
  uintptr_t hooks_end = priv_end.load(std::memory_order_acquire);
  int actual_hooks_end = 0;
  for (int i = 0; i < hooks_end && n > 0; ++i) {
    T data = cast_priv_data(i)->load(std::memory_order_acquire);
    if (data != T{}) {
      *output_array++ = data;
      ++actual_hooks_end;
      --n;
    }
  }
  return actual_hooks_end;
}

template <typename T>
T HookList<T>::ExchangeSingular(T value) {
  T old_value;
  SpinLockHolder l(&hooklist_spinlock);
  old_value = cast_priv_data(kHookListSingularIdx)->load(std::memory_order_relaxed);
  cast_priv_data(kHookListSingularIdx)->store(value, std::memory_order_relaxed);
  if (value != T{}) {
    priv_end.store(kHookListSingularIdx + 1, std::memory_order_relaxed);
  } else {
    FixupPrivEndLocked();
  }
  return old_value;
}

// Explicit instantiation for malloc_hook_test.cc.  This ensures all the methods
// are instantiated.
template struct HookList<MallocHook::NewHook>;

HookList<MallocHook::NewHook> new_hooks_;
HookList<MallocHook::DeleteHook> delete_hooks_;

} }  // namespace base::internal

using base::internal::kHookListMaxValues;
using base::internal::new_hooks_;
using base::internal::delete_hooks_;

// These are available as C bindings as well as C++, hence their
// definition outside the MallocHook class.
extern "C"
int MallocHook_AddNewHook(MallocHook_NewHook hook) {
  RAW_VLOG(10, "AddNewHook(%p)", hook);
  return new_hooks_.Add(hook);
}

extern "C"
int MallocHook_RemoveNewHook(MallocHook_NewHook hook) {
  RAW_VLOG(10, "RemoveNewHook(%p)", hook);
  return new_hooks_.Remove(hook);
}

extern "C"
int MallocHook_AddDeleteHook(MallocHook_DeleteHook hook) {
  RAW_VLOG(10, "AddDeleteHook(%p)", hook);
  return delete_hooks_.Add(hook);
}

extern "C"
int MallocHook_RemoveDeleteHook(MallocHook_DeleteHook hook) {
  RAW_VLOG(10, "RemoveDeleteHook(%p)", hook);
  return delete_hooks_.Remove(hook);
}

// Next are "legacy" singular new/delete hooks

// The code below is DEPRECATED.
extern "C"
MallocHook_NewHook MallocHook_SetNewHook(MallocHook_NewHook hook) {
  RAW_VLOG(10, "SetNewHook(%p)", hook);
  return new_hooks_.ExchangeSingular(hook);
}

extern "C"
MallocHook_DeleteHook MallocHook_SetDeleteHook(MallocHook_DeleteHook hook) {
  RAW_VLOG(10, "SetDeleteHook(%p)", hook);
  return delete_hooks_.ExchangeSingular(hook);
}

namespace tcmalloc {

void InvokeNewHookSlow(const void* p, size_t s) {
  if (IsEmergencyPtr(p)) {
    return;
  }
  MallocHook::NewHook hooks[kHookListMaxValues];
  int num_hooks = base::internal::new_hooks_.Traverse(hooks, kHookListMaxValues);
  for (int i = 0; i < num_hooks; i++) {
    hooks[i](p, s);
  }
}

void InvokeDeleteHookSlow(const void* p) {
  if (IsEmergencyPtr(p)) {
    return;
  }
  MallocHook::DeleteHook hooks[kHookListMaxValues];
  int num_hooks = base::internal::delete_hooks_.Traverse(hooks, kHookListMaxValues);
  for (int i = 0; i < num_hooks; i++) {
    hooks[i](p);
  }
}

}  // namespace tcmalloc

// We can improve behavior/compactness of this function
// if we pass a generic test function (with a generic arg)
// into the implementations for GetStackTrace instead of the skip_count.
extern "C" int MallocHook_GetCallerStackTrace(void** result, int max_depth,
                                              int skip_count) {
#if defined(NO_TCMALLOC_SAMPLES)
  return 0;
#else
  if (max_depth < 1) {
    return 0;
  }
  // Fall back to GetStackTrace and good old but fragile frame skip counts.
  // Note: this path is inaccurate when a hook is not called directly by an
  // allocation function but is daisy-chained through another hook,
  // search for MallocHook::(Get|Set|Invoke)* to find such cases.
  int retval = tcmalloc::GrabBacktrace(result, max_depth, skip_count);
  // prevent tail-call above
  *(void* volatile *)result;
  return retval;
#endif
}

// All mmap hooks functions are empty and bogus. All of those below
// are no op and we keep them only because we have them exposed in
// headers we ship. So keep them for somewhat formal ABI compat.
//
extern "C"
int MallocHook_AddPreMmapHook(MallocHook_PreMmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemovePreMmapHook(MallocHook_PreMmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_SetMmapReplacement(MallocHook_MmapReplacement hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveMmapReplacement(MallocHook_MmapReplacement hook) {
  return 0;
}

extern "C"
int MallocHook_AddMmapHook(MallocHook_MmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveMmapHook(MallocHook_MmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_AddMunmapHook(MallocHook_MunmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveMunmapHook(MallocHook_MunmapHook hook) {
  return 0;
}

extern "C"
int MallocHook_SetMunmapReplacement(MallocHook_MunmapReplacement hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveMunmapReplacement(MallocHook_MunmapReplacement hook) {
  return 0;
}

extern "C"
int MallocHook_AddMremapHook(MallocHook_MremapHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveMremapHook(MallocHook_MremapHook hook) {
  return 0;
}

extern "C"
int MallocHook_AddPreSbrkHook(MallocHook_PreSbrkHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemovePreSbrkHook(MallocHook_PreSbrkHook hook) {
  return 0;
}

extern "C"
int MallocHook_AddSbrkHook(MallocHook_SbrkHook hook) {
  return 0;
}

extern "C"
int MallocHook_RemoveSbrkHook(MallocHook_SbrkHook hook) {
  return 0;
}

/*static*/void* MallocHook::UnhookedMMap(void *start, size_t length, int prot,
                                         int flags, int fd, off_t offset) {
  errno = ENOSYS;
  return MAP_FAILED;
}

/*static*/int MallocHook::UnhookedMUnmap(void *start, size_t length) {
  errno = ENOSYS;
  return -1;
}

extern "C"
MallocHook_PreMmapHook MallocHook_SetPreMmapHook(MallocHook_PreMmapHook hook) {
  return 0;
}

extern "C"
MallocHook_MmapHook MallocHook_SetMmapHook(MallocHook_MmapHook hook) {
  return 0;
}

extern "C"
MallocHook_MunmapHook MallocHook_SetMunmapHook(MallocHook_MunmapHook hook) {
  return 0;
}

extern "C"
MallocHook_MremapHook MallocHook_SetMremapHook(MallocHook_MremapHook hook) {
  return 0;
}

extern "C"
MallocHook_PreSbrkHook MallocHook_SetPreSbrkHook(MallocHook_PreSbrkHook hook) {
  return 0;
}

extern "C"
MallocHook_SbrkHook MallocHook_SetSbrkHook(MallocHook_SbrkHook hook) {
  return 0;
}

