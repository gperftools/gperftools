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
// Author: Arun Sharma
#include "config_for_unittests.h"

#include "gperftools/malloc_extension.h"

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>

#include <algorithm>
#include <limits>

#include "base/cleanup.h"
#include "tests/testutil.h"

#include "gtest/gtest.h"

class TestSysAllocator : public SysAllocator {
public:
  // Was this allocator invoked at least once?
  bool invoked_ = false;

  TestSysAllocator(SysAllocator* prev) : SysAllocator(), prev_(prev) {}
  ~TestSysAllocator() override {}

  void* Alloc(size_t size, size_t *actual_size, size_t alignment) override {
    invoked_ = true;
    return prev_->Alloc(size, actual_size, alignment);
  }
private:
  SysAllocator* const prev_;
};

TEST(SystemAllocTest, GetsInvoked) {
  SysAllocator* prev = MallocExtension::instance()->GetSystemAllocator();
  tcmalloc::Cleanup restore_sys_allocator([prev] () {
    MallocExtension::instance()->SetSystemAllocator(prev);
  });

  // Note, normally SysAllocator instances cannot be destroyed, but
  // we're single-threaded isolated unit test. And we know what we're
  // doing.
  TestSysAllocator test_allocator{prev};
  MallocExtension::instance()->SetSystemAllocator(&test_allocator);

  // An allocation size that is likely to trigger the system allocator.
  char *p =  noopt(new char[20 << 20]);
  delete [] p;

  // Make sure that our allocator was invoked.
  ASSERT_TRUE(test_allocator.invoked_);
}

TEST(SystemAllocTest, RetryAfterFail) {
  // Check with the allocator still works after a failed allocation.
  //
  // There is no way to call malloc and guarantee it will fail.  malloc takes a
  // size_t parameter and the C++ standard does not constrain the size of
  // size_t.  For example, consider an implementation where size_t is 32 bits
  // and pointers are 64 bits.
  //
  // It is likely, though, that sizeof(size_t) == sizeof(void*).  In that case,
  // the first allocation here might succeed but the second allocation must
  // fail.
  //
  // If the second allocation succeeds, you will have to rewrite or
  // disable this test.
  // The weird parens are to avoid macro-expansion of 'max' on windows.
  constexpr size_t kHugeSize = std::numeric_limits<size_t>::max() / 2;
  void* p1 = noopt(malloc(kHugeSize));
  void* p2 = noopt(malloc(kHugeSize));
  ASSERT_EQ(p2, nullptr);

  free(p1);

  char* q = noopt(new char[1024]);
  delete [] q;
}
