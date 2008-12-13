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
// Author: Arun Sharma
//
// Produce stack trace using libunwind

extern "C" {
#include <assert.h>
#include <string.h>   // for memset()
#include <libunwind.h>
}
#include "google/stacktrace.h"
#include "base/logging.h"
#include "base/spinlock.h"

// Sometimes, we can try to get a stack trace from within a stack
// trace, because libunwind can call mmap (maybe indirectly via an
// internal mmap based memory allocator), and that mmap gets trapped
// and causes a stack-trace request.  If were to try to honor that
// recursive request, we'd end up with infinite recursion or deadlock.
// Luckily, it's safe to ignore those subsequent traces.  In such
// cases, we return 0 to indicate the situation.
static SpinLock libunwind_lock(SpinLock::LINKER_INITIALIZED);

// If you change this function, also change GetStackFrames below.
int GetStackTrace(void** result, int max_depth, int skip_count) {
  void *ip;
  int n = 0;
  unw_cursor_t cursor;
  unw_context_t uc;

  if (!libunwind_lock.TryLock()) {
    return 0;
  }

  unw_getcontext(&uc);
  int ret = unw_init_local(&cursor, &uc);
  assert(ret >= 0);
  skip_count++;         // Do not include the "GetStackTrace" frame

  while (n < max_depth) {
    int ret = unw_get_reg(&cursor, UNW_REG_IP, (unw_word_t *) &ip);
    if (ret < 0)
      break;
    if (skip_count > 0) {
      skip_count--;
    } else {
      result[n++] = ip;
    }
    ret = unw_step(&cursor);
    if (ret <= 0)
      break;
  }

  libunwind_lock.Unlock();
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
  void *ip;
  int n = 0;
  unw_cursor_t cursor;
  unw_context_t uc;
  unw_word_t sp = 0, next_sp = 0;

  if (!libunwind_lock.TryLock()) {
    return 0;
  }

  unw_getcontext(&uc);
  RAW_CHECK(unw_init_local(&cursor, &uc) >= 0, "unw_init_local failed");
  skip_count++;         // Do not include the "GetStackFrames" frame

  while (skip_count--) {
    if (unw_step(&cursor) <= 0 ||
        unw_get_reg(&cursor, UNW_REG_SP, &next_sp) < 0) {
      goto out;
    }
  }
  while (n < max_depth) {
    sp = next_sp;
    if (unw_get_reg(&cursor, UNW_REG_IP, (unw_word_t *) &ip) < 0)
      break;
    if (unw_step(&cursor) <= 0 ||
        unw_get_reg(&cursor, UNW_REG_SP, &next_sp)) {
      // We couldn't step any further (possibly because we reached _start).
      // Provide the last good PC we've got, and get out.
      sizes[n] = 0;
      pcs[n++] = ip;
      break;
    }
    sizes[n] = next_sp - sp;
    pcs[n++] = ip;
  }
 out:
  libunwind_lock.Unlock();
  return n;
}
