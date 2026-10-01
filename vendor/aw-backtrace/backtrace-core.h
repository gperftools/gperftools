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
  // The row covering the pc has an explicit Undefined rule on the return
  // address column: a frame with nothing to return to, which is an answer
  // rather than a failure. Callers stop the walk; they must not treat it as
  // missing information and go looking for a substitute.
  kUndefinedRA,
  kFail,
  // The lookup failed specifically because it hit an unsupported
  // DW_CFA_{def_cfa_,}expression on a register/CFA we track. Callers use
  // this to try PLT/signal-frame/DRAP recovery before deciding it's a real
  // problem -- see the fallback chain in aw-backtrace.cc's UnwindLoop.
  kFailExpression,
};

// `diag` is applied immediately, inside this call, to every way a lookup can
// fail (unsupported opcodes, bogus registers, and the like) -- except an
// unsupported expression, which is reported only if
// diag.report_expression_diag is set. That lets a caller attempt recovery
// first (see the fallback chain in UnwindLoop) and, if recovery fails, call
// DoUnwindLookup again with report_expression_diag to get the diagnostic
// reported (a second full lookup, but this path is already the exceptional,
// off-the-fast-path case).
LookupOutcome DoUnwindLookup(uintptr_t lookup_ip, FrameInfo* info, DiagFlags diag);

// The half of DoUnwindLookup below LocateEHFrame: runs the CFI decoder
// against an already-built EHReaderInputs (inputs.lookup_pc is what gets
// looked up), without touching _dl_find_object. For callers that build their
// own EHReaderInputs -- e.g. an offline tool reading an mmap'd ELF file that
// isn't loaded into this process at all.
LookupOutcome DoUnwindLookupFromInputs(const EHReaderInputs& inputs, FrameInfo* info, DiagFlags diag);

EHReaderInputs* LocateEHFrame(uintptr_t lookup_ip, EHReaderInputs* inputs);

}  // namespace aw_backtrace_internal

#endif  // BACKTRACE_CORE_H_
