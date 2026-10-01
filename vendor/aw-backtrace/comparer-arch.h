/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// The comparer's machine-dependent seam.
//
// backtrace-comparer.cc maintains a shadow call stack by watching every
// instruction go by and comparing it against what the unwinder reports. Two
// halves of that are machine-dependent and nothing else is:
//
//   * recognizing the instruction at the trapped pc as a call or a return,
//     and saying what a retired call put where, and
//   * recognizing a signal delivery -- the one control transfer that inserts
//     frames without a call -- in the frame the kernel built.
//
// Both live behind `struct ComparerArch`, one per arch, with a fixed set of
// statics. Adding an arch means implementing exactly that set; see
// comparer-arch-x86_64.h for the reference implementation.
//
// This is deliberately *not* aw-arch.h's `struct Arch`. The comparer is an
// independent oracle: if it borrowed the unwinder's notion of what a signal
// frame looks like, a wrong answer in `Arch::IsSignalFrame` would agree with
// itself and the test would pass. So the two arch layers share no code, and
// this one does no byte matching on the sigreturn trampoline at all -- it
// reads the kernel's frame and nothing else.
//
// Layout note: the vocabulary types the per-arch headers are written in terms
// of live in comparer-types.h, which both they and this file include, so
// neither depends on the other being included first. The one arch-neutral
// helper written in terms of `ComparerArch` -- LooksLikeNonLocalTransfer, the
// gate in front of TryRecognizeSignalEntry -- sits next to its caller in
// backtrace-comparer.cc.
#ifndef COMPARER_ARCH_H_
#define COMPARER_ARCH_H_

#include "comparer-types.h"

#if defined(__x86_64__)
#include "comparer-arch-x86_64.h"
#elif defined(__aarch64__)
#include "comparer-arch-aarch64.h"
#else
#error "backtrace-comparer has no ComparerArch for this architecture"
#endif

#endif  // COMPARER_ARCH_H_
