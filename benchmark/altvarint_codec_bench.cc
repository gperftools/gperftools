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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "run_benchmark.h"
#include "tracing/altvarint_codec.h"

using namespace tcmalloc;

static char *decode_varint(char *place, uint64_t *pval)
{
  AltVarintCodec::DecodeResult<uint64_t> rv = AltVarintCodec::DecodeUnsigned(place);
  *pval = rv.value;
  return place + rv.advance;
}

static
void test_roundtrip(uint64_t i, char *buf)
{
  char *p = buf;
  char *q;
  p = AltVarintCodec::EncodeUnsigned(p, i);
  p = AltVarintCodec::EncodeUnsigned(p, 0x64);
  __builtin_memset(p, 0xff, 64);

  q = p;
  p = buf;
  uint64_t v;
  p = decode_varint(p, &v);
  if (v != i) {
    __builtin_trap();
  }
  p = decode_varint(p, &v);
  if (v != 0x64) {
    __builtin_trap();
  }

  if (p != q) {
    __builtin_trap();
  }
}

static
void test_varint(void)
{
  __attribute__((aligned(64))) char buf[256];
  uint64_t i;

  for (i = 0; i < 64; i++) {
    test_roundtrip(1ULL<<i, buf);
    test_roundtrip((1ULL<<i)+0x0f, buf);
    test_roundtrip((1ULL<<i)+0x33, buf);
    test_roundtrip((1ULL<<i)+0x55, buf);
  }

  for (i = 0; i < 1ULL << 18; i += 13) {
    test_roundtrip(i, buf);
  }
  for (; i < 1ULL << 40; i += 262139) {
    test_roundtrip(i, buf);
  }
  printf("tested!\n");
}

volatile int64_t signed_param;
static void bench_encode_signed(long iterations, uintptr_t _param)
{
  signed_param = _param;
  static __attribute__((aligned(64))) char buf[32*1024];
  char *p = buf;
  do {
    if (__builtin_expect(p + 8*10 > buf + sizeof(buf), 0)) {
      p = buf;
    }

    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);

    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);
    p = AltVarintCodec::EncodeSigned(p, signed_param);

    iterations -= 8;
  } while (iterations > 0);
}

volatile uint64_t param;
static void bench_encode_varint(long iterations, uintptr_t _param)
{
  param = _param;
  static __attribute__((aligned(64))) char buf[32*1024];
  char *p = buf;
  do {
    if (__builtin_expect(p + 8*10 > buf + sizeof(buf), 0)) {
      p = buf;
    }

    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);

    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);
    p = AltVarintCodec::EncodeUnsigned(p, param);

    iterations -= 8;
  } while (iterations > 0);
}

int main(void)
{
  test_varint();

  report_benchmark("encode_varint", bench_encode_varint, 0x20);
  report_benchmark("encode_varint", bench_encode_varint, 0x200);
  report_benchmark("encode_varint", bench_encode_varint, 0x20000);
  report_benchmark("encode_varint", bench_encode_varint, 1ULL << 54);

  report_benchmark("encode_signed", bench_encode_signed, 0x20);
  report_benchmark("encode_signed", bench_encode_signed, 0x200);
  report_benchmark("encode_signed", bench_encode_signed, 0x20000);
  report_benchmark("encode_signed", bench_encode_signed, 1ULL << 54);
  return 0;
}
