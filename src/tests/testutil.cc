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

#include "config.h"
#include <stdlib.h>           // for NULL, abort()
#include "tests/testutil.h"

#ifdef HAVE_PTHREAD

#include <pthread.h>

#define SAFE_PTHREAD(fncall)  do { if ((fncall) != 0) abort(); } while (0)

// Run a function in a thread of its own and wait for it to finish.
// This is useful for tcmalloc testing, because each thread is handled
// separately in tcmalloc, so there's interesting stuff to test even if
// the threads are not running concurrently.

// This helper function has the signature that pthread_create wants.
extern "C" {
  static void* RunFunctionInThread(void *ptr_to_ptr_to_fn) {
    (**static_cast<void (**)()>(ptr_to_ptr_to_fn))();    // runs fn
    return NULL;
  }
}

void RunInThread(void (*fn)()) {
  pthread_t thr;
  // Even though fn is on the stack, it's safe to pass a pointer to it,
  // because we pthread_join immediately (ie, before RunInThread exits).
  SAFE_PTHREAD(pthread_create(&thr, NULL, RunFunctionInThread, &fn));
  SAFE_PTHREAD(pthread_join(thr, NULL));
}

#else   // !HAVE_PTHREAD

void RunInThread(void (*fn)()) {
  fn();         // best we can do: just run it
}

#endif
