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

// This test links libtcmalloc and libprofiler as two separate shared
// libraries (as opposed to the combined libtcmalloc_and_profiler).
// Each of them has its own copy of the stacktrace code (including
// aw-backtrace) and those copies must not interfere. We used to crash
// at startup when both were loaded, because aw-backtrace symbols were
// exported and one library's copy interposed the other's.

#include "config_for_unittests.h"

#include <stdio.h>

#include <gperftools/malloc_hook.h>
#include <gperftools/profiler.h>

int main() {
  // Calling into both libraries also ensures linker's --as-needed
  // doesn't drop either one.
  void* stack[16];

  int tcmalloc_depth = MallocHook::GetCallerStackTrace(stack, 16, 0);
  int profiler_depth = ProfilerGetStackTrace(stack, 16, 0, nullptr);

  printf("tcmalloc depth: %d, profiler depth: %d\n", tcmalloc_depth, profiler_depth);
  if (tcmalloc_depth <= 0 || profiler_depth <= 0) {
    printf("FAIL\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
