/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "interval-map.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace perf_convert {
namespace {

using IM = IntervalMap<int>;

std::vector<std::tuple<uint64_t, uint64_t, int>> Dump(const IM& m) {
  std::vector<std::tuple<uint64_t, uint64_t, int>> out;
  for (const auto& [start, e] : m) out.emplace_back(start, e.end, e.value);
  return out;
}

TEST(IntervalMap, DisjointInsertsCoexist) {
  IM m;
  m.Insert(0, 10, 1);
  m.Insert(20, 30, 2);
  m.Insert(10, 20, 3);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 10, 1}, {10, 20, 3}, {20, 30, 2}}));
  EXPECT_EQ(*m.Find(5), 1);
  EXPECT_EQ(*m.Find(15), 3);
  EXPECT_EQ(*m.Find(29), 2);
  EXPECT_EQ(m.Find(30), nullptr);
  EXPECT_EQ(m.Find(100), nullptr);
}

TEST(IntervalMap, ExactOverlapReplaces) {
  IM m;
  m.Insert(100, 200, 1);
  m.Insert(100, 200, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{100, 200, 2}}));
}

TEST(IntervalMap, NewRangeInMiddleSplitsOld) {
  IM m;
  m.Insert(0, 100, 1);
  m.Insert(40, 60, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 40, 1}, {40, 60, 2}, {60, 100, 1}}));
  EXPECT_EQ(*m.Find(39), 1);
  EXPECT_EQ(*m.Find(40), 2);
  EXPECT_EQ(*m.Find(59), 2);
  EXPECT_EQ(*m.Find(60), 1);
}

TEST(IntervalMap, OverlapsStartOfExisting) {
  IM m;
  m.Insert(50, 150, 1);
  m.Insert(0, 100, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 100, 2}, {100, 150, 1}}));
}

TEST(IntervalMap, OverlapsEndOfExisting) {
  IM m;
  m.Insert(0, 100, 1);
  m.Insert(50, 200, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 50, 1}, {50, 200, 2}}));
}

TEST(IntervalMap, CoversSeveralExisting) {
  IM m;
  m.Insert(0, 10, 1);
  m.Insert(10, 20, 2);
  m.Insert(20, 30, 3);
  m.Insert(40, 50, 4);
  m.Insert(5, 45, 9);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 5, 1}, {5, 45, 9}, {45, 50, 4}}));
}

TEST(IntervalMap, IdenticalStartLongerNew) {
  IM m;
  m.Insert(0, 10, 1);
  m.Insert(0, 30, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 30, 2}}));
}

TEST(IntervalMap, IdenticalStartShorterNewSplitsTail) {
  IM m;
  m.Insert(0, 30, 1);
  m.Insert(0, 10, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 10, 2}, {10, 30, 1}}));
}

TEST(IntervalMap, SharedEndClipsFront) {
  IM m;
  m.Insert(0, 30, 1);
  m.Insert(20, 30, 2);
  EXPECT_EQ(Dump(m), (std::vector<std::tuple<uint64_t, uint64_t, int>>{{0, 20, 1}, {20, 30, 2}}));
}

TEST(IntervalMap, EmptyRangeIgnored) {
  IM m;
  m.Insert(10, 10, 1);
  m.Insert(20, 5, 1);
  EXPECT_TRUE(m.empty());
}

TEST(IntervalMap, NonOwningValueType) {
  IntervalMap<std::string> m;
  m.Insert(0, 10, "a");
  m.Insert(3, 6, "b");
  EXPECT_EQ(*m.Find(2), "a");
  EXPECT_EQ(*m.Find(4), "b");
  EXPECT_EQ(*m.Find(7), "a");
}

}  // namespace
}  // namespace perf_convert
