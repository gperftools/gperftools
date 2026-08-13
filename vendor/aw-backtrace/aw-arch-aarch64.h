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
    if (!InBounds(pc, 2 * sizeof(uint32_t), pc_bounds)) {
      return false;
    }

    const uint32_t* code = reinterpret_cast<const uint32_t*>(pc);

    // Pattern: mov x8, #0x8b; svc #0
    // 0xd2801168, 0xd4000001
    if (code[0] == 0xd2801168 && code[1] == 0xd4000001) {
      // On arm64 Linux, ucontext is at sp + 128 (after siginfo)
      *uc_loc = reinterpret_cast<const ucontext_t*>(sp + 128);
      return true;
    }
    return false;
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

    return false;
  }

  template <typename AddrChecker>
  static bool GuessUnwindInfo(Cursor cursor, const ucontext_t* uc_if_leaf, AddrChecker* checker, FrameInfo* info) {
    (void)cursor;
    (void)uc_if_leaf;
    (void)info;
    (void)checker;
    return false;
  }

  static void ResetFrameInfo(FrameInfo* info) {
    info->cfa = CfaRule::SpRel(0);
    info->fp = RegisterRule::SameValue();
    info->ra = RegisterRule::InReg(30);
  }
};

}  // namespace aw_backtrace_internal

#endif  // AW_ARCH_AARCH64_H_
