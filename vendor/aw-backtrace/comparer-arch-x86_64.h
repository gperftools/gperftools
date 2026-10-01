/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// x86-64 slice of the comparer's arch seam. Included by comparer-arch.h,
// which defines the vocabulary types used below; see the contract there.
#ifndef COMPARER_ARCH_X86_64_H_
#define COMPARER_ARCH_X86_64_H_

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>

#include "comparer-types.h"

namespace aw_backtrace_comparer {

struct ComparerArch {
  // Longest x86-64 instruction. Only used to tell "the pc advanced to the
  // next instruction" from "the pc jumped".
  static constexpr uint64_t kMaxInsnLength = 15;

  static uint64_t PC(const ucontext_t* uc) {
    return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
  }

  static uint64_t SP(const ucontext_t* uc) {
    return static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RSP]);
  }

  static ControlTransfer ClassifyInsn(const ucontext_t* uc) {
    const uint8_t* at_rip = reinterpret_cast<const uint8_t*>(PC(uc));
    if (IsAtSigreturn(at_rip, uc)) {
      return ControlTransfer::kReturn;
    }
    at_rip = EatPrefixes(at_rip);
    if (IsAtCallInstruction(at_rip)) {
      return ControlTransfer::kCall;
    }
    if (IsAtReturnInstruction(at_rip)) {
      return ControlTransfer::kReturn;
    }
    return ControlTransfer::kNone;
  }

  // A call classified on the previous step has retired; `uc` / `sp` describe
  // the register file it landed in (they must be the same register file --
  // see the ResolvePendingTransfer comment in backtrace-comparer.cc). `call`
  // pushed the return address, so it is at *sp, and the matching `ret` pops
  // it: the entry retires at sp + 8.
  static ShadowEntry EntryForRetiredCall(const ucontext_t* uc, uint64_t sp) {
    (void)uc;
    uint64_t return_address = *reinterpret_cast<const uint64_t*>(sp);
    return ShadowEntry{return_address, sp + 8};
  }

  // The frame the kernel builds at the new sp when it delivers a signal, from
  // arch/x86/include/asm/sigframe.h. qemu-user builds the same shape for the
  // guest, so this reads the same emulated or not. Nothing here looks at the
  // siginfo_t that follows uc.
  struct KernelSigFrame {
    uint64_t pretcode;  // return address into the sigreturn trampoline
    ucontext_t uc;      // the interrupted register file
  };
  static_assert(offsetof(KernelSigFrame, uc) == 8, "rt_sigframe's ucontext must directly follow pretcode");

  // Recognize a signal delivery in the frame at `sp`. Gated on
  // LooksLikeNonLocalTransfer, so every read below is a small fixed offset off
  // a stack pointer we have just seen the target running on and this cannot be
  // reached with a wild sp.
  //
  // prev_sp is the tie back to the previous step: nothing of the target's ran
  // between it and the delivery, so the frame's saved sp is the one we last
  // saw -- give or take the push/pop/call/ret that retired just before an
  // asynchronous signal landed. (For a fault -- SIGILL, SIGSEGV -- the saved
  // pc is the instruction we last stepped, exactly; for an async signal it is
  // one we have not stepped yet.)
  //
  // The saved pc is deliberately *not* rejected for being zero. Zero is a real
  // state, not a corrupt frame: a `call` through a null pointer pushes the
  // return address and only then faults on the fetch at 0 -- AGENT.md 4.1, and
  // amd64-leaf-test's jump-through-null case. Rejecting it here lost both
  // delivered frames *and* sent the pending call down ResolvePendingTransfer's
  // live-register-file path, which is the case that path exists to avoid.
  static bool TryRecognizeSignalEntry(const ucontext_t* uc, uint64_t sp, uint64_t prev_sp, SignalDelivery* out) {
    (void)uc;
    auto* frame = reinterpret_cast<const KernelSigFrame*>(sp);
    uint64_t pc = PC(&frame->uc);
    uint64_t interrupted_sp = SP(&frame->uc);

    int64_t sp_slack = static_cast<int64_t>(interrupted_sp) - static_cast<int64_t>(prev_sp);
    if (sp_slack < -16 || sp_slack > 16) {
      return false;
    }
    if (frame->pretcode == 0) {
      return false;
    }
    // The kernel zeroes uc_link and puts nothing but UC_FP_XSTATE |
    // UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS in uc_flags; qemu-user zeroes
    // both.
    if (frame->uc.uc_link != nullptr || frame->uc.uc_flags > 7) {
      return false;
    }

    // The interrupted pc is what the unwinder reports verbatim (it comes from
    // the register file, not from an unwind step); the rt_sigreturn restores
    // exactly this sp, so the ordinary pop retires it. The trampoline frame is
    // an ordinary one whose return address the kernel wrote rather than a
    // call, so the handler's own `ret` -- at sp + 8 -- retires it.
    *out = SignalDelivery{&frame->uc, ShadowEntry{pc, interrupted_sp}, ShadowEntry{frame->pretcode, sp + 8}};
    return true;
  }

 private:
  // Eat common x86 prefixes, to help us recognize calls and returns.
  static const uint8_t* EatPrefixes(const uint8_t* at_rip) {
    if (*at_rip == 0x66) {  // operand-size prefix
      return EatPrefixes(at_rip + 1);
    }
    if (*at_rip == 0x67) {  // address-size prefix
      return EatPrefixes(at_rip + 1);
    }
    if ((*at_rip & 0xf0) == 0x40) {  // REX prefix
      return EatPrefixes(at_rip + 1);
    }
    if (at_rip[0] == 0xf3) {  // rep prefix
      return EatPrefixes(at_rip + 1);
    }
    return at_rip;
  }

  static bool IsAtCallInstruction(const uint8_t* at_rip) {
    if (at_rip[0] == 0xe8)  // regular "constant" call
      return true;
    if (at_rip[0] == 0xff && (at_rip[1] & 0x38) == 0x10)  // indirect call
      return true;
    return false;
  }

  static bool IsAtReturnInstruction(const uint8_t* at_rip) {
    return at_rip[0] == 0xc3;
  }

  static bool IsAtSigreturn(const uint8_t* at_rip, const ucontext_t* uc) {
    if (!(at_rip[0] == 0x0f && at_rip[1] == 0x05)) {  // syscall
      return false;
    }
    return uc->uc_mcontext.gregs[REG_RAX] == SYS_rt_sigreturn;
  }
};

}  // namespace aw_backtrace_comparer

#endif  // COMPARER_ARCH_X86_64_H_
