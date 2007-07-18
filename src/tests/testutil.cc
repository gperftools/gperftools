// Copyright (c) 2007, Google Inc.
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
// A few routines that are useful for multiple tests in this directory.

#include "config_for_unittests.h"
#include <stdlib.h>           // for NULL, abort()
#include "tests/testutil.h"

#if defined(NO_THREADS) || !defined(HAVE_PTHREADS)

extern "C" void RunThread(void (*fn)()) {
  (*fn)();
}

extern "C" void RunManyThreads(void (*fn)(), int count) {
  // I guess the best we can do is run fn sequentially, 'count' times
  for (int i = 0; i < count; i++)
    (*fn)();
}

extern "C" void RunManyThreadsWithId(void (*fn)(int), int count, int) {
  for (int i = 0; i < count; i++)
    (*fn)(i);    // stacksize doesn't make sense in a non-threaded context
}

#else  // NO_THREADS || !HAVE_PTHREAD

#include <pthread.h>

#define SAFE_PTHREAD(fncall)  do { if ((fncall) != 0) abort(); } while (0)

extern "C" {
  struct FunctionAndId {
    void (*ptr_to_function)(int);
    int id;
  };

// This helper function has the signature that pthread_create wants.
  static void* RunFunctionInThread(void *ptr_to_ptr_to_fn) {
    (**static_cast<void (**)()>(ptr_to_ptr_to_fn))();    // runs fn
    return NULL;
  }

  static void* RunFunctionInThreadWithId(void *ptr_to_fnid) {
    FunctionAndId* fn_and_id = static_cast<FunctionAndId*>(ptr_to_fnid);
    (*fn_and_id->ptr_to_function)(fn_and_id->id);   // runs fn
    return NULL;
  }

  // Run a function in a thread of its own and wait for it to finish.
  // This is useful for tcmalloc testing, because each thread is
  // handled separately in tcmalloc, so there's interesting stuff to
  // test even if the threads are not running concurrently.
  void RunThread(void (*fn)()) {
    pthread_t thr;
    // Even though fn is on the stack, it's safe to pass a pointer to it,
    // because we pthread_join immediately (ie, before RunInThread exits).
    SAFE_PTHREAD(pthread_create(&thr, NULL, RunFunctionInThread, &fn));
    SAFE_PTHREAD(pthread_join(thr, NULL));
  }

  void RunManyThreads(void (*fn)(), int count) {
    pthread_t* thr = new pthread_t[count];
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_create(&thr[i], NULL, RunFunctionInThread, &fn));
    }
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_join(thr[i], NULL));
    }
    delete[] thr;
  }

  void RunManyThreadsWithId(void (*fn)(int), int count, int stacksize) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stacksize);

    pthread_t* thr = new pthread_t[count];
    FunctionAndId* fn_and_ids = new FunctionAndId[count];
    for (int i = 0; i < count; i++) {
      fn_and_ids[i].ptr_to_function = fn;
      fn_and_ids[i].id = i;
      SAFE_PTHREAD(pthread_create(&thr[i], &attr,
                                  RunFunctionInThreadWithId, &fn_and_ids[i]));
    }
    for (int i = 0; i < count; i++) {
      SAFE_PTHREAD(pthread_join(thr[i], NULL));
    }
    delete[] fn_and_ids;
    delete[] thr;

    pthread_attr_destroy(&attr);
  }
}

#endif
