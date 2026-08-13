/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef MINI_X86_INT_H_
#define MINI_X86_INT_H_

#include <ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

// mini_x86_try_advance attempts to "step" given x86 state one
// instruction. Returns true iff it was able to understand and execute
// that instruction. uc's state is only updated when attempt was
// successful.
extern bool mini_x86_int_try_advance(ucontext_t* uc);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MINI_X86_INT_H_
