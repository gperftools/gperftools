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
#include <libunwind.h>
}
#include "google/stacktrace.h"
#include "base/spinlock.h"

// Sometimes, we can try to get a stack trace from within a stack
// trace, because libunwind can call mmap/sbrk (maybe indirectly via
// malloc), and that mmap gets trapped and causes a stack-trace
// request.  If were to try to honor that recursive request, we'd end
// up with infinite recursion or deadlock.  Luckily, it's safe to
// ignore those subsequent traces.  In such cases, we return 0 to
// indicate the situation.
static SpinLock libunwind_lock(SpinLock::LINKER_INITIALIZED);
static bool in_get_stack_trace = false;

int GetStackTrace(void** result, int max_depth, int skip_count) {
  void *ip;
  int n = 0;
  unw_cursor_t cursor;
  unw_context_t uc;

  {
    SpinLockHolder sh(&libunwind_lock);
    if (in_get_stack_trace) {
      return 0;
    } else {
      in_get_stack_trace = true;
    }
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

  SpinLockHolder sh(&libunwind_lock);
  in_get_stack_trace = false;

  return n;
}
