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

#include <google/malloc_hook.h>
#include "base/basictypes.h"

MallocHook::NewHook    MallocHook::new_hook_ = NULL;
MallocHook::DeleteHook MallocHook::delete_hook_ = NULL;
MallocHook::MmapHook   MallocHook::mmap_hook_ = NULL;
MallocHook::MunmapHook MallocHook::munmap_hook_ = NULL;

// On Linux/x86, we override mmap/munmap and provide support for
// calling the related hooks.  
#if defined(__i386__) && defined(__linux)

#include <unistd.h>
#include <syscall.h>
#include <sys/mman.h>
#include <errno.h>

// This somewhat reimplements libc's mmap syscall stubs. Unfortunately
// libc only exports the stubs via weak symbols (which we're
// overriding with our mmap64() and mmap() wrappers) so we can't just
// call through to them.
extern "C" void* mmap64(void *start, size_t length,
                        int prot, int flags, 
                        int fd, __off64_t offset) __THROW {

  void *result;

  // Try mmap2() unless it's not supported
  static bool have_mmap2 = true;
  if (have_mmap2) {
    static int pagesize = 0;
    if (!pagesize) pagesize = getpagesize();

    // Check that the offset is page aligned
    if (offset & (pagesize - 1)) {
      result = MAP_FAILED;
      errno = EINVAL;
      goto out;
    }

    result = (void *)syscall(SYS_mmap2, 
                             start, length, prot, flags, fd, offset / pagesize);
    if (result != MAP_FAILED || errno != ENOSYS)  goto out;

    // We don't have mmap2() after all - don't bother trying it in future
    have_mmap2 = false;
  }

  if (((off_t)offset) != offset) {
    // If we're trying to map a 64-bit offset, fail now since we don't
    // have 64-bit mmap() support.
    result = MAP_FAILED;
    errno = EINVAL;
    goto out;
  }

  {
    // Fall back to old 32-bit offset mmap() call
    // Old syscall interface cannot handle six args, so pass in an array
    int32 args[6] = { (int32) start, length, prot, flags, fd, (off_t) offset };
    result = (void *)syscall(SYS_mmap, args);
  }
 out:
  MallocHook::InvokeMmapHook(result, start, length, prot, flags, fd, offset);
  return result;

}

extern "C" void* mmap(void *start, size_t length,
                      int prot, int flags, 
                      int fd, off_t offset) __THROW {
  return mmap64(start, length, prot, flags, fd, 
                static_cast<size_t>(offset)); // avoid sign extension
}

extern "C" int munmap(void* start, size_t length) __THROW {
  MallocHook::InvokeMunmapHook(start, length);
  return syscall(SYS_munmap, start, length);
}

#endif
