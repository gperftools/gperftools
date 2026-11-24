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

#ifndef TCMALLOC_TRACING_MALLOC_TRACE_ENCODER_H_
#define TCMALLOC_TRACING_MALLOC_TRACE_ENCODER_H_

#include <sys/types.h>  // for ssize_t

#include <utility>  // for std::pair

#include "tracing/altvarint_codec.h"

namespace tcmalloc {

struct MallocTraceEncoder {
  static constexpr uint32_t kMagic = 0x6CBD63E4;

  static constexpr unsigned kEventMalloc = 0x00;
  static constexpr unsigned kEventFree = 0x01;
  static constexpr unsigned kEventTok = 0x02;
  static constexpr unsigned kEventBuf = 0x03;
  static constexpr unsigned kEventFreeSized = 0x4;
  static constexpr unsigned kEventExtBase = 0x07;

  static constexpr unsigned kTypeShift = 3;
  static constexpr unsigned kTypeMask = (1 << kTypeShift) - 1;

  static constexpr unsigned kEventDeath = kEventExtBase + 0;
  static constexpr unsigned kEventEnd = kEventExtBase + 010;
  static constexpr unsigned kEventRealloc = kEventExtBase + 020;
  static constexpr unsigned kEventMemalign = kEventExtBase + 030;
  static constexpr unsigned kEventSyncBarrier = kEventExtBase + 040;
  static constexpr unsigned kEventMisc = kEventExtBase + 050;

  static constexpr unsigned kExtTypeShift = 8;
  static constexpr unsigned kExtTypeMask = 0xff;

  static constexpr int kTSShift = 10;
  static constexpr uint64_t kTSMask = ~((1ULL << kTSShift) - 1);

  using pair = std::pair<uint64_t, uint64_t>;
  using triple = std::pair<uint64_t, std::pair<uint64_t, uint64_t>>;

  static uint64_t BundleTSAndCPU(uint64_t ts, uint64_t cpu) { return (ts & kTSMask) | (cpu & ~kTSMask); }

  template <typename I1, typename I2>
  static void UnbundleTSAndCPU(uint64_t word, I1* ts, I2* cpu) {
    *ts = word & kTSMask;
    *cpu = word & ~kTSMask;
  }

  static uint64_t EncodeMalloc(size_t _size, ssize_t* prev_size) {
    ssize_t size = static_cast<ssize_t>((_size + 7) >> 3);
    uint64_t to_encode = AltVarintCodec::Zigzag(size - *prev_size);
    to_encode <<= kTypeShift;
    to_encode |= kEventMalloc;
    *prev_size = size;
    return to_encode;
  }

  static uint64_t EncodeFree(uint64_t token, uint64_t* prev_token) {
    uint64_t to_encode = AltVarintCodec::Zigzag(token - *prev_token);
    to_encode <<= kTypeShift;
    to_encode |= kEventFree;
    *prev_token = token;
    return to_encode;
  }

  static uint64_t EncodeFreeSized(uint64_t token, uint64_t* prev_token) {
    uint64_t to_encode = AltVarintCodec::Zigzag(token - *prev_token);
    to_encode <<= kTypeShift;
    to_encode |= kEventFreeSized;
    *prev_token = token;
    return to_encode;
  }

  static pair EncodeRealloc(uint64_t old_token, size_t new_size, ssize_t* prev_size, uint64_t* prev_token) {
    ssize_t size = static_cast<ssize_t>((new_size + 7) >> 3);
    uint64_t to_encode = AltVarintCodec::Zigzag(size - *prev_size);
    to_encode <<= kExtTypeShift;
    to_encode |= kEventRealloc;
    *prev_size = size;

    uint64_t to_encode2 = AltVarintCodec::Zigzag(old_token - *prev_token);
    *prev_token = old_token;
    return std::make_pair(to_encode, to_encode2);
  }

  static pair EncodeMemalign(size_t _size, size_t alignment, ssize_t* prev_size) {
    ssize_t size = static_cast<ssize_t>((_size + 7) >> 3);
    uint64_t to_encode = AltVarintCodec::Zigzag(size - *prev_size);
    to_encode <<= kExtTypeShift;
    to_encode |= kEventMemalign;
    *prev_size = size;
    return std::make_pair(to_encode, alignment);
  }

  static triple EncodeBuffer(uint64_t thread_id, uint64_t ts_and_cpu, uint64_t size) {
    return std::make_pair((thread_id << kTypeShift) | kEventBuf, std::make_pair(ts_and_cpu, size));
  }

  static pair EncodeToken(uint64_t token_base, uint64_t ts_and_cpu) {
    return std::make_pair((token_base << kTypeShift) | kEventTok, ts_and_cpu);
  }

  static pair EncodeDeath(uint64_t thread_id, uint64_t ts_and_cpu) {
    uint64_t first = (thread_id << kExtTypeShift) | kEventDeath;
    return std::make_pair(first, ts_and_cpu);
  }

  static uint64_t EncodeEnd() { return kEventEnd; }

  static pair EncodeSyncBarrier(uint64_t ts_and_cpu) { return std::make_pair(kEventSyncBarrier, ts_and_cpu); }

  // Misc messages are encoded like kEventBuf events but with arbitrary
  // "misc" message data instead of trace data. Note, kEventMisc is
  // extended event type, so (like for death events) thread id is
  // shifted by kExtTypeShift.
  static triple EncodeMisc(uint64_t thread_id, uint64_t ts_and_cpu, uint64_t size) {
    return std::make_pair((thread_id << kExtTypeShift) | kEventMisc, std::make_pair(ts_and_cpu, size));
  }

  static unsigned DecodeType(uint64_t first_word) {
    unsigned evtype = first_word & kTypeMask;
    if (__builtin_expect(evtype != kEventExtBase, 1)) {
      return evtype;
    }
    return first_word & kExtTypeMask;
  }

  template <typename T>
  static void DecodeMalloc(T* m, uint64_t first_word, uint64_t* prev_size, uint64_t* malloc_tok_seq) {
    uint64_t sz = AltVarintCodec::Unzigzag(first_word >> kTypeShift) + *prev_size;
    *prev_size = sz;
    sz = sz << 3;
    m->size = sz;
    m->token = (*malloc_tok_seq)++;
  }

  template <typename T>
  static void DecodeFree(T* f, uint64_t first_word, uint64_t* prev_token) {
    uint64_t tok = AltVarintCodec::Unzigzag(first_word >> kTypeShift) + *prev_token;
    *prev_token = tok;
    f->token = tok;
  }

  template <typename T>
  static void DecodeFreeSized(T* f, uint64_t first_word, uint64_t* prev_token) {
    uint64_t tok = AltVarintCodec::Unzigzag(first_word >> kTypeShift) + *prev_token;
    *prev_token = tok;
    f->token = tok;
  }

  template <typename T>
  static void DecodeRealloc(T* r, uint64_t first_word, uint64_t second_word, uint64_t* prev_size, uint64_t* prev_token,
                            uint64_t* malloc_tok_seq) {
    uint64_t sz = AltVarintCodec::Unzigzag(first_word >> kExtTypeShift) + *prev_size;
    *prev_size = sz;
    sz = sz << 3;
    r->new_size = sz;
    r->new_token = (*malloc_tok_seq)++;

    uint64_t tok = AltVarintCodec::Unzigzag(second_word) + *prev_token;
    *prev_token = tok;
    r->old_token = tok;
  }

  template <typename T>
  static void DecodeMemalign(T* m, uint64_t first_word, uint64_t second_word, uint64_t* prev_size,
                             uint64_t* malloc_tok_seq) {
    uint64_t sz = AltVarintCodec::Unzigzag(first_word >> kExtTypeShift) + *prev_size;
    *prev_size = sz;
    sz = sz << 3;
    m->size = sz;
    m->token = (*malloc_tok_seq)++;
    m->alignment = second_word;
  }

  template <typename T>
  static void DecodeBuffer(T* b, uint64_t first_word, uint64_t second_word, uint64_t third_word) {
    b->thread_id = first_word >> kTypeShift;
    UnbundleTSAndCPU(second_word, &b->ts, &b->cpu);
    b->size = third_word;
  }

  // Note, misc event payload (of size b->size) follows the 3 words.
  template <typename T>
  static void DecodeMisc(T* b, uint64_t first_word, uint64_t second_word, uint64_t third_word) {
    b->thread_id = first_word >> kExtTypeShift;
    UnbundleTSAndCPU(second_word, &b->ts, &b->cpu);
    b->size = third_word;
  }

  template <typename T>
  static void DecodeToken(T* t, uint64_t first_word, uint64_t second_word) {
    t->token_base = first_word >> kTypeShift;
    UnbundleTSAndCPU(second_word, &t->ts, &t->cpu);
  }

  template <typename T>
  static void DecodeDeath(T* d, uint64_t first_word, uint64_t second_word) {
    d->thread_id = first_word >> kExtTypeShift;
    UnbundleTSAndCPU(second_word, &d->ts, &d->cpu);
  }

  template <typename T>
  static void DecodeSyncBarrier(T* ev, uint64_t first_word, uint64_t second_word) {
    UnbundleTSAndCPU(second_word, &ev->ts, &ev->cpu);
  }
};

}  // namespace tcmalloc

#endif  // TCMALLOC_TRACING_MALLOC_TRACE_ENCODER_H_
