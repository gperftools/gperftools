// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2006, Google Inc.
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

// A test for low_level_alloc.cc

#include "base/low_level_alloc.h"

#include <stdio.h>
#include <map>

#include "gtest/gtest.h"

using tcmalloc::LowLevelAlloc;

// a block of memory obtained from the allocator
struct BlockDesc {
  char *ptr;      // pointer to memory
  int len;        // number of bytes
  int fill;       // filled with data starting with this
};

// Check that the pattern placed in the block d
// by RandomizeBlockDesc is still there.
static void CheckBlockDesc(const BlockDesc &d) {
  for (int i = 0; i != d.len; i++) {
    ASSERT_TRUE((d.ptr[i] & 0xff) == ((d.fill + i) & 0xff));
  }
}

// Fill the block "*d" with a pattern
// starting with a random byte.
static void RandomizeBlockDesc(BlockDesc *d) {
  d->fill = rand() & 0xff;
  for (int i = 0; i != d->len; i++) {
    d->ptr[i] = (d->fill + i) & 0xff;
  }
}

class TestPagesAllocator : public LowLevelAlloc::PagesAllocator {
public:
  struct TestHeader {
    static inline constexpr uint32_t kMagic = 0x74e5ca8;

    const uint32_t magic = kMagic;
    const size_t size;
    TestHeader(size_t size) : size(size) {}
  };

  uint64_t uses_count{};
  uint64_t in_use{};

  ~TestPagesAllocator() override = default;

  std::pair<void *, size_t> MapPages(size_t size) override {
    auto memory = (::operator new)(size + sizeof(TestHeader));
    TestHeader* hdr = new (memory) TestHeader(size);
    uses_count++;
    in_use += size;
    return {hdr + 1, size};
  }

  void UnMapPages(void *addr, size_t size) override {
    TestHeader* hdr = reinterpret_cast<TestHeader*>(addr) - 1;
    ASSERT_TRUE(hdr->size == size);
    ASSERT_TRUE(hdr->magic == TestHeader::kMagic);
    in_use -= size;
    (::operator delete)(hdr, size + sizeof(TestHeader));
  }
};

// n times, toss a coin, and based on the outcome
// either allocate a new block or deallocate an old block.
// New blocks are placed in a map with a random key
// and initialized with RandomizeBlockDesc().
// If keys conflict, the older block is freed.
// Old blocks are always checked with CheckBlockDesc()
// before being freed.  At the end of the run,
// all remaining allocated blocks are freed.
// If use_new_arena is true, use a fresh arena, and then delete it.
static void ExerciseAllocator(bool use_new_arena, int n) {
  typedef std::map<int, BlockDesc> AllocMap;
  AllocMap allocated;
  AllocMap::iterator it;
  BlockDesc block_desc;
  int rnd;
  LowLevelAlloc::Arena *arena = 0;

  TestPagesAllocator test_allocator;

  if (use_new_arena) {
    arena = LowLevelAlloc::NewArenaWithCustomAlloc(&test_allocator);
  }
  for (int i = 0; i != n; i++) {
    if (i != 0 && i % 10000 == 0) {
      printf(".");
      fflush(stdout);
    }

    switch(rand() & 1) {      // toss a coin
    case 0:     // coin came up heads: add a block
      block_desc.len = rand() & 0x3fff;
      block_desc.ptr =
        reinterpret_cast<char *>(
                        arena == 0
                        ? LowLevelAlloc::Alloc(block_desc.len)
                        : LowLevelAlloc::AllocWithArena(block_desc.len, arena));
      RandomizeBlockDesc(&block_desc);
      rnd = rand();
      it = allocated.find(rnd);
      if (it != allocated.end()) {
        CheckBlockDesc(it->second);
        LowLevelAlloc::Free(it->second.ptr);
        it->second = block_desc;
      } else {
        allocated[rnd] = block_desc;
      }
      break;
    case 1:     // coin came up tails: remove a block
      it = allocated.begin();
      if (it != allocated.end()) {
        CheckBlockDesc(it->second);
        LowLevelAlloc::Free(it->second.ptr);
        allocated.erase(it);
      }
      break;
    }
  }
  // remove all remaniing blocks
  while ((it = allocated.begin()) != allocated.end()) {
    CheckBlockDesc(it->second);
    LowLevelAlloc::Free(it->second.ptr);
    allocated.erase(it);
  }
  if (use_new_arena) {
    ASSERT_GT(test_allocator.uses_count, 0);
    ASSERT_GT(test_allocator.in_use, 0);
    ASSERT_TRUE(LowLevelAlloc::DeleteArena(arena));
    ASSERT_EQ(test_allocator.in_use, 0);
  }
}

TEST(LowLevelAllocTest, Basic) {
  ExerciseAllocator(false, 50000);
  for (int i = 0; i < 8; i++) {
    ExerciseAllocator(true, 15000);
  }
}
