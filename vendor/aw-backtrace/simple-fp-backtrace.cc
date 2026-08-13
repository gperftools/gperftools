/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "simple-fp-backtrace.h"

#include <stdint.h>
#include <sys/types.h>
#include <ucontext.h>

#include "aw-arch.h"
#include "aw-structs.h"
#include "utils.h"

using aw_backtrace_internal::Cursor;
using aw_backtrace_internal::PrepareCursor;

int do_capture_fp_backtrace(void** result, int max_frames, void* initial_pc, void* initial_frame) {
  if (max_frames < 1) {
    return 0;
  }

  struct frame {
    frame* parent;
    void* pc;
  };
  int count = 0;
  result[count++] = initial_pc;

  frame* f = static_cast<frame*>(initial_frame);
  while (count < max_frames) {
    if (!f || (uintptr_t)f < 4096) {
      break;
    }
    void* pc =
        reinterpret_cast<void*>(aw_backtrace_internal::Arch::CleanReturnAddress(reinterpret_cast<uintptr_t>(f->pc)));
    if (pc == nullptr) {
      break;
    }
    result[count++] = pc;
    f = f->parent;
  }

  return count;
}

NEVER_INLINE int simple_backtrace(const void* _uc, void** result, int max_frames) {
  const ucontext_t* uc = static_cast<const ucontext_t*>(_uc);
  Cursor cursor = PrepareCursor(uc, __builtin_frame_address(0));

  return do_capture_fp_backtrace(result, max_frames, reinterpret_cast<void*>(cursor.pc),
                                 reinterpret_cast<void*>(cursor.fp));
}
