// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
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

#include "tests/testutil.h"

#include <stdlib.h>
// On FreeBSD, if you #include <sys/resource.h>, you have to get stdint first.
#include <stdint.h>
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <functional>
#include <thread>
#include <vector>

// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit() {
#ifdef HAVE_SYS_RESOURCE_H
  // The actual resource we need to set varies depending on which flavour of
  // unix.  On Linux we need RLIMIT_AS because that covers the use of mmap.
  // Otherwise hopefully RLIMIT_RSS is good enough.  (Unfortunately 64-bit
  // and 32-bit headers disagree on the type of these constants!)
#ifdef RLIMIT_AS
#define USE_RESOURCE RLIMIT_AS
#else
#define USE_RESOURCE RLIMIT_RSS
#endif

  // Restrict the test to 1GiB, which should fit comfortably well on both
  // 32-bit and 64-bit hosts, and executes in ~1s.
  const rlim_t kMaxMem = 1<<30;

  struct rlimit rlim;
  if (getrlimit(USE_RESOURCE, &rlim) == 0) {
    if (rlim.rlim_cur == RLIM_INFINITY || rlim.rlim_cur > kMaxMem) {
      rlim.rlim_cur = kMaxMem;
      setrlimit(USE_RESOURCE, &rlim); // ignore result
    }
  }
#endif  /* HAVE_SYS_RESOURCE_H */
}


struct FunctionAndId {
  void (*ptr_to_function)(int);
  int id;
};

#if defined(NO_THREADS)

extern "C" void RunThread(void (*fn)()) {
  (*fn)();
}

extern "C" void RunManyThreads(void (*fn)(), int count) {
  // I guess the best we can do is run fn sequentially, 'count' times
  for (int i = 0; i < count; i++)
    (*fn)();
}

extern "C" void RunManyThreadsWithId(void (*fn)(int), int count) {
  for (int i = 0; i < count; i++)
    (*fn)(i);    // stacksize doesn't make sense in a non-threaded context
}

#else

extern "C" {
  void RunThread(void (*fn)()) {
    std::thread{fn}.join();
  }

  static void RunMany(const std::function<void(int)>& fn, int count) {
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (int i = 0; i < count; i++) {
      threads.emplace_back(fn, i);
    }
    for (auto& t : threads) {
      t.join();
    }
  }

  void RunManyThreads(void (*fn)(), int count) {
    RunMany([fn] (int dummy) {
      fn();
    }, count);
  }

  void RunManyThreadsWithId(void (*fn)(int), int count) {
    RunMany(fn, count);
  }
}

#endif
