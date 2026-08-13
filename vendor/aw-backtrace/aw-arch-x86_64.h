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
    uintptr_t code_size = pc_bounds.second - pc;

    const uint8_t* code = reinterpret_cast<const uint8_t*>(pc);

    // Note, pattern 1 is what we currently have. But pattern 2 is
    // more logical equivalent (load into %eax is a little shorter
    // encoding). So we check both.
    //
    // Pattern 1: 48 c7 c0 0f 00 00 00; 0f 05 (mov $15, %rax; syscall)
    static constexpr std::array kMovRAX =
        std::to_array<uint8_t>({0x48, 0xc7, 0xc0, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05});
    // Pattern 2: b8 0f 00 00 00; 0f 05 (mov $15, %eax; syscall)
    static constexpr std::array kMovEAX = std::to_array<uint8_t>({0xb8, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05});

    auto check_match = [&](std::span<const uint8_t> pattern) -> bool {
      return (pattern.size() <= code_size) && (memcmp(pattern.data(), code, pattern.size()) == 0);
    };

    if (!check_match(kMovRAX) && !check_match(kMovEAX)) {
      return false;
    }

    *uc_loc = reinterpret_cast<const ucontext_t*>(sp);
    return true;
  }

  static bool DetectPLTEntry(uintptr_t ip, FrameInfo* info, std::pair<uintptr_t, uintptr_t> bounds) {
    // PLT entries on x86-64 are 16-byte aligned.
    uintptr_t slot_start = ip & ~uintptr_t{0xf}; // yes, we round _back_ to the start of possible plt entry
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
    if (code[0] == 0xff && code[1] == 0x35 && code[6] == 0xff && code[7] == 0x25) {
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

    if (code[0] == 0xf3 && code[1] == 0x0f && code[2] == 0x1e && code[3] == 0xfa) {
      // 0xff 0x25 <offset> is rip-relative indirect jump: jmp *<offset>(%rip)
      if (code[4] == 0xff && code[5] == 0x25) {
        if (slot_offset == 0 || slot_offset == 4) {
          info->cfa = CfaRule::SpRel(8);
          return true;
        }
        return false;
      }
      // 0x68 is push <32-bit literal>
      // 0xe9 is jump with 32-bit relative offset
      if (code[4] == 0x68 && code[9] == 0xe9) {
        if (slot_offset == 9) {
          return fill_sp_rel(16);
        }
        if (slot_offset == 0 || slot_offset == 4) {
          return fill_sp_rel(8);
        }
        return false;
      }
    }

    // TODO: plt_rewrite feature produces one of this:
    //
    // * endbr64; jmp <rel32>
    // * jmp <rel32>
    // * jmpabs <abs64> ; jmpabs is new upcoming instruction

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
