/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Exercises the `skip` argument of aw_backtrace: skipping N innermost
// frames must give the same addresses as a full capture with the first N
// entries dropped, and the returned count is the number of entries actually
// filled (i.e. after skipping), never more than max_frames.

#include "include/aw-backtrace/aw-backtrace.h"

#include <array>

#include "gtest/gtest.h"

namespace {

constexpr int kBufSize = 64;

// Two captures from the exact same point, so frame i of `skipped` should equal
// frame i+skip of `full`. NEVER_INLINE-ish: keep it out of line and deep enough
// that there is something to skip.
__attribute__((noinline)) void CaptureBoth(std::array<void*, kBufSize>* full, int* full_count,
                                           std::array<void*, kBufSize>* skipped, int* skipped_count, int skip) {
  *full_count = aw_backtrace(nullptr, full->data(), kBufSize, 0);
  *skipped_count = aw_backtrace(nullptr, skipped->data(), kBufSize, skip);
}

__attribute__((noinline)) void Level2(std::array<void*, kBufSize>* full, int* full_count,
                                      std::array<void*, kBufSize>* skipped, int* skipped_count, int skip) {
  CaptureBoth(full, full_count, skipped, skipped_count, skip);
  // Defeat tail-call optimization so this frame really is on the stack.
  asm volatile("" ::: "memory");
}

__attribute__((noinline)) void Level1(std::array<void*, kBufSize>* full, int* full_count,
                                      std::array<void*, kBufSize>* skipped, int* skipped_count, int skip) {
  Level2(full, full_count, skipped, skipped_count, skip);
  asm volatile("" ::: "memory");
}

TEST(SimpleBacktraceSkip, DropsInnermostFrames) {
  std::array<void*, kBufSize> full{};
  std::array<void*, kBufSize> skipped{};
  int full_count = 0;
  int skipped_count = 0;
  constexpr int kSkip = 2;

  Level1(&full, &full_count, &skipped, &skipped_count, kSkip);

  ASSERT_GT(full_count, kSkip + 2);
  EXPECT_EQ(skipped_count, full_count - kSkip);
  for (int i = 0; i < skipped_count; i++) {
    EXPECT_EQ(skipped[i], full[i + kSkip]) << "mismatch at i=" << i;
  }
}

TEST(SimpleBacktraceSkip, NegativeSkipTreatedAsZero) {
  std::array<void*, kBufSize> full{};
  std::array<void*, kBufSize> neg{};
  int full_count = 0;
  int neg_count = 0;

  Level1(&full, &full_count, &neg, &neg_count, -5);

  ASSERT_GT(full_count, 1);
  EXPECT_EQ(neg_count, full_count);
  // Frame 0 differs (the two captures happen at different call sites inside
  // CaptureBoth); everything below it is the shared caller chain.
  for (int i = 1; i < full_count; i++) {
    EXPECT_EQ(neg[i], full[i]) << "mismatch at i=" << i;
  }
}

TEST(SimpleBacktraceSkip, SkipPastEndYieldsZero) {
  std::array<void*, kBufSize> buf{};
  int count = aw_backtrace(nullptr, buf.data(), kBufSize, 100000);
  EXPECT_EQ(count, 0);
}

TEST(SimpleBacktraceSkip, NonPositiveMaxFramesYieldsZero) {
  void* buf[1];
  EXPECT_EQ(aw_backtrace(nullptr, buf, 0, 0), 0);
  EXPECT_EQ(aw_backtrace(nullptr, buf, -1, 3), 0);
}

TEST(SimpleBacktraceSkip, CountCappedByMaxFrames) {
  std::array<void*, kBufSize> full{};
  int full_count = aw_backtrace(nullptr, full.data(), kBufSize, 0);
  ASSERT_GT(full_count, 3);

  // Ask for only 3 (with a skip as well); must fill exactly 3, never more.
  std::array<void*, kBufSize> capped{};
  int capped_count = aw_backtrace(nullptr, capped.data(), 3, 1);
  EXPECT_EQ(capped_count, 3);
  for (int i = 0; i < capped_count; i++) {
    EXPECT_EQ(capped[i], full[i + 1]) << "mismatch at i=" << i;
  }
}

}  // namespace
