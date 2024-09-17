// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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
// Author: Craig Silverstein
//
// Does some simple arithmetic and a few libc routines, so we can profile it.
// Define WITH_THREADS to add pthread functionality as well (otherwise, btw,
// the num_threads argument to this program is ingored).

#include "config_for_unittests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>                 // for fork()
#include <sys/wait.h>               // for wait()
#endif

#include <atomic>
#include <mutex>

#include "gperftools/profiler.h"
#include "tests/testutil.h"

static int g_iters;   // argv[1]

// g_ticks_count points to internal profiler's tick count that
// increments each profiling tick. Makes it possible for this test
// loops to run long enough to get enough ticks.
static int volatile *g_ticks_count = ([] () {
  ProfilerState state;
  memset(&state, 0, sizeof(state));
  ProfilerGetCurrentState(&state);
  size_t sz = strlen(state.profile_name);
  if (sz + 1 + sizeof(g_ticks_count) > sizeof(state.profile_name)) {
    fprintf(stderr, "too long profile_name?: %zu (%s)\n", sz, state.profile_name);
    abort();
  }
  int volatile* ptr;
  memcpy(&ptr, state.profile_name + sz + 1, sizeof(ptr));
  return ptr;
})();

std::mutex mutex;

static void test_other_thread() {
#ifndef NO_THREADS
  ProfilerRegisterThread();

  int result = 0;
  char b[128];
  // Get at least 30 ticks
  int limit = *g_ticks_count + 30;

  std::lock_guard ml(mutex);

  while (*g_ticks_count < limit) {
    for (int i = 0; i < g_iters * 10; ++i ) {
      *const_cast<volatile int*>(&result) ^= i;
    }
    snprintf(b, sizeof(b), "other: %d", result);  // get some libc action
    (void)noopt(b); // 'consume' b. Ensure that smart compiler doesn't
                    // remove snprintf call
  }
#endif
}

static void test_main_thread() {
  int result = 0;
  char b[128];
  // Get at least 30 ticks
  int limit = *g_ticks_count + 30;

  std::lock_guard ml(mutex);

  while (*g_ticks_count < limit) {
    for (int i = 0; i < g_iters * 10; ++i ) {
      *const_cast<volatile int*>(&result) ^= i;
    }
    snprintf(b, sizeof(b), "same: %d", result);  // get some libc action
    (void)noopt(b); // 'consume' b
  }
}



int main(int argc, char** argv) {
  if ( argc <= 1 ) {
    fprintf(stderr, "USAGE: %s <iters> [num_threads] [filename]\n", argv[0]);
    fprintf(stderr, "   iters: How many million times to run the XOR test.\n");
    fprintf(stderr, "   num_threads: how many concurrent threads.\n");
    fprintf(stderr, "                0 or 1 for single-threaded mode,\n");
    fprintf(stderr, "                -# to fork instead of thread.\n");
    fprintf(stderr, "   filename: The name of the output profile.\n");
    fprintf(stderr, ("             If you don't specify, set CPUPROFILE "
                     "in the environment instead!\n"));
    return 1;
  }

  g_iters = atoi(argv[1]);
  int num_threads = 1;
  const char* filename = nullptr;
  if (argc > 2) {
    num_threads = atoi(argv[2]);
  }
  if (argc > 3) {
    filename = argv[3];
  }

  if (filename) {
    ProfilerStart(filename);
  }

  test_main_thread();

  ProfilerFlush();                           // just because we can

  // The other threads, if any, will run only half as long as the main thread
  if(num_threads > 0) {
    RunManyThreads(test_other_thread, num_threads);
  } else {
  // Or maybe they asked to fork.  The fork test is only interesting
  // when we use CPUPROFILE to name, so check for that
#ifdef HAVE_UNISTD_H
    for (; num_threads < 0; ++num_threads) {   // -<num_threads> to fork
      if (filename) {
        printf("FORK test only makes sense when no filename is specified.\n");
        return 2;
      }
      switch (fork()) {
        case -1:
          printf("FORK failed!\n");
          return 1;
        case 0:             // child
          return execl(argv[0], argv[0], argv[1], nullptr);
        default:
          wait(nullptr);       // we'll let the kids run one at a time
      }
    }
#else
    fprintf(stderr, "%s was compiled without support for fork() and exec()\n", argv[0]);
#endif
  }

  test_main_thread();

  if (filename) {
    ProfilerStop();
  }

  return 0;
}
