/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef BACKTRACE_DRAP_H_
#define BACKTRACE_DRAP_H_

#if __x86_64__

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

#include <array>
#include <optional>

#include "aw-arch.h"
#include "backtrace-core.h"
#include "eh-frame-reader.h"
#include "function_ref.h"

namespace aw_backtrace_internal {
namespace drap {

// See doc/amd64-drap-problem.adoc

enum class DRAPState {
  START,              // right after call, so everything is normal yet
  R10_CFA,            // CFA at r10; after lea 8(%rsp), %r10
  RBP_SAVED_R10_CFA,  // after push %rbp; mov %rsp, %rbp
  EXPR_CFA,           // funky expression CFA, after push %r10
};

// V is lightweight UnwindVisitor (see eh-frame-reader.h) that only
// tracks enough state to do DRAP frames. This is used after we've
// already encountered DW_CFA_expression in the main code
// (backtrace-core.cc). The state tracking is hopefully clearly seen
// in the code below.
struct V : public UnwindVisitor {
  const uintptr_t pc;
  V(uintptr_t pc) : pc(pc) {
  }

  bool success = false;
  DRAPState current_state{DRAPState::START};
  std::optional<DRAPState> saved_state;

  bool StartFDE(uintptr_t) {
    return true;
  }

  bool HandleAugS() {
    return false;
  }
  bool AfterCIE() {
    return true;
  }

  bool HandleSetLoc(uintptr_t new_pc) {
    success = (new_pc > pc);
    return !success;
  }

  bool HandleDefCFA(uintptr_t reg, uintptr_t offset) {
    if (reg == 10 && offset == 0) {
      if (current_state == DRAPState::START) {
        current_state = DRAPState::R10_CFA;
        return true;
      }
      if (current_state == DRAPState::EXPR_CFA) {
        current_state = DRAPState::RBP_SAVED_R10_CFA;
        return true;
      }
    }
    if (reg == Arch::kSPReg && offset == 8) {
      current_state = DRAPState::START;
      return true;
    }
    return false;
  }

  // DRAP uses .cfi_def_cfa and not these forms
  bool HandleDefCFAReg(uintptr_t) {
    return false;
  }
  bool HandleDefCFAOffset(uintptr_t) {
    return false;
  }

  bool HandleDefCFAExpression(std::span<const uint8_t> expr) {
    static constexpr std::array kFunkyExpr = std::to_array<const uint8_t>({0x76, 0x78, 0x06});
    if (current_state != DRAPState::RBP_SAVED_R10_CFA) {
      return false;
    }
    if (std::ranges::equal(expr, kFunkyExpr)) {
      current_state = DRAPState::EXPR_CFA;
      return true;
    }
    return false;
  }

  static bool IsCriticalReg(uintptr_t reg) {
    return (reg == Arch::kSPReg) || (reg == Arch::kRAReg) || (reg == Arch::kFPReg) || (reg == 10);
  }

  bool HandleOffset(uintptr_t reg, uintptr_t) {
    return reg == Arch::kRAReg ||
           !IsCriticalReg(reg);  // ignore cfa_offset on non-critical registers and fail on anything else
  }
  bool HandleRegister(uintptr_t reg, uintptr_t) {
    return !IsCriticalReg(reg);
  }
  bool HandleRestore(uintptr_t reg) {
    if (current_state == DRAPState::RBP_SAVED_R10_CFA && reg == Arch::kFPReg) {
      current_state = DRAPState::R10_CFA;
      return true;
    }
    return !IsCriticalReg(reg);
  }
  bool HandleSameValue(uintptr_t) {
    return false;
  }
  bool HandleUndefined(uintptr_t) {
    return false;
  }
  bool HandleValOffset(uintptr_t, uintptr_t) {
    return false;
  }
  bool HandleExpression(uintptr_t reg, std::span<const uint8_t> expr) {
    if (reg == Arch::kFPReg && current_state == DRAPState::R10_CFA) {
      static constexpr std::array kRestoreRBPExpr = std::to_array<uint8_t>({0x76, 0x00});
      if (std::ranges::equal(expr, kRestoreRBPExpr)) {
        current_state = DRAPState::RBP_SAVED_R10_CFA;
        return true;
      }
    }
    return false;
  }
  bool HandleValExpression(uintptr_t, std::span<const uint8_t>) {
    return false;
  }

  bool HandleRememberState() {
    if (saved_state.has_value()) {
      return false;  // overflow
    }
    saved_state = current_state;
    return true;
  }
  bool HandleRestoreState() {
    if (!saved_state.has_value()) {
      return false;
    }
    current_state = saved_state.value();
    saved_state.reset();
    return true;
  }

  bool ReportErrorVA(const char*, va_list) {
    return false;
  }
};

}  // namespace drap

#if AW_DEBUG_DRAP
inline __attribute__((format(printf, 1, 2))) void drap_debug_printf(const char* fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vprintf(fmt, va);
  va_end(va);
}
inline void drap_debug_trap() {
  asm volatile("int $3; nop");
}
#
#else
inline __attribute__((format(printf, 1, 2))) void drap_debug_printf(const char* fmt, ...) {
  (void)fmt;
}
inline void drap_debug_trap() {
}
#endif

inline std::optional<Cursor> TryLookupWithDRAP(uintptr_t lookup_ip, Cursor cursor, const ucontext_t* uc,
                                               FunctionRef<std::optional<uintptr_t>(uintptr_t)> read_ptr,
                                               std::pair<uintptr_t, uintptr_t> pc_bounds) {
  EHReaderInputs input_storage;
  EHReaderInputs* inputs = LocateEHFrame(lookup_ip, &input_storage);
  if (!inputs) {
    return {};
  }

  drap::V v{lookup_ip};
  FindAndDecodeFDE(*inputs, &v);
  if (!v.success) {
    return {};
  }

  std::optional<uintptr_t> sp;
  std::optional<uintptr_t> fp;
  switch (v.current_state) {
    case drap::DRAPState::START:
      drap_debug_printf("DRAP unwind at 0x%zx got START rule\n", cursor.pc);
      fp = cursor.fp;
      sp = cursor.sp + 8;
      break;
    case drap::DRAPState::R10_CFA: {
      drap_debug_printf("DRAP unwind at 0x%zx got R10_CFA rule\n", cursor.pc);
      if (!uc) {
        drap_debug_trap();
        break;
      }
      fp = cursor.fp;
      sp = uc->uc_mcontext.gregs[REG_R10];
      break;
    }
    case drap::DRAPState::RBP_SAVED_R10_CFA: {
      drap_debug_printf("DRAP unwind at 0x%zx got RBP_SAVED_R10_CFA rule\n", cursor.pc);
      if (!uc) {
        drap_debug_trap();
        break;
      }
      // As noted in the doc (also see amd64-drap-test.s) there is
      // lack of correct unwind info at the end of function after
      // r10 is set again as CFA and after original value of RBP is
      // restored (e.g. LEAVE in the .s mentioned above) and normal
      // RSP-based CFA is set. "Restoring" RBP as directed by such
      // stale unwind info will corrupt it (we'll read at the wrong
      // address). Typically lea is followed immediately after leave
      // (or pop %rbp), so we workaround. If we're in this state (so
      // either prologue or epilogue) and we see lea -8(%r10), %rsp
      // as next instruction, then we know we're after RBP is
      // restored.
      static constexpr uint8_t kLeaInsn[] = {0x49, 0x8d, 0x62, 0xf8};
      auto next_pc = cursor.pc + sizeof(kLeaInsn);
      bool safe_to_read = (pc_bounds.first <= cursor.pc && cursor.pc < next_pc && next_pc <= pc_bounds.second);
      if (!safe_to_read) {
        drap_debug_printf(
            "fetching 4 bytes at the PC for kLeaInsn check is not safe. pc = 0x%zx, bounds: [0x%zx, 0x%zx)\n",
            cursor.pc, pc_bounds.first, pc_bounds.second);
      }
      if (safe_to_read && memcmp(kLeaInsn, reinterpret_cast<const uint8_t*>(cursor.pc), sizeof(kLeaInsn)) == 0) {
        drap_debug_printf("DRAP unwind detected lea!\n");
        fp = cursor.fp;
      } else {
        // Respect unwind info even if we got here due to
        // !safe_to_read (note: next instruction could be shorter than
        // 4 bytes).
        fp = read_ptr(cursor.fp);
      }
      sp = uc->uc_mcontext.gregs[REG_R10];
      break;
    }
    case drap::DRAPState::EXPR_CFA:
      drap_debug_printf("DRAP unwind at 0x%zx got EXPR_CFA rule\n", cursor.pc);
      sp = read_ptr(cursor.fp - 8);
      fp = read_ptr(cursor.fp);
      break;
  }
  std::optional<uintptr_t> pc = (sp && fp) ? read_ptr(sp.value() - 8) : std::nullopt;
  drap_debug_printf("DRAP result: pc = 0x%zx, fp = 0x%zx, sp = 0x%zx\n", pc.value_or(~uintptr_t{}),
                    fp.value_or(~uintptr_t{}), sp.value_or(~uintptr_t{}));
  if (pc) {
    return Cursor{.pc = pc.value(), .sp = sp.value(), .fp = fp.value()};
  }

  return {};
}

}  // namespace aw_backtrace_internal

#endif  // __x86_64__

#endif  // BACKTRACE_DRAP_H_
