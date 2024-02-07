// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2009, Google Inc.
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
// Author: Andrew Fikes

#include "config.h"

#include "stack_trace_table.h"

#include "base/spinlock.h"              // for SpinLockHolder
#include "common.h"            // for StackTrace
#include "internal_logging.h"  // for ASSERT, Log
#include "page_heap_allocator.h"  // for PageHeapAllocator
#include "static_vars.h"       // for Static

namespace tcmalloc {

std::unique_ptr<void*[]> ProduceStackTracesDump(const StackTrace* (*next_fn)(const void** current_head),
                                                const void* head) {
  int depth_total = 0;
  int bucket_total = 0;
  for (const void* entry = head; entry != nullptr;) {
    const StackTrace* trace = next_fn(&entry);
    depth_total += trace->depth;
    bucket_total++;
  }

  int out_len = bucket_total * 3 + depth_total + 1;
  std::unique_ptr<void*[]> out{new void*[out_len]};

  int idx = 0;
  for (const void* entry = head; entry != nullptr;) {
    const StackTrace* trace = next_fn(&entry);
    out[idx++] = reinterpret_cast<void*>(uintptr_t{1});   // count
    out[idx++] = reinterpret_cast<void*>(trace->size);  // cumulative size
    out[idx++] = reinterpret_cast<void*>(trace->depth);
    for (int d = 0; d < trace->depth; ++d) {
      out[idx++] = trace->stack[d];
    }
  }
  out[idx++] = nullptr;
  ASSERT(idx == out_len);

  return out;
}

// In order to avoid dependencies we're only unit-testing function
// above. Stuff below pulls too much and isn't worth own unit-test
// (already covered by sampling_test).
#ifndef STACK_TRACE_TABLE_IS_TESTED

void StackTraceTable::AddTrace(const StackTrace& t) {
  if (error_) {
    return;
  }

  Entry* entry = allocator_.allocate(1);
  if (entry == nullptr) {
    Log(kLog, __FILE__, __LINE__,
        "tcmalloc: could not allocate bucket", sizeof(*entry));
    error_ = true;
  } else {
    entry->trace = t;
    entry->next = head_;
    head_ = entry;
  }
}

void** StackTraceTable::ReadStackTracesAndClear() {
  std::unique_ptr<void*[]> out = ProduceStackTracesDump(
    +[] (const void** current_head) -> const StackTrace* {
      const Entry* head = static_cast<const Entry*>(*current_head);
      *current_head = head->next;
      return &head->trace;
    }, head_);

  // Clear state
  error_ = false;

  SpinLockHolder h(Static::pageheap_lock());
  Entry* entry = head_;
  while (entry != nullptr) {
    Entry* next = entry->next;
    allocator_.deallocate(entry, 1);
    entry = next;
  }
  head_ = nullptr;

  return out.release();
}

#endif // STACK_TRACE_TABLE_IS_TESTED

}  // namespace tcmalloc
