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
// Some of our malloc implementations can invoke the following hooks
// whenever memory is allocated or deallocated.  If the hooks are
// NULL, they are not invoked.  MallocHook is thread-safe, and things
// you do before calling SetFooHook(MyHook) are visible to any
// resulting calls to MyHook.  Hooks must be thread-safe, and if you
// write:
//
//   MallocHook::NewHook old_new_hook_ = NULL;
//   ...
//   old_new_hook_ = MallocHook::SetNewHook(&MyNewHook);
//
// old_new_hook_ could still be NULL the first couple times MyNewHook
// is called.
//
// One important user of these hooks is the heap profiler.
//
// CAVEAT: If you add new MallocHook::Invoke* calls (not for chaining hooks),
// then those calls must be directly in the code of the (de)allocation
// function that is provided to the user and that function must have
// an ATTRIBUTE_SECTION(malloc_hook) attribute.
//
// Note: Get*Hook() and Invoke*Hook() functions are defined in
// malloc_hook-inl.h.  If you need to get or invoke a hook (which you
// shouldn't unless you're part of tcmalloc), be sure to #include
// malloc_hook-inl.h in addition to malloc_hook.h.
//
// TODO(csilvers): support a non-inlined function called
// Assert*HookIs()?  This is the context in which I normally see
// Get*Hook() called in non-tcmalloc code.

#ifndef _MALLOC_HOOK_H_
#define _MALLOC_HOOK_H_

#include <stddef.h>
#include <sys/types.h>

// Annoying stuff for windows -- makes sure clients can import these functions
#ifndef PERFTOOLS_DLL_DECL
# ifdef WIN32
#   define PERFTOOLS_DLL_DECL  __declspec(dllimport)
# else
#   define PERFTOOLS_DLL_DECL
# endif
#endif

class PERFTOOLS_DLL_DECL MallocHook {
 public:
  // The NewHook is invoked whenever an object is allocated.
  // It may be passed NULL if the allocator returned NULL.
  typedef void (*NewHook)(const void* ptr, size_t size);
  inline static NewHook GetNewHook();
  static NewHook SetNewHook(NewHook hook);
  inline static void InvokeNewHook(const void* p, size_t s);

  // The DeleteHook is invoked whenever an object is deallocated.
  // It may be passed NULL if the caller is trying to delete NULL.
  typedef void (*DeleteHook)(const void* ptr);
  inline static DeleteHook GetDeleteHook();
  static DeleteHook SetDeleteHook(DeleteHook hook);
  inline static void InvokeDeleteHook(const void* p);

  // The MmapHook is invoked whenever a region of memory is mapped.
  // It may be passed MAP_FAILED if the mmap failed.
  typedef void (*MmapHook)(const void* result,
                           const void* start,
                           size_t size,
                           int protection,
                           int flags,
                           int fd,
                           off_t offset);
  inline static MmapHook GetMmapHook();
  static MmapHook SetMmapHook(MmapHook hook);
  inline static void InvokeMmapHook(const void* result,
                                    const void* start,
                                    size_t size,
                                    int protection,
                                    int flags,
                                    int fd,
                                    off_t offset);

  // The MunmapHook is invoked whenever a region of memory is unmapped.
  typedef void (*MunmapHook)(const void* ptr, size_t size);
  inline static MunmapHook GetMunmapHook();
  static MunmapHook SetMunmapHook(MunmapHook hook);
  inline static void InvokeMunmapHook(const void* p, size_t size);

  // The MremapHook is invoked whenever a region of memory is remapped.
  typedef void (*MremapHook)(const void* result,
                             const void* old_addr,
                             size_t old_size,
                             size_t new_size,
                             int flags,
                             const void* new_addr);
  inline static MremapHook GetMremapHook();
  static MremapHook SetMremapHook(MremapHook hook);
  inline static void InvokeMremapHook(const void* result,
                                      const void* old_addr,
                                      size_t old_size,
                                      size_t new_size,
                                      int flags,
                                      const void* new_addr);

  // The SbrkHook is invoked whenever sbrk is called -- except when
  // the increment is 0.  This is because sbrk(0) is often called
  // to get the top of the memory stack, and is not actually a
  // memory-allocation call.
  typedef void (*SbrkHook)(const void* result, ptrdiff_t increment);
  inline static SbrkHook GetSbrkHook();
  static SbrkHook SetSbrkHook(SbrkHook hook);
  inline static void InvokeSbrkHook(const void* result, ptrdiff_t increment);

  // Get the current stack trace.  Try to skip all routines up to and
  // and including the caller of MallocHook::Invoke*.
  // Use "skip_count" (similarly to GetStackTrace from stacktrace.h)
  // as a hint about how many routines to skip if better information
  // is not available.
  static int GetCallerStackTrace(void** result, int max_depth, int skip_count);
};

#endif /* _MALLOC_HOOK_H_ */
