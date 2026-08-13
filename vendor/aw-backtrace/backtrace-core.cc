/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
#include "renamings.h"
//
#include "backtrace-core.h"
//
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>

#include "aw-arch.h"
#include "eh-frame-reader.h"
#include "utils.h"

namespace aw_backtrace_internal {

// The unwind "policy": what the CFI rows decoded by eh-frame-reader.h
// mean for the three registers we track. All the .eh_frame parsing and
// opcode decoding lives there; everything that knows about FrameInfo
// and Arch lives here.
//
// NOTE: callbacks return false to stop decoding. Stopping is how both
// success (we've reached the row covering pc_) and failure (something
// we don't support) are expressed, so outcome_ is what tells them apart.
class FrameLookupState : public UnwindVisitor {
 public:
  FrameLookupState(uintptr_t pc, DiagFlags diag) : pc_(pc), diag_(diag) {
  }

  LookupOutcome DoLookup(const EHReaderInputs& inputs, FrameInfo* info) {
    info_ = info;
    FindAndDecodeFDE(inputs, this);
    return outcome_;
  }

  // --- UnwindVisitor ---

  bool StartFDE(uintptr_t return_reg) {
    if (return_reg != Arch::kRAReg) {
      return ReportError("bogus return reg: %d", (int)return_reg);
    }
    return true;
  }

  bool HandleAugS() {
    return false;  // bail out and let higher level IsSignalFrame logic work. This is rare.
  }

  bool AfterCIE() {
    // info_ is now the CIE's initial row. See cie_matches_defaults_.
    FrameInfo defaults;
    Arch::ResetFrameInfo(&defaults);
    cie_matches_defaults_ = (*info_ == defaults);

    return true;
  }

  // Every row of the FDE is closed here, including the last one: the
  // reader invokes us one final time with function_end when the
  // instruction stream runs out.
  bool HandleSetLoc(uintptr_t new_pc) {
    if (new_pc > pc_) {
      // Found the row covering pc_.
      outcome_ = LookupOutcome::kOk;
      return false;
    }
    return true;
  }

  // NOTE: unlike eh-frame-reader.h, nothing here exits non-locally. These
  // narrowing helpers report and return false, and every caller has to
  // propagate that -- hence [[nodiscard]], so forgetting to is a compile
  // error rather than a silently truncated offset or a register that
  // quietly became %rax.
  template <typename T>
  [[nodiscard]] ALWAYS_INLINE bool NarrowOffset(T offset, const char* where, int32_t* out) {
    *out = static_cast<int32_t>(offset);
    if (PREDICT_FALSE(static_cast<uintptr_t>(*out) != static_cast<uintptr_t>(offset))) {
      return ReportError("%s: signed 32-bit offset overflow: %zu", where, (size_t)offset);
    }
    return true;
  }

  [[nodiscard]] ALWAYS_INLINE bool NarrowReg(uintptr_t reg, const char* where, int8_t* out) {
    if (PREDICT_FALSE(!Arch::IsValidDWARFReg(reg))) {
      return ReportError("%s: unexpected register: %zu", where, reg);
    }
    *out = static_cast<int8_t>(reg);
    return true;
  }

  bool HandleDefCFA(uintptr_t reg, uintptr_t offset) {
    int32_t o32;
    if (!NarrowOffset(offset, "def_cfa", &o32)) {
      return false;
    }
    // Note, we normally expect CFA to be based on stack pointer or
    // frame pointer register. Because those are registers we're
    // "unwinding" as part of grabbing the backtrace. But in
    // "is_leaf" frames where we have ucontext we're perfectly
    // capable of using any register. gcc amd64 DRAP stuff
    // requires it (uses either r10 or r13). And this is why we
    // have distinct SP and FP rules, even if general CfaRule::RegRel
    // subsumes them in theory.
    if (reg == Arch::kSPReg) {
      info_->cfa = CfaRule::SpRel(o32);
    } else if (reg == Arch::kFPReg) {
      info_->cfa = CfaRule::FpRel(o32);
    } else {
      int8_t r;
      if (!NarrowReg(reg, "def_cfa", &r)) {
        return false;
      }
      info_->cfa = CfaRule::RegRel(r, o32);
    }
    return true;
  }

  bool HandleDefCFAReg(uintptr_t reg) {
    return HandleDefCFA(reg, static_cast<uintptr_t>(intptr_t{info_->cfa.offset}));
  }

  bool HandleDefCFAOffset(uintptr_t offset) {
    // Only valid for "register" rules. Used by DW_CFA_def_cfa_offset{,_sf} commands
    switch (info_->cfa.kind) {
      case CfaRule::Kind::SpRel:
      case CfaRule::Kind::FpRel:
      case CfaRule::Kind::RegRel:
        return NarrowOffset(offset, "def_cfa_offset{,_sf}", &info_->cfa.offset);
      default:
        return ReportError("def_cfa_offset{,_sf} on unsupported cfa rule");
    }
  }

  bool HandleDefCFAExpression(std::span<const uint8_t> expr) {
    return ReportExpressionFailure(~uintptr_t{}, expr);
  }

  bool HandleOffset(uintptr_t reg, uintptr_t new_offset) {
    if (reg == Arch::kFPReg) {
      int32_t o32;
      if (!NarrowOffset(new_offset, "offset", &o32)) {
        return false;
      }
      info_->fp = RegisterRule::MemCfaRel(o32);
    } else if (reg == Arch::kRAReg) {
      int32_t o32;
      if (!NarrowOffset(new_offset, "offset", &o32)) {
        return false;
      }
      info_->ra = RegisterRule::MemCfaRel(o32);
    } else if (reg == Arch::kSPReg) {
      return ReportError("saved offset for SP is not supported");
    }
    return true;
  }

  // NOTE: stored_in_reg is only validated on the branches that keep it.
  // For everything else we don't look at it at all -- see the comment at
  // the bottom about ignoring unknown registers.
  bool HandleRegister(uintptr_t reg, uintptr_t stored_in_reg) {
    if (reg == Arch::kRAReg) {
      int8_t r;
      if (!NarrowReg(stored_in_reg, "register", &r)) {
        return false;
      }
      info_->ra = RegisterRule::InReg(r);
    } else if (reg == Arch::kFPReg) {
      int8_t r;
      if (!NarrowReg(stored_in_reg, "register", &r)) {
        return false;
      }
      info_->fp = RegisterRule::InReg(r);
    } else if (IsCriticalReg(reg)) {
      return ReportError("register for critical register %zu", reg);
    }
    // non-critical or even unknown registers (e.g. upcoming intel
    // APX) we simply ignore
    return true;
  }

  bool HandleRestore(uintptr_t reg) {
    if (IsCriticalReg(reg)) {
      // DW_CFA_restore is defined as restoring the rule the CIE's
      // initial instructions set up, and we implement it as restoring
      // the architectural defaults instead. When the CIE turns out to
      // have set up something else we have no idea what the right rule
      // is, so fail the frame rather than silently unwind with the wrong
      // one.
      if (!cie_matches_defaults_) {
        return ReportError("restore of critical register %lu with non-default CIE initial rules", (unsigned long)reg);
      }

      FrameInfo defaults;
      Arch::ResetFrameInfo(&defaults);
      if (reg == Arch::kFPReg) {
        info_->fp = defaults.fp;
      } else if (reg == Arch::kRAReg) {
        info_->ra = defaults.ra;
      }
    }
    return true;
  }

  bool HandleSameValue(uintptr_t reg) {
    if (!IsCriticalReg(reg)) {
      return true;
    }
    if (reg == Arch::kFPReg) {
      info_->fp = RegisterRule::SameValue();
      return true;
    }
    return ReportError("same_value for critical register %lu", (unsigned long)reg);
  }

  bool HandleUndefined(uintptr_t reg) {
    if (!IsCriticalReg(reg)) {
      return true;
    }
    if (reg == Arch::kRAReg) {
      // In startup object we actually have a case were RIP is set to
      // undefined because there is nothing to return to. While we bail
      // out in this case (nothing to backtrace further), lets not
      // report any specific errors, since it is "normal".
      //
      // NOTE: we used to only stay quiet when this was the FDE's last
      // instruction, which is the only place it legitimately occurs.
      // The reader doesn't show us the position past the instruction
      // we're being told about, so we can't tell anymore. Either way
      // the frame fails; only the diagnostic differs.
      return false;
    }
    return ReportError("undefined for critical register %lu", (unsigned long)reg);
  }

  bool HandleValOffset(uintptr_t reg, uintptr_t offset) {
    (void)offset;
    if (IsCriticalReg(reg)) {
      return ReportError("val_offset{,_sf} for critical register %lu", (unsigned long)reg);
    }
    return true;
  }

  bool HandleExpression(uintptr_t reg, std::span<const uint8_t> expr) {
    // Same as HandleDefCFAExpression. But we simply ignore expressions
    // for registers we don't care about
    if (!IsCriticalReg(reg)) {
      return true;
    }

    return ReportExpressionFailure(reg, expr);
  }

  bool HandleValExpression(uintptr_t reg, std::span<const uint8_t> expr) {
    (void)expr;
    if (IsCriticalReg(reg)) {
      return ReportError("val_expression for critical register %lu", (unsigned long)reg);
    }
    return true;
  }

  bool HandleRememberState() {
    if (state_stack_size_ >= kStateStackSize) {
      return ReportError("CFA state stack overflow");
    }
    state_stack_[state_stack_size_++] = *info_;
    return true;
  }

  bool HandleRestoreState() {
    if (state_stack_size_ == 0) {
      return ReportError("CFA state stack underflow");
    }
    *info_ = state_stack_[--state_stack_size_];
    return true;
  }

  // Invoked by the reader for the errors it detects itself.
  NEVER_INLINE bool ReportErrorVA(const char* fmt, va_list va) {
    if (diag_.report) {
      char msg[1024];
      size_t used = (size_t)snprintf(msg, sizeof(msg), "error! diagnostics: ");
      used += (size_t)vsnprintf(msg + used, sizeof(msg) - used, fmt, va);
      if (used < sizeof(msg) - 1) {
        msg[used++] = '\n';
        msg[used] = '\0';
      }
      ssize_t written = write(2, msg, std::min<size_t>(used, sizeof(msg) - 1));
      (void)written;  // ignore if write of diagnostics failed
    }
    if (diag_.trap) {
      __builtin_trap();
    }
    return false;
  }

  NEVER_INLINE __attribute__((format(printf, 2, 3))) bool ReportError(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    // Note, packaging of va_list is ugly and won't be used every
    // time. But this is slow-path anyways and helps defeat compiler
    // warnings.
    bool rv = ReportErrorVA(fmt, va);
    va_end(va);
    return rv;
  }

 private:
  // Builds the "unsupported dwarf expression" diagnostic under diag_.report_expression_diag.
  bool ReportExpressionFailure(uintptr_t reg, std::span<const uint8_t> expr) {
    outcome_ = LookupOutcome::kFailExpression;
    if (!diag_.report_expression_diag) {
      return false;
    }
    char hex[64];
    size_t pos = 0;
    size_t shown = std::min<size_t>(expr.size(), 8);
    for (size_t i = 0; i < shown && pos + 8 < sizeof(hex); i++) {
      pos += static_cast<size_t>(snprintf(hex + pos, sizeof(hex) - pos, "0x%x ", expr[i]));
    }
    if (shown < expr.size() && pos + 4 < sizeof(hex)) {
      snprintf(hex + pos, sizeof(hex) - pos, "...");
    }
    return ReportError("at location: 0x%zx: unsupported dwarf expression for reg/cfa: 0x%zx. Expr (len %zu): %s", pc_,
                       reg, expr.size(), hex);
  }

  static bool IsCriticalReg(uintptr_t reg) {
    return reg == Arch::kFPReg || reg == Arch::kRAReg || reg == Arch::kSPReg;
  }

  const uintptr_t pc_;
  const DiagFlags diag_;

  FrameInfo* info_ = nullptr;
  LookupOutcome outcome_ = LookupOutcome::kFail;

  static constexpr int kStateStackSize = 4;
  FrameInfo state_stack_[kStateStackSize];
  int state_stack_size_ = 0;

  // Whether the row the CIE's initial instructions established is the
  // architectural default one, which is what makes our approximation
  // of DW_CFA_restore correct (see HandleRestore). AfterCIE sets it for
  // real once those instructions have run.
  //
  // It starts out true because while the CIE's own initial instructions
  // are running there is no "rule the CIE established" yet, so a
  // DW_CFA_restore in there -- which the spec gives no meaning to
  // anyway -- just takes the defaults.
  bool cie_matches_defaults_ = true;
};

EHReaderInputs* LocateEHFrame(uintptr_t lookup_ip, EHReaderInputs* inputs) {
  struct dl_find_object dlfo;

  if (_dl_find_object(reinterpret_cast<void*>(lookup_ip), &dlfo) != 0) {
    return nullptr;
  }

  const void* eh_hdr = dlfo.dlfo_eh_frame;
  if (!eh_hdr) {
    // sometimes in 'all static' binaries eh_frame{,_hdr} bits are missing
    return nullptr;
  }

  inputs->lookup_pc = lookup_ip;
  inputs->eh_frame_hdr = reinterpret_cast<uintptr_t>(eh_hdr);
  inputs->eh_frame_start = inputs->map_start = reinterpret_cast<uintptr_t>(dlfo.dlfo_map_start);
  inputs->eh_frame_end = inputs->map_end = reinterpret_cast<uintptr_t>(dlfo.dlfo_map_end);

  // We need bounds for the eh_frame{,_hdr} stuff, for safeness of
  // accesses reading eh_frame stuff. Those bits are often one
  // contiguous "memory segment" with .text (with our lookup_pc) and
  // other stuff. But sometimes it is disjoint (frankly, cases I see
  // look like "off by couple bytes" in lld, but I don't mind;
  // e.g. see 0x2ff4 versus 0x4000 below).
  //
  // Program Headers:
  // Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  // PHDR           0x000040 0x0000000000000040 0x0000000000000040 0x0002a0 0x0002a0 R   0x8
  // INTERP         0x0002e0 0x00000000000002e0 0x00000000000002e0 0x00001c 0x00001c R   0x1
  //     [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
  // LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x002ff4 0x002ff4 R   0x1000
  // LOAD           0x003000 0x0000000000004000 0x0000000000004000 0x00de80 0x00de80 R E 0x1000
  if (!(dlfo.dlfo_map_start <= eh_hdr && eh_hdr < dlfo.dlfo_map_end)) {
    if (_dl_find_object(reinterpret_cast<void*>(inputs->eh_frame_hdr), &dlfo) == 0) {
      inputs->eh_frame_start = reinterpret_cast<uintptr_t>(dlfo.dlfo_map_start);
      inputs->eh_frame_end = reinterpret_cast<uintptr_t>(dlfo.dlfo_map_end);
    } else {
      // Okay this is too broken. Better safe than sorry.
      return nullptr;
    }
  }

  return inputs;
}

LookupOutcome DoUnwindLookup(uintptr_t lookup_ip, FrameInfo* info, DiagFlags diag) {
  EHReaderInputs inputs_storage;
  EHReaderInputs* inputs = LocateEHFrame(lookup_ip, &inputs_storage);
  if (!inputs) {
    return LookupOutcome::kFail;
  }

  FrameLookupState lookup{lookup_ip, diag};

  Arch::ResetFrameInfo(info);
  return lookup.DoLookup(*inputs, info);
}

}  // namespace aw_backtrace_internal
