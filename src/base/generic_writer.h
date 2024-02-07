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
#ifndef BASE_GENERIC_WRITER_H_
#define BASE_GENERIC_WRITER_H_
#include "config.h"

#include <stdint.h>
#include <string.h>

#include <memory>
#include <utility>
#include <string>

#include "base/basictypes.h"
#include "base/logging.h"

namespace tcmalloc {

// Generic Writer is abstract sink of usually text data. It can be
// printf-ed into.
class ATTRIBUTE_VISIBILITY_HIDDEN GenericWriter {
public:
#if defined(HAVE___ATTRIBUTE__)
  void AppendF(const char* fmt, ...) __attribute__ ((format(printf, 2, 3)));
#else
  void AppendF(const char* fmt, ...);
#endif

  void AppendMem(const char* str, size_t sz);
  void AppendStr(const char* str) {
    AppendMem(str, strlen(str));
  }

protected:
  virtual ~GenericWriter();

  virtual std::pair<char*, char*> RecycleBuffer(char* buf_begin, char* buf_end, int want_at_least) = 0;

  // Must be called by child's destructor
  void FinalRecycle() {
    RecycleBuffer(buf_, buf_fill_, 0);
    buf_fill_ = buf_;
  }

private:
  char* buf_{};
  char* buf_fill_{};
  char* buf_end_{};
};

// WriteFnWriter is implementation of GenericWriter that writes via
// given abstract writer fn (i.e. could be lambda in practice). Note,
// this implementation is good for usage from inside guts of heap
// profiler and what not, under very strict locks. In particular it
// avoids any memory allocation by holding it's buffer within itself.
template <typename WriteFn, int kSize>
class ATTRIBUTE_VISIBILITY_HIDDEN WriteFnWriter : public GenericWriter {
public:
  explicit WriteFnWriter(const WriteFn& write_fn) : write_fn_(write_fn) {}
  ~WriteFnWriter() override {
    FinalRecycle();
  }

private:
  std::pair<char*, char*> RecycleBuffer(char* buf_begin, char* buf_end, int want_at_least) override {
    int actually_filled = buf_end - buf_begin;
    if (actually_filled > 0) {
      write_fn_(static_buffer_, actually_filled);
    }

    return {static_buffer_, static_buffer_ + kSize};
  }

  const WriteFn& write_fn_;
  char static_buffer_[kSize];
};

struct ATTRIBUTE_VISIBILITY_HIDDEN RawFDWriteFn {
  const RawFD fd;
  explicit RawFDWriteFn(RawFD fd) : fd(fd) {}
  void operator()(const char* buf, size_t amt) const {
    RawWrite(fd, buf, amt);
  }
};

// RawFDGenericWriter is implementation of GenericWriter that writes to
// given file descriptor. Note, this implementation is good for usage
// from inside guts of heap profiler and what not, under very strict
// locks. In particular it avoids any memory allocation by holding
// it's buffer within itself.
template <int kSize = 8192>
class ATTRIBUTE_VISIBILITY_HIDDEN RawFDGenericWriter : private RawFDWriteFn, public WriteFnWriter<RawFDWriteFn, kSize> {
public:
  explicit RawFDGenericWriter(RawFD fd) : RawFDWriteFn(fd), WriteFnWriter<RawFDWriteFn, kSize>{*static_cast<RawFDWriteFn*>(this)} {}
  ~RawFDGenericWriter() override = default;
};

// StringGenericWriter is GenericWriter implementation that appends to
// given std::string instance.
class ATTRIBUTE_VISIBILITY_HIDDEN StringGenericWriter : public GenericWriter {
public:
  explicit StringGenericWriter(std::string* s) : s_(s) {}
  ~StringGenericWriter() override;

private:
  std::pair<char*, char*> RecycleBuffer(char* buf_begin, char* buf_end, int want_at_least) override;

  std::string* s_;
  int unused_size_{}; // suffix of s_'s contents available to be filled
};

// ChunkedWriterConfig config is used by WithWriterToStrDup. See below
// for details. It is used to describe API to allocate memory for
// chunks holding data (Profiler{Malloc, Free} are used in practice).
struct ATTRIBUTE_VISIBILITY_HIDDEN ChunkedWriterConfig {
  typedef void* (*malloc_fn)(size_t);
  typedef void (*free_fn)(void*);

  malloc_fn chunk_malloc;
  free_fn chunk_free;
  int buffer_size;

  ChunkedWriterConfig(malloc_fn chunk_malloc, free_fn chunk_free, int buffer_size = 1 << 20)
    : chunk_malloc(chunk_malloc), chunk_free(chunk_free), buffer_size(buffer_size) {}
};

// Internal. Same as WithWriterToStrDup below.
ATTRIBUTE_VISIBILITY_HIDDEN
char* DoWithWriterToStrDup(const ChunkedWriterConfig& config, void (*body)(GenericWriter* writer, void* arg), void* arg);

// WithWriterToStrDup constructs GenericWriter instance that
// accumulates data in linked list of memory chunks allocated via
// means described in ChunkedWriterConfig. It passes that writer to
// given body (usually lambda) then after `body' is done with this
// writer, it's contents is converted into malloc-ed asciiz string.
//
// This unusual heap profiler's GetHeapProfile which processes profile
// under lock, so cannot allocate normally and which API and ABI
// returns malloc-ed asciiz string.
template <typename Body>
char* WithWriterToStrDup(const ChunkedWriterConfig& config, const Body& body) {
  return DoWithWriterToStrDup(config, [] (GenericWriter* writer, void* arg) {
    const Body& body = *const_cast<const Body*>(static_cast<Body*>(arg));
    body(writer);
  }, static_cast<void*>(const_cast<Body*>(&body)));
}

}  // namespace tcmalloc

#endif  // BASE_GENERIC_WRITER_H_
