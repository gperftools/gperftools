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
#include "base/generic_writer.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace tcmalloc {

GenericWriter::~GenericWriter() {
  RAW_DCHECK(buf_ == buf_fill_, "writer has to be finalized by call to FinalRecycle()");
}

void GenericWriter::AppendF(const char* fmt, ...) {
  int space_left = buf_end_ - buf_fill_;

  va_list va;
  va_start(va, fmt);
  int written = vsnprintf(buf_fill_, space_left, fmt, va);
  va_end(va);

  if (PREDICT_FALSE(written >= space_left)) {
    std::tie(buf_, buf_end_) = RecycleBuffer(buf_, buf_fill_, written + 1);
    RAW_DCHECK(written < buf_end_ - buf_, "recycled buffer needs to have space for this append");
    buf_fill_ = buf_;

    va_list va;
    va_start(va, fmt);
    space_left = buf_end_ - buf_;
    written = vsnprintf(buf_, space_left, fmt, va);
    va_end(va);

    RAW_DCHECK(written < space_left, "");
    // In case condition above holds (which we only test in debug builds)
    written = std::min<int>(written, space_left - 1);
  }

  buf_fill_ += written;
}

void GenericWriter::AppendMem(const char* str, size_t sz) {
  for (;;) {
    // first, cap amount to be at most MAX_INT
    int amount = static_cast<int>(std::min<size_t>(std::numeric_limits<int>::max(), sz));
    amount = std::min<int>(amount, buf_end_ - buf_fill_);

    memcpy(buf_fill_, str, amount);

    str += amount;
    buf_fill_ += amount;
    sz -= amount;

    if (sz == 0) {
      return;
    }

    std::tie(buf_, buf_end_) = RecycleBuffer(buf_, buf_fill_, 1);
    buf_fill_ = buf_;
  }
}

StringGenericWriter::~StringGenericWriter() {
  FinalRecycle();
  if (unused_size_) {
    s_->resize(s_->size() - unused_size_);
  }
}

std::pair<char*, char*> StringGenericWriter::RecycleBuffer(char* buf_begin, char* buf_end, int want_at_least) {
  unused_size_ -= buf_end - buf_begin;

  int deficit = want_at_least - unused_size_;
  size_t size = s_->size();
  if (deficit > 0) {
    size_t new_size = std::max(size + deficit, size * 2);
    s_->resize(new_size);
    unused_size_ += new_size - size;
    size = new_size;
  }

  char* ptr = const_cast<char*>(s_->data() + size - unused_size_);
  return {ptr, ptr + unused_size_};
}

namespace {

// We use this special storage for GetHeapProfile implementation,
// where we're unable to use regular memory allocation facilities, and
// where we must return free()-able chunk of memory.
struct ChunkedStorage {
  struct Chunk {
    Chunk* next;
    const int size;
    int used;
    char data[1];

    explicit Chunk(int size) : next(nullptr), size(size), used(0) {}
  };

  const ChunkedWriterConfig& config;
  Chunk* last_chunk{};

  explicit ChunkedStorage(const ChunkedWriterConfig& config) : config(config) {}

  ~ChunkedStorage() {
    RAW_DCHECK(last_chunk == nullptr, "storage must be released");
  }

  void CloseChunk(int actually_filled) {
    RAW_DCHECK(last_chunk->used == 0, "");
    last_chunk->used = actually_filled;
  }

  Chunk* AppendChunk(int want_at_least) {
    RAW_DCHECK(last_chunk == nullptr || last_chunk->used > 0, "");

    int size = std::max<int>(want_at_least + sizeof(Chunk), config.buffer_size);

    constexpr auto kChunkSize = sizeof(Chunk) - 1;
    Chunk* chunk = new (config.chunk_malloc(size)) Chunk(size - kChunkSize);
    chunk->next = last_chunk;
    last_chunk = chunk;
    return chunk;
  }

  char* StrDupAndRelease() {
    size_t total_size = 0;

    // On first pass we calculate total size.
    Chunk* ptr = last_chunk;
    while (ptr) {
      total_size += ptr->used;
      ptr = ptr->next;
    }

    char* data = static_cast<char*>(malloc(total_size + 1));
    data[total_size] = 0;

    // Then we fill data backwards and free accumulated chunks.
    ptr = last_chunk;
    while (ptr) {
      memcpy(data + total_size - ptr->used, ptr->data, ptr->used);
      total_size -= ptr->used;
      Chunk* next = ptr->next;
      config.chunk_free(ptr);
      ptr = next;
    }
    last_chunk = nullptr;
    return data;
  }
};

class ChunkedStorageWriter : public GenericWriter {
public:
  explicit ChunkedStorageWriter(ChunkedStorage* storage) : storage_(storage) {}
  ~ChunkedStorageWriter() override {
    FinalRecycle();
  }
private:
  std::pair<char*, char*> RecycleBuffer(char* buf_begin, char* buf_end, int want_at_least) override {
    if (storage_->last_chunk != nullptr) {
      storage_->CloseChunk(buf_end - buf_begin);
    }
    if (want_at_least == 0) {
      return {nullptr, 0};
    }
    auto* chunk = storage_->AppendChunk(want_at_least);
    return {chunk->data, chunk->data + chunk->size};
  }
  ChunkedStorage* const storage_;
};

}  // namespace

char* DoWithWriterToStrDup(const ChunkedWriterConfig& config, void (*body)(GenericWriter* writer, void* arg), void* arg) {
  ChunkedStorage storage(config);
  {
    ChunkedStorageWriter writer{&storage};
    body(&writer, arg);
    // Ensure writer is destroyed and releases it's entire output
    // into storage.
  }
  return storage.StrDupAndRelease();
}

}  // namespace tcmalloc
