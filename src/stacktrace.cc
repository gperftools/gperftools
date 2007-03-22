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
// Produce stack trace

#include "google/perftools/config.h"
#include "google/stacktrace.h"

#undef IMPLEMENTED_STACK_TRACE

// Linux/x86 implementation (requires the binary to be compiled with
// frame pointers)
#if (defined(__i386__) || defined(__x86_64)) && \
    defined(__linux) && !defined(NO_FRAME_POINTER) && !defined(_LP64)
#define IMPLEMENTED_STACK_TRACE

int GetStackTrace(void** result, int max_depth, int skip_count) {
  void **sp;
#ifdef __i386__
  // Stack frame format:
  //    sp[0]   pointer to previous frame
  //    sp[1]   caller address
  //    sp[2]   first argument
  //    ...
  sp = (void **)&result - 2;
#endif

#ifdef __x86_64__
  // Arguments are passed in registers on x86-64, so we can't just
  // offset from &result
  sp = (void **) __builtin_frame_address(0);
#endif

  int n = 0;
  skip_count++;         // Do not include the "GetStackTrace" frame
  while (sp && n < max_depth) {
    if (skip_count > 0) {
      skip_count--;
    } else {
      result[n++] = *(sp+1);
    }
    void** new_sp = (void**) *sp;

    // A little bit of sanity checking to avoid crashes
    if (new_sp < sp || new_sp > sp + 100000) {
      break;
    }
    sp = new_sp;
  }
  return n;
}
#endif

// Portable implementation - just use glibc
#if !defined(IMPLEMENTED_STACK_TRACE) && defined(HAVE_EXECINFO_H)
#include <stdlib.h>
#include <execinfo.h>

int GetStackTrace(void** result, int max_depth, int skip_count) {
  static const int kStackLength = 64;
  void * stack[kStackLength];
  int size;
  
  size = backtrace(stack, kStackLength);
  skip_count++;  // we want to skip the current frame as well
  int result_count = size - skip_count;
  if ( result_count < 0 )
    result_count = 0;
  else if ( result_count > max_depth )
    result_count = max_depth;

  for (int i = 0; i < result_count; i++)
    result[i] = stack[i + skip_count];

  return result_count;
}
#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && !defined(HAVE_EXECINFO_H)
#error Cannot calculate stack trace: will need to write for your environment
#endif
