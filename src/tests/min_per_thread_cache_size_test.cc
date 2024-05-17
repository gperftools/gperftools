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

#include "config_for_unittests.h"

#include <new>
#include <thread>
#include <vector>

#include <gperftools/malloc_extension.h>
#include <gperftools/malloc_extension_c.h>

#include "base/logging.h"
#include "gtest/gtest.h"

// Number of allocations per thread.
static const int kAllocationsPerThread = 10000;

// Number of threads to create.
static const int kNumThreads = 50;

// Per thread cache size to set.
static const size_t kPerThreadCacheSize = 64 << 10;

// Number of passes to run.
static const int kNumPasses = 10;

// Get current total thread-cache size.
static size_t CurrentThreadCacheSize() {
  size_t result = 0;
  EXPECT_TRUE(MallocExtension::instance()->GetNumericProperty(
                "tcmalloc.current_total_thread_cache_bytes",
                &result));
  return result;
}

// Maximum cache size seen so far.
static size_t max_cache_size;

// Mutex and condition variable to synchronize threads.
std::mutex filler_mtx;
std::condition_variable filler_cv;
int current_thread = 0;

// A thread that cycles through allocating lots of objects of varying
// size, in an attempt to fill up its thread cache.
void Filler(int thread_id, int num_threads) {
  std::unique_lock<std::mutex> filler_lock(filler_mtx);
  for (int i = 0; i < kNumPasses; i++) {
    // Wait for the current thread to be the one that should run.
    filler_cv.wait(filler_lock, [thread_id] { return thread_id == current_thread; });

    // Fill the cache by allocating and deallocating objects of varying sizes.
    int size = 0;
    for (int i = 0; i < kAllocationsPerThread; i++) {
      void* p = ::operator new(size);
      ::operator delete(p);
      size += 64;
      if (size > (32 << 10)) size = 0;
    }

    // Get the maximum cache size seen so far.
    const size_t cache_size = CurrentThreadCacheSize();
    max_cache_size = std::max(max_cache_size, cache_size);

    // Move to the next thread.
    current_thread = (current_thread + 1) % num_threads;
    filler_cv.notify_all();
  }
}

TEST(MinPerThreadCacheSizeTest, Basics) {
  // Start all threads.
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  // Set the lower bound on per cache size.
  CHECK(MallocExtension::instance()->SetNumericProperty(
        "tcmalloc.min_per_thread_cache_bytes", kPerThreadCacheSize));

  // Setting the max total thread cache size to 0 to ensure that the
  // per thread cache size is set to the lower bound.
  CHECK(MallocExtension::instance()->SetNumericProperty(
        "tcmalloc.max_total_thread_cache_bytes", 0));

  for (int i = 0; i < kNumThreads; i++) {
    threads.emplace_back(Filler, i, kNumThreads);
  }

  // Wait for all threads to finish.
  for (auto& t : threads) { t.join(); }

  // Check that the maximum cache size does not exceed the limit set.
  ASSERT_LT(max_cache_size, kPerThreadCacheSize * kNumThreads);
}

