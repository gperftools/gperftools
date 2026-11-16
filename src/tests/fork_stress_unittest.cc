// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2026, gperftools Contributors
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

// Fork stress test. Several threads keep creating and destroying
// threads (i.e. thread caches), allocating, and poking at
// thread-cache-related MallocExtension knobs, while the main thread
// forks repeatedly. Each child does a bit of malloc/free and
// _exit-s. Neither parent nor children may deadlock. This exercises
// our atfork handlers (LockAllForFork) together with all the locks
// they take.
#include "config_for_unittests.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <thread>
#include <vector>

#include <gperftools/malloc_extension.h>

#include "base/cleanup.h"

#include "gtest/gtest.h"

namespace {

std::atomic<bool> stop;

void AllocSome(unsigned seed) {
  static constexpr size_t kSizes[] = {8, 32, 100, 1024, 4000, 32 << 10, 300 << 10, 2 << 20};
  void* ptrs[16];
  for (int i = 0; i < 16; i++) {
    ptrs[i] = (::operator new)(kSizes[(seed + i) % (sizeof(kSizes) / sizeof(kSizes[0]))]);
  }
  for (int i = 15; i >= 0; i--) {
    (::operator delete)(ptrs[i]);
  }
}

void ChurnThreads(unsigned id) {
  MallocExtension* ext = MallocExtension::instance();
  unsigned iter = 0;
  while (!stop.load(std::memory_order_relaxed)) {
    iter++;
    // Short-lived thread: creates and destroys a thread cache.
    std::thread t([seed = id + iter]() { AllocSome(seed); });
    AllocSome(id * 7 + iter);
    t.join();

    switch (iter % 8) {
      case 0:
        ext->MarkThreadIdle();
        break;
      case 3: {
        size_t value;
        ext->GetNumericProperty("tcmalloc.current_total_thread_cache_bytes", &value);
        ext->GetNumericProperty("tcmalloc.impl.thread_cache_count", &value);
        break;
      }
      case 5: {
        // Alternate the total thread cache budget, which recomputes
        // and steals per-thread limits.
        size_t value = (iter & 16) ? (4 << 20) : (32 << 20);
        ext->SetNumericProperty("tcmalloc.max_total_thread_cache_bytes", value);
        break;
      }
      default:
        break;
    }
  }
}

TEST(ForkStressTest, ForkWhileChurningThreads) {
  constexpr int kThreads = 6;
  constexpr int kForks = 1000;

  size_t old_budget;
  ASSERT_TRUE(MallocExtension::instance()->GetNumericProperty("tcmalloc.max_total_thread_cache_bytes", &old_budget));

  std::vector<std::thread> threads;
  for (unsigned i = 0; i < kThreads; i++) {
    threads.emplace_back(ChurnThreads, i);
  }
  tcmalloc::Cleanup join_threads([&]() {
    stop.store(true);
    for (auto& t : threads) {
      t.join();
    }
    MallocExtension::instance()->SetNumericProperty("tcmalloc.max_total_thread_cache_bytes", old_budget);
  });

  for (int i = 0; i < kForks; i++) {
    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      // If the child deadlocks, it gets killed by SIGALRM, which the
      // parent detects below.
      alarm(10);
      AllocSome(i);
      AllocSome(i + 1);
      MallocExtension::instance()->MarkThreadIdle();
      AllocSome(i + 2);
      _exit(0);
    }
    AllocSome(i);
    int status = 0;
    pid_t ret;
    do {
      ret = waitpid(child, &status, 0);
    } while (ret < 0 && errno == EINTR);
    ASSERT_EQ(ret, child);
    ASSERT_TRUE(WIFEXITED(status)) << "child " << i << " died with status " << status;
    ASSERT_EQ(WEXITSTATUS(status), 0);
  }
}

}  // namespace
