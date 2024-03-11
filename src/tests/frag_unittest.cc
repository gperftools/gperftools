// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2003, Google Inc.
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
// Test speed of handling fragmented heap

#include "config_for_unittests.h"

#include <gperftools/malloc_extension.h>

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <memory>
#include <optional>
#include <vector>

#include "testing_portal.h"
#include "tests/testutil.h"

#include "gtest/gtest.h"

using tcmalloc::TestingPortal;

static double GetCPUTime() {
  // Note, we do plain wall-clock time instead of cpu time, but this
  // is close enough for this file's purpose.
  return clock() / static_cast<double>(CLOCKS_PER_SEC);
}

static std::optional<size_t> GetSlackBytes() {
  size_t slack;
  if (!MallocExtension::instance()->GetNumericProperty(
        "tcmalloc.slack_bytes",
        &slack)) {
    return {};
  }
  return slack;
}

TEST(FragTest, Slack) {
  TestingPortal* portal = TestingPortal::Get();

  // Make kAllocSize one page larger than the maximum small object size.
  const int kAllocSize = portal->GetMaxSize() + portal->GetPageSize();
  // Allocate 400MB in total.
  const int kTotalAlloc = 400 << 20;
  const int kAllocIterations = kTotalAlloc / kAllocSize;

  // Allocate lots of objects
  std::vector<std::unique_ptr<char[]>> saved;
  saved.reserve(kAllocIterations);
  for (int i = 0; i < kAllocIterations; i++) {
    saved.emplace_back(noopt(new char[kAllocSize]));
  }

  // Check the current "slack".
  size_t slack_before = GetSlackBytes().value();

  // Free alternating ones to fragment heap
  size_t free_bytes = 0;
  for (int i = 0; i < saved.size(); i += 2) {
    saved[i].reset();
    free_bytes += kAllocSize;
  }

  // Check that slack delta is within 10% of expected.
  size_t slack_after = GetSlackBytes().value();

  ASSERT_GE(slack_after, slack_before);
  size_t slack = slack_after - slack_before;

  ASSERT_GT(double(slack), 0.9*free_bytes);
  ASSERT_LT(double(slack), 1.1*free_bytes);

  // Dump malloc stats
  static const int kBufSize = 1<<20;
  char* buffer = new char[kBufSize];
  MallocExtension::instance()->GetStats(buffer, kBufSize);
  puts(buffer);
  delete[] buffer;

  // Now do timing tests
  for (int i = 0; i < 5; i++) {
    static constexpr int kIterations = 100000;
    double start = GetCPUTime();

    for (int i = 0; i < kIterations; i++) {
      size_t s = GetSlackBytes().value();
      (void)s;
    }

    double end = GetCPUTime();
    fprintf(stderr, "getproperty: %6.1f ns/call\n",
            (end-start) * 1e9 / kIterations);
  }
}
