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

#include <stdint.h>   // for uintptr_t
#include <stdlib.h>   // for NULL
#include "google/stacktrace.h"

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
  // Stack frame format:
  //    sp[0]   pointer to previous frame
  //    sp[1]   caller address
  //    sp[2]   first argument
  //    ...
  sp = (void **)&result - 2;

  int n = 0;
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
//       change both if changing one.
bool GetStackExtent(void* sp,  void** stack_top, void** stack_bottom) {
  void** cur_sp;

  if (sp != NULL) {
    cur_sp = (void**)sp;
    *stack_top = sp;
  } else {
    // Stack frame format:
    //    sp[0]   pointer to previous frame
    //    sp[1]   caller address
    //    sp[2]   first argument
    //    ...
    cur_sp = (void**)&sp - 2;

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
