/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef BACKTRACE_CORE_H_
#define BACKTRACE_CORE_H_

#include <stdint.h>

#include "aw-structs.h"
#include "eh-frame-reader.h"

namespace aw_backtrace_internal {

// Whether a diagnostic gets emitted at all, and whether emitting it is
// followed by __builtin_trap(). {} (both false) means "stay quiet."
struct DiagFlags {
  bool report = false;
  bool trap = false;
  bool report_expression_diag = false;
};

enum class LookupOutcome {
  kOk,
  kFail,
  // The lookup failed specifically because it hit an unsupported
  // DW_CFA_{def_cfa_,}expression on a register/CFA we track. Callers use
  // this to try PLT/signal-frame/DRAP recovery before deciding it's a real
  // problem -- see the fallback chain in aw-backtrace.cc's UnwindLoop.
  kFailExpression,
};

// `diag` covers every other way a lookup can fail (unsupported opcodes,
// bogus registers, and the like) and is applied immediately, inside this
// call. `expr_diag` applies only when the outcome is kFailExpression, so a
// caller that wants to attempt recovery first can pass DiagFlags{} here and,
// if recovery fails, call DoUnwindLookup again with real flags to get the
// diagnostic reported (a second full lookup, but this path is already the
// exceptional, off-the-fast-path case).
LookupOutcome DoUnwindLookup(uintptr_t lookup_ip, FrameInfo* info, DiagFlags diag);

EHReaderInputs* LocateEHFrame(uintptr_t lookup_ip, EHReaderInputs* inputs);

}  // namespace aw_backtrace_internal

#endif  // BACKTRACE_CORE_H_
