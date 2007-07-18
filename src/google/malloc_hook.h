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
// NULL, they are not invoked.
//
// One important user of these hooks is the heap profiler.
//
// CAVEAT: If you add new MallocHook::Invoke* calls (not for chaining hooks),
// then those calls must be directly in the code of the (de)allocation
// function that is provided to the user and that function must have
// an ATTRIBUTE_SECTION(malloc_hook) attribute.

#ifndef _MALLOC_HOOK_H
#define _MALLOC_HOOK_H

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
  typedef void (*NewHook)(void* ptr, size_t size);
  inline static NewHook GetNewHook() { return new_hook_; }
  inline static NewHook SetNewHook(NewHook hook) {
    NewHook result = new_hook_;
    new_hook_ = hook;
    return result;
  }
  inline static void InvokeNewHook(void* p, size_t s) {
    if (new_hook_ != NULL) (*new_hook_)(p, s);
  }

  // The DeleteHook is invoked whenever an object is deallocated.
  // It may be passed NULL if the caller is trying to delete NULL.
  typedef void (*DeleteHook)(void* ptr);
  inline static DeleteHook GetDeleteHook() { return delete_hook_; }
  inline static DeleteHook SetDeleteHook(DeleteHook hook) {
    DeleteHook result = delete_hook_;
    delete_hook_ = hook;
    return result;
  }
  inline static void InvokeDeleteHook(void* p) {
    if (delete_hook_ != NULL) (*delete_hook_)(p);
  }

  // The MmapHook is invoked whenever a region of memory is mapped.
  // It may be passed MAP_FAILED if the mmap failed.
  typedef void (*MmapHook)(void* result,
                           void* start,
                           size_t size,
                           int protection,
                           int flags,
                           int fd,
                           off_t offset);
  inline static MmapHook GetMmapHook() { return mmap_hook_; }
  inline static MmapHook SetMmapHook(MmapHook hook) {
    MmapHook result = mmap_hook_;
    mmap_hook_ = hook;
    return result;
  }
  inline static void InvokeMmapHook(void* result,
                                    void* start,
                                    size_t size,
                                    int protection,
                                    int flags,
                                    int fd,
                                    off_t offset) {
    if (mmap_hook_ != NULL) (*mmap_hook_)(result,
                                          start, size,
                                          protection, flags,
                                          fd, offset);
  }

  // The MunmapHook is invoked whenever a region of memory is unmapped.
  typedef void (*MunmapHook)(void* ptr, size_t size);
  inline static MunmapHook GetMunmapHook() { return munmap_hook_; }
  inline static MunmapHook SetMunmapHook(MunmapHook hook) {
    MunmapHook result = munmap_hook_;
    munmap_hook_ = hook;
    return result;
  }
  inline static void InvokeMunmapHook(void* p, size_t size) {
    if (munmap_hook_ != NULL) (*munmap_hook_)(p, size);
  }

  // The MremapHook is invoked whenever a region of memory is remapped.
  typedef void (*MremapHook)(void* result,
                             void* old_addr,
                             size_t old_size,
                             size_t new_size,
                             int flags,
                             void* new_addr);
  inline static MremapHook GetMremapHook() { return mremap_hook_; }
  inline static MremapHook SetMremapHook(MremapHook hook) {
    MremapHook result = mremap_hook_;
    mremap_hook_ = hook;
    return result;
  }
  inline static void InvokeMremapHook(void* result,
                                      void* old_addr,
                                      size_t old_size,
                                      size_t new_size,
                                      int flags,
                                      void* new_addr) {
    if (mremap_hook_ != NULL) (*mremap_hook_)(result,
                                              old_addr, old_size,
                                              new_size, flags, new_addr);
  }

  // The SbrkHook is invoked whenever sbrk is called.
  typedef void (*SbrkHook)(void* result, ptrdiff_t increment);
  inline static SbrkHook GetSbrkHook() { return sbrk_hook_; }
  inline static SbrkHook SetSbrkHook(SbrkHook hook) {
    SbrkHook result = sbrk_hook_;
    sbrk_hook_ = hook;
    return result;
  }
  inline static void InvokeSbrkHook(void* result, ptrdiff_t increment) {
    if (sbrk_hook_ != NULL) (*sbrk_hook_)(result, increment);
  }

  // Get the current stack trace.  Try to skip all routines up to and
  // and including the caller of MallocHook::Invoke*.
  // Use "skip_count" (similarly to GetStackTrace from stacktrace.h)
  // as a hint about how many routines to skip if better information
  // is not available.
  static int GetCallerStackTrace(void** result, int max_depth, int skip_count);

 private:

  static NewHook     new_hook_;
  static DeleteHook  delete_hook_;
  static MmapHook    mmap_hook_;
  static MunmapHook  munmap_hook_;
  static MremapHook  mremap_hook_;
  static SbrkHook    sbrk_hook_;
};

#endif /* _MALLOC_HOOK_H */
