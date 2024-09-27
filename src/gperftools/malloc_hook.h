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
// Author: Sanjay Ghemawat
//
// Some of our malloc implementations can invoke the following hooks whenever
// memory is allocated or deallocated.  MallocHook is thread-safe, and things
// you do before calling AddFooHook(MyHook) are visible to any resulting calls
// to MyHook.  Hooks must be thread-safe.  If you write:
//
//   CHECK(MallocHook::AddNewHook(&MyNewHook));
//
// MyNewHook will be invoked in subsequent calls in the current thread, but
// there are no guarantees on when it might be invoked in other threads.
//
// There are a limited number of slots available for each hook type.  Add*Hook
// will return false if there are no slots available.  Remove*Hook will return
// false if the given hook was not already installed.
//
// The order in which individual hooks are called in Invoke*Hook is undefined.
//
// It is safe for a hook to remove itself within Invoke*Hook and add other
// hooks.  Any hooks added inside a hook invocation (for the same hook type)
// will not be invoked for the current invocation.
//
// One important user of these hooks is the heap profiler.
//
// NOTE FOR C USERS: If you want to use malloc_hook functionality from
// a C program, #include malloc_hook_c.h instead of this file.

#ifndef _MALLOC_HOOK_H_
#define _MALLOC_HOOK_H_

#include <stddef.h>
#include <sys/types.h>
extern "C" {
#include "malloc_hook_c.h"  // a C version of the malloc_hook interface
}

// Annoying stuff for windows -- makes sure clients can import these functions
#ifndef PERFTOOLS_DLL_DECL
# ifdef _WIN32
#   define PERFTOOLS_DLL_DECL  __declspec(dllimport)
# else
#   define PERFTOOLS_DLL_DECL
# endif
#endif

// The C++ methods below call the C version (MallocHook_*), and thus
// convert between an int and a bool.  Windows complains about this
// (a "performance warning") which we don't care about, so we suppress.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4800)
#endif

// Note: malloc_hook_c.h defines MallocHook_*Hook and
// MallocHook_{Add,Remove}*Hook.  The version of these inside the MallocHook
// class are defined in terms of the malloc_hook_c version.  See malloc_hook_c.h
// for details of these types/functions.

class PERFTOOLS_DLL_DECL MallocHook {
 public:
  // The NewHook is invoked whenever an object is allocated.
  // It may be passed nullptr if the allocator returned nullptr.
  typedef MallocHook_NewHook NewHook;
  inline static bool AddNewHook(NewHook hook) {
    return MallocHook_AddNewHook(hook);
  }
  inline static bool RemoveNewHook(NewHook hook) {
    return MallocHook_RemoveNewHook(hook);
  }
  // The DeleteHook is invoked whenever an object is deallocated.
  // It may be passed nullptr if the caller is trying to delete nullptr.
  typedef MallocHook_DeleteHook DeleteHook;
  inline static bool AddDeleteHook(DeleteHook hook) {
    return MallocHook_AddDeleteHook(hook);
  }
  inline static bool RemoveDeleteHook(DeleteHook hook) {
    return MallocHook_RemoveDeleteHook(hook);
  }
  inline static void InvokeDeleteHook(const void* p);

  // Get the current stack trace.  Try to skip all routines up to and
  // and including the caller of tcmalloc::Invoke*.
  // Use "skip_count" (similarly to GetStackTrace from stacktrace.h)
  // as a hint about how many routines to skip if better information
  // is not available.
  inline static int GetCallerStackTrace(void** result, int max_depth,
                                        int skip_count) {
    return MallocHook_GetCallerStackTrace(result, max_depth, skip_count);
  }

  // Unhooked versions of mmap() and munmap().   These should be used
  // only by experts, since they bypass heapchecking, etc.
  // Note: These do not run hooks, but they still use the MmapReplacement
  // and MunmapReplacement.
  static void* UnhookedMMap(void *start, size_t length, int prot, int flags,
                            int fd, off_t offset);
  static int UnhookedMUnmap(void *start, size_t length);

  // The following are DEPRECATED. Also all mmap and sbrk
  // hooks/prehooks/replacement hooks are no-ops.

  typedef MallocHook_PreMmapHook PreMmapHook;
  inline static bool AddPreMmapHook(PreMmapHook hook) {
    return MallocHook_AddPreMmapHook(hook);
  }
  inline static bool RemovePreMmapHook(PreMmapHook hook) {
    return MallocHook_RemovePreMmapHook(hook);
  }

  typedef MallocHook_MmapReplacement MmapReplacement;
  inline static bool SetMmapReplacement(MmapReplacement hook) {
    return MallocHook_SetMmapReplacement(hook);
  }
  inline static bool RemoveMmapReplacement(MmapReplacement hook) {
    return MallocHook_RemoveMmapReplacement(hook);
  }


  typedef MallocHook_MmapHook MmapHook;
  inline static bool AddMmapHook(MmapHook hook) {
    return MallocHook_AddMmapHook(hook);
  }
  inline static bool RemoveMmapHook(MmapHook hook) {
    return MallocHook_RemoveMmapHook(hook);
  }

  typedef MallocHook_MunmapReplacement MunmapReplacement;
  inline static bool SetMunmapReplacement(MunmapReplacement hook) {
    return MallocHook_SetMunmapReplacement(hook);
  }
  inline static bool RemoveMunmapReplacement(MunmapReplacement hook) {
    return MallocHook_RemoveMunmapReplacement(hook);
  }

  typedef MallocHook_MunmapHook MunmapHook;
  inline static bool AddMunmapHook(MunmapHook hook) {
    return MallocHook_AddMunmapHook(hook);
  }
  inline static bool RemoveMunmapHook(MunmapHook hook) {
    return MallocHook_RemoveMunmapHook(hook);
  }

  typedef MallocHook_MremapHook MremapHook;
  inline static bool AddMremapHook(MremapHook hook) {
    return MallocHook_AddMremapHook(hook);
  }
  inline static bool RemoveMremapHook(MremapHook hook) {
    return MallocHook_RemoveMremapHook(hook);
  }

  typedef MallocHook_PreSbrkHook PreSbrkHook;
  inline static bool AddPreSbrkHook(PreSbrkHook hook) {
    return MallocHook_AddPreSbrkHook(hook);
  }
  inline static bool RemovePreSbrkHook(PreSbrkHook hook) {
    return MallocHook_RemovePreSbrkHook(hook);
  }

  typedef MallocHook_SbrkHook SbrkHook;
  inline static bool AddSbrkHook(SbrkHook hook) {
    return MallocHook_AddSbrkHook(hook);
  }
  inline static bool RemoveSbrkHook(SbrkHook hook) {
    return MallocHook_RemoveSbrkHook(hook);
  }

  inline static NewHook SetNewHook(NewHook hook) {
    return MallocHook_SetNewHook(hook);
  }

  inline static DeleteHook SetDeleteHook(DeleteHook hook) {
    return MallocHook_SetDeleteHook(hook);
  }

  inline static PreMmapHook SetPreMmapHook(PreMmapHook hook) {
    return MallocHook_SetPreMmapHook(hook);
  }

  inline static MmapHook SetMmapHook(MmapHook hook) {
    return MallocHook_SetMmapHook(hook);
  }

  inline static MunmapHook SetMunmapHook(MunmapHook hook) {
    return MallocHook_SetMunmapHook(hook);
  }

  inline static MremapHook SetMremapHook(MremapHook hook) {
    return MallocHook_SetMremapHook(hook);
  }

  inline static PreSbrkHook SetPreSbrkHook(PreSbrkHook hook) {
    return MallocHook_SetPreSbrkHook(hook);
  }

  inline static SbrkHook SetSbrkHook(SbrkHook hook) {
    return MallocHook_SetSbrkHook(hook);
  }
  // End of DEPRECATED methods.
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif


#endif /* _MALLOC_HOOK_H_ */
