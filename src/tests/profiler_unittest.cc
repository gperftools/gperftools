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

#include "google/perftools/config.h"
#include <stdio.h>
#include <stdlib.h>
#include "google/profiler.h"

static int result = 0;

#ifdef WITH_THREADS
#include <pthread.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

#define LOCK   pthread_mutex_lock(&mutex)    /* ignore errors; oh well */
#define UNLOCK pthread_mutex_unlock(&mutex)  /* ignore errors; oh well */

void* test_other_thread(void* data) {
  ProfilerRegisterThread();

  int iters = *(int*)data;
  int i, m;
  char b[128];
  for (m = 0; m < 1000000; ++m) {          // run millions of times
    for (i = 0; i < iters; ++i ) {
      LOCK;
      result ^= i;
      UNLOCK;
    }
    LOCK;
    snprintf(b, sizeof(b), "%d", result);  // get some libc action
    UNLOCK;
  }
  
  return NULL;                             // success
}

#else   /* WITH_THREADS */

#define LOCK
#define UNLOCK

#endif  /* WITH_THREADS */

static int test_main_thread(int iters) {
  int i, m;
  char b[128];
  for (m = 0; m < 1000000; ++m) {          // run millions of times
    for (i = 0; i < iters; ++i ) {
      LOCK;
      result ^= i;
      UNLOCK;
    }
    LOCK;
    snprintf(b, sizeof(b), "%d", result);  // get some libc action
    UNLOCK;
  }
  return result;
}

int main(int argc, char** argv) {
  if ( argc <= 1 ) {
    fprintf(stderr, "USAGE: %s <iters> [num_threads] [filename]\n", argv[0]);
    fprintf(stderr, "   iters: How many million times to run the XOR test.\n");
    fprintf(stderr, "   num_threads: how many concurrent threads.\n");
    fprintf(stderr, "                0 or 1 for single-threaded mode.\n");
    fprintf(stderr, "   filename: The name of the output profile.\n");
    fprintf(stderr, ("             If you don't specify, set CPUPROFILE "
                     "in the environment instead!\n"));
    return 1;
  }

  int iters = atoi(argv[1]);
  int num_threads = 1;
  const char* filename = NULL;
  if (argc > 2) {
    num_threads = atoi(argv[2]);
  }
  if (argc > 3) {
    filename = argv[3];
  }

  if (filename) {
    ProfilerStart(filename);
  }

  test_main_thread(iters);

  ProfilerFlush();                           // just because we can

  // The other threads, if any, will run only half as long as the main thread
#ifdef WITH_THREADS
  for (; num_threads > 1; --num_threads) {
    int thread_id;
    pthread_t thr;
    thread_id = pthread_create(&thr, NULL, &test_other_thread, &iters);
  }
#endif

  int r = test_main_thread(iters);
  printf("The XOR test returns %d\n", r);

  if (filename) {
    ProfilerStop();
  }

  return 0;
}
