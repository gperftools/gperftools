/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
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
#ifndef MEMMAP_H_
#define MEMMAP_H_

#include "config.h"

#ifdef _WIN32
// Windows has mmap bits defined in it's port.h header
#else
// Everything we assume is sufficiently POSIX-compatible. Also we
// assume ~everyone has MAP_ANONYMOUS or similar (POSIX, strangely,
// doesn't!)
#include <sys/mman.h>
#include <sys/types.h>

// Someone still cares about those near-obsolete OSes that fail to
// supply MAP_ANONYMOUS.
# ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
# endif

#endif

#include "base/basictypes.h"

namespace tcmalloc {

struct MMapResult {
  void* addr;
  bool success;
};

// MapAnonymous does mmap of r+w anonymous memory, simply saving us
// some hassle of spreading (not 100% portable) flags.
static inline MMapResult MapAnonymous(size_t length) {
  MMapResult result;
  result.addr = mmap(nullptr, length, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  result.success = (result.addr != MAP_FAILED);
  return result;
}

}  // namespace tcmalloc

#endif  // MEMMAP_H_
