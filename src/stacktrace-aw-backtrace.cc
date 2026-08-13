/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2026, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "aw-backtrace/aw-backtrace.h"

#include <stdint.h>

#include "stacktrace_internal.h"

namespace {

struct Capturer {
  void** const result;
  int* const sizes;
  const int max_depth;
  int skip_count;
  int i = 0;
  uintptr_t prev_sp = 0;

  Capturer(void** result, int* sizes, int max_depth, int skip_count)
    : result{result}, sizes{sizes}, max_depth{max_depth}, skip_count{skip_count} {}

  bool DoAdd(void* pc, void* sp) {
    if (skip_count > 0) {
      skip_count--;
      return true;
    }
    uintptr_t current_sp = reinterpret_cast<uintptr_t>(sp);
    if (sizes && i > 0) {
      // Only now, seeing this frame's sp, do we learn the previously
      // captured frame's caller's sp -- i.e. that previous frame's own
      // size (distance from its sp to its caller's sp).
      sizes[i - 1] = static_cast<int>(current_sp - prev_sp);
    }
    if (i >= max_depth) {
      // We got called only to learn this frame's sp, to close out
      // sizes[max_depth - 1]. Nothing else to record.
      return false;
    }
    result[i] = pc;
    if (sizes) {
      sizes[i] = 0;  // unknown unless/until the next callback fills it in
    }
    prev_sp = current_sp;
    i++;
    // When sizes are wanted we need one extra callback beyond the last
    // requested frame purely to learn its size; otherwise stop as soon
    // as result[] is full.
    return sizes ? true : (i < max_depth);
  }

  static bool Add(void* pc, void* sp, bool pc_before_insn, void* data) {
    Capturer* c = static_cast<Capturer*>(data);
    (void)pc_before_insn;
    return c->DoAdd(pc, sp);
  }
};

static int adjust_skip(int skip_count, const void* uc) {
  if (uc) {
    return 0;
  }
  return skip_count + 2;
}

static int get_stack_frames(void** result, int* sizes, int max_depth, int skip_count) {
  Capturer c{result, sizes, max_depth, adjust_skip(skip_count, nullptr)};
  aw_backtrace_full(nullptr, &Capturer::Add, &c);
  return c.i;
}

static int get_stack_frames_with_uc(void** result, int* sizes, int max_depth, int skip_count, const void* uc) {
  Capturer c{result, sizes, max_depth, adjust_skip(skip_count, uc)};
  aw_backtrace_full(uc, &Capturer::Add, &c);
  return c.i;
}

static int get_stack_trace(void** result, int max_depth, int skip_count) {
  Capturer c{result, nullptr, max_depth, adjust_skip(skip_count, nullptr)};
  aw_backtrace_full(nullptr, &Capturer::Add, &c);
  return c.i;
}

static int get_stack_trace_with_uc(void** result, int max_depth, int skip_count, const void* uc) {
  Capturer c{result, nullptr, max_depth, adjust_skip(skip_count, uc)};
  aw_backtrace_full(uc, &Capturer::Add, &c);
  return c.i;
}

}  // namespace

GetStackImplementation tcmalloc_gst_impl_aw_backtrace = {
  &get_stack_frames,
  &get_stack_frames_with_uc,
  &get_stack_trace,
  &get_stack_trace_with_uc,
  "aw-backtrace"
};
