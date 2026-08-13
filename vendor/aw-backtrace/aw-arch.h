/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_ARCH_H_
#define AW_ARCH_H_

#include <stdint.h>
#include <ucontext.h>

#if defined(__x86_64__)
#include "aw-arch-x86_64.h"
#elif defined(__aarch64__)
#include "aw-arch-aarch64.h"
#else
#error "Unsupported architecture"
#endif

namespace aw_backtrace_internal {
inline Cursor PrepareCursor(const ucontext_t* uc, const void* frame_address) {
  if (uc) {
    return Arch::CursorFromContext(uc);
  } else {
    return Arch::InitializeUnwindForCaller(frame_address);
  }
}
}  // namespace aw_backtrace_internal

#endif  // AW_ARCH_H_
