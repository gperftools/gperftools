/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef COMPARER_TYPES_H_
#define COMPARER_TYPES_H_

#include <stdint.h>
#include <ucontext.h>

namespace aw_backtrace_comparer {

// One shadow-stack entry, as the arch describes it: a return address, and the
// sp the matching return will be observed with. The second half is what lets
// a single return retire several entries at once -- which is what a longjmp
// leaves behind, having restored sp with no returns at all. See the
// BacktraceBuffer comments in backtrace-comparer.cc.
struct ShadowEntry {
  uint64_t pc;
  uint64_t retire_sp;
};

// What the instruction at the trapped pc is about to do to the call stack. A
// sigreturn counts as kReturn: it restores the sp the interrupted frame was
// pushed with, so the ordinary pop retires it.
enum class ControlTransfer {
  kNone,
  kCall,
  kReturn,
};

// The frames a recognized signal delivery inserts without a call, and the
// kernel-built context they came out of.
struct SignalDelivery {
  // The interrupted frame's pc is re-read from here at every comparison,
  // never remembered: a handler is free to edit the context it is going to
  // resume through, and the unwinder reports the edited value.
  // aw-backtrace-test's SIGILL handler does exactly that.
  const ucontext_t* interrupted_uc;
  ShadowEntry interrupted;
  ShadowEntry trampoline;
};

}  // namespace aw_backtrace_comparer

#endif  // COMPARER_TYPES_H_
