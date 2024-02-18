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
#include "config_for_unittests.h"

#include "pagemap.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <random>
#include <vector>

#include "gtest/gtest.h"

// Note: we leak memory every time a map is constructed, so do not
// create too many maps.

// Test specified map type
template <class Type>
void TestMap(int limit, bool limit_is_below_the_overflow_boundary) {
  printf("Running test with %d iterations...\n", limit);

  { // Test sequential ensure/assignment
    Type map(malloc);
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      map.Ensure(i, 1);
      map.set(i, (void*)(i+1));
      ASSERT_EQ(map.get(i), (void*)(i+1));
    }
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      ASSERT_EQ(map.get(i), (void*)(i+1));
    }
  }

  { // Test bulk Ensure
    Type map(malloc);
    map.Ensure(0, limit);
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      map.set(i, (void*)(i+1));
      ASSERT_EQ(map.get(i), (void*)(i+1));
    }
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      ASSERT_EQ(map.get(i), (void*)(i+1));
    }
  }

  // Test that we correctly notice overflow
  {
    Type map(malloc);
    ASSERT_EQ(map.Ensure(limit, limit+1), limit_is_below_the_overflow_boundary);
  }

  { // Test randomized accesses
    std::vector<intptr_t> elements;
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) elements.push_back(i);
    std::shuffle(elements.begin(), elements.end(), std::mt19937(42));

    Type map(malloc);
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      map.Ensure(elements[i], 1);
      map.set(elements[i], (void*)(elements[i]+1));
      ASSERT_EQ(map.get(elements[i]), (void*)(elements[i]+1));
    }
    for (intptr_t i = 0; i < static_cast<intptr_t>(limit); i++) {
      ASSERT_EQ(map.get(i), (void*)(i+1));
    }
  }
}

// REQUIRES: BITS==10, i.e., valid range is [0,1023].
// Representations for different types will end up being:
//    PageMap1: array[1024]
//    PageMap2: array[32][32]
//    PageMap3: array[16][16][4]
template <class Type>
void TestNext(const char* name) {
  printf("Running NextTest %s\n", name);
  Type map(malloc);
  char a, b, c, d, e;

  // When map is empty
  ASSERT_EQ(map.Next(0), nullptr);
  ASSERT_EQ(map.Next(5), nullptr);
  ASSERT_EQ(map.Next(1<<30), nullptr);

  // Add a single value
  map.Ensure(40, 1);
  map.set(40, &a);
  ASSERT_EQ(map.Next(0), &a);
  ASSERT_EQ(map.Next(39), &a);
  ASSERT_EQ(map.Next(40), &a);
  ASSERT_EQ(map.Next(41), nullptr);
  ASSERT_EQ(map.Next(1<<30), nullptr);

  // Add a few values
  map.Ensure(41, 1);
  map.Ensure(100, 3);
  map.set(41, &b);
  map.set(100, &c);
  map.set(101, &d);
  map.set(102, &e);
  ASSERT_EQ(map.Next(0), &a);
  ASSERT_EQ(map.Next(39), &a);
  ASSERT_EQ(map.Next(40), &a);
  ASSERT_EQ(map.Next(41), &b);
  ASSERT_EQ(map.Next(42), &c);
  ASSERT_EQ(map.Next(63), &c);
  ASSERT_EQ(map.Next(64), &c);
  ASSERT_EQ(map.Next(65), &c);
  ASSERT_EQ(map.Next(99), &c);
  ASSERT_EQ(map.Next(100), &c);
  ASSERT_EQ(map.Next(101), &d);
  ASSERT_EQ(map.Next(102), &e);
  ASSERT_EQ(map.Next(103), nullptr);
}

TEST(PageMapTest, Everything) {
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap1<10>>(100, true));
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap1<10>>(1 << 10, false));
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap2<20>>(100, true));
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap2<20>>(1 << 20, false));
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap3<20>>(100, true));
  ASSERT_NO_FATAL_FAILURE(TestMap<TCMalloc_PageMap3<20>>(1 << 20, false));

  ASSERT_NO_FATAL_FAILURE(TestNext<TCMalloc_PageMap1<10>>("PageMap1"));
  ASSERT_NO_FATAL_FAILURE(TestNext<TCMalloc_PageMap2<10>>("PageMap2"));
  ASSERT_NO_FATAL_FAILURE(TestNext<TCMalloc_PageMap3<10>>("PageMap3"));
}
