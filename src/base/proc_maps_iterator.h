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
#ifndef BASE_PROC_MAPS_ITERATOR_H_
#define BASE_PROC_MAPS_ITERATOR_H_
#include "config.h"

#include <stdint.h>

#include "base/basictypes.h"
#include "base/generic_writer.h"
#include "base/logging.h"

namespace tcmalloc {

// ProcMapping struct contains description of proc/pid/maps entry as
// used by ForEachProcMapping below.
struct ProcMapping {
  uint64_t start;
  uint64_t end;
  const char* flags;
  uint64_t offset;
  int64_t inode;
  const char* filename;
};

// Internal version of ForEachProcMapping below.
bool DoForEachProcMapping(void (*body)(const ProcMapping& mapping, void* arg), void* arg);

// Iterates VMA entries in /proc/self/maps (or similar on other
// OS-es). Returns false if open() failed.
template <typename Body>
bool ForEachProcMapping(const Body& body) {
  return DoForEachProcMapping([] (const ProcMapping& mapping, void* arg) {
    const Body& body = *const_cast<const Body*>(static_cast<Body*>(arg));
    body(mapping);
  }, static_cast<void*>(const_cast<Body*>(&body)));
}

// Helper to add the list of mapped shared libraries to a profile.
// Write formatted "/proc/self/maps" contents into a given `writer'.
//
// See man 5 proc (on GNU/Linux system), or Google for "linux man 5
// proc" and search proc/pid/maps entry there for description of
// format.
void SaveProcSelfMaps(GenericWriter* writer);
// Helper to add the list of mapped shared libraries to a profile.
// Write formatted "/proc/self/maps" contents into a given file
// descriptor.
void SaveProcSelfMapsToRawFD(RawFD fd);

}  // namespace tcmalloc

#endif  // BASE_PROC_MAPS_ITERATOR_H_
