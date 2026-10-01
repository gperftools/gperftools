/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// aarch64 slice of the comparer's arch seam. Included by comparer-arch.h,
// which defines the vocabulary types used below; see the contract there.
//
// **The shadow stack model is the same, but for a different reason.** On
// x86-64 an entry retires at `sp + 8` because `call` pushed the return
// address and `ret` pops it. On aarch64 `bl` writes x30 and touches no stack
// at all, so a callee is entered with the *caller's* sp and a well-behaved
// `ret` is reached with that same sp again: an entry retires at exactly the
// sp the call was made with. That single difference is the whole port -- the
// shadow stack, the resync-on-mismatched-sp rule and the comparison against
// the capture are unchanged.
//
// The corollary is that `*sp` must never be consulted here. At the entry to a
// callee it still belongs to the caller, and very often holds the caller's own
// spilled x30, so x86-64's "the word at *sp looks like a return address"
// reasoning would quietly report the grandparent. Same trap as
// Arch::GuessUnwindInfo -- see AGENT.md 4.8.
//
#ifndef COMPARER_ARCH_AARCH64_H_
#define COMPARER_ARCH_AARCH64_H_

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>

#include "comparer-types.h"

namespace aw_backtrace_comparer {

struct ComparerArch {
  // Every aarch64 instruction is exactly four bytes, so "the pc advanced to
  // the next instruction" is exact here rather than the upper bound it is on
  // x86-64.
  static constexpr uint64_t kMaxInsnLength = 4;

  static uint64_t PC(const ucontext_t* uc) {
    return uc->uc_mcontext.pc;
  }

  static uint64_t SP(const ucontext_t* uc) {
    return uc->uc_mcontext.sp;
  }

  static ControlTransfer ClassifyInsn(const ucontext_t* uc) {
    uint64_t pc = PC(uc);
    // The architecture guarantees this alignment, and an unaligned load here
    // would be undefined.
    if ((pc & 3) != 0) {
      return ControlTransfer::kNone;
    }
    uint32_t insn = *reinterpret_cast<const uint32_t*>(pc);

    // bl <label>: the only direct branch that writes x30.
    if ((insn & kMaskBL) == kInsnBL) {
      return ControlTransfer::kCall;
    }

    // Everything else that can transfer through a register lives in the
    // "unconditional branch (register)" group, distinguished by its opc
    // field. Taking the whole group by opc rather than matching individual
    // encodings is what makes the pointer-authentication variants fall out
    // for free: blraa/blrab share opc with each other, and blraaz/blrabz
    // share opc with plain blr, so a -mbranch-protection build needs no
    // special case.
    if ((insn & kMaskBranchReg) == kInsnBranchReg) {
      switch ((insn >> 21) & 0xf) {
        case kOpcBLR:   // blr, blraaz, blrabz
        case kOpcBLRA:  // blraa, blrab
          return ControlTransfer::kCall;
        case kOpcRET:  // ret, retaa, retab
          return ControlTransfer::kReturn;
        default:
          // br / braa / brab are tail calls and indirect jumps: they leave
          // x30 alone, so the shadow stack entry the original call made is
          // still the right one and there is nothing to do. Code that returns
          // with a bare `br x30` instead of `ret` is therefore invisible here
          // -- the same blind spot x86-64 has for a `jmp` used as a return.
          return ControlTransfer::kNone;
      }
    }

    // A sigreturn counts as a return: it restores the sp the interrupted
    // frame was pushed with. The sigreturn trampoline is `mov x8, #139; svc
    // #0`, but nothing here matches those bytes -- we read the syscall number
    // out of the register file, exactly as the x86-64 side reads %rax.
    if (insn == kInsnSVC0 && uc->uc_mcontext.regs[8] == SYS_rt_sigreturn) {
      return ControlTransfer::kReturn;
    }

    return ControlTransfer::kNone;
  }

  // A call classified on the previous step has retired; `uc` / `sp` describe
  // the register file it landed in (they must be the same register file --
  // see the ResolvePendingTransfer comment in backtrace-comparer.cc).
  //
  // The branch wrote the return address into x30 and left sp alone, so x30 is
  // it, verbatim: we are at the callee's first instruction, which has not had
  // a chance to sign or spill it yet. The entry retires when the matching
  // `ret` is seen with the sp the call was made with, i.e. this one.
  static ShadowEntry EntryForRetiredCall(const ucontext_t* uc, uint64_t sp) {
    return ShadowEntry{uc->uc_mcontext.regs[30], sp};
  }

  // The frame the kernel builds at the new sp when it delivers a signal, from
  // arch/arm64/kernel/signal.c. qemu-user builds the same shape for the guest
  // (linux-user/aarch64/signal.c), so this reads the same emulated or not.
  // Unlike x86-64 there is no pretcode word: the kernel puts the sigreturn
  // trampoline's address in x30 instead, so the handler's own `ret` is what
  // reaches it.
  struct KernelSigFrame {
    siginfo_t info;
    ucontext_t uc;  // the interrupted register file
  };
  // aw-arch-aarch64.h's IsSignalFrame hard-codes this same 128, deliberately
  // arriving at it a different way. If this assert ever fires, that is the
  // other place to look.
  static_assert(offsetof(KernelSigFrame, uc) == 128, "rt_sigframe's ucontext must follow a 128-byte siginfo");

  // Recognize a signal delivery in the frame at `sp`. Gated on
  // LooksLikeNonLocalTransfer, so every read below is a small fixed offset off
  // a stack pointer we have just seen the target running on and this cannot be
  // reached with a wild sp.
  //
  // prev_sp is the tie back to the previous step: nothing of the target's ran
  // between it and the delivery, so the frame's saved sp is the one we last
  // saw -- give or take the instruction that retired just before an
  // asynchronous signal landed. (For a fault -- SIGILL, SIGSEGV -- the saved
  // pc is the instruction we last stepped, exactly; for an async signal it is
  // one we have not stepped yet.)
  //
  // The saved pc is deliberately *not* rejected for being zero. Zero is a real
  // state, not a corrupt frame: a `blr` through a zeroed register writes x30
  // and only then faults on the fetch at 0 -- AGENT.md 4.1, and
  // arm64-leaf-test's jump-through-null case. Rejecting it here lost both
  // delivered frames *and* sent the pending call down ResolvePendingTransfer's
  // live-register-file path, which is the case that path exists to avoid.
  static bool TryRecognizeSignalEntry(const ucontext_t* uc, uint64_t sp, uint64_t prev_sp, SignalDelivery* out) {
    if ((sp & 15) != 0) {  // the kernel always lands the frame 16-aligned
      return false;
    }
    auto* frame = reinterpret_cast<const KernelSigFrame*>(sp);
    uint64_t pc = PC(&frame->uc);
    uint64_t interrupted_sp = SP(&frame->uc);

    int64_t sp_slack = static_cast<int64_t>(interrupted_sp) - static_cast<int64_t>(prev_sp);
    if (sp_slack < -16 || sp_slack > 16) {
      return false;
    }
    // x30 is the trampoline frame we are about to push -- a delivery that did
    // not set one up is not one we can account for.
    uint64_t trampoline_pc = uc->uc_mcontext.regs[30];
    if (trampoline_pc == 0) {
      return false;
    }
    // The kernel zeroes both, and so does qemu-user. There is no aarch64
    // analogue of x86-64's UC_FP_XSTATE, so unlike there this is exact.
    if (frame->uc.uc_link != nullptr || frame->uc.uc_flags != 0) {
      return false;
    }

    // The interrupted pc is what the unwinder reports verbatim (it comes from
    // the register file, not from an unwind step); the rt_sigreturn restores
    // exactly this sp, so the ordinary pop retires it. The trampoline frame is
    // an ordinary one whose return address the kernel wrote into x30 rather
    // than a call, so the handler's own `ret` -- reached with the sp the
    // handler was entered on -- retires it.
    *out = SignalDelivery{&frame->uc, ShadowEntry{pc, interrupted_sp}, ShadowEntry{trampoline_pc, sp}};
    return true;
  }

 private:
  // bl <label>: 0b100101 imm26.
  static constexpr uint32_t kMaskBL = 0xfc000000;
  static constexpr uint32_t kInsnBL = 0x94000000;

  // Unconditional branch (register): 0b1101011 opc op2 op3 Rn op4. Only the
  // top seven bits identify the group; opc says which member.
  static constexpr uint32_t kMaskBranchReg = 0xfe000000;
  static constexpr uint32_t kInsnBranchReg = 0xd6000000;
  static constexpr uint32_t kOpcBLR = 0x1;   // blr / blraaz / blrabz
  static constexpr uint32_t kOpcRET = 0x2;   // ret / retaa / retab
  static constexpr uint32_t kOpcBLRA = 0x9;  // blraa / blrab

  static constexpr uint32_t kInsnSVC0 = 0xd4000001;
};

}  // namespace aw_backtrace_comparer

#endif  // COMPARER_ARCH_AARCH64_H_
