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

#ifndef EMERGENCY_MALLOC_H
#define EMERGENCY_MALLOC_H
#include "config.h"

#include <stddef.h>

#include "base/basictypes.h"
#include "common.h"
#include "thread_cache_ptr.h"

namespace tcmalloc {

static constexpr uintptr_t kEmergencyArenaShift = 20+4; // 16 megs
static constexpr uintptr_t kEmergencyArenaSize = uintptr_t{1} << kEmergencyArenaShift;

ATTRIBUTE_VISIBILITY_HIDDEN extern char *emergency_arena_start;
ATTRIBUTE_VISIBILITY_HIDDEN extern uintptr_t emergency_arena_start_shifted;;

ATTRIBUTE_VISIBILITY_HIDDEN void *EmergencyMalloc(size_t size);
ATTRIBUTE_VISIBILITY_HIDDEN void EmergencyFree(void *p);
ATTRIBUTE_VISIBILITY_HIDDEN void *EmergencyRealloc(void *old_ptr, size_t new_size);
ATTRIBUTE_VISIBILITY_HIDDEN size_t EmergencyAllocatedSize(const void* p);

static inline bool IsEmergencyPtr(const void *_ptr) {
  uintptr_t ptr = reinterpret_cast<uintptr_t>(_ptr);
  return PREDICT_FALSE((ptr >> kEmergencyArenaShift) == emergency_arena_start_shifted)
    && emergency_arena_start_shifted;
}

} // namespace tcmalloc

#endif
