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

#include "guarded_page_allocator.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include "base/basictypes.h"
#include "base/googleinit.h"
#include "base/logging.h"
#include "base/spinlock.h"
#include "common.h"
#include "internal_logging.h"
#include "page_heap.h"
#include "sampler.h"
#include "static_vars.h"
#include "symbolize.h"

namespace tcmalloc {

void GuardedPageAllocator::Init(size_t num_pages) {
  ASSERT(num_pages > 0 && num_pages <= kFreePagesNumBits);
  num_pages_ = num_pages;

  // If the system page size is larger than kPageSize, we need to use the
  // system page size for this allocator since mprotect operates on full pages
  // only.  This case happens on PPC.
  page_size_ = std::max(kPageSize, static_cast<size_t>(getpagesize()));
  ASSERT(page_size_ % kPageSize == 0);

  rand_ = reinterpret_cast<uint64>(this);  // Initialize RNG seed.
  MapPages();
}

void GuardedPageAllocator::Destroy() {
  if (pages_base_addr_) {
    size_t len = pages_end_addr_ - pages_base_addr_;
    int err = munmap(reinterpret_cast<void *>(pages_base_addr_), len);
    ASSERT(err != -1);
    (void)err;
  }
}

void *GuardedPageAllocator::Allocate(size_t size) {
  ASSERT(size <= page_size_);
  if (!first_page_addr_) return nullptr;

  ssize_t free_slot = ReserveFreeSlot();
  if (free_slot == -1) return nullptr;  // All slots are reserved.

  void *free_page = reinterpret_cast<void *>(SlotToAddr(free_slot));
  int err = mprotect(free_page, page_size_, PROT_READ | PROT_WRITE);
  ASSERT(err != -1);
  if (err == -1) {
    FreeSlot(free_slot);
    return nullptr;
  }

  // Record stack trace.
  SlotMetadata &d = data_[free_slot];
  d.dealloc_trace.depth = 0;
  d.alloc_trace.depth = GetStackTrace(d.alloc_trace.stack, kMaxStackDepth,
                                      /*skip_count=*/3);
  d.requested_size = size;

  return free_page;
}

void GuardedPageAllocator::Deallocate(void *ptr) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  ASSERT(PointerIsMine(ptr));
  ASSERT(GetPageAddr(addr) == addr);
  int err = mprotect(ptr, page_size_, PROT_NONE);
  CHECK_CONDITION(err != -1);

  // Record stack trace.
  size_t slot = AddrToSlot(addr);
  StackTrace &trace = data_[slot].dealloc_trace;
  trace.depth = GetStackTrace(trace.stack, kMaxStackDepth, /*skip_count=*/2);

  FreeSlot(slot);
}

size_t GuardedPageAllocator::GetRequestedSize(const void *ptr) const {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  ASSERT(PointerIsMine(ptr));
  ASSERT(GetPageAddr(addr) == addr);
  size_t slot = AddrToSlot(addr);
  return data_[slot].requested_size;
}

GuardedPageAllocator::ErrorType GuardedPageAllocator::GetStackTraces(
    const void *ptr, StackTrace *alloc_trace, StackTrace *dealloc_trace) const {
  ASSERT(PointerIsMine(ptr));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  size_t slot = GetNearestSlot(addr);
  *alloc_trace = data_[slot].alloc_trace;
  *dealloc_trace = data_[slot].dealloc_trace;
  return GetErrorType(addr, alloc_trace->depth, dealloc_trace->depth);
}

void GuardedPageAllocator::AllowAllocations() {
  SpinLockHolder h(Static::guardedpage_lock());
  allow_allocations_ = true;
}

// Maps 2 * num_pages + 1 pages so that there are num_pages pages we can return
// from Allocate with guard pages before and after them.  Each page has size
// page_size_, which is a multiple of kPageSize so that we can piggy-back on
// existing alignment checks on the fast path of tcmalloc's free.
void GuardedPageAllocator::MapPages() {
  SpinLockHolder h(Static::guardedpage_lock());
  ASSERT(!first_page_addr_);
  ASSERT(page_size_ % getpagesize() == 0);
  size_t len = (2 * num_pages_ + 1) * page_size_;
  void *base_addr = mmap(/*addr=*/nullptr, len, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, /*fd=*/-1,
                         /*offset=*/0);
  ASSERT(base_addr != MAP_FAILED);
  if (base_addr == MAP_FAILED) return;

  // Tell TCMalloc's PageMap about the memory we own.
  const PageID page = reinterpret_cast<uintptr_t>(base_addr) >> kPageShift;
  const Length page_len = len >> kPageShift;
  if (!Static::pageheap()->Ensure(page, page_len)) {
    ASSERT(false && "Failed to notify page map of page-guarded memory.");
    return;
  }

  pages_base_addr_ = reinterpret_cast<uintptr_t>(base_addr);
  pages_end_addr_ = pages_base_addr_ + len;

  // Align first page to page_size_.
  first_page_addr_ = GetPageAddr(pages_base_addr_ + page_size_);

  free_pages_ =
      (num_pages_ == kFreePagesNumBits) ? ~0ULL : (1ULL << num_pages_) - 1;
}

// Selects a random slot in O(1) time by rotating the free_pages bitmap by a
// random amount, using an intrinsic to get the least-significant 1-bit after
// the rotation, and then computing the position of the bit before the rotation.
ssize_t GuardedPageAllocator::ReserveFreeSlot() {
  SpinLockHolder h(Static::guardedpage_lock());
  if (!allow_allocations_ || !free_pages_) return -1;

  rand_ = Sampler::NextRandom(rand_);
  int rot = rand_ % kFreePagesNumBits;
  BitMap rotated_bitmap =
      (free_pages_ << rot) | (free_pages_ >> (kFreePagesNumBits - rot));
  int rotated_selection = __builtin_ctzll(rotated_bitmap);
  int selection =
      (rotated_selection - rot + kFreePagesNumBits) % kFreePagesNumBits;
  ASSERT(selection >= 0 && selection < kFreePagesNumBits);
  ASSERT(free_pages_ & (1ULL << selection));
  free_pages_ &= ~(1ULL << selection);
  return selection;
}

void GuardedPageAllocator::FreeSlot(size_t slot) {
  ASSERT(slot < kFreePagesNumBits);
  BitMap bit = 1ULL << slot;
  SpinLockHolder h(Static::guardedpage_lock());
  free_pages_ |= bit;
}

uintptr_t GuardedPageAllocator::GetPageAddr(uintptr_t addr) const {
  const uintptr_t addr_mask = ~(page_size_ - 1ULL);
  return addr & addr_mask;
}

uintptr_t GuardedPageAllocator::GetNearestValidPage(uintptr_t addr) const {
  if (addr < first_page_addr_) return first_page_addr_;
  uintptr_t offset = addr - first_page_addr_;

  // If addr is already on a valid page, just return addr.
  if ((offset / page_size_) % 2 == 0) return addr;

  // ptr points to a guard page, so get nearest valid page.
  const size_t kHalfPageSize = page_size_ / 2;
  if ((offset / kHalfPageSize) % 2 == 0) {
    return addr - kHalfPageSize;  // Round down.
  }
  return addr + kHalfPageSize;  // Round up.
}

size_t GuardedPageAllocator::GetNearestSlot(uintptr_t addr) const {
  return AddrToSlot(GetPageAddr(GetNearestValidPage(addr)));
}

GuardedPageAllocator::ErrorType GuardedPageAllocator::GetErrorType(
    uintptr_t addr, uintptr_t alloc_trace_depth,
    uintptr_t dealloc_trace_depth) const {
  if (!alloc_trace_depth) return ErrorType::kUnknown;
  if (dealloc_trace_depth) return ErrorType::kUseAfterFree;
  if (addr < first_page_addr_) return ErrorType::kBufferUnderflow;
  const uintptr_t offset = addr - first_page_addr_;
  ASSERT((offset / page_size_) % 2 != 0);
  const size_t kHalfPageSize = page_size_ / 2;
  return (offset / kHalfPageSize) % 2 == 0 ? ErrorType::kBufferOverflow
                                           : ErrorType::kBufferUnderflow;
}

uintptr_t GuardedPageAllocator::SlotToAddr(size_t slot) const {
  ASSERT(slot < kFreePagesNumBits);
  return first_page_addr_ + 2 * slot * page_size_;
}

size_t GuardedPageAllocator::AddrToSlot(uintptr_t addr) const {
  uintptr_t offset = addr - first_page_addr_;
  ASSERT(offset % page_size_ == 0);
  ASSERT((offset / page_size_) % 2 == 0);
  int slot = offset / page_size_ / 2;
  ASSERT(slot >= 0 && slot < kFreePagesNumBits);
  return slot;
}

static struct sigaction old_sa = {};

static void ForwardSignal(int signo, siginfo_t *info, void *context) {
  if (old_sa.sa_flags & SA_SIGINFO) {
    old_sa.sa_sigaction(signo, info, context);
  } else if (old_sa.sa_handler == SIG_DFL) {
    // No previous handler registered.  Re-raise signal for core dump.
    int err = sigaction(signo, &old_sa, nullptr);
    if (err == -1) {
      Log(kLog, __FILE__, __LINE__, "Couldn't restore previous sigaction!");
    }
    raise(signo);
  } else if (old_sa.sa_handler == SIG_IGN) {
    return;  // Previous sigaction ignored signal, so do the same.
  } else {
    old_sa.sa_handler(signo);
  }
}

// Logs a symbolized (if possible) stack trace.
static void DumpStackTrace(const StackTrace &trace) {
  SymbolTable symbol_table;
  for (size_t i = 0; i < trace.depth; i++) symbol_table.Add(trace.stack[i]);
  symbol_table.Symbolize();
  for (size_t i = 0; i < trace.depth; i++) {
    RAW_LOG(ERROR, "    @\t%p\t%s\n", trace.stack[i],
            symbol_table.GetSymbol(trace.stack[i]));
  }
}

// A SEGV handler that prints stack traces for the allocation and deallocation
// of relevant memory and then forwards the SEGV to the previous handler for the
// rest of the crash dump.
static void SegvHandler(int signo, siginfo_t *info, void *context) {
  if (Static::guardedpage_allocator()->PointerIsMine(info->si_addr)) {
    StackTrace alloc_trace, dealloc_trace;
    GuardedPageAllocator::ErrorType error =
        Static::guardedpage_allocator()->GetStackTraces(
            info->si_addr, &alloc_trace, &dealloc_trace);
    if (error != GuardedPageAllocator::ErrorType::kUnknown) {
      Log(kLog, __FILE__, __LINE__,
          "*** go/gwp-asan has detected a memory error ***");
      Log(kLog, __FILE__, __LINE__,
          "Error originates from memory allocated at:");
      DumpStackTrace(alloc_trace);
      switch (error) {
        case GuardedPageAllocator::ErrorType::kUseAfterFree:
          Log(kLog, __FILE__, __LINE__, "The memory was freed at:");
          DumpStackTrace(dealloc_trace);
          Log(kLog, __FILE__, __LINE__, "Use-after-free occurs at:");
          break;
        case GuardedPageAllocator::ErrorType::kBufferUnderflow:
          Log(kLog, __FILE__, __LINE__, "Buffer underflow occurs at:");
          break;
        case GuardedPageAllocator::ErrorType::kBufferOverflow:
          Log(kLog, __FILE__, __LINE__, "Buffer overflow occurs at:");
          break;
        case GuardedPageAllocator::ErrorType::kUnknown:
          Log(kCrash, __FILE__, __LINE__, "Unexpected ErrorType::kUnknown");
      }
      StackTrace current_trace;
      current_trace.depth = GetStackTrace(current_trace.stack, kMaxStackDepth,
                                          /*skip_count=*/1);
      DumpStackTrace(current_trace);
    }
  }
  ForwardSignal(signo, info, context);
}

// Registers SegvHandler() during module initialization.
static void RegisterSegvHandler() {
  struct sigaction new_sa = {};
  int err = sigaction(SIGSEGV, nullptr, &new_sa);
  ASSERT(err != -1);
  if (err == -1) return;
  new_sa.sa_flags |= SA_SIGINFO;
  new_sa.sa_sigaction = SegvHandler;
  err = sigaction(SIGSEGV, &new_sa, &old_sa);
  ASSERT(err != -1);
  if (err != -1) Static::guardedpage_allocator()->AllowAllocations();
}
REGISTER_MODULE_INITIALIZER(tcmalloc_segv_handler, RegisterSegvHandler());

}  // namespace tcmalloc
