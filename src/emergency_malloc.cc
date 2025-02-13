// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2014, gperftools Contributors
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
//

#include "config.h"

#include "emergency_malloc.h"

#include <tuple>

#include <errno.h>                      // for ENOMEM, errno
#include <string.h>                     // for memset

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/low_level_alloc.h"
#include "base/spinlock.h"
#include "base/static_storage.h"
#include "internal_logging.h"
#include "memmap.h"
#include "thread_cache_ptr.h"

namespace tcmalloc {

ATTRIBUTE_VISIBILITY_HIDDEN char *emergency_arena_start;
ATTRIBUTE_VISIBILITY_HIDDEN uintptr_t emergency_arena_start_shifted;

static CACHELINE_ALIGNED SpinLock emergency_malloc_lock;
static char *emergency_arena_end;
static LowLevelAlloc::Arena *emergency_arena;

class EmergencyArenaPagesAllocator : public LowLevelAlloc::PagesAllocator {
  ~EmergencyArenaPagesAllocator() {}
  std::pair<void *, size_t> MapPages(size_t size) override {
    char *new_end = emergency_arena_end + size;
    if (new_end > emergency_arena_start + kEmergencyArenaSize) {
      RAW_LOG(FATAL, "Unable to allocate %zu bytes in emergency zone.", size);
    }
    char *rv = emergency_arena_end;
    emergency_arena_end = emergency_arena_start + kEmergencyArenaSize;
    return {static_cast<void *>(rv), emergency_arena_end - rv};
  }
  void UnMapPages(void *addr, size_t size) override {
    RAW_LOG(FATAL, "UnMapPages is not implemented for emergency arena");
  }
};

static void InitEmergencyMalloc(void) {
  auto [arena, success] = MapAnonymous(kEmergencyArenaSize * 2);
  CHECK_CONDITION(success);

  uintptr_t arena_ptr = reinterpret_cast<uintptr_t>(arena);
  uintptr_t ptr = (arena_ptr + kEmergencyArenaSize - 1) & ~(kEmergencyArenaSize-1);

  emergency_arena_end = emergency_arena_start = reinterpret_cast<char *>(ptr);

  static StaticStorage<EmergencyArenaPagesAllocator> pages_allocator_place;
  EmergencyArenaPagesAllocator* allocator = pages_allocator_place.Construct();

  emergency_arena = LowLevelAlloc::NewArenaWithCustomAlloc(allocator);

  emergency_arena_start_shifted = reinterpret_cast<uintptr_t>(emergency_arena_start) >> kEmergencyArenaShift;

  uintptr_t head_unmap_size = ptr - arena_ptr;
  CHECK_CONDITION(head_unmap_size < kEmergencyArenaSize);
  if (head_unmap_size != 0) {
    // Note, yes, we ignore any potential, but ~impossible
    // failures. It should be harmless.
    (void)munmap(arena, ptr - arena_ptr);
  }

  uintptr_t tail_unmap_size = kEmergencyArenaSize - head_unmap_size;
  void *tail_start = reinterpret_cast<void *>(arena_ptr + head_unmap_size + kEmergencyArenaSize);
  // Failures are ignored. See above.
  (void)munmap(tail_start, tail_unmap_size);
}

ATTRIBUTE_VISIBILITY_HIDDEN void *EmergencyMalloc(size_t size) {
  SpinLockHolder l(&emergency_malloc_lock);

  if (emergency_arena_start == nullptr) {
    InitEmergencyMalloc();
    CHECK_CONDITION(emergency_arena_start != nullptr);
  }

  void *rv = LowLevelAlloc::AllocWithArena(size, emergency_arena);
  if (rv == nullptr) {
    errno = ENOMEM;
  }
  return rv;
}

ATTRIBUTE_VISIBILITY_HIDDEN void EmergencyFree(void *p) {
  SpinLockHolder l(&emergency_malloc_lock);
  CHECK_CONDITION(emergency_arena_start);
  LowLevelAlloc::Free(p);
}

ATTRIBUTE_VISIBILITY_HIDDEN size_t EmergencyAllocatedSize(const void *p) {
  CHECK_CONDITION(emergency_arena_start);
  return LowLevelAlloc::UsableSize(p);
}

ATTRIBUTE_VISIBILITY_HIDDEN void *EmergencyRealloc(void *_old_ptr, size_t new_size) {
  if (_old_ptr == nullptr) {
    return EmergencyMalloc(new_size);
  }
  if (new_size == 0) {
    EmergencyFree(_old_ptr);
    return nullptr;
  }
  SpinLockHolder l(&emergency_malloc_lock);
  CHECK_CONDITION(emergency_arena_start);

  char *old_ptr = static_cast<char *>(_old_ptr);
  CHECK_CONDITION(old_ptr <= emergency_arena_end);
  CHECK_CONDITION(emergency_arena_start <= old_ptr);

  // NOTE: we don't know previous size of old_ptr chunk. So instead
  // of trying to figure out right size of copied memory, we just
  // copy largest possible size. We don't care about being slow.
  size_t old_ptr_size = emergency_arena_end - old_ptr;
  size_t copy_size = (new_size < old_ptr_size) ? new_size : old_ptr_size;

  void *new_ptr = LowLevelAlloc::AllocWithArena(new_size, emergency_arena);
  if (new_ptr == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  memcpy(new_ptr, old_ptr, copy_size);

  LowLevelAlloc::Free(old_ptr);
  return new_ptr;
}

}  // namespace tcmalloc
