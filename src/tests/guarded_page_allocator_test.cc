// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2018, Google Inc.
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
// Author: Matt Morehouse

#if defined(__GNUC__) && defined(__linux__)

#include "guarded_page_allocator.h"

#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <set>
#include <thread>

#include "base/logging.h"
#include "common.h"

namespace {

int test_to_run = 0;
int test_counter = 0;
#define EXPECT_DEATH(statement, regex)                   \
  do {                                                   \
    if (test_counter++ == test_to_run) {                 \
      fprintf(stderr, "EXPECT_DEATH(%s)\n", #statement); \
      if (*regex != '\0')                                \
        fprintf(stderr, "Expected regex:%s\n", regex);   \
      else                                               \
        fprintf(stderr, "No regex\n");                   \
      statement;                                         \
    }                                                    \
  } while (false)

constexpr size_t kMaxGpaPages = tcmalloc::GuardedPageAllocator::kGpaMaxPages;
volatile char sink;  // Used to avoid optimization in several tests.

// Size of pages used by GuardedPageAllocator.
size_t PageSize() {
  static const size_t page_size =
      std::max(kPageSize, static_cast<size_t>(getpagesize()));
  return page_size;
}

struct GpaWrapper {
  explicit GpaWrapper(size_t num_pages) {
    gpa_.Init(num_pages);
    gpa_.AllowAllocations();
  }

  GpaWrapper() {
    gpa_.Init(kMaxGpaPages);
    gpa_.AllowAllocations();
  }

  ~GpaWrapper() { gpa_.Destroy(); }

  tcmalloc::GuardedPageAllocator gpa_;
};

void GpaSingleAllocDealloc() {
  GpaWrapper w;
  char *buf = reinterpret_cast<char *>(w.gpa_.Allocate(PageSize()));
  EXPECT_NE(buf, nullptr);
  EXPECT_TRUE(w.gpa_.PointerIsMine(buf));
  memset(buf, 'A', PageSize());
  EXPECT_DEATH(buf[-1] = 'A', "");
  EXPECT_DEATH(buf[PageSize()] = 'A', "");
  w.gpa_.Deallocate(buf);
  EXPECT_DEATH(buf[0] = 'B', "");
  EXPECT_DEATH(buf[PageSize() / 2] = 'B', "");
  EXPECT_DEATH(buf[PageSize() - 1] = 'B', "");
}

void GpaAllocDeallocAllPages(size_t num_pages) {
  GpaWrapper w(num_pages);
  char *bufs[kMaxGpaPages];
  for (size_t i = 0; i < num_pages; i++) {
    bufs[i] = reinterpret_cast<char *>(w.gpa_.Allocate(1));
    EXPECT_NE(bufs[i], nullptr);
    EXPECT_TRUE(w.gpa_.PointerIsMine(bufs[i]));
  }
  EXPECT_EQ(w.gpa_.Allocate(1), nullptr);
  w.gpa_.Deallocate(bufs[0]);
  bufs[0] = reinterpret_cast<char *>(w.gpa_.Allocate(1));
  EXPECT_NE(bufs[0], nullptr);
  EXPECT_TRUE(w.gpa_.PointerIsMine(bufs[0]));
  for (size_t i = 0; i < num_pages; i++) {
    bufs[i][0] = 'A';
    EXPECT_DEATH(bufs[i][-1] = 'A', "");
    EXPECT_DEATH(bufs[i][PageSize()] = 'A', "");
    w.gpa_.Deallocate(bufs[i]);
    EXPECT_DEATH(bufs[i][0] = 'B', "");
    EXPECT_DEATH(sink = bufs[i][0], "");
  }
}

void GpaPointerIsMine() {
  GpaWrapper w;
  void *buf = w.gpa_.Allocate(1);
  int stack_var;
  char *malloc_ptr = new char;
  EXPECT_TRUE(w.gpa_.PointerIsMine(buf));
  EXPECT_FALSE(w.gpa_.PointerIsMine(&stack_var));
  EXPECT_FALSE(w.gpa_.PointerIsMine(malloc_ptr));
  delete malloc_ptr;
}

// Test that no pages are double-allocated or left unallocated, and that no
// extra pages are allocated when there's concurrent calls to Allocate().
void GpaThreadedAllocCount() {
  GpaWrapper w;
  constexpr size_t num_threads = 2;
  void *allocations[num_threads][kMaxGpaPages];
  std::thread threads[num_threads];
  for (size_t i = 0; i < num_threads; i++) {
    threads[i] = std::thread([&w, &allocations, i]() {
      for (size_t j = 0; j < kMaxGpaPages; j++) {
        allocations[i][j] = w.gpa_.Allocate(1);
      }
    });
  }
  std::set<void *> allocations_set;
  for (size_t i = 0; i < num_threads; i++) {
    threads[i].join();
    for (size_t j = 0; j < kMaxGpaPages; j++) {
      allocations_set.insert(allocations[i][j]);
    }
  }
  allocations_set.erase(nullptr);
  EXPECT_EQ(allocations_set.size(), kMaxGpaPages);
}

// Test that allocator remains in consistent state under high contention and
// doesn't double-allocate pages or fail to deallocate pages.
void GpaThreadedHighContention() {
  GpaWrapper w;
  constexpr size_t num_threads = 1000;
  std::thread threads[num_threads];
  for (size_t i = 0; i < num_threads; i++) {
    threads[i] = std::thread([&w]() {
      auto Sleep = []() {
        const struct timespec sleep_time = {0, 5000};
        nanosleep(&sleep_time, nullptr);
      };
      char *buf;
      while ((buf = reinterpret_cast<char *>(w.gpa_.Allocate(1))) == nullptr) {
        Sleep();
      }

      // Verify that no other thread has access to this page.
      EXPECT_EQ(buf[0], 0);

      // Mark this page and allow some time for another thread to potentially
      // gain access to this page.
      buf[0] = 'A';
      Sleep();

      // Unmark this page and deallocate.
      buf[0] = 0;
      w.gpa_.Deallocate(buf);
    });
  }
  for (size_t i = 0; i < num_threads; i++) {
    threads[i].join();
  }
  for (size_t i = 0; i < kMaxGpaPages; i++) {
    EXPECT_NE(w.gpa_.Allocate(1), nullptr);
  }
}

void TcMallocUnderflowReadDetected() {
  auto RepeatUnderflowRead = []() {
    for (int i = 0; i < 1000000; i++) {
      char *volatile sink_buf = new char[kPageSize];
      sink = sink_buf[-1];
      delete[] sink_buf;
    }
  };
  EXPECT_DEATH(RepeatUnderflowRead(), "Buffer underflow occurs at");
}

void TcMallocOverflowReadDetected() {
  auto RepeatOverflowRead = []() {
    for (int i = 0; i < 1000000; i++) {
      char *volatile sink_buf = new char[kPageSize];
      sink = sink_buf[PageSize()];
      delete[] sink_buf;
    }
  };
  EXPECT_DEATH(RepeatOverflowRead(), "Buffer overflow occurs at");
}

void TcMallocUseAfterFreeDetected() {
  auto RepeatUseAfterFree = []() {
    for (int i = 0; i < 1000000; i++) {
      char *volatile sink_buf = new char[kPageSize];
      delete[] sink_buf;
      sink = sink_buf[0];
    }
  };
  EXPECT_DEATH(RepeatUseAfterFree(), "Use-after-free occurs at");
}

void __attribute__((noinline)) UseAfterFree() {
  char *volatile sink_buf = new char[kPageSize];
  delete[] sink_buf;
  sink = sink_buf[0];
}
void __attribute__((noinline)) ValidUse() {
  char *volatile sink_buf = new char[kPageSize];
  sink = sink_buf[0];
  delete[] sink_buf;
}
// Verify that the error report gives stack traces correctly showing the
// use-after-free in UseAfterFree() rather than in ValidUse().
void TcMallocStackTraceCorrect() {
  auto RepeatValidUseAndUseAfterFree = []() {
    for (int i = 0; i < 1000000; i++) {
      ValidUse();
      ValidUse();
      UseAfterFree();
      ValidUse();
    }
  };
  EXPECT_DEATH(RepeatValidUseAndUseAfterFree(),
               "Error originates from memory allocated at:\\\\n.*\\\\n?.*\\\\n?"
               ".*\\\\n?.*@.*UseAfterFree");
  EXPECT_DEATH(RepeatValidUseAndUseAfterFree(),
               "The memory was freed at:\\\\n.*\\\\n?.*\\\\n?\\\\n?"
               ".*@.*UseAfterFree");
}

}  // namespace

int main(int argc, char *argv[]) {
  ASSERT_TRUE(argc > 1);
  test_to_run = atoi(argv[1]);

  GpaSingleAllocDealloc();
  GpaAllocDeallocAllPages(1);
  GpaAllocDeallocAllPages(kMaxGpaPages / 2);
  GpaAllocDeallocAllPages(kMaxGpaPages);
  GpaPointerIsMine();
  GpaThreadedAllocCount();
  GpaThreadedHighContention();

  TcMallocUnderflowReadDetected();
  TcMallocOverflowReadDetected();
  TcMallocUseAfterFreeDetected();
  TcMallocStackTraceCorrect();

  fprintf(stderr, "DONE\n");
  return 0;
}

#else
int main(int argc, char *argv[]) {
  fprintf(stderr, "DONE\n");
  return 0;
}
#endif  // defined(__GNUC__) && defined(__linux__)
