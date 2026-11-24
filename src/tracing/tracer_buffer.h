// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2026, gperftools Contributors
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

#ifndef TCMALLOC_TRACING_TRACER_BUFFER_H_
#define TCMALLOC_TRACING_TRACER_BUFFER_H_
#include <stddef.h>
#include <string.h>

#include "base/static_storage.h"
#include "internal_logging.h"

namespace tcmalloc {

// The global trace buffer. Tracer appends trace data here (under
// its lock), and buffer passes filled chunks to saver thread (see
// tracer_buffer.cc).
class TracerBuffer {
 public:
  static constexpr int kMinSizeAfterRefresh = 1 << 20;

  // Creates the (only) instance and mmaps the buffers. Called once,
  // during very first traced malloc call (so must not malloc).
  static TracerBuffer* Create();

  void AppendData(const char* buf, size_t size) {
    ASSERT(size <= kMinSizeAfterRefresh);

    if (limit_ - current_ < size) {
      Refresh();
    }
    ASSERT(limit_ - current_ >= size);

    memcpy(current_, buf, size);
    current_ += size;
  }

  // Opens trace output and starts saver thread. Called once, later
  // (from tracer's module initializer).
  void InitLate();

  // Passes all remaining data to saver and waits until saver is done.
  void Finalize();

 private:
  friend class StaticStorage<TracerBuffer>;
  TracerBuffer();

  // Passes (kDirectIOGranularity multiple prefix of) current buffer to saver
  // and switches to the next buffer.
  void Refresh();
  void RefreshInternal(int to_write);
  void SetBuffer(char* buffer, size_t used, size_t size) {
    start_ = buffer;
    current_ = buffer + used;
    limit_ = buffer + size;
  }

  bool saver_started_ = false;
  char* start_ = nullptr;
  char* current_ = nullptr;
  char* limit_ = nullptr;
};

// Creates tracer's service thread (saver or dumper) with all
// asynchronous signals blocked. Those threads are created early (before main), so they'd
// otherwise inherit empty signal mask, and could receive
// process-directed signals that the program expects to only see via
// sigwait/signalfd (since it blocks them in all of its threads).
// Aborts on failure.
void CreateServiceThread(void* (*body)(void*), void* arg);

}  // namespace tcmalloc

#endif  // TCMALLOC_TRACING_TRACER_BUFFER_H_
