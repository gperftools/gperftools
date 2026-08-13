/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef FUZZ_COMMON_H_
#define FUZZ_COMMON_H_

#include <stdint.h>
#include <stdlib.h>

#include <memory>
#include <vector>

#include "aw-backtrace-fastpath.h"
#include "check.h"

using aw_backtrace_internal::EHReaderInputs;
using aw_backtrace_internal::FrameInfo;
using aw_backtrace_internal::fastpath::FastPathFrame;
using aw_backtrace_internal::fastpath::IdentityXlate;
using aw_backtrace_internal::fastpath::TryFastFrameInfo;

struct Mut {
  uint32_t off;
  uint8_t data;
};

struct FreeDeleter {
  using pointer = uint8_t*;
  void operator()(uint8_t* p) {
    free(p);
  }
};

using CleanupPtr = std::unique_ptr<uint8_t, FreeDeleter>;
using CleanupVec = std::vector<CleanupPtr>;
using MutVec = std::vector<Mut>;

struct XlateData {
  const uint8_t* data;
  size_t data_size;
  uintptr_t load_bias = 0x800000000000ULL;
  bool always_alloc_chunk = false;

  CleanupVec cleanups;
  MutVec mutations;

  uintptr_t low_pc_offset;
  uintptr_t high_pc_offset;
};

struct FuzzXlate {
  XlateData* const data;

  explicit FuzzXlate(XlateData* data) : data(data) {
  }

  uint8_t* AllocChunk(const uint8_t* bytes, size_t n, size_t align) {
    uint8_t* chunk = (uint8_t*)aligned_alloc(align, n);
    memcpy(chunk, bytes, n);
    data->cleanups.emplace_back(chunk);
    return chunk;
  }

  const void* Map(uintptr_t ptr, size_t n, size_t align) {
    assert((ptr & (align - 1)) == 0);
    uintptr_t access_off = ptr - data->load_bias;

    if (access_off > data->data_size) {
      return nullptr;
    }

    const uint8_t* bytes = data->data + access_off;
    uint8_t* chunk = nullptr;

    if (access_off + n > data->data_size) {
      // partially exceed data. This is just map, and we don't know
      // yet if the memory will be accessed.
      n = data->data_size - access_off;
      chunk = AllocChunk(bytes, n, 1);
    }

    for (const auto& [off, new_byte] : data->mutations) {
      // __sanitizer_cov_trace_cmp4(off, access_off);
      if (off - access_off < n) {
        if (!chunk)
          chunk = AllocChunk(bytes, n, align);
        chunk[off - access_off] = new_byte;
      }
    }

    if (!chunk) {
      if (data->always_alloc_chunk) {
        chunk = AllocChunk(bytes, n, align);
      } else {
        chunk = const_cast<uint8_t*>(bytes);
      }
    }
    return (const void*)chunk;
  }
};

inline EHReaderInputs MakeInputs(uintptr_t addr, size_t size) {
  return EHReaderInputs{.lookup_pc = 0,
                        .eh_frame_hdr = addr,
                        .map_start = 0,
                        .map_end = 0,
                        .eh_frame_start = addr,
                        .eh_frame_end = addr + size};
}

inline FastPathFrame Run(XlateData* data, int64_t lookup_pc_off) {
  EHReaderInputs i = MakeInputs(data->load_bias, data->data_size);
  return TryFastFrameInfo(i.eh_frame_start, i.eh_frame_end, i.eh_frame_hdr, data->load_bias + lookup_pc_off,
                          FuzzXlate{data});
}

#endif  // FUZZ_COMMON_H_
