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

#ifndef TCMALLOC_TRACING_ALTVARINT_CODEC_H_
#define TCMALLOC_TRACING_ALTVARINT_CODEC_H_
#include "config.h"

#include <stdint.h>
#include <string.h>

#include "base/basictypes.h"

namespace tcmalloc {

// This encoding is variant of ULEB128 (aka varint) that places
// "continuation bits" into lowest bits. It is optimized for very fast
// branchless encoding and decoding.
//
// This is same encoding as described here:
// https://github.com/WebAssembly/design/issues/601#issuecomment-196022303
//
// Quoting from there: (this is first byte and how many total bytes
// are used for encoding)
//
// xxxxxxx1  7 bits in 1 byte
// xxxxxx10 14 bits in 2 bytes
// xxxxx100 21 bits in 3 bytes
// xxxx1000 28 bits in 4 bytes
// xxx10000 35 bits in 5 bytes
// xx100000 42 bits in 6 bytes
// x1000000 49 bits in 7 bytes
// 10000000 56 bits in 8 bytes
// 00000000 64 bits in 9 bytes
//
// I.e. largest words are exception to "leading zeros count" rule.
//
// Signed values are zigzag-ed. See
// https://developers.google.com/protocol-buffers/docs/encoding#signed-integers
//
// This implementation only works for little endian machines.
class AltVarintCodec {
 public:
  // 9 bytes is what largest ints get encoded into.
  static constexpr int kMaxSize = 9;

  // DecodeResult is just a pair of values returned from decode_XXX
  // functions.
  template <typename T>
  struct DecodeResult {
    uint64_t advance;
    T value;

    static DecodeResult Make(uint64_t advance, const T& value) {
      DecodeResult rv;
      rv.advance = advance;
      rv.value = value;
      return rv;
    }
  };

  static char* EncodeUnsigned(char* place, uint64_t val);
  static char* EncodeSigned(char* place, int64_t val);

  static DecodeResult<uint64_t> DecodeUnsigned(const char* place);
  static DecodeResult<int64_t> DecodeSigned(const char* place);

  // zigzag is used to transform signed value into unsigned. unzigzag
  // is reverse transform. Values closer to 0 get mapped to small
  // outputs. I.e. unlike 2's complementary encoding for negative
  // values which has same binary representation as huge unsigned
  // integers. See
  // https://developers.google.com/protocol-buffers/docs/encoding#signed-integers
  static uint64_t Zigzag(int64_t val) {
    val = (val << 1) ^ (val >> 63);
    return static_cast<uint64_t>(val);
  }

  static int64_t Unzigzag(uint64_t val) {
    uint64_t sign = val & 1;
    return (val >> 1) ^ (0 - sign);
  }
};

inline char* AltVarintCodec::EncodeSigned(char* place, int64_t val) { return EncodeUnsigned(place, Zigzag(val)); }

ALWAYS_INLINE
char* AltVarintCodec::EncodeUnsigned(char* place, uint64_t val) {
  if (PREDICT_FALSE((val >> 56) != 0)) {
    uint8_t high = val >> 56;
    val = (val << 8);
    memcpy(place, &val, sizeof(val));
    memcpy(place + 8, &high, sizeof(high));
    return place + 9;
  }

  val = (val << 1) | 1;

  // note, this is +1 due to shift above
  static uint8_t encode_bits[64] CACHELINE_ALIGNED = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2,
                                                      3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 6,
                                                      6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8};

#if !defined(__x86_64__)
  unsigned bit = (63 - __builtin_clzll(val));
  unsigned bits = encode_bits[bit];
  val = val << bits;
#else
  // asm for code above. Rare compiler is able to optimize bsr well. I
  // experimentally found seemingly unnecessary XOR instruction to
  // help a lot. Might be that "famous" false dependency thingy for
  // BSR.
  uint64_t bits;
  __asm__(
      "xorl %%ecx, %%ecx\n\t"
      "bsrq %0, %%rcx\n\t"
      "movzbl (%3, %%rcx), %%ecx\n\t"
      "shlq %%cl, %0"
      : "=&r"(val), "=&c"(bits)
      : "0"(val), "r"(&encode_bits));
#endif

  memcpy(place, &val, sizeof(val));
  return place + bits + 1;
}

inline AltVarintCodec::DecodeResult<int64_t> AltVarintCodec::DecodeSigned(const char* place) {
  DecodeResult<uint64_t> t = DecodeUnsigned(place);
  int64_t val = static_cast<int64_t>(t.value >> 1);
  val ^= -static_cast<int64_t>(t.value & 1);
  return DecodeResult<int64_t>::Make(t.advance, val);
}

inline AltVarintCodec::DecodeResult<uint64_t> AltVarintCodec::DecodeUnsigned(const char* place) {
  uint64_t val;
  memcpy(&val, place, 8);

  // 9 bytes case is when all 8 lowest bits are 0. This is special case
  // when no leading bit is encoded.
  if (PREDICT_FALSE(!(val & 0xff))) {
    memcpy(&val, place + 1, 8);
    return DecodeResult<uint64_t>::Make(9, val);
  }

  // At this point we know there is at least 1 bit set among lowest 8
  // bits of val, so bsf-ing is safe and well-defined.

#if !defined(__x86_64__)
  unsigned p = __builtin_ffsll(val);
#else
  unsigned p;
  __asm__("xorl %0, %0; bsfl %1, %0" : "=&r"(p) : "r"((uint32_t)val));
#endif

  p += 1;
  val >>= p;

  static uint64_t decode_masks[9] = {0,  // due to p += 1 index 0 is unused and bogus
                                     0x0000007FLLU,
                                     0x00003FFFLLU,
                                     0x001FFFFFLLU,
                                     0x0FFFFFFFLLU,
                                     0x7FFFFFFFFLLU,
                                     0x3FFFFFFFFFFLLU,
                                     0x1FFFFFFFFFFFFLLU,
                                     0xFFFFFFFFFFFFFFFFLLU};

  uint64_t mask = decode_masks[p];
  return DecodeResult<uint64_t>::Make(p, val & mask);
}

}  // namespace tcmalloc

#endif  // TCMALLOC_TRACING_ALTVARINT_CODEC_H_
