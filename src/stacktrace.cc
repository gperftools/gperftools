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

#include "config.h"
#include <stdlib.h>
#include "google/stacktrace.h"

#undef IMPLEMENTED_STACK_TRACE

// Linux/x86 implementation (requires the binary to be compiled with
// frame pointers)
#if (defined(__i386__) || defined(__x86_64__)) && \
    defined(__linux) && !defined(NO_FRAME_POINTER)
#define IMPLEMENTED_STACK_TRACE

#include <stdint.h>   // for uintptr_t

// Given a pointer to a stack frame, locate and return the calling
// stackframe, or return NULL if no stackframe can be found. Perform
// sanity checks to reduce the chance that a bad pointer is returned.
static void **NextStackFrame(void **old_sp) {
  void **new_sp = (void **) *old_sp;

  // Check that the transition from frame pointer old_sp to frame
  // pointer new_sp isn't clearly bogus
  if (new_sp <= old_sp) return NULL;
  if ((uintptr_t)new_sp & (sizeof(void *) - 1)) return NULL;
#ifdef __i386__
  // On 64-bit machines, the stack pointer can be very close to
  // 0xffffffff, so we explicitly check for a pointer into the
  // last two pages in the address space
  if ((uintptr_t)new_sp >= 0xffffe000) return NULL;
#endif
  if ((uintptr_t)new_sp - (uintptr_t)old_sp > 100000) return NULL;
  return new_sp;
}

// Note: the code for GetStackExtent below is pretty similar to this one;
//       change both if chaning one.
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
    if (*(sp+1) == (void *)0) {
      // In 64-bit code, we often see a frame that
      // points to itself and has a return address of 0.
      break;
    }
    if (skip_count > 0) {
      skip_count--;
    } else {
      result[n++] = *(sp+1);
    }
    void** new_sp = NextStackFrame(sp);
    if (!new_sp) break;
    sp = new_sp;
  }
  return n;
}

// Note: the code is pretty similar to GetStackTrace above;
//       change both if chaning one.
bool GetStackExtent(void* sp,  void** stack_top, void** stack_bottom) {
  void** cur_sp;

  if (sp != NULL) {
    cur_sp = (void**)sp;
    *stack_top = sp;
  } else {
#ifdef __i386__
    // Stack frame format:
    //    sp[0]   pointer to previous frame
    //    sp[1]   caller address
    //    sp[2]   first argument
    //    ...
    cur_sp = (void**)&sp - 2;
#endif

#ifdef __x86_64__
    // Arguments are passed in registers on x86-64, so we can't just
    // offset from &result
    cur_sp = (void**)__builtin_frame_address(0);
#endif
    *stack_top = NULL;
  }

  while (cur_sp) {
    void** new_sp = NextStackFrame(cur_sp);
    if (!new_sp) {
      *stack_bottom = (void*)cur_sp;
      return true;
    }
    cur_sp = new_sp;
    if (*stack_top == NULL)  *stack_top = (void*)cur_sp;
    // get out of the stack frame for this call
  }
  return false;
}

#endif

// Portable implementation - just use glibc
// 
// Note:  The glibc implementation may cause a call to malloc.
// This can cause a deadlock in HeapProfiler.
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

bool GetStackExtent(void* sp,  void** stack_bottom, void** stack_top) {
  return false;  // can't climb up
}

#endif

#if !defined(IMPLEMENTED_STACK_TRACE) && !defined(HAVE_EXECINFO_H)
#error Cannot calculate stack trace: will need to write for your environment
#endif
