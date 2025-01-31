/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
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
#include "config.h"

#define THIS_IS_MALLOC_BACKTRACE_CC
#include "malloc_backtrace.h"

#include "thread_cache_ptr.h"

extern "C" {
  void* tc_new(size_t);
  void tc_delete(void*);
}

namespace tcmalloc {
#ifndef NO_TCMALLOC_SAMPLES
// GrabBacktrace is the API to use when capturing backtrace for
// various tcmalloc features. It has optional emergency malloc
// integration for occasional case where stacktrace capturing method
// calls back to malloc (so we divert those calls to emergency malloc
// facility).
ATTRIBUTE_HIDDEN ATTRIBUTE_NOINLINE
int GrabBacktrace(void** result, int max_depth, int skip_count) {
  struct Args {
    void** result;
    int max_depth;
    int skip_count;
    int result_depth;
  } args;

  args.result = result;
  args.max_depth = max_depth;
  args.skip_count = skip_count;
  args.result_depth = 0;

  struct Body {
    static ATTRIBUTE_NOINLINE
    void Run(bool stacktrace_allowed, void* _args) {
      Args* args = static_cast<Args*>(_args);
      if (!stacktrace_allowed) {
        return;
      }

#if (!defined(NDEBUG) || defined(TCMALLOC_FORCE_BAD_TLS)) && defined(ENABLE_EMERGENCY_MALLOC)
      // Lets ensure test coverage of emergency malloc even in
      // configurations that otherwise don't exercise it.
      (tc_delete)(tc_new(32));
#endif

      args->result_depth = GetStackTrace(args->result, args->max_depth, args->skip_count + 3);
    }
  };

  ThreadCachePtr::WithStacktraceScope(Body::Run, &args);

  // Prevent tail calling WithStacktraceScope above
  return *const_cast<volatile int*>(&args.result_depth);
}
#endif
}  // namespace tcmalloc
