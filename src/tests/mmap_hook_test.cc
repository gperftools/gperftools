/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2023, gperftools Contributors
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
#undef NDEBUG
#include "config.h"

// When we end up running this on 32-bit glibc/uclibc/bionic system,
// lets ask for 64-bit off_t (only for stuff like lseek and ftruncate
// below). But ***only*** for this file.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "config_for_unittests.h"

#include "mmap_hook.h"

#include "base/function_ref.h"
#include "gperftools/stacktrace.h"

#include "tests/testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gtest/gtest.h"

static bool got_first_allocation;

extern "C" int MallocHook_InitAtFirstAllocation_HeapLeakChecker() {
#ifndef __FreeBSD__
  // Effing, FreeBSD. Super-annoying with broken everything when it is
  // early.
  printf("first mmap!\n");
#endif
  if (got_first_allocation) {
    abort();
  }
  got_first_allocation = true;
  return 1;
}

#ifdef HAVE_MMAP

#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

static_assert(sizeof(off_t) == sizeof(int64_t), "");

class MMapHookTest : public ::testing::Test {
public:
  static void HandleMappingEvent(const tcmalloc::MappingEvent& evt) {
    memcpy(&last_evt_, &evt, sizeof(evt));
    have_last_evt_ = true;
    if (evt.stack_depth > 0) {
      backtrace_address_ = evt.stack[0];
    }
  }

  void SetUp() {
    have_last_evt_ = false;
    backtrace_address_ = nullptr;
  }

  static void SetUpTestSuite() {
    tcmalloc::HookMMapEventsWithBacktrace(&hook_space_, &HandleMappingEvent,
                                          [] (const tcmalloc::MappingEvent& evt) {
                                            return 1;
                                          });
  }
  static void TearDownTestSuite() {
    tcmalloc::UnHookMMapEvents(&hook_space_);
  }

protected:
  static inline tcmalloc::MappingEvent last_evt_;
  static inline void* backtrace_address_;
  static inline bool have_last_evt_;
  static inline tcmalloc::MappingHookSpace hook_space_;
};

TEST_F(MMapHookTest, MMap) {
  if (!tcmalloc::mmap_hook_works) {
    puts("mmap test SKIPPED");
    return;
  }

  FILE* f = tmpfile();
  ASSERT_NE(f, nullptr) << "errno: " << strerror(errno);

  int fd = fileno(f);

  ASSERT_GE(ftruncate(fd, off_t{1} << 40), 0) << "errno: " << strerror(errno);

  int pagesz = getpagesize();

  off_t test_off = (off_t{1} << 40) - pagesz * 2;
  ASSERT_EQ(lseek(fd, -pagesz * 2, SEEK_END), test_off) << "errno: " << strerror(errno);

  static constexpr char contents[] = "foobarXYZ";

  ASSERT_EQ(write(fd, contents, sizeof(contents)), sizeof(contents)) << "errno: " << strerror(errno);

  have_last_evt_ = false;
  char* mm_addr = static_cast<char*>(mmap(nullptr, pagesz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, test_off));
  ASSERT_EQ(memcmp(mm_addr, contents, sizeof(contents)), 0);

  ASSERT_TRUE(have_last_evt_ && !last_evt_.before_valid && last_evt_.after_valid && last_evt_.file_valid);
  ASSERT_EQ(last_evt_.after_address, mm_addr);
  ASSERT_EQ(last_evt_.after_length, pagesz);
  ASSERT_EQ(last_evt_.file_fd, fd);
  ASSERT_EQ(last_evt_.file_off, test_off);
  ASSERT_EQ(last_evt_.flags, MAP_SHARED);
  ASSERT_EQ(last_evt_.prot, (PROT_READ|PROT_WRITE));

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());

  ASSERT_TRUE(got_first_allocation);

#ifdef __linux__
  void* reserve = mmap(nullptr, pagesz * 2, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(reserve, MAP_FAILED) << "errno: " << strerror(errno);
  ASSERT_TRUE(have_last_evt_);

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());

  char* new_addr = static_cast<char*>(mremap(mm_addr, pagesz,
                                             pagesz * 2, MREMAP_MAYMOVE | MREMAP_FIXED,
                                             reserve));
  ASSERT_NE(new_addr, MAP_FAILED);
  ASSERT_EQ(new_addr, reserve);
  ASSERT_TRUE(have_last_evt_);

  ASSERT_TRUE(!last_evt_.is_sbrk && last_evt_.after_valid && last_evt_.before_valid && !last_evt_.file_valid);
  ASSERT_EQ(last_evt_.after_address, new_addr);
  ASSERT_EQ(last_evt_.after_length, pagesz * 2);
  ASSERT_EQ(last_evt_.before_address, mm_addr);
  ASSERT_EQ(last_evt_.before_length, pagesz);

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());

  ASSERT_EQ(pwrite(fd, contents, sizeof(contents), test_off + pagesz + 1), sizeof(contents)) << "errno: " << strerror(errno);
  mm_addr = new_addr;

  ASSERT_EQ(memcmp(mm_addr + pagesz + 1, contents, sizeof(contents)), 0);
  puts("mremap test PASS");
#endif

  ASSERT_GE(munmap(mm_addr, pagesz), 0) << "errno: " << strerror(errno);

  ASSERT_TRUE(have_last_evt_ && !last_evt_.is_sbrk && !last_evt_.after_valid && last_evt_.before_valid && !last_evt_.file_valid);
  ASSERT_EQ(last_evt_.before_address, mm_addr);
  ASSERT_EQ(last_evt_.before_length, pagesz);

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());

  size_t sz = 10 * pagesz;
  auto result = tcmalloc::DirectAnonMMap(/* invoke_hooks = */false, sz);
  ASSERT_TRUE(result.success) << "errno: " << strerror(errno);
  ASSERT_NE(result.addr, MAP_FAILED);
  ASSERT_FALSE(have_last_evt_);

  ASSERT_EQ(tcmalloc::DirectMUnMap(false, result.addr, sz), 0) << "errno: " << strerror(errno);

  sz = 13 * pagesz;
  result = tcmalloc::DirectAnonMMap(/* invoke_hooks = */true, sz);
  ASSERT_TRUE(result.success) << "errno: " << strerror(errno);
  ASSERT_NE(result.addr, MAP_FAILED);

  ASSERT_TRUE(have_last_evt_ && !last_evt_.is_sbrk && !last_evt_.before_valid && last_evt_.after_valid);
  ASSERT_EQ(last_evt_.after_address, result.addr);
  ASSERT_EQ(last_evt_.after_length, sz);

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());

  sz = sz - pagesz; // lets also check unmapping sub-segment of previously allocated one
  ASSERT_EQ(tcmalloc::DirectMUnMap(true, result.addr, sz), 0) << "errno: " << strerror(errno);
  ASSERT_TRUE(have_last_evt_ && !last_evt_.is_sbrk && last_evt_.before_valid && !last_evt_.after_valid);
  ASSERT_EQ(last_evt_.before_address, result.addr);
  ASSERT_EQ(last_evt_.before_length, sz);

  have_last_evt_ = false;
  ASSERT_FALSE(HasFatalFailure());
}

TEST_F(MMapHookTest, MMapBacktrace) {
  if (!tcmalloc::mmap_hook_works) {
    puts("mmap backtrace test SKIPPED");
    return;
  }

  using mmap_fn = void* (*)(void*, size_t, int, int, int, off_t);

  static void* expected_address;

  struct Helper {
    // noinline ensures that all trampoline invocations will call fn
    // with same return address (inside trampoline). We use that to
    // test backtrace accuracy.
    static ATTRIBUTE_NOINLINE
    void trampoline(void** res, mmap_fn fn) {
      *res = noopt(fn)(nullptr, getpagesize(), PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    }
    static void* prepare(void* hint, size_t sz, int prot, int flags, int fd, off_t off) {
      EXPECT_EQ(1, GetStackTrace(&expected_address, 1, 1));
      return nullptr;
    }
  };

  void* addr;
  Helper::trampoline(&addr, Helper::prepare);
  ASSERT_NE(nullptr, expected_address);
  ASSERT_EQ(nullptr, addr);

  Helper::trampoline(&addr, mmap);
  ASSERT_NE(nullptr, addr);
  ASSERT_EQ(backtrace_address_, expected_address);
}

#ifdef HAVE_SBRK

extern "C" void* tcmalloc_hooked_sbrk(intptr_t increment);

static bool sbrk_works = ([] () {
  void* result = tcmalloc_hooked_sbrk(8);
  return result != reinterpret_cast<void*>(intptr_t{-1});
}());

TEST_F(MMapHookTest, Sbrk) {
  if (!sbrk_works) {
    puts("sbrk test SKIPPED");
    return;
  }

  void* addr = tcmalloc_hooked_sbrk(8);

  ASSERT_TRUE(got_first_allocation);

  EXPECT_TRUE(last_evt_.is_sbrk);
  EXPECT_TRUE(!last_evt_.before_valid && !last_evt_.file_valid && last_evt_.after_valid);
  EXPECT_EQ(last_evt_.after_address, addr);
  EXPECT_EQ(last_evt_.after_length, 8);

  ASSERT_FALSE(HasFatalFailure());
  have_last_evt_ = false;

  void* addr2 = tcmalloc_hooked_sbrk(16);

  EXPECT_TRUE(last_evt_.is_sbrk);
  EXPECT_TRUE(!last_evt_.before_valid && !last_evt_.file_valid && last_evt_.after_valid);
  EXPECT_EQ(last_evt_.after_address, addr2);
  EXPECT_EQ(last_evt_.after_length, 16);

  ASSERT_FALSE(HasFatalFailure());
  have_last_evt_ = false;

  char* addr3 = static_cast<char*>(tcmalloc_hooked_sbrk(-13));

  EXPECT_TRUE(last_evt_.is_sbrk);
  EXPECT_TRUE(last_evt_.before_valid && !last_evt_.file_valid && !last_evt_.after_valid);
  EXPECT_EQ(last_evt_.before_address, addr3-13);
  EXPECT_EQ(last_evt_.before_length, 13);

  ASSERT_FALSE(HasFatalFailure());
  have_last_evt_ = false;
}

TEST_F(MMapHookTest, SbrkBacktrace) {
  if (!sbrk_works) {
    puts("sbrk backtrace test SKIPPED");
    return;
  }

  static void* expected_address;

  struct Helper {
    // noinline ensures that all trampoline invocations will call fn
    // with same return address (inside trampoline). We use that to
    // test backtrace accuracy.
    static ATTRIBUTE_NOINLINE
    void trampoline(void** res, void* (*fn)(intptr_t increment)) {
      *res = noopt(fn)(32);
    }
    static void* prepare(intptr_t increment) {
      EXPECT_EQ(1, GetStackTrace(&expected_address, 1, 1));
      return nullptr;
    }
  };

  void* addr;
  Helper::trampoline(&addr, Helper::prepare);
  ASSERT_NE(nullptr, expected_address);
  ASSERT_EQ(nullptr, addr);

  printf("expected_address: %p, &trampoline: %p\n",
         expected_address, reinterpret_cast<void*>(&Helper::trampoline));

  // Why cast? Because some OS-es define sbrk as accepting long.
  Helper::trampoline(&addr, reinterpret_cast<void*(*)(intptr_t)>(tcmalloc_hooked_sbrk));
  ASSERT_NE(nullptr, addr);
  ASSERT_EQ(backtrace_address_, expected_address);
}

#endif // HAVE_SBRK

#endif // HAVE_MMAP
