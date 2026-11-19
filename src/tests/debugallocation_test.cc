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
// Author: Fred Akalin

#include "config_for_unittests.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>  // for memcmp

#include <new>

#include "gperftools/malloc_extension.h"
#include "gperftools/nallocx.h"
#include "gperftools/tcmalloc.h"

#include "base/basictypes.h"
#include "base/logging.h"
#include "testing_portal.h"
#include "tests/testutil.h"

#include "gtest/gtest.h"

using tcmalloc::TestingPortal;

// Test match as well as mismatch rules.  But do not test on OS X; on
// OS X the OS converts new/new[] to malloc before it gets to us, so
// we are unable to catch these mismatch errors.
#ifndef __APPLE__
TEST(DebugAllocationTest, DeallocMismatch) {
  // malloc can be matched only by free
  // new can be matched only by delete and delete(nothrow)
  // new[] can be matched only by delete[] and delete[](nothrow)
  // new(nothrow) can be matched only by delete and delete(nothrow)
  // new(nothrow)[] can be matched only by delete[] and delete[](nothrow)

  // Allocate with malloc.
  {
    int* x = static_cast<int*>(noopt(malloc(sizeof(*x))));
    EXPECT_DEATH(delete x, "mismatch.*being dealloc.*delete");
    EXPECT_DEATH(delete[] x, "mismatch.*being dealloc.*delete *[[]");
    // Should work fine.
    free(x);
  }

  // Allocate with new.
  {
    int* x = noopt(new int);
    int* y = noopt(new int);
    EXPECT_DEATH(free(x), "mismatch.*being dealloc.*free");
    EXPECT_DEATH(delete[] x, "mismatch.*being dealloc.*delete *[[]");
    delete x;
    ::operator delete(y, std::nothrow);
  }

  // Allocate with new[].
  {
    int* x = noopt(new int[1]);
    int* y = noopt(new int[1]);
    EXPECT_DEATH(free(x), "mismatch.*being dealloc.*free");
    EXPECT_DEATH(delete x, "mismatch.*being dealloc.*delete");
    delete[] x;
    ::operator delete[](y, std::nothrow);
  }

  // Allocate with new(nothrow).
  {
    int* x = noopt(new (std::nothrow) int);
    int* y = noopt(new (std::nothrow) int);
    EXPECT_DEATH(free(x), "mismatch.*being dealloc.*free");
    EXPECT_DEATH(delete[] x, "mismatch.*being dealloc.*delete *[[]");
    delete x;
    ::operator delete(y, std::nothrow);
  }

  // Allocate with new(nothrow)[].
  {
    int* x = noopt(new (std::nothrow) int[1]);
    int* y = noopt(new (std::nothrow) int[1]);
    EXPECT_DEATH(free(x), "mismatch.*being dealloc.*free");
    EXPECT_DEATH(delete x, "mismatch.*being dealloc.*delete");
    delete[] x;
    ::operator delete[](y, std::nothrow);
  }
}
#endif  // #ifdef OS_MACOSX

TEST(DebugAllocationTest, DoubleFree) {
  int* pint = noopt(new int);
  delete pint;
  EXPECT_DEATH(delete pint, "has been already deallocated");
}

TEST(DebugAllocationTest, StompBefore) {
  int* pint = noopt(new int);
  (void)pint;
  pint[-1] = 5;
  EXPECT_DEATH(delete pint, "a word before object");
}

TEST(DebugAllocationTest, StompAfter) {
  int* pint = noopt(new int);
  (void)pint;
  pint[1] = 5;
  EXPECT_DEATH(delete pint, "a word after object");
}

TEST(DebugAllocationTest, FreeQueueTest) {
  // Verify that the allocator doesn't return blocks that were recently freed.
  int* x = noopt(new int);
  int* old_x = x;
  delete x;
  x = noopt(new int);
#if 1
  // This check should not be read as a universal guarantee of behavior.  If
  // other threads are executing, it would be theoretically possible for this
  // check to fail despite the efforts of debugallocation.cc to the contrary.
  // It should always hold under the controlled conditions of this unittest,
  // however.
  EXPECT_NE(x, old_x);  // Allocator shouldn't return recently freed blocks
#else
  // The below check passes, but since it isn't *required* to pass, I've left
  // it commented out.
  // EXPECT_EQ(x, old_x);
#endif
  old_x = nullptr;  // avoid breaking opt build with an unused variable warning.
  delete x;
}

TEST(DebugAllocationTest, DanglingPointerWriteTest) {
  // This test can only be run if debugging.
  //
  // If not debugging, the 'new' following the dangling write might not be
  // safe.  When debugging, we expect the (trashed) deleted block to be on the
  // list of recently-freed blocks, so the following 'new' will be safe.
#if 1
  int* x = noopt(new int);
  delete x;
  int poisoned_x_value = *x;
  *x = 1;  // a dangling write.

  char* s = noopt(new char[TestingPortal::Get()->GetMaxFreeQueueSize()]);
  // When we delete s, we push the storage that was previously allocated to x
  // off the end of the free queue.  At that point, the write to that memory
  // will be detected.
  EXPECT_DEATH(delete[] s, "Memory was written to after being freed.");

  // restore the poisoned value of x so that we can delete s without causing a
  // crash.
  *x = poisoned_x_value;
  delete[] s;
#endif
}

TEST(DebugAllocationTest, DanglingWriteAtExitTest) {
  int* x = noopt(new int);
  delete x;
  int old_x_value = *x;
  *x = 1;
  // verify that dangling writes are caught at program termination if the
  // corrupted block never got pushed off of the end of the free queue.
  EXPECT_DEATH(exit(0), "Memory was written to after being freed.");
  *x = old_x_value;  // restore x so that the test can exit successfully.
}

namespace FooBar {
ATTRIBUTE_NOINLINE
void StackTraceMarker(int* x) {
  (::operator delete)(x);
  // prevent tail-call above
  noopt(x);
}
}  // namespace FooBar

TEST(DebugAllocationTest, StackTraceWithDanglingWriteAtExitTest) {
  int* x = noopt(new int);
  FooBar::StackTraceMarker(x);
  int old_x_value = *x;
  *x = 1;
  if (!getenv("TCM_DEBUG_BT_SYMBOLIZATION_TEST")) {
    // verify that we also get a stack trace when we have a dangling write.
    // The " @ " is part of the stack trace output.
    EXPECT_DEATH(exit(0), " @ .*FooBar::StackTraceMarker");
    *x = old_x_value;  // restore x so that the test can exit successfully.
  } else {
    exit(0);
  }
}

static size_t CurrentlyAllocatedBytes() {
  size_t value;
  CHECK(MallocExtension::instance()->GetNumericProperty("generic.current_allocated_bytes", &value));
  return value;
}

TEST(DebugAllocationTest, CurrentlyAllocated) {
  // Clear the free queue
#if 1
  TestingPortal::Get()->GetMaxFreeQueueSize() = 0;
  // Force a round-trip through the queue management code so that the
  // new size is seen and the queue of recently-freed blocks is flushed.
  free(noopt(malloc(1)));
  TestingPortal::Get()->GetMaxFreeQueueSize() = 1048576;
#endif

  // Free something and check that it disappears from allocated bytes
  // immediately.
  char* p = noopt(new char[1000]);
  size_t after_malloc = CurrentlyAllocatedBytes();
  delete[] p;
  size_t after_free = CurrentlyAllocatedBytes();
  EXPECT_LE(after_free, after_malloc - 1000);
}

TEST(DebugAllocationTest, GetAllocatedSizeTest) {
  // When debug_allocation is in effect, GetAllocatedSize should return
  // exactly requested size, since debug_allocation doesn't allow users
  // to write more than that.
  for (int i = 0; i < 10; ++i) {
#ifdef __APPLE__
    if (i == 0) continue;
#endif
    void* p = noopt(malloc(i));
    EXPECT_EQ(i, MallocExtension::instance()->GetAllocatedSize(p));
    free(p);

    p = tc_memalign(16, i);
    auto amount = MallocExtension::instance()->GetAllocatedSize(p);
    EXPECT_LE(i, amount);
    memset(p, 0xff, amount);
    tc_free(p);
  }

  void* a = noopt(malloc(1000));
  EXPECT_GE(MallocExtension::instance()->GetAllocatedSize(a), 1000);
  // This is just a sanity check.  If we allocated too much, alloc is broken
  EXPECT_LE(MallocExtension::instance()->GetAllocatedSize(a), 5000);
  free(a);

  EXPECT_GE(MallocExtension::instance()->GetEstimatedAllocatedSize(1000), 1000);

  a = tc_memalign(16, 1000);
  auto amount = MallocExtension::instance()->GetAllocatedSize(a);
  EXPECT_GE(amount, 1000);
  // This is just a sanity check.  If we allocated too much, alloc is broken
  EXPECT_LE(amount, 5000);
  memset(a, 0xff, amount);
  tc_free(a);
}

TEST(DebugAllocationTest, HugeAlloc) {
  // This must not be a const variable so it doesn't form an
  // integral-constant-expression which can be *statically* rejected by the
  // compiler as too large for the allocation.
  size_t kTooBig = ~static_cast<size_t>(0);
  void* a = nullptr;

  (void)kTooBig;
  (void)a;

  a = noopt(malloc(noopt(kTooBig)));
  EXPECT_EQ(nullptr, a);

  // kAlsoTooBig is small enough not to get caught by debugallocation's check,
  // but will still fall through to tcmalloc's check. This must also be
  // a non-const variable. See kTooBig for more details.
  size_t kAlsoTooBig = kTooBig - 1024;

  a = noopt(malloc(noopt(kAlsoTooBig)));
  EXPECT_EQ(nullptr, a);
}

// based on test program contributed by mikesart@gmail.com aka
// mikesart@valvesoftware.com. See issue-464.
// Debug allocator never lets users touch more than they asked
// for. Also for memalign-ed chunks (which have alignment slack).
TEST(DebugAllocationTest, MemalignUsableSizeIsUserSize) {
  for (size_t align : {16, 64, 4096, 1 << 16}) {
    for (size_t size : {1, 33, 1000, 100000}) {
      void* p = noopt(tc_memalign(align, size));
      ASSERT_NE(p, nullptr);
      EXPECT_EQ(MallocExtension::instance()->GetAllocatedSize(p), size) << align;
      EXPECT_EQ(tc_malloc_size(p), size) << align;
      free(p);
    }
  }
}

#ifdef ENABLE_SIZED_DELETE
// Sized deletes of aligned chunks are checked against the size user
// asked for.
TEST(DebugAllocationTest, SizedAlignedDelete) {
  for (size_t align : {16, 64, 4096, 1 << 16}) {
    std::align_val_t al{align};

    void* p = noopt((::operator new)(33, al));
    (::operator delete)(p, 33, al);

    p = noopt((::operator new[])(33, al));
    (::operator delete[])(p, 33, al);

    p = noopt(tc_memalign(align, 33));
    tc_free_aligned_sized(p, align, 33);
  }

  std::align_val_t al{64};
  void* p = noopt((::operator new)(33, al));
  EXPECT_DEATH((::operator delete)(p, 34, al), "right size must be passed to sized delete");
  (::operator delete)(p, 33, al);

  p = noopt((::operator new[])(33, al));
  EXPECT_DEATH((::operator delete[])(p, 32, al), "right size must be passed to sized delete");
  (::operator delete[])(p, 33, al);

  p = noopt(tc_memalign(64, 33));
  EXPECT_DEATH(tc_free_aligned_sized(p, 64, 34), "right size must be passed to sized delete");
  tc_free_aligned_sized(p, 64, 33);
}
#endif

TEST(DebugAllocationTest, ReallocAfterMemalign) {
  char stuff[50];
  memset(stuff, 0x11, sizeof(stuff));
  void* p = tc_memalign(16, sizeof(stuff));
  EXPECT_NE(p, nullptr);
  memcpy(stuff, p, sizeof(stuff));

  p = noopt(realloc(p, sizeof(stuff) + 10));
  EXPECT_NE(p, nullptr);

  int rv = memcmp(stuff, p, sizeof(stuff));
  EXPECT_EQ(rv, 0);
}

static bool IsAligned(const void* p, size_t align) { return (reinterpret_cast<uintptr_t>(p) & (align - 1)) == 0; }

TEST(DebugAllocationTest, MemalignLargeAlignment) {
  for (size_t align : {size_t{1} << 16, size_t{1} << 20}) {
    char stuff[100];
    for (int i = 0; i < sizeof(stuff); i++) {
      stuff[i] = i;
    }

    void* p = tc_memalign(align, sizeof(stuff));
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(IsAligned(p, align));
    EXPECT_GE(MallocExtension::instance()->GetAllocatedSize(p), sizeof(stuff));
    EXPECT_EQ(MallocExtension::instance()->GetOwnership(p), MallocExtension::kOwned);
    EXPECT_TRUE(MallocExtension::instance()->VerifyMallocMemory(p));
    memcpy(p, stuff, sizeof(stuff));

    p = noopt(realloc(p, 2 * sizeof(stuff)));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(memcmp(p, stuff, sizeof(stuff)), 0);
    free(p);

    p = tc_memalign(align, sizeof(stuff));
    EXPECT_TRUE(IsAligned(p, align));
    free(p);

    ASSERT_EQ(tc_posix_memalign(&p, align, sizeof(stuff)), 0);
    EXPECT_TRUE(IsAligned(p, align));
    tc_free_aligned_sized(p, align, sizeof(stuff));
  }
}

TEST(DebugAllocationTest, NAllocXMatchesUsableSize) {
  for (size_t size = 0; size < 100000; size = size * 3 + 1) {
#ifdef __APPLE__
    if (size == 0) continue;
#endif
    void* p = noopt(malloc(size));
    EXPECT_EQ(nallocx(size, 0), MallocExtension::instance()->GetAllocatedSize(p));
    EXPECT_EQ(nallocx(size, 0), tc_malloc_size(p));
    EXPECT_EQ(MallocExtension::instance()->GetEstimatedAllocatedSize(size), nallocx(size, 0));
    free(p);
  }
}

TEST(DebugAllocationTest, CallocKeepsRedzones) {
  for (size_t n = 0; n < 300; n += 7) {
    char* p = static_cast<char*>(noopt(calloc(1, n)));
    for (size_t i = 0; i < n; i++) {
      ASSERT_EQ(p[i], 0);
    }
    // We'd crash with "a word after object" if calloc zeroed past
    // requested size.
    EXPECT_TRUE(MallocExtension::instance()->VerifyMallocMemory(p));
    free(p);
  }
}

#if __cpp_exceptions
static int new_handler_calls;
static int new_handler_calls_before_giving_up;

static void CountingNewHandler() {
  new_handler_calls++;
  if (new_handler_calls >= new_handler_calls_before_giving_up) {
    std::set_new_handler(nullptr);
  }
}

TEST(DebugAllocationTest, OOMPolicy) {
  // Near SIZE_MAX so it can never succeed, but with enough room that
  // debug allocator's overhead doesn't trip its own overflow checks
  // (so failure comes from base).
  size_t kTooBig = noopt(~size_t{0} - 100000);

  std::new_handler old_handler = std::set_new_handler(nullptr);

  errno = 0;
  EXPECT_EQ(noopt(malloc(kTooBig)), nullptr);
  EXPECT_EQ(errno, ENOMEM);
  EXPECT_EQ(noopt(calloc(1, kTooBig)), nullptr);
  void* p = noopt(malloc(10));
  EXPECT_EQ(noopt(realloc(p, kTooBig)), nullptr);
  free(p);

  // operator new retries after new_handler returns, then throws.
  new_handler_calls = 0;
  new_handler_calls_before_giving_up = 3;
  std::set_new_handler(CountingNewHandler);
  EXPECT_THROW(noopt((::operator new)(kTooBig)), std::bad_alloc);
  EXPECT_EQ(new_handler_calls, 3);

  new_handler_calls = 0;
  new_handler_calls_before_giving_up = 2;
  std::set_new_handler(CountingNewHandler);
  EXPECT_THROW(noopt((::operator new[])(kTooBig, std::align_val_t{64})), std::bad_alloc);
  EXPECT_EQ(new_handler_calls, 2);

  new_handler_calls = 0;
  new_handler_calls_before_giving_up = 1;
  std::set_new_handler(CountingNewHandler);
  EXPECT_EQ(noopt((::operator new)(kTooBig, std::nothrow)), nullptr);
  EXPECT_EQ(new_handler_calls, 1);

  // malloc calls new_handler only in "new mode".
  new_handler_calls = 0;
  new_handler_calls_before_giving_up = 1;
  std::set_new_handler(CountingNewHandler);
  EXPECT_EQ(noopt(malloc(kTooBig)), nullptr);
  EXPECT_EQ(new_handler_calls, 0);
  int old_mode = tc_set_new_mode(1);
  EXPECT_EQ(noopt(malloc(kTooBig)), nullptr);
  EXPECT_EQ(new_handler_calls, 1);
  tc_set_new_mode(old_mode);

  std::set_new_handler(old_handler);
}
#endif  // __cpp_exceptions

int main(int argc, char** argv) {
#if __APPLE__
  // OSX needs dsymutil crap
  std::string cmd = "dsymutil ";
  cmd += argv[0];
  int result = system(cmd.c_str());
  printf("OSX-specific '%s' -> %d\n", cmd.c_str(), result);
#endif

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
