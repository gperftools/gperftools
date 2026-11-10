/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "simple-fp-backtrace.h"

#include <stdint.h>
#include <sys/types.h>
#include <ucontext.h>

#include "aw-arch.h"
#include "aw-structs.h"
#include "utils.h"

using aw_backtrace_internal::Arch;
using aw_backtrace_internal::Cursor;

// A frame record: what the frame pointer points at, on both arches.
struct frame {
  frame* parent;
  void* pc;
};

int do_capture_fp_backtrace(void** result, int max_frames, void* initial_pc, void* initial_frame) {
  if (max_frames < 1) {
    return 0;
  }

  int count = 0;
  result[count++] = initial_pc;

  frame* f = static_cast<frame*>(initial_frame);
  while (count < max_frames) {
    if (!f || (uintptr_t)f < 4096) {
      break;
    }
    void* pc = reinterpret_cast<void*>(Arch::CleanReturnAddress(reinterpret_cast<uintptr_t>(f->pc)));
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
  if (uc) {
    Cursor cursor = Arch::CursorFromContext(uc);
    return do_capture_fp_backtrace(result, max_frames, reinterpret_cast<void*>(cursor.pc),
                                   reinterpret_cast<void*>(cursor.fp));
  }

  // Start from our own frame record, which gives the caller's pc and fp. That
  // is all a frame-pointer walk needs. (The caller's sp is not recoverable
  // from it on aarch64, which is why the CFI unwinder starts from
  // aw_capture_caller_cursor instead; see aw-arch.h.)
  const frame* self = static_cast<const frame*>(__builtin_frame_address(0));
  void* pc = reinterpret_cast<void*>(Arch::CleanReturnAddress(reinterpret_cast<uintptr_t>(self->pc)));
  return do_capture_fp_backtrace(result, max_frames, pc, self->parent);
}
