// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2004, Google Inc.
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
// Author: Sanjay Ghemawat
//
// Check that we do not leak memory when cycling through lots of threads.

#include "config_for_unittests.h"
#include <stdio.h>
#include "base/logging.h"
#include <gperftools/malloc_extension.h>

#include <memory>
#include <thread>
#include <vector>

// Size/number of objects to allocate per thread (1 MB per thread)
static const int kObjectSize = 1024;
static const int kNumObjects = 1024;

size_t GetThreadHeapCount() {
  size_t rv;
  CHECK(MallocExtension::instance()->GetNumericProperty("tcmalloc.impl.thread_cache_count", &rv));
  return rv;
}

// This breaks on glibc. What happens is do_early_stuff below is run
// early on AllocStuff thread. It calls to pthread_setspecific which
// (being first setspecific for range of keys [32,64) ), will
// calloc. That calloc call will create thread cache and
// pthread_setspecific to 'nearby' pthread_key. Then calloc returns
// and original call to setspecific overwrites array of TLS
// values. And "looses" pthread_setspecific update we made as part of
// initializing thread cache.
//
// Do note, though, that attribute constructor trick only succeeds to
// reproduce the issue when tcmalloc is linked statically to this
// test. Only then we're able to "insert" a bunch of pthread keys
// before tcmalloc allocates its own.
//
// Why glibc works in regular case? Because usually pthread_key_t
// value for ThreadCache instance is allocated early. So it gets low
// numeric key value. And for those low numeric values, glibc uses
// "static" TLS storage, which is safe. It looks like glibc does that
// specifically to enable our (and other malloc implementations) case.
//
// Similar cases might happen on other pthread implementations
// (depending on how, if at all, their pthread_setspecific
// implementation does malloc). There appears to be no portable way to
// prevent this problem.
//
// Mingw's libwinpthread would simply deadlock. They do call into
// malloc, and they don't allow *any* reentrancy into pthread TLS
// bits. But we're using windows native TLS there.
//
// Musl and bionic use "static" arrays for thread specific values, so
// we're safe there. Same applies to NetBSD.
//
// FreeBSD appears to be using some internal memory allocation routine
// for allocation of storage thread specific values. So should be fine
// too. Same appears to be the case for OpenSolaris (and perhaps just
// Solaris), but they also no-memory-allocation thread specific for
// low pthread_key values (same as glibc).
//
// NOTE: jemalloc uses FreeBSD-specific _malloc_thread_cleanup, which
// explicitly avoids the issue. So we can do same if necessary.
#if defined(TEST_HARD_THREAD_DEALLOC)
static pthread_key_t early_tls_key;

static __attribute__((constructor(101)))
void early_stuff() {
  // When this is defined, the "leak" part is skipped. So both thread
  // cache and early_tls_key get low values, so we're passing the
  // test. See above for details.
  //
  // I.e. CPPFLAGS=-DTEST_HARD_THREAD_DEALLOC fails on glibc, and
  // 'CPPFLAGS=-DTEST_HARD_THREAD_DEALLOC -DTEST_LESS_HARD_THREAD_DEALLOC' works
#if !defined(TEST_LESS_HARD_THREAD_DEALLOC)
  pthread_key_t leaked;
  for (int i = 0; i < 32; i++) {
    CHECK(pthread_key_create(&leaked, nullptr) == 0);
  }
#endif

  CHECK(pthread_key_create(&early_tls_key, +[] (void* arg) {
    auto sz = reinterpret_cast<uintptr_t>(arg);
    (operator delete)((operator new)(sz));
  }) == 0);
}

static void do_early_stuff() {
  pthread_setspecific(early_tls_key, reinterpret_cast<void*>(uintptr_t{32}));
}
#else
static void do_early_stuff() {}
#endif

// Allocate lots of stuff
static void AllocStuff() {
  do_early_stuff();

  std::unique_ptr<void*[]> objects{new void*[kNumObjects]};

  for (int i = 0; i < kNumObjects; i++) {
    objects[i] = malloc(kObjectSize);
  }
  for (int i = 0; i < kNumObjects; i++) {
    free(objects[i]);
  }
}

int main(int argc, char** argv) {
  constexpr int kDisplaySize = 1 << 20;
  std::unique_ptr<char[]> display{new char[kDisplaySize]};

  printf("thread count before: %zu\n", GetThreadHeapCount());

  // Number of threads to create and destroy
  constexpr int kNumThreads = 1000;

  for (int i = 0; i < kNumThreads; i++) {
    std::thread{AllocStuff}.join();

    if (((i+1) % 200) == 0) {
      printf("Iteration: %d of %d\n", (i+1), kNumThreads);
      MallocExtension::instance()->GetStats(display.get(), kDisplaySize);
      printf("%s\n", display.get());
      printf("Thread count: %zu\n", GetThreadHeapCount());
    }
  }

  size_t thread_count_after = GetThreadHeapCount();
  printf("thread count after: %zu\n", thread_count_after);
  CHECK_EQ(thread_count_after, 1);

  printf("PASS\n");

  return 0;
}
