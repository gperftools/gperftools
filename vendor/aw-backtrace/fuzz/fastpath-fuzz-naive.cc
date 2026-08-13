/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "aw-backtrace-fastpath.h"
#include "check.h"
#include "fuzz-common.h"

// To be used again raw eh_frame{,_hdr} data in fuzz/data/*. I.e. run with --seeds=fuzz/data/
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 4)
    return 0;

  XlateData xd;
  xd.data = data;
  xd.data_size = size - 4;

  uint32_t fuzz_pc;
  memcpy(&fuzz_pc, data + size - 4, 4);

  uint64_t pc_off = fuzz_pc % (1<<20);
  FastPathFrame ff = Run(&xd, pc_off);

  if (!(ff == FastPathFrame::Failure())) {
    FrameInfo fi;
    (void)ff.ToFrameInfo(&fi);
  }
  return 0;
}
