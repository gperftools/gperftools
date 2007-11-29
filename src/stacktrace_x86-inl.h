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
// stackframe, or return NULL if no stackframe can be found. Perform sanity
// checks (the strictness of which is controlled by the boolean parameter
// "STRICT_UNWINDING") to reduce the chance that a bad pointer is returned.
template<bool STRICT_UNWINDING>
static void **NextStackFrame(void **old_sp) {
  void **new_sp = (void **) *old_sp;

  // Check that the transition from frame pointer old_sp to frame
  // pointer new_sp isn't clearly bogus
  if (STRICT_UNWINDING) {
    // With the stack growing downwards, older stack frame must be
    // at a greater address that the current one.
    if (new_sp <= old_sp) return NULL;
    // Assume stack frames larger than 100,000 bytes are bogus.
    if ((uintptr_t)new_sp - (uintptr_t)old_sp > 100000) return NULL;
  } else {
    // In the non-strict mode, allow discontiguous stack frames.
    // (alternate-signal-stacks for example).
    if (new_sp == old_sp) return NULL;
    // And allow frames upto about 1MB.
    if ((new_sp > old_sp)
        && ((uintptr_t)new_sp - (uintptr_t)old_sp > 1000000)) return NULL;
  }
  if ((uintptr_t)new_sp & (sizeof(void *) - 1)) return NULL;
#ifdef __i386__
  // On 64-bit machines, the stack pointer can be very close to
  // 0xffffffff, so we explicitly check for a pointer into the
  // last two pages in the address space
  if ((uintptr_t)new_sp >= 0xffffe000) return NULL;
#endif
  return new_sp;
}

// If you change this function, also change GetStackFrames below.
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
  // __builtin_frame_address(0) can return the wrong address on gcc-4.1.0-k8
  unsigned long rbp;
  // Move the value of the register %rbp into the local variable rbp.
  // We need 'volatile' to prevent this instruction from getting moved
  // around during optimization to before function prologue is done.
  // An alternative way to achieve this
  // would be (before this __asm__ instruction) to call Noop() defined as
  //   static void Noop() __attribute__ ((noinline));  // prevent inlining
  //   static void Noop() { asm(""); }  // prevent optimizing-away
  __asm__ volatile ("mov %%rbp, %0" : "=r" (rbp));
  // Arguments are passed in registers on x86-64, so we can't just
  // offset from &result
  sp = (void **) rbp;
#endif

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
    // Use strict unwinding rules.
    sp = NextStackFrame<true>(sp);
  }
  return n;
}

// If you change this function, also change GetStackTrace above:
//
// This GetStackFrames routine shares a lot of code with GetStackTrace
// above. This code could have been refactored into a common routine,
// and then both GetStackTrace/GetStackFrames could call that routine.
// There are two problems with that:
//
// (1) The performance of the refactored-code suffers substantially - the
//     refactored needs to be able to record the stack trace when called
//     from GetStackTrace, and both the stack trace and stack frame sizes,
//     when called from GetStackFrames - this introduces enough new
//     conditionals that GetStackTrace performance can degrade by as much
//     as 50%.
//
// (2) Whether the refactored routine gets inlined into GetStackTrace and
//     GetStackFrames depends on the compiler, and we can't guarantee the
//     behavior either-way, even with "__attribute__ ((always_inline))"
//     or "__attribute__ ((noinline))". But we need this guarantee or the
//     frame counts may be off by one.
//
// Both (1) and (2) can be addressed without this code duplication, by
// clever use of template functions, and by defining GetStackTrace and
// GetStackFrames as macros that expand to these template functions.
// However, this approach comes with its own set of problems - namely,
// macros and  preprocessor trouble - for example,  if GetStackTrace
// and/or GetStackFrames is ever defined as a member functions in some
// class, we are in trouble.
int GetStackFrames(void** pcs, int* sizes, int max_depth, int skip_count) {
  void **sp;
#ifdef __i386__
  // Stack frame format:
  //    sp[0]   pointer to previous frame
  //    sp[1]   caller address
  //    sp[2]   first argument
  //    ...
  sp = (void **)&pcs - 2;
#endif

#ifdef __x86_64__
  // __builtin_frame_address(0) can return the wrong address on gcc-4.1.0-k8
  unsigned long rbp;
  // Move the value of the register %rbp into the local variable rbp.
  // We need 'volatile' to prevent this instruction from getting moved
  // around during optimization to before function prologue is done.
  // An alternative way to achieve this
  // would be (before this __asm__ instruction) to call Noop() defined as
  //   static void Noop() __attribute__ ((noinline));  // prevent inlining
  //   static void Noop() { asm(""); }  // prevent optimizing-away
  __asm__ volatile ("mov %%rbp, %0" : "=r" (rbp));
  // Arguments are passed in registers on x86-64, so we can't just
  // offset from &pcs
  sp = (void **) rbp;
#endif

  int n = 0;
  while (sp && n < max_depth) {
    if (*(sp+1) == (void *)0) {
      // In 64-bit code, we often see a frame that
      // points to itself and has a return address of 0.
      break;
    }
    // The GetStackFrames routine is called when we are in some
    // informational context (the failure signal handler for example).
    // Use the non-strict unwinding rules to produce a stack trace
    // that is as complete as possible (even if it contains a few bogus
    // entries in some rare cases).
    void **next_sp = NextStackFrame<false>(sp);
    if (skip_count > 0) {
      skip_count--;
    } else {
      pcs[n] = *(sp+1);
      if (next_sp > sp) {
        sizes[n] = (uintptr_t)next_sp - (uintptr_t)sp;
      } else {
        // A frame-size of 0 is used to indicate unknown frame size.
        sizes[n] = 0;
      }
      n++;
    }
    sp = next_sp;
  }
  return n;
}
