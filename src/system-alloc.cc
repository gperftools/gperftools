// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2005, Google Inc.
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

#include <config.h>
#include <errno.h>                      // for EAGAIN, errno
#include <fcntl.h>                      // for open, O_RDWR
#include <stddef.h>                     // for size_t, ptrdiff_t
#include <stdint.h>                     // for uintptr_t, intptr_t
#ifdef HAVE_MMAP
#include <sys/mman.h>                   // for munmap, mmap, MADV_DONTNEED, etc
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>                     // for sbrk, getpagesize, off_t
#endif
#include <new>                          // for operator new
#include <gperftools/malloc_extension.h>
#include "base/basictypes.h"
#include "base/commandlineflags.h"
#include "base/spinlock.h"              // for SpinLockHolder, SpinLock, etc
#include "base/static_storage.h"
#include "common.h"
#include "internal_logging.h"

// On systems (like freebsd) that don't define MAP_ANONYMOUS, use the old
// form of the name instead.
#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

// Linux added support for MADV_FREE in 4.5 but we aren't ready to use it
// yet. Among other things, using compile-time detection leads to poor
// results when compiling on a system with MADV_FREE and running on a
// system without it. See https://github.com/gperftools/gperftools/issues/780.
#if defined(__linux__) && defined(MADV_FREE) && !defined(TCMALLOC_USE_MADV_FREE)
# undef MADV_FREE
#endif

// MADV_FREE is specifically designed for use by malloc(), but only
// FreeBSD supports it; in linux we fall back to the somewhat inferior
// MADV_DONTNEED.
#if !defined(MADV_FREE) && defined(MADV_DONTNEED)
# define MADV_FREE  MADV_DONTNEED
#endif

// Set kDebugMode mode so that we can have use C++ conditionals
// instead of preprocessor conditionals.
#ifdef NDEBUG
static const bool kDebugMode = false;
#else
static const bool kDebugMode = true;
#endif

// TODO(sanjay): Move the code below into the tcmalloc namespace
using tcmalloc::kLog;
using tcmalloc::Log;

// Check that no bit is set at position ADDRESS_BITS or higher.
static bool CheckAddressBits(uintptr_t ptr) {
  bool always_ok = (kAddressBits == 8 * sizeof(void*));
  // this is a bit insane but otherwise we get compiler warning about
  // shifting right by word size even if this code is dead :(
  int shift_bits = always_ok ? 0 : kAddressBits;
  return always_ok || ((ptr >> shift_bits) == 0);
}

static_assert(kAddressBits <= 8 * sizeof(void*),
              "address bits larger than pointer size");

static SpinLock spinlock;

#if defined(HAVE_MMAP) || defined(MADV_FREE)
// Page size is initialized on demand (only needed for mmap-based allocators)
static size_t pagesize = 0;
#endif

// The current system allocator
SysAllocator* tcmalloc_sys_alloc;

// Number of bytes taken from system.
size_t TCMalloc_SystemTaken = 0;

DEFINE_bool(malloc_skip_sbrk,
            EnvToBool("TCMALLOC_SKIP_SBRK", false),
            "Whether sbrk can be used to obtain memory.");
DEFINE_bool(malloc_skip_mmap,
            EnvToBool("TCMALLOC_SKIP_MMAP", false),
            "Whether mmap can be used to obtain memory.");
DEFINE_bool(malloc_disable_memory_release,
            EnvToBool("TCMALLOC_DISABLE_MEMORY_RELEASE", false),
            "Whether MADV_FREE/MADV_DONTNEED should be used"
            " to return unused memory to the system.");

// static allocators
class SbrkSysAllocator : public SysAllocator {
public:
  SbrkSysAllocator() : SysAllocator() {
  }
  void* Alloc(size_t size, size_t *actual_size, size_t alignment);
};
static tcmalloc::StaticStorage<SbrkSysAllocator> sbrk_space;

class MmapSysAllocator : public SysAllocator {
public:
  MmapSysAllocator() : SysAllocator() {
  }
  void* Alloc(size_t size, size_t *actual_size, size_t alignment);
private:
  uintptr_t hint_ = 0;
};
static tcmalloc::StaticStorage<MmapSysAllocator> mmap_space;

class DefaultSysAllocator : public SysAllocator {
 public:
  DefaultSysAllocator() : SysAllocator() {
    for (int i = 0; i < kMaxAllocators; i++) {
      failed_[i] = true;
      allocs_[i] = nullptr;
      names_[i] = nullptr;
    }
  }
  void SetChildAllocator(SysAllocator* alloc, unsigned int index,
                         const char* name) {
    if (index < kMaxAllocators && alloc != nullptr) {
      allocs_[index] = alloc;
      failed_[index] = false;
      names_[index] = name;
    }
  }
  void* Alloc(size_t size, size_t *actual_size, size_t alignment);

 private:
  static const int kMaxAllocators = 2;
  bool failed_[kMaxAllocators];
  SysAllocator* allocs_[kMaxAllocators];
  const char* names_[kMaxAllocators];
};
static tcmalloc::StaticStorage<DefaultSysAllocator> default_space;
static const char sbrk_name[] = "SbrkSysAllocator";
static const char mmap_name[] = "MmapSysAllocator";

void* SbrkSysAllocator::Alloc(size_t size, size_t *actual_size,
                              size_t alignment) {
#if !defined(HAVE_SBRK) || defined(__UCLIBC__)
  return nullptr;
#else
  // Check if we should use sbrk allocation.
  // FLAGS_malloc_skip_sbrk starts out as false (its uninitialized
  // state) and eventually gets initialized to the specified value.  Note
  // that this code runs for a while before the flags are initialized.
  // That means that even if this flag is set to true, some (initial)
  // memory will be allocated with sbrk before the flag takes effect.
  if (FLAGS_malloc_skip_sbrk) {
    return nullptr;
  }

  // sbrk will release memory if passed a negative number, so we do
  // a strict check here
  if (static_cast<ptrdiff_t>(size + alignment) < 0) return nullptr;

  // This doesn't overflow because TCMalloc_SystemAlloc has already
  // tested for overflow at the alignment boundary.
  size = ((size + alignment - 1) / alignment) * alignment;

  // "actual_size" indicates that the bytes from the returned pointer
  // p up to and including (p + actual_size - 1) have been allocated.
  if (actual_size) {
    *actual_size = size;
  }

  // Check that we we're not asking for so much more memory that we'd
  // wrap around the end of the virtual address space.  (This seems
  // like something sbrk() should check for us, and indeed opensolaris
  // does, but glibc does not:
  //    http://src.opensolaris.org/source/xref/onnv/onnv-gate/usr/src/lib/libc/port/sys/sbrk.c?a=true
  //    http://sourceware.org/cgi-bin/cvsweb.cgi/~checkout~/libc/misc/sbrk.c?rev=1.1.2.1&content-type=text/plain&cvsroot=glibc
  // Without this check, sbrk may succeed when it ought to fail.)
  if (reinterpret_cast<intptr_t>(sbrk(0)) + size < size) {
    return nullptr;
  }

  void* result = sbrk(size);
  if (result == reinterpret_cast<void*>(-1)) {
    return nullptr;
  }

  // Is it aligned?
  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  if ((ptr & (alignment-1)) == 0)  return result;

  // Try to get more memory for alignment
  size_t extra = alignment - (ptr & (alignment-1));
  void* r2 = sbrk(extra);
  if (reinterpret_cast<uintptr_t>(r2) == (ptr + size)) {
    // Contiguous with previous result
    return reinterpret_cast<void*>(ptr + extra);
  }

  // Give up and ask for "size + alignment - 1" bytes so
  // that we can find an aligned region within it.
  result = sbrk(size + alignment - 1);
  if (result == reinterpret_cast<void*>(-1)) {
    return nullptr;
  }
  ptr = reinterpret_cast<uintptr_t>(result);
  if ((ptr & (alignment-1)) != 0) {
    ptr += alignment - (ptr & (alignment-1));
  }
  return reinterpret_cast<void*>(ptr);
#endif  // HAVE_SBRK
}

void* MmapSysAllocator::Alloc(size_t size, size_t *actual_size,
                              size_t alignment) {
#ifndef HAVE_MMAP
  return nullptr;
#else
  // Check if we should use mmap allocation.
  // FLAGS_malloc_skip_mmap starts out as false (its uninitialized
  // state) and eventually gets initialized to the specified value.  Note
  // that this code runs for a while before the flags are initialized.
  // Chances are we never get here before the flags are initialized since
  // sbrk is used until the heap is exhausted (before mmap is used).
  if (FLAGS_malloc_skip_mmap) {
    return nullptr;
  }

  // Enforce page alignment
  if (pagesize == 0) pagesize = getpagesize();
  if (alignment < pagesize) alignment = pagesize;
  size_t aligned_size = ((size + alignment - 1) / alignment) * alignment;
  if (aligned_size < size) {
    return nullptr;
  }
  size = aligned_size;

  // "actual_size" indicates that the bytes from the returned pointer
  // p up to and including (p + actual_size - 1) have been allocated.
  if (actual_size) {
    *actual_size = size;
  }

  if (hint_ && hint_ + size > size && (hint_ & (alignment - 1)) == 0) {
    // We try to 'continue' previous mapping. But we first check that
    // alignment requirements are met and that it won't overflow
    // address space.
    void* result = mmap(reinterpret_cast<void*>(hint_), size,
                        PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS,
                        -1, 0);

    uintptr_t ptr = reinterpret_cast<uintptr_t>(result);

    // If the new mapping (even if at different address than hint
    // passed) requested alignment, then we return it.
    if ((ptr & (alignment - 1)) == 0) {
      hint_ = ptr + size;
      return result;
    }

    // Otherwise, we unmap and run "full" logic that is able to align
    // to arbitrary alignment. And that doesn't use hint.
    munmap(result, size);
  }

  // Ask for extra memory if alignment > pagesize
  size_t extra = 0;
  if (alignment > pagesize) {
    extra = alignment - pagesize;
  }

  // Note: size + extra does not overflow since:
  //            size + alignment < (1<<NBITS).
  // and        extra <= alignment
  // therefore  size + extra < (1<<NBITS)
  void* result = mmap(nullptr, size + extra,
                      PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS,
                      -1, 0);
  if (result == reinterpret_cast<void*>(MAP_FAILED)) {
    return nullptr;
  }

  // Adjust the return memory so it is aligned
  uintptr_t ptr = reinterpret_cast<uintptr_t>(result);
  size_t adjust = 0;
  if ((ptr & (alignment - 1)) != 0) {
    adjust = alignment - (ptr & (alignment - 1));
  }

  // Return the unused memory to the system
  if (adjust > 0) {
    munmap(reinterpret_cast<void*>(ptr), adjust);
  }
  if (adjust < extra) {
    munmap(reinterpret_cast<void*>(ptr + adjust + size), extra - adjust);
  }

  ptr += adjust;
  hint_ = ptr + size;
  return reinterpret_cast<void*>(ptr);
#endif  // HAVE_MMAP
}

void* DefaultSysAllocator::Alloc(size_t size, size_t *actual_size,
                                 size_t alignment) {
  for (int i = 0; i < kMaxAllocators; i++) {
    if (!failed_[i] && allocs_[i] != nullptr) {
      void* result = allocs_[i]->Alloc(size, actual_size, alignment);
      if (result != nullptr) {
        return result;
      }
      failed_[i] = true;
    }
  }
  // After both failed, reset "failed_" to false so that a single failed
  // allocation won't make the allocator never work again.
  for (int i = 0; i < kMaxAllocators; i++) {
    failed_[i] = false;
  }
  return nullptr;
}

ATTRIBUTE_WEAK ATTRIBUTE_NOINLINE
SysAllocator *tc_get_sysalloc_override(SysAllocator *def)
{
  return def;
}

static bool system_alloc_inited = false;
void InitSystemAllocators(void) {
  MmapSysAllocator *mmap = mmap_space.Construct();
  SbrkSysAllocator *sbrk = sbrk_space.Construct();

  // In 64-bit debug mode, place the mmap allocator first since it
  // allocates pointers that do not fit in 32 bits and therefore gives
  // us better testing of code's 64-bit correctness.  It also leads to
  // less false negatives in heap-checking code.  (Numbers are less
  // likely to look like pointers and therefore the conservative gc in
  // the heap-checker is less likely to misinterpret a number as a
  // pointer).
  DefaultSysAllocator *sdef = default_space.Construct();
  bool want_mmap = kDebugMode && (sizeof(void*) > 4);
  if (want_mmap) {
    sdef->SetChildAllocator(mmap, 0, mmap_name);
    sdef->SetChildAllocator(sbrk, 1, sbrk_name);
  } else {
    sdef->SetChildAllocator(sbrk, 0, sbrk_name);
    sdef->SetChildAllocator(mmap, 1, mmap_name);
  }

  tcmalloc_sys_alloc = tc_get_sysalloc_override(sdef);
}

void* TCMalloc_SystemAlloc(size_t size, size_t *actual_size,
                           size_t alignment) {
  // Discard requests that overflow
  if (size + alignment < size) return nullptr;

  SpinLockHolder lock_holder(&spinlock);

  if (!system_alloc_inited) {
    InitSystemAllocators();
    system_alloc_inited = true;
  }

  // Enforce minimum alignment
  if (alignment < sizeof(MemoryAligner)) alignment = sizeof(MemoryAligner);

  size_t actual_size_storage;
  if (actual_size == nullptr) {
    actual_size = &actual_size_storage;
  }

  void* result = tcmalloc_sys_alloc->Alloc(size, actual_size, alignment);
  if (result != nullptr) {
    CHECK_CONDITION(
      CheckAddressBits(reinterpret_cast<uintptr_t>(result) + *actual_size - 1));
    TCMalloc_SystemTaken += *actual_size;
  }
  return result;
}

bool TCMalloc_SystemRelease(void* start, size_t length) {
#if defined(FREE_MMAP_PROT_NONE) && defined(HAVE_MMAP) || defined(MADV_FREE)
  if (FLAGS_malloc_disable_memory_release) return false;
  if (pagesize == 0) pagesize = getpagesize();
  const size_t pagemask = pagesize - 1;

  size_t new_start = reinterpret_cast<size_t>(start);
  size_t end = new_start + length;
  size_t new_end = end;

  // Round up the starting address and round down the ending address
  // to be page aligned:
  new_start = (new_start + pagesize - 1) & ~pagemask;
  new_end = new_end & ~pagemask;

  ASSERT((new_start & pagemask) == 0);
  ASSERT((new_end & pagemask) == 0);
  ASSERT(new_start >= reinterpret_cast<size_t>(start));
  ASSERT(new_end <= end);

  if (new_end > new_start) {
    bool result, retry;
    do {
#if defined(FREE_MMAP_PROT_NONE) && defined(HAVE_MMAP)
      // mmap PROT_NONE is similar to munmap by freeing backing pages by
      // physical memory except using MAP_FIXED keeps virtual memory range
      // reserved to be remapped back later
      void* ret = mmap(reinterpret_cast<char*>(new_start), new_end - new_start,
          PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);

      result = ret != MAP_FAILED;
#else
      int ret = madvise(reinterpret_cast<char*>(new_start),
          new_end - new_start, MADV_FREE);

      result = ret != -1;
#endif
      retry = errno == EAGAIN;
    } while (!result && retry);

    return result;
  }
#endif 
  return false;
}

void TCMalloc_SystemCommit(void* start, size_t length) {
#if defined(FREE_MMAP_PROT_NONE) && defined(HAVE_MMAP)
  // remaping as MAP_FIXED to same address assuming span size did not change 
  // since last TCMalloc_SystemRelease
  mmap(start, length, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,
       -1, 0);
#else
  // Nothing to do here.  TCMalloc_SystemRelease does not alter pages
  // such that they need to be re-committed before they can be used by the
  // application.
#endif
}

SpinLock* GetSysAllocLock() {
  return &spinlock;
}
