/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_ARCH_X86_64_H_
#define AW_ARCH_X86_64_H_

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>

#include <array>
#include <optional>
#include <span>
#include <utility>

#include "aw-structs.h"
#include "dwarf-constants.h"

namespace aw_backtrace_internal {

struct Arch {
  static constexpr int kSPReg = DWARF_RSP;
  static constexpr int kFPReg = DWARF_RBP;
  static constexpr int kRAReg = DWARF_RIP;

  // Fast-path decoder constants (aw-backtrace-fastpath.h). The CIE's code
  // and data alignment factors are checked against these rather than
  // decoded, so a toolchain emitting anything else falls back to the slow
  // path instead of being mis-decoded. Every x86-64 toolchain agrees on
  // code_align 1 / data_align -8, so the scaling folds away entirely.
  // kInitialCFAOffset is the CFA the architectural default row implies,
  // i.e. what `call` pushed.
  static constexpr int32_t kCodeAlign = 1;
  static constexpr int32_t kDataAlign = -8;
  static constexpr uint32_t kInitialCFAOffset = 8;

 private:
  static int to_greg(int dwarf_reg) {
    switch (dwarf_reg) {
      case DWARF_RAX:
        return REG_RAX;
      case DWARF_RDX:
        return REG_RDX;
      case DWARF_RCX:
        return REG_RCX;
      case DWARF_RBX:
        return REG_RBX;
      case DWARF_RSI:
        return REG_RSI;
      case DWARF_RDI:
        return REG_RDI;
      case DWARF_RBP:
        return REG_RBP;
      case DWARF_RSP:
        return REG_RSP;
      case DWARF_RIP:
        return REG_RIP;
      case DWARF_R8:
        return REG_R8;
      case DWARF_R9:
        return REG_R9;
      case DWARF_R10:
        return REG_R10;
      case DWARF_R11:
        return REG_R11;
      case DWARF_R12:
        return REG_R12;
      case DWARF_R13:
        return REG_R13;
      case DWARF_R14:
        return REG_R14;
      case DWARF_R15:
        return REG_R15;
      default:
        return -1;
    }
  }

 public:
  static bool IsValidDWARFReg(uintptr_t dwarf_reg) {
    int ireg = (int)dwarf_reg;
    return (uintptr_t)ireg == dwarf_reg && to_greg(ireg) != -1;
  }

  static uintptr_t GetDWARFReg(const ucontext_t* uc, int dwarf_reg) {
    int greg = to_greg(dwarf_reg);
    assert(greg != -1);
    return static_cast<uintptr_t>(uc->uc_mcontext.gregs[greg]);
  }

  static Cursor CursorFromContext(const ucontext_t* uc) {
    Cursor cursor;
    cursor.pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    cursor.sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
    cursor.fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    return cursor;
  }

  static Cursor InitializeUnwindForCaller(const void* bin_frame_addr) {
    struct Frame {
      uintptr_t save_fp;
      uintptr_t return_addr;
    };
    const Frame* f = static_cast<const Frame*>(bin_frame_addr);
    Cursor ret;
    ret.pc = f->return_addr;
    ret.fp = f->save_fp;
    ret.sp = reinterpret_cast<uintptr_t>(f + 1);
    return ret;
  }

  static uintptr_t CleanReturnAddress(uintptr_t addr) {
    return addr;
  }

  static bool IsSignalFrame(uintptr_t pc, uintptr_t sp, std::pair<uintptr_t, uintptr_t> pc_bounds,
                            const ucontext_t** uc_loc) {
    if (!(pc_bounds.first <= pc && pc < pc_bounds.second)) {
      return false;
    }
    // Note, pattern 1 is what we currently have. But pattern 2 is
    // more logical equivalent (load into %eax is a little shorter
    // encoding). So we check both.
    //
    // Pattern 1: 48 c7 c0 0f 00 00 00; 0f 05 (mov $15, %rax; syscall)
    static constexpr std::array kMovRAX =
        std::to_array<uint8_t>({0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05});
    // Pattern 2: b8 0f 00 00 00; 0f 05 (mov $15, %eax; syscall)
    static constexpr std::array kMovEAX = std::to_array<uint8_t>({0xb8, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05});

    auto check_match_at = [&](std::span<const uint8_t> pattern, uintptr_t at) -> bool {
      if (at < pc_bounds.first || pattern.size() > pc_bounds.second - at) {
        return false;
      }
      return memcmp(pattern.data(), reinterpret_cast<const uint8_t*>(at), pattern.size()) == 0;
    };

    auto check_match = [&](std::span<const uint8_t> pattern) -> bool {
      // Two positions to try. pc at the entry is the frame reached by
      // unwinding *into* the trampoline: its return address is what the
      // kernel put in the signal frame. pc at the `syscall` -- the last two
      // bytes of either pattern -- is what a capture taken *while* the
      // trampoline runs sees, i.e. a sample landing on that one instruction.
      // sp is the same for both: the mov does not touch it.
      return check_match_at(pattern, pc) || check_match_at(pattern, pc - (pattern.size() - 2));
    };

    if (!check_match(kMovRAX) && !check_match(kMovEAX)) {
      return false;
    }

    *uc_loc = reinterpret_cast<const ucontext_t*>(sp);
    return true;
  }

  static bool DetectPLTEntry(uintptr_t ip, FrameInfo* info, std::pair<uintptr_t, uintptr_t> bounds) {
    // PLT entries on x86-64 are 16-byte aligned.
    uintptr_t slot_start = ip & ~uintptr_t{0xf};  // yes, we round _back_ to the start of possible plt entry
    uintptr_t slot_offset = ip % 16;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(slot_start);

    if (slot_start + 16 < slot_start || !(bounds.first <= slot_start && slot_start + 16 <= bounds.second)) {
      return false;
    }

    auto fill_sp_rel = [&](int rel) -> bool {
      info->fp = RegisterRule::SameValue();
      info->ra = RegisterRule::MemCfaRel(-8);
      info->cfa = CfaRule::SpRel(rel);
      return true;
    };

    auto match_at = [&](size_t at, std::initializer_list<uint8_t> pattern) {
      return memcmp(code + at, pattern.begin(), pattern.size()) == 0;
    };

    // Classic PLT check:
    // 0: ff 25 ... (jmpq *GOT(%rip))
    // 6: 68 ...    (pushq $index)
    // 11: e9 ...   (jmpq rel32)
    if (code[0] == 0xff && code[1] == 0x25 && code[6] == 0x68 && code[11] == 0xe9) {
      if (slot_offset == 11) {
        // past push
        return fill_sp_rel(16);
      }
      if (slot_offset == 0 || slot_offset == 6) {
        return fill_sp_rel(8);
      }
      return false;
    }

    // PLT0 (The resolver header):
    // 0: ff 35 ... (pushq GOT+8(%rip))
    // 6: ff 25 ... (jmpq *GOT+16(%rip))
    //
    // Both PLT0 and the IBT entries below may carry an MPX-era `bnd` (0xf2)
    // prefix on their branches, e.g. Ubuntu 22.04's binutils 2.38 emits
    // `f2 ff 25` / `f2 e9`. The prefix is part of the branch instruction,
    // so the offsets we match on are unchanged.
    auto skip_bnd = [&](int i) { return i + (code[i] == 0xf2); };

    if (int j = skip_bnd(6); code[0] == 0xff && code[1] == 0x35 && code[j] == 0xff && code[j + 1] == 0x25) {
      if (slot_offset == 0) {
        return fill_sp_rel(16);
      }
      if (slot_offset == 6) {
        return fill_sp_rel(24);
      }
      return false;
    }

    // Modern IBT PLT check (endbr64 prefix)
    //
    // plt entries look like this:
    // 402030:       f3 0f 1e fa             endbr64
    // 402034:       68 00 00 00 00          push   $0x0
    // 402039:       e9 e2 ff ff ff          jmp    402020 <_init+0x20>
    //
    // plt.sec entries like this:
    // 00000000004025d0 <printf@plt>:
    // 4025d0:       f3 0f 1e fa             endbr64
    // 4025d4:       ff 25 66 73 36 00       jmp    *0x367366(%rip)        # 769940 <printf@GLIBC_2.2.5>
    //
    // With bnd prefix (Ubuntu 22.04) the branches are `f2 e9 ...` and
    // `f2 ff 25 ...`.

    if (match_at(0, {0xf3, 0x0f, 0x1e, 0xfa})) {
      // 0xff 0x25 <offset> is rip-relative indirect jump: jmp *<offset>(%rip)
      if (int j = skip_bnd(4); code[j] == 0xff && code[j + 1] == 0x25) {
        if (slot_offset == 0 || slot_offset == 4) {
          return fill_sp_rel(8);
        }
        return false;
      }
      // 0x68 is push <32-bit literal>
      // 0xe9 is jump with 32-bit relative offset
      if (code[4] == 0x68 && code[skip_bnd(9)] == 0xe9) {
        if (slot_offset == 9) {
          return fill_sp_rel(16);
        }
        if (slot_offset == 0 || slot_offset == 4) {
          return fill_sp_rel(8);
        }
        return false;
      }
    }

    // MPX-style PLT (`ld -z bndplt` without IBT), lazy stub in .plt. Can be reproduced for inspection like this (on
    // e.g. ubuntu 22.04 container):
    //
    // cd v/pstepper
    // LDFLAGS='-Wl,-z,bndplt' CFLAGS='-O2 -fcf-protection=none' ./genbuild.rb ninja
    //
    // 0: 68 ...    (push $index)
    // 5: f2 e9 ... (bnd jmp PLT0)
    // 11: 0f 1f 44 00 00 (nopl)
    if (code[0] == 0x68 && code[skip_bnd(5)] == 0xe9) {
      if (slot_offset == 5) {
        // past push
        return fill_sp_rel(16);
      }
      if (slot_offset == 0) {
        return fill_sp_rel(8);
      }
      return false;
    }

    // ... and its .plt.sec, and GNU ld's non-lazy .plt.got. Their entries are
    // only 8 bytes, so two per slot:
    // 0: f2 ff 25 ... (bnd jmp *GOT(%rip)); 7: 90 (nop)   -- bnd flavor
    // 0: ff 25 ...    (jmp *GOT(%rip));     6: 66 90      -- plain flavor
    {
      uintptr_t entry_offset = slot_offset & 8;
      const uint8_t* entry = code + entry_offset;
      bool bnd = entry[0] == 0xf2 && entry[1] == 0xff && entry[2] == 0x25 && entry[7] == 0x90;
      bool plain = entry[0] == 0xff && entry[1] == 0x25 && entry[6] == 0x66 && entry[7] == 0x90;
      if (bnd || plain) {
        return slot_offset == entry_offset && fill_sp_rel(8);
      }
    }

    bool has_extra_16b = (slot_start < slot_start + 32 && slot_start + 32 <= bounds.second);

    // lld -z retpolineplt. The indirect jump is replaced by a call to a
    // thunk that overwrites its own return address with the target and
    // rets. Entries are 32 bytes (lazy) or 16 bytes (with -z now) and the
    // header is 48 or 32 bytes, so we identify each 16-byte slot by content.
    // See lld/ELF/Arch/X86_64.cpp, Retpoline and RetpolineZNow.
    //
    // The pcs below that are never executed architecturally (the pause/lfence
    // loop the thunk's call "returns" to) are deliberately not matched.

    // The thunk: 4c 89 1c 24 (mov %r11, (%rsp)); c3 (ret). Reached by a call,
    // so the CFA is 16 here. (In the lazy variant it is also reachable from
    // PLT0's own call, i.e. while resolving, where it is really 32. We can't
    // tell those apart from the code bytes and go with the common case.)
    if (match_at(0, {0x4c, 0x89, 0x1c, 0x24, 0xc3})) {
      return (slot_offset == 0 || slot_offset == 4) && fill_sp_rel(16);
    }
    // -z now entry: mov GOT, %r11; jmp plt0
    if (match_at(0, {0x4c, 0x8b, 0x1d}) && code[7] == 0xe9 && match_at(12, {0xcc, 0xcc, 0xcc, 0xcc})) {
      return (slot_offset == 0 || slot_offset == 7) && fill_sp_rel(8);
    }
    // -z now header, first slot: call next; pause; lfence; jmp loop; int3...
    if (match_at(0, {0xe8, 0x0b, 0, 0, 0, 0xf3, 0x90, 0x0f, 0xae, 0xe8, 0xeb, 0xf9})) {
      return slot_offset == 0 && fill_sp_rel(8);
    }
    // lazy entry, first slot: mov GOT, %r11; call thunk; jmp loop
    if (match_at(0, {0x4c, 0x8b, 0x1d}) && code[7] == 0xe8 && code[12] == 0xe9) {
      return (slot_offset == 0 || slot_offset == 7) && fill_sp_rel(8);
    }
    // lazy entry, second slot (starts inside the previous jmp's rel32):
    // 1: 68 idx (push); 6: e9 rel32 (jmp plt0); int3 padding
    if (code[1] == 0x68 && code[6] == 0xe9 && match_at(11, {0xcc, 0xcc, 0xcc, 0xcc, 0xcc})) {
      if (slot_offset == 1) {
        // reached by the thunk's ret, same stack as entry
        return fill_sp_rel(8);
      }
      if (slot_offset == 6) {
        // past push
        return fill_sp_rel(16);
      }
      return false;
    }
    // lazy header, first slot: push GOT+8; mov GOT+16, %r11; call next
    if (has_extra_16b && code[0] == 0xff && code[1] == 0x35 && match_at(6, {0x4c, 0x8b, 0x1d}) &&
        match_at(13, {0xe8, 0x0e, 0, 0, 0})) {  // rel32 spills into the next slot
      if (slot_offset == 0) {
        return fill_sp_rel(16);
      }
      if (slot_offset == 6 || slot_offset == 13) {
        return fill_sp_rel(24);
      }
      return false;
    }

    // glibc's runtime PLT rewrite (tunable glibc.cpu.plt_rewrite, glibc >=
    // 2.39; sysdeps/x86_64/dl-machine.h:x86_64_rewrite_plt). For objects
    // linked `-z now -z mark-plt`, ld.so may, after relocation, overwrite an
    // ordinary lazy-style .plt entry in place with a single direct branch:
    //
    //   [endbr64] e9 <rel32>        -- direct jmp, when the target is
    //                                   within 32-bit reach (the common case)
    //   [endbr64] d5 00 a1 <abs64>  -- jmpabs, APX-only, when it isn't
    //                                   (JMPABS_INSN_OPCODE is emitted as
    //                                   bytes d5 00 a1)
    //
    // endbr64 survives only if the object has IBT enabled. Either way there
    // is no push and nothing else touches the stack before the branch, so
    // cfa is sp+8 for the whole entry -- unlike every other shape above,
    // there is no offset-dependent case here. Whatever's left of the entry
    // is padded with int3 (0xcc); requiring that padding is what keeps this
    // check (otherwise just "e9 + 4 bytes", a very generic pattern) from
    // false-positiving on unrelated code.
    //
    // This still needs matching here, not left to fall through to the
    // generic no-CFI guess: rewriting only ever touches executable bytes,
    // never .eh_frame, so the *static* CFI describing this address is still
    // elf_x86_64_eh_frame_lazy_plt's DW_CFA_def_cfa_expression -- i.e.
    // LookupOutcome::kFailExpression, not kFail -- and skipping this check
    // would head straight into the DRAP-or-refuse path instead of the guess.
    {
      int at = match_at(0, {0xf3, 0x0f, 0x1e, 0xfa}) ? 4 : 0;
      int end = -1;
      if (code[at] == 0xe9) {
        end = at + 5;
      } else if (match_at(at, {0xd5, 0x00, 0xa1})) {
        end = at + 11;
      }
      if (end >= 0) {
        for (int i = end; i < 16; i++) {
          if (code[i] != 0xcc)
            goto not_plt_rewrite;
        }
        if (slot_offset == 0 || (uintptr_t)at == slot_offset) {
          return fill_sp_rel(8);
        }
        return false;
      }
    not_plt_rewrite:
        /* nop */;
    }

    return false;
  }

 private:
  template <typename AddrChecker>
  static bool CheckPossiblePC(AddrChecker* checker, uintptr_t maybe_pc) {
    // pc_vma is really std::optional<aw_addrcheck_entry>, but we
    // avoid include here. Real AddrChecker wrapper is in .cc anyways.
    auto pc_vma = checker->Lookup(maybe_pc);
    return (pc_vma && pc_vma->perm_exec);
  }

  template <typename AddrChecker>
  static bool GuessFPFrame(Cursor cursor, AddrChecker* checker, uintptr_t stack_low, uintptr_t stack_high) {
    assert(stack_low <= cursor.sp && cursor.sp < stack_high);  // checked by the caller
    (void)stack_low;
    static constexpr uintptr_t kMaxHeuristicsFrameSize = 32 << 10;
    if (cursor.fp < cursor.sp || cursor.fp - cursor.sp > kMaxHeuristicsFrameSize ||
        (cursor.fp & (sizeof(uintptr_t) - 1)) != 0) {
      return false;
    }
    uintptr_t ret_location = cursor.fp + sizeof(uintptr_t);
    if (ret_location < cursor.fp) {
      return false;  // overflow
    }
    if (ret_location >= stack_high) {
      return false;
    }

    uintptr_t caller_pc = *reinterpret_cast<uintptr_t*>(ret_location);
    return CheckPossiblePC(checker, caller_pc);
  }

 public:
  template <typename AddrChecker>
  static bool GuessUnwindInfo(Cursor cursor, const ucontext_t*, AddrChecker* checker, FrameInfo* info) {
    auto stack_vma = checker->Lookup(cursor.sp);
    if (!stack_vma || !stack_vma->perm_read || !stack_vma->perm_write || (cursor.sp & (sizeof(uintptr_t) - 1)) != 0) {
      return false;
    }

    uintptr_t top_stack_value = *reinterpret_cast<uintptr_t*>(cursor.sp);

    // auto rp = [](auto ptr) -> uintptr_t {
    //   return *reinterpret_cast<const uintptr_t*>(ptr);
    // };

    // One thing we can detect is if we're in the middle of
    // setting up frame-pointer. Just after push %rbp, but before
    // saving rsp (new frame address) into rbp.
    if (top_stack_value == cursor.fp) {
      Cursor modified{cursor};
      modified.fp = modified.sp;
      if (GuessFPFrame(modified, checker, stack_vma->start, stack_vma->end)) {
        // printf("at 0x%zx guessed cfa: (sp + 16) = 0x%zx, fp *(cfa - 16) = 0x%zx, and pc = *(cfa - 8) = %zx\n",
        //        cursor.pc, cursor.sp + 16, rp(cursor.sp + 16 - 16),
        //        rp(cursor.sp + 16 - 8));
        ResetFrameInfo(info);
        info->cfa = CfaRule::SpRel(16);  // use SP+16 as CFA (also SP at the call site)
        info->fp = RegisterRule::MemCfaRel(-16);
        return true;
      }
    }

    // A number of simpler asm codes which often lack unwind info
    // simply don't use stack. So we check if top of the stack
    // contains probable return address.
    if (CheckPossiblePC(checker, top_stack_value)) {
      // printf("at 0x%zx guessed cfa: (sp + 8) = 0x%zx, fp same = 0x%zx, and pc = *(cfa - 8) = %zx\n",
      //        cursor.pc, cursor.sp + 8, cursor.fp,
      //        rp(cursor.sp + 8 - 8));
      ResetFrameInfo(info);
      return true;
    }

    // Some unwind-info-less codes have straightforward frame-pointer setup.
    if (GuessFPFrame(cursor, checker, stack_vma->start, stack_vma->end)) {
      // printf("at 0x%zx guessed cfa: (fp + 16) = 0x%zx, fp *(cfa - 16) = 0x%zx, and pc = *(cfa - 8) = %zx\n",
      //        cursor.pc, cursor.fp + 16, rp(cursor.fp + 16 - 16),
      //        rp(cursor.fp + 16 - 8));
      ResetFrameInfo(info);
      info->cfa = CfaRule::FpRel(16);
      info->fp = RegisterRule::MemCfaRel(-16);
      return true;
    }

    return false;
  }

  static void ResetFrameInfo(FrameInfo* info) {
    info->cfa = CfaRule::SpRel(8);
    info->fp = RegisterRule::SameValue();
    info->ra = RegisterRule::MemCfaRel(-8);
  }
};

}  // namespace aw_backtrace_internal

#endif  // AW_ARCH_X86_64_H_
