/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_ARCH_AARCH64_H_
#define AW_ARCH_AARCH64_H_

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#include <utility>

#include "aw-structs.h"
#include "dwarf-constants.h"

namespace aw_backtrace_internal {

struct Arch {
  static constexpr int kSPReg = DWARF_SP;
  static constexpr int kFPReg = DWARF_FP;
  static constexpr int kRAReg = DWARF_LR;

  // Fast-path decoder constants (aw-backtrace-fastpath.h). The CIE's code and
  // data alignment factors are checked against these rather than decoded, so
  // the fast path accepts exactly one convention: code_align 4 -- every
  // aarch64 instruction is 4 bytes -- with data_align -8. That is what gas
  // (and every other assembler that has thought about it) emits. Current
  // clang instead emits code_align 1 / data_align -4, which is not wrong but
  // is pointlessly odd; rather than carry a second convention through the hot
  // path we let clang-built objects fall to the slow path, which decodes the
  // factors as LEBs and does not care, until clang is fixed.
  //
  // kInitialCFAOffset is 0 because the architectural CFA is sp+0: `bl` writes
  // x30 and touches no stack. That is also why the fast path cannot use a
  // zero cfa_offset as its failure sentinel (see FastPathFrame).
  static constexpr int32_t kCodeAlign = 4;  // every aarch64 instruction is 4 bytes
  static constexpr int32_t kDataAlign = -8;
  static constexpr uint32_t kInitialCFAOffset = 0;

 private:
  static int to_greg(int dwarf_reg) {
    if (dwarf_reg >= 0 && dwarf_reg <= 32) {
      return dwarf_reg;
    }
    return -1;
  }

  // Is [addr, addr + size) entirely inside bounds? Note the callers
  // pass bounds of the executable vma the pc was found in, and a
  // default-constructed {0, 0} when there was none, which this
  // rejects.
  static bool InBounds(uintptr_t addr, uintptr_t size, std::pair<uintptr_t, uintptr_t> bounds) {
    if (addr + size < addr) {
      return false;  // overflow
    }
    return bounds.first <= addr && addr + size <= bounds.second;
  }

 public:
  static bool IsValidDWARFReg(uintptr_t dwarf_reg) {
    int ireg = (int)dwarf_reg;
    return (uintptr_t)ireg == dwarf_reg && to_greg(ireg) != -1;
  }

  static uintptr_t GetDWARFReg(const ucontext_t* uc, int dwarf_reg) {
    int greg = to_greg(dwarf_reg);
    assert(greg != -1);
    if (greg == 31)
      return uc->uc_mcontext.sp;
    if (greg == 32)
      return uc->uc_mcontext.pc;
    return uc->uc_mcontext.regs[greg];
  }

  static uintptr_t CleanReturnAddress(uintptr_t addr) {
    register uintptr_t x30 __asm__("x30") = addr;
    asm("xpaclri" : "+r"(x30));
    return x30;
  }

  static Cursor CursorFromContext(const ucontext_t* uc) {
    Cursor cursor;
    cursor.pc = CleanReturnAddress(uc->uc_mcontext.pc);
    cursor.sp = uc->uc_mcontext.sp;
    cursor.fp = uc->uc_mcontext.regs[29];
    return cursor;
  }

  static Cursor InitializeUnwindForCaller(const void* bin_frame_addr) {
    struct Frame {
      uintptr_t save_fp;
      uintptr_t return_addr;
    };
    const Frame* f = static_cast<const Frame*>(bin_frame_addr);
    Cursor ret;
    ret.pc = CleanReturnAddress(f->return_addr);
    ret.fp = f->save_fp;
    ret.sp = ret.fp;
    return ret;
  }

  static bool IsSignalFrame(uintptr_t pc, uintptr_t sp, std::pair<uintptr_t, uintptr_t> pc_bounds,
                            const ucontext_t** uc_loc) {
    // Two instructions, and every aarch64 instruction is 4-byte
    // aligned. An unaligned pc cannot be pointing at the trampoline,
    // and would make the load below undefined anyways.
    if ((pc & 3) != 0) {
      return false;
    }
    // Pattern: mov x8, #0x8b; svc #0
    // 0xd2801168, 0xd4000001
    auto check_match_at = [&](uintptr_t at) -> bool {
      if (!InBounds(at, 2 * sizeof(uint32_t), pc_bounds)) {
        return false;
      }
      const uint32_t* code = reinterpret_cast<const uint32_t*>(at);
      return code[0] == 0xd2801168 && code[1] == 0xd4000001;
    };

    // Two positions to try, as on x86-64. pc at the entry is the frame
    // reached by unwinding *into* the trampoline: the handler's `ret`
    // goes there. pc at the `svc` is what a capture taken *while* the
    // trampoline runs sees. sp is the same for both: the mov does not
    // touch it. (pc - 4 wrapping is caught by InBounds' overflow check.)
    if (!check_match_at(pc) && !check_match_at(pc - sizeof(uint32_t))) {
      return false;
    }

    // On arm64 Linux, ucontext is at sp + 128 (after siginfo)
    *uc_loc = reinterpret_cast<const ucontext_t*>(sp + 128);
    return true;
  }

  static bool DetectPLTEntry(uintptr_t ip, FrameInfo* info, std::pair<uintptr_t, uintptr_t> bounds) {
    // PLT entries on AArch64 are 16-byte aligned.
    uintptr_t slot_start = ip & ~uintptr_t{0xf};

    // We look at the whole 16 byte slot, not just at ip.
    if (!InBounds(slot_start, 16, bounds)) {
      return false;
    }

    const uint32_t* code = reinterpret_cast<const uint32_t*>(slot_start);

    // Typical AArch64 PLT entry:
    // 0: adrp x16, ...
    // 4: ldr x17, [x16, ...]
    // 8: add x16, x16, ...
    // 12: br x17
    if ((code[0] & 0x9f000000) == 0x90000000 && code[3] == 0xd61f0220) {
      info->cfa = CfaRule::SpRel(0);
      info->fp = RegisterRule::SameValue();
      info->ra = RegisterRule::InReg(30);

      return true;
    }

    return DetectPLT0(ip, info, bounds);
  }

 private:
  // PLT0, the lazy-binding header every not-yet-resolved PLT entry branches
  // to. GNU ld and lld both emit
  //
  //   bti  c                        only in a BTI-marked link
  //   stp  x16, x30, [sp, #-16]!
  //   adrp x16, <GOT page>
  //   ldr  x17, [x16, #lo12]        the resolver, _dl_runtime_resolve
  //   add  x16, x16, #lo12
  //   br   x17
  //
  // with no CFI, and unlike an ordinary entry it moves sp. Past the stp the
  // CFA is sp + 16, which the x30-is-live guess would take as sp -- and the
  // caller's sp-based CFI then reads its return address out of the slot this
  // very stp filled, reporting the caller twice. x30 itself is untouched (the
  // stp only copied it), so the return address stays in the register.
  //
  // It is 32 bytes and does not fit the 16-byte slot logic above, so instead
  // try ip at each instruction position and match the whole sequence.
  static bool DetectPLT0(uintptr_t ip, FrameInfo* info, std::pair<uintptr_t, uintptr_t> bounds) {
    static constexpr uint32_t kBtiC = 0xd503245f;
    static constexpr uint32_t kStpX16X30PreIndex = 0xa9bf7bf0;
    static constexpr uint32_t kBrX17 = 0xd61f0220;

    if ((ip & 3) != 0) {
      return false;
    }
    // idx is ip's position relative to the stp; -1 is the bti in front of it.
    for (int idx = -1; idx <= 4; idx++) {
      uintptr_t stp_addr = ip - static_cast<uintptr_t>(4 * idx);
      if (!InBounds(stp_addr, 5 * sizeof(uint32_t), bounds)) {
        continue;
      }
      const uint32_t* code = reinterpret_cast<const uint32_t*>(stp_addr);
      if (code[0] != kStpX16X30PreIndex ||         // stp x16, x30, [sp, #-16]!
          (code[1] & 0x9f00001f) != 0x90000010 ||  // adrp x16
          (code[2] & 0xffc003ff) != 0xf9400211 ||  // ldr x17, [x16, #imm]
          (code[3] & 0xffc003ff) != 0x91000210 ||  // add x16, x16, #imm
          code[4] != kBrX17) {
        continue;
      }
      if (idx == -1 && (!InBounds(ip, sizeof(uint32_t), bounds) || *reinterpret_cast<const uint32_t*>(ip) != kBtiC)) {
        continue;
      }

      info->cfa = CfaRule::SpRel(idx >= 1 ? 16 : 0);
      info->fp = RegisterRule::SameValue();
      info->ra = RegisterRule::InReg(30);
      return true;
    }
    return false;
  }

  template <typename AddrChecker>
  static bool CheckPossiblePC(AddrChecker* checker, uintptr_t maybe_pc) {
    // Every aarch64 instruction is 4-byte aligned, so this is free precision
    // the x86-64 side cannot have.
    if (maybe_pc == 0 || (maybe_pc & 3) != 0) {
      return false;
    }
    // pc_vma is really std::optional<aw_addrcheck_entry>, but we avoid the
    // include here. The real AddrChecker wrapper is in the .cc anyways.
    auto pc_vma = checker->Lookup(maybe_pc);
    // Executable is all we require: execute-only text (FEAT_EPAN) is a
    // perfectly valid place for a return address to point.
    return (pc_vma && pc_vma->perm_exec);
  }

  // H2: walk the AAPCS64 frame record at fp, {caller's x29, return address}.
  //
  // The CFA -- the caller's sp -- is not recoverable from this side: the
  // record sits at the *bottom* of the frame in both gcc's and clang's
  // layouts, so the frame size, which we cannot know without unwind info,
  // separates fp from the CFA. It is recoverable from the other side.
  // *(fp) is the caller's x29, and gcc keeps x29 == sp through the whole body
  // of any function with a static frame -- which is exactly the case where it
  // also leaves the CFA sp-based. Once sp becomes dynamic gcc switches the CFA
  // to x29-relative and the value we supply goes unused (clang switches
  // always). So `cfa = *(fp)` is right in both branches, and it is the same
  // identity InitializeUnwindForCaller already relies on.
  template <typename AddrChecker>
  static bool GuessFrameRecord(Cursor cursor, AddrChecker* checker, FrameInfo* info) {
    static constexpr uintptr_t kMaxHeuristicsFrameSize = 32 << 10;

    uintptr_t fp = cursor.fp;
    // sp is always 16-aligned on aarch64, and x29 is set from it.
    if ((fp & 15) != 0 || fp < cursor.sp || fp - cursor.sp > kMaxHeuristicsFrameSize) {
      return false;
    }

    auto stack_vma = checker->Lookup(fp);
    if (!stack_vma || !stack_vma->perm_read || !stack_vma->perm_write) {
      return false;
    }
    if (fp + 16 < fp || fp + 16 > stack_vma->end) {
      return false;
    }

    const uintptr_t* record = reinterpret_cast<const uintptr_t*>(fp);
    uintptr_t saved_fp = record[0];
    uintptr_t saved_lr = CleanReturnAddress(record[1]);

    if (!CheckPossiblePC(checker, saved_lr)) {
      return false;
    }
    // Either the chain terminator libc plants in the outermost frame, or a
    // record strictly further up the stack. The +16 is the far end of the
    // bracket the CFA lives in: a caller's x29 below fp + 16 cannot be real.
    if (saved_fp != 0 && (saved_fp < fp + 16 || (saved_fp & 15) != 0)) {
      return false;
    }

    info->cfa = CfaRule::DerefFpRel(0);    // caller's sp  == caller's x29 == *(fp)
    info->fp = RegisterRule::MemFpRel(0);  // caller's x29 == *(fp)
    info->ra = RegisterRule::MemFpRel(8);  // caller's pc  == *(fp + 8)
    return true;
  }

 public:
  // Recovering a frame with no unwind info at all. Unlike x86-64, `bl` puts
  // the return address in x30 rather than on the stack, so there is nothing
  // like "the word at *sp looks like a return address" to lean on -- at the
  // entry to a callee, *sp still belongs to the caller. That leaves exactly
  // two shapes:
  //
  //   H1: x30 is live. True of a leaf that never spilled it, of the window
  //       before a prologue's stp and after an epilogue's ldp, of PLT stubs,
  //       and of a `blr` through null. CFA == sp, because such code has not
  //       moved sp either. Only usable on a frame we have a register file for.
  //
  //   H2: the AAPCS64 frame record at fp, {caller's x29, return address}.
  //
  // H1 goes first: the dominant no-CFI shape is a leaf helper that sets up
  // nothing, where fp still describes the *caller* and H2 would silently drop
  // a frame. When x30 is stale instead (the function did call something and we
  // caught it mid-body), H1 costs one spurious frame pointing back into the
  // same function -- and only one, because the next step is no longer a leaf,
  // so H2 runs off the same still-valid fp and recovers the real caller.
  // Deleting a caller is worse than repeating a callee, so that is the trade.
  //
  // Note there is no equivalent of x86-64's mid-prologue guess. On a leaf we
  // always have the register file, so x30 answers that window correctly and
  // with an exact CFA; and a non-leaf pc, being a return address, can never
  // land inside a prologue.
  template <typename AddrChecker>
  static bool GuessUnwindInfo(Cursor cursor, const ucontext_t* uc_if_leaf, AddrChecker* checker, FrameInfo* info) {
    if (uc_if_leaf != nullptr) {
      // CleanReturnAddress first: with FEAT_PAuth the register holds a signed
      // pointer, which would fail the checks below on its tag bits alone.
      uintptr_t lr = CleanReturnAddress(uc_if_leaf->uc_mcontext.regs[30]);
      if (CheckPossiblePC(checker, lr)) {
        // Exactly the architectural default: CFA = sp, RA in x30, fp
        // unchanged.
        ResetFrameInfo(info);
        return true;
      }
    }

    return GuessFrameRecord(cursor, checker, info);
  }

  static void ResetFrameInfo(FrameInfo* info) {
    info->cfa = CfaRule::SpRel(0);
    info->fp = RegisterRule::SameValue();
    info->ra = RegisterRule::InReg(30);
  }
};

}  // namespace aw_backtrace_internal

#endif  // AW_ARCH_AARCH64_H_
