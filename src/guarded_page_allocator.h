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

#ifndef TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_
#define TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_

#include "base/basictypes.h"
#include "base/spinlock.h"
#include "common.h"

namespace tcmalloc {

// An allocator that gives each allocation a new region, with guard pages on
// either side of the allocated region.  If a buffer is overflowed to the next
// guard page or underflowed to the previous guard page, a segfault occurs.
// After an allocation is freed, the underlying page is marked as inaccessible,
// and any future accesses to it will also cause segfaults until the page is
// reallocated.
//
// Is safe to use with static storage duration and is thread safe with the
// exception of calls to Init() and Destroy() (see corresponding function
// comments).
//
// SYNCHRONIZATION
//   Requires the SpinLock Static::guardedpage_lock to be defined externally.
//   This is required so that this class may be instantiated with static storage
//   duration.  The lock is held by this class during initialization and when
//   accessing the internal free page map.
//
// Example:
//   GuardedPageAllocator gpa;
//
//   void foo() {
//     char *buf = reinterpret_cast<char *>(gpa.Allocate(8000));
//     buf[0] = 'A';            // OK. No segfault occurs.
//     memset(buf, 'A', 8000);  // OK. No segfault occurs.
//     buf[-1] = 'A';           // Segfault!
//     buf[9000] = 'A';         // Segfault!
//     gpa.Deallocate(buf);
//     buf[0] = 'B';            // Segfault!
//   }
//
//   int main() {
//     gpa.Init(GuardedPageAllocator::kGpaMaxPages);  // Call Init() only once.
//     gpa.AllowAllocations();
//     for (int i = 0; i < 1000; i++) foo();
//     return 0;
//   }
class GuardedPageAllocator {
#if defined(__GNUC__) && defined(__linux__)
 public:
  // Maximum number of pages this class can allocate.
  static constexpr size_t kGpaMaxPages = 64;

  enum class ErrorType {
    kUseAfterFree,
    kBufferUnderflow,
    kBufferOverflow,
    kUnknown,
  };

  constexpr GuardedPageAllocator()
      : free_pages_(0ULL),
        data_{},
        pages_base_addr_(0),
        pages_end_addr_(0),
        first_page_addr_(0),
        num_pages_(0),
        page_size_(0),
        rand_(0),
        allow_allocations_(false) {}

  GuardedPageAllocator(const GuardedPageAllocator &) = delete;
  GuardedPageAllocator &operator=(const GuardedPageAllocator &) = delete;

  ~GuardedPageAllocator() = default;

  // Configures this allocator to map memory for num_pages pages (excluding
  // guard pages).  num_pages must be in the range [1, kGpaMaxPages].
  //
  // This method should be called non-concurrently and only once to complete
  // initialization.  Dynamic initialization is deliberately done here and not
  // in the constructor, thereby allowing the constructor to be constexpr and
  // avoiding static initialization order issues.
  void Init(size_t num_pages);

  // Unmaps memory allocated by this class.
  //
  // This method should be called non-concurrently and only once to complete
  // destruction.  Destruction is deliberately done here and not in the
  // destructor, thereby allowing the destructor to be trivial (i.e. a no-op)
  // and avoiding use-after-destruction issues for static/global instances.
  void Destroy();

  // On success, returns a pointer to size bytes of page-guarded memory.  On
  // failure, returns nullptr.  Failure can occur if memory could not be mapped
  // or protected, or if all guarded pages are already allocated.
  //
  // Precondition:  size <= page_size_
  void *Allocate(size_t size);

  // Deallocates memory pointed to by ptr.  ptr must have been previously
  // returned by a call to Allocate.
  void Deallocate(void *ptr);

  // Returns the size requested when ptr was allocated.  ptr must have been
  // previously returned by a call to Allocate.
  size_t GetRequestedSize(const void *ptr) const;

  // Records stack traces in alloc_trace and dealloc_trace for the page nearest
  // to ptr.  alloc_trace is the trace at the time the page was allocated.  If
  // the page is still allocated, dealloc_trace->depth will be 0. If the page
  // has been deallocated, dealloc_trace is the trace at the time the page was
  // deallocated.
  //
  // Returns the likely error type for an access at ptr.
  //
  // Requires that ptr points to memory mapped by this class.
  ErrorType GetStackTraces(const void *ptr, StackTrace *alloc_trace,
                           StackTrace *dealloc_trace) const;

  // Returns true if ptr points to memory managed by this class.
  inline bool PointerIsMine(const void *ptr) const {
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    return pages_base_addr_ <= addr && addr < pages_end_addr_;
  }

  // Allows Allocate() to start returning allocations.
  void AllowAllocations();

 private:
  using BitMap = uint64;

  // Structure for storing data about a slot.
  struct SlotMetadata {
    StackTrace alloc_trace;
    StackTrace dealloc_trace;
    size_t requested_size;
  };

  // Number of bits in the free_pages_ bitmap.
  static constexpr size_t kFreePagesNumBits = sizeof(BitMap) * 8;

  // Maps pages into memory.
  void MapPages();

  // Reserves and returns a slot randomly selected from the free slots in
  // free_pages_.  Returns -1 if no slots available, or if AllowAllocations()
  // hasn't been called yet.
  ssize_t ReserveFreeSlot();

  // Marks the specified slot as unreserved.
  void FreeSlot(size_t slot);

  // Returns the address of the page that addr resides on.
  uintptr_t GetPageAddr(uintptr_t addr) const;

  // Returns an address somewhere on the valid page nearest to addr.
  uintptr_t GetNearestValidPage(uintptr_t addr) const;

  // Returns the slot number for the page nearest to addr.
  size_t GetNearestSlot(uintptr_t addr) const;

  // Returns the likely error type for the given trace depths and access
  // address.
  ErrorType GetErrorType(uintptr_t addr, uintptr_t alloc_trace_depth,
                         uintptr_t dealloc_trace_depth) const;

  uintptr_t SlotToAddr(size_t slot) const;
  size_t AddrToSlot(uintptr_t addr) const;

  // Maps each bit to one page.
  // Bit=1: Free.  Bit=0: Reserved.
  BitMap free_pages_;

  // Stack trace data captured when each page is allocated/deallocated.  Printed
  // by the SEGV handler when an overflow, underflow, or use-after-free is
  // detected.
  SlotMetadata data_[kFreePagesNumBits];

  uintptr_t pages_base_addr_;  // Points to start of mapped region.
  uintptr_t pages_end_addr_;   // Points to the end of mapped region.
  uintptr_t first_page_addr_;  // Points to first page returnable by Allocate.
  size_t num_pages_;  // Number of pages mapped (excluding guard pages).
  size_t page_size_;  // Size of pages we allocate.
  uint64 rand_;       // RNG seed.

  // Flag to control whether we can return allocations or not.
  bool allow_allocations_;
#else
 public:
  static constexpr size_t kGpaMaxPages = 64;

  enum class ErrorType {
    kUseAfterFree,
    kBufferUnderflow,
    kBufferOverflow,
    kUnknown,
  };

  constexpr GuardedPageAllocator() {}
  GuardedPageAllocator(const GuardedPageAllocator &) = delete;
  GuardedPageAllocator &operator=(const GuardedPageAllocator &) = delete;
  ~GuardedPageAllocator() = default;

  void Init(size_t num_pages) {}
  void Destroy() {}
  void *Allocate(size_t size) { return nullptr; }
  void Deallocate(void *ptr) {}
  size_t GetRequestedSize(const void *ptr) const { return 0; }
  ErrorType GetStackTraces(const void *ptr, StackTrace *alloc_trace,
                           StackTrace *dealloc_trace) const {
    return ErrorType::kUnknown;
  }
  inline bool PointerIsMine(const void *ptr) const { return false; }
  void AllowAllocations() {}
#endif  // defined(__GNUC__) && defined(__linux__)
};

}  // namespace tcmalloc

#endif  // TCMALLOC_GUARDED_PAGE_ALLOCATOR_H_
