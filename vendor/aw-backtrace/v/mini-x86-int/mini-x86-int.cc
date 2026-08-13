/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "mini-x86-int.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

#include <type_traits>

enum class Reg : uint8_t {
  AX = 0,
  CX = 1,
  DX = 2,
  BX = 3,
  SP = 4,
  BP = 5,
  SI = 6,
  DI = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,
};

// clang-format off
static constexpr int kX86ToUContext[] = {
    REG_RAX,
    REG_RCX,
    REG_RDX,
    REG_RBX,
    REG_RSP,
    REG_RBP,
    REG_RSI,
    REG_RDI,
    REG_R8,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15
};
// clang-format on

using reg_value_type = std::remove_reference_t<decltype(((ucontext_t*){})->uc_mcontext.gregs[0])>;

class Accessor;
// Arg is part of Accessor. It represents value possibly with location (like lvalue in C). Possible locations
// are registers, memory, or immediate values.
//
// We don't construct Arg directly, but through Accessor methods. To enable future flexibility.
class Arg {
 public:
  friend class Accessor;

  uint64_t Load(int width) const {
    if (isimm_) {
      return imm_or_addr_;
    }
    if (width == 32) {
      uint32_t retval;
      memcpy(&retval, addr(), sizeof(retval));
      return retval;
    } else {
      assert(width == 64);
      uint64_t retval;
      memcpy(&retval, addr(), sizeof(retval));
      return retval;
    }
  }
  void Store(uint64_t value, int width) {
    if (isimm_) {
      assert(!isimm_);
      __builtin_trap();
    }
    assert(width == 32 || width == 64);
    if (width == 32) {
      if (!isreg_) {
        uint32_t v32 = value;
        memcpy(addr(), &v32, sizeof(v32));
        return;
      }

      // is reg and 32-bit.
      // clear upper half when storing into 32-bit version of a
      // register
      value = value & ((uint64_t{1} << 32) - 1);
      // and store into 64-bit location
    }

    memcpy(addr(), &value, sizeof(value));
  }

  uint64_t EffectiveAddr() {
    if (isreg_ || isimm_) {
      assert(!isreg_);
      assert(!isimm_);
      __builtin_trap();
    }

    return imm_or_addr_;
  }

  bool HasEffectiveAddr() {
    return (!isreg_ && !isimm_);
  }

  Arg Add(Arg other) {
    // Note add being 64-bit is good enough for us for now, given that we don't support 0x67 (address size override)
    // prefix.
    return FromImm(Load(64) + other.Load(64));
  }

  Arg ScaleBy(uint8_t constant) {
    return FromImm(Load(64) * constant);
  }

  Arg Deref() {
    return FromMemLoc(reinterpret_cast<uint8_t*>(Load(64)));
  }

 private:
  static Arg FromRegLoc(reg_value_type* reg) {
    return Arg{reinterpret_cast<uint64_t>(reg), true, false};
  }
  static Arg FromMemLoc(uint8_t* mem) {
    return Arg{reinterpret_cast<uint64_t>(mem), false, false};
  }
  static Arg FromImm(uint64_t imm) {
    return Arg{imm, false, true};
  }

  explicit Arg(uint64_t imm_or_addr, bool isreg, bool isimm) : imm_or_addr_(imm_or_addr), isreg_(isreg), isimm_(isimm) {
  }
  void* addr() const {
    return reinterpret_cast<void*>(imm_or_addr_);
  }

  const uint64_t imm_or_addr_;
  const bool isreg_;
  const bool isimm_;
};
static_assert(sizeof(Arg) == 16);

// Accessor encapsulates register and memory access
class Accessor {
 public:
  explicit Accessor(ucontext_t* uc) : uc_(uc) {
  }

  reg_value_type* rip_place() {
    return &uc_->uc_mcontext.gregs[REG_RIP];
  }
  reg_value_type* rsp_place() {
    return &uc_->uc_mcontext.gregs[REG_RSP];
  }
  reg_value_type* flags_place() {
    return &uc_->uc_mcontext.gregs[REG_EFL];
  }

  reg_value_type perform_pop() {
    auto at_rsp = rsp_place();
    reg_value_type rsp = *at_rsp;
    reg_value_type retval = *reinterpret_cast<reg_value_type*>(rsp);
    rsp += 8;
    *at_rsp = rsp;
    return retval;
  }

  void perform_push(reg_value_type value) {
    auto at_rsp = rsp_place();
    reg_value_type rsp = *at_rsp;
    rsp -= 8;
    *reinterpret_cast<reg_value_type*>(rsp) = value;
    *at_rsp = rsp;
  }

  Arg Reg(Reg reg) {
    return Arg::FromRegLoc(&uc_->uc_mcontext.gregs[kX86ToUContext[static_cast<uint8_t>(reg)]]);
  }

  static Arg Mem(uintptr_t addr) {
    return Arg::FromMemLoc(reinterpret_cast<uint8_t*>(addr));
  }

  static Arg Imm(uint64_t imm) {
    return Arg::FromImm(imm);
  }

  Arg RipOffsetCareful(uint8_t* rip, int64_t offset) {
    return Imm(reinterpret_cast<uintptr_t>(rip + offset));
  }

  bool HasCF() {
    return FlagSet(kCF);
  }
  bool HasPF() {
    return FlagSet(kPF);
  }
  bool HasAF() {
    return FlagSet(kAF);
  }
  bool HasZF() {
    return FlagSet(kZF);
  }
  bool HasSF() {
    return FlagSet(kSF);
  }
  bool HasOF() {
    return FlagSet(kOF);
  }

 private:
  static inline constexpr int kCF = 0;
  static inline constexpr int kPF = 2;
  static inline constexpr int kAF = 4;
  static inline constexpr int kZF = 6;
  static inline constexpr int kSF = 7;
  static inline constexpr int kOF = 11;

  bool FlagSet(int flag) {
    reg_value_type flag_mask = reg_value_type{1} << flag;
    return (*flags_place() & flag_mask) != 0;
  }

  ucontext_t* const uc_;
};

extern "C" {
extern uint32_t mini_x86_int_perform_wrapper_32(uint32_t a, uint32_t b, reg_value_type* flags_loc,
                                                uint32_t (*fn)(void));
extern uint64_t mini_x86_int_perform_wrapper_64(uint64_t a, uint64_t b, reg_value_type* flags_loc,
                                                uint64_t (*fn)(void));

extern uint32_t mini_x86_int_perform_sub_32();
extern uint64_t mini_x86_int_perform_sub_64();
extern uint32_t mini_x86_int_perform_cmp_32();
extern uint64_t mini_x86_int_perform_cmp_64();
extern uint32_t mini_x86_int_perform_add_32();
extern uint64_t mini_x86_int_perform_add_64();
extern uint32_t mini_x86_int_perform_and_32();
extern uint64_t mini_x86_int_perform_and_64();
extern uint32_t mini_x86_int_perform_or_32();
extern uint64_t mini_x86_int_perform_or_64();
extern uint32_t mini_x86_int_perform_xor_32();
extern uint64_t mini_x86_int_perform_xor_64();
extern uint32_t mini_x86_int_perform_test_32();
extern uint64_t mini_x86_int_perform_test_64();
extern uint32_t mini_x86_int_perform_shl_32();
extern uint64_t mini_x86_int_perform_shl_64();
extern uint32_t mini_x86_int_perform_shr_32();
extern uint64_t mini_x86_int_perform_shr_64();
extern uint32_t mini_x86_int_perform_sar_32();
extern uint64_t mini_x86_int_perform_sar_64();
extern uint32_t mini_x86_int_perform_neg_32();
extern uint64_t mini_x86_int_perform_neg_64();
extern uint32_t mini_x86_int_perform_not_32();
extern uint64_t mini_x86_int_perform_not_64();
}

// Executor encapsulates the logic to execute x86 ops we're able to execute
class Executor {
 public:
  explicit Executor(Accessor* accessor) : a_(accessor) {
  }

  bool DoMOV(uint8_t reg_size, Arg dst, Arg src) {
    if (reg_size == 32) {
      dst.Store(src.Load(32), 32);
    } else {
      dst.Store(src.Load(64), 64);
    }
    return true;
  }

  template <bool read_only = false>
  struct OpPerformer {
    uint32_t (*p32)();
    uint64_t (*p64)();
    Accessor* a;

    void operator()(uint8_t reg_size, Arg dst, Arg src) {
      reg_value_type* flags_loc = a->flags_place();
      if (reg_size == 32) {
        auto value = mini_x86_int_perform_wrapper_32(dst.Load(32), src.Load(32), flags_loc, p32);
        if (!read_only) {
          dst.Store(value, 32);
        }
      } else {
        assert(reg_size == 64);
        auto value = mini_x86_int_perform_wrapper_64(dst.Load(64), src.Load(64), flags_loc, p64);
        if (!read_only) {
          dst.Store(value, 64);
        }
      }
    }
  };
  bool DoSUB(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_sub_32, mini_x86_int_perform_sub_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoADD(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_add_32, mini_x86_int_perform_add_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoAND(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_and_32, mini_x86_int_perform_and_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoOR(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_or_32, mini_x86_int_perform_or_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoXOR(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_xor_32, mini_x86_int_perform_xor_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoTEST(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<true>{mini_x86_int_perform_test_32, mini_x86_int_perform_test_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoCMP(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<true>{mini_x86_int_perform_cmp_32, mini_x86_int_perform_cmp_64, a_}(reg_size, dst, src);
    return true;
  }

  bool DoCALL(uint8_t* at_rip, Arg arg) {
    uint64_t new_pc = arg.Load(64);
    uint64_t old_pc = reinterpret_cast<uint64_t>(at_rip);
    a_->perform_push(old_pc);
    *a_->rip_place() = new_pc;
    return true;
  }

  bool DoRET() {
    *a_->rip_place() = a_->perform_pop();
    return true;
  }

  bool DoSHL(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_shl_32, mini_x86_int_perform_shl_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoSHR(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_shr_32, mini_x86_int_perform_shr_64, a_}(reg_size, dst, src);
    return true;
  }
  bool DoSAR(uint8_t reg_size, Arg dst, Arg src) {
    OpPerformer<>{mini_x86_int_perform_sar_32, mini_x86_int_perform_sar_64, a_}(reg_size, dst, src);
    return true;
  }

  bool Run32Unary(uint8_t reg_size, Arg arg, uint32_t (*p32)(), uint64_t (*p64)()) {
    reg_value_type* flags_loc = a_->flags_place();
    if (reg_size == 32) {
      uint32_t value = mini_x86_int_perform_wrapper_32(arg.Load(32), 0, flags_loc, p32);
      arg.Store(value, 32);
    } else {
      assert(reg_size == 64);
      uint64_t value = mini_x86_int_perform_wrapper_64(arg.Load(64), 0, flags_loc, p64);
      arg.Store(value, 64);
    }
    return true;
  }
  bool DoNEG(uint8_t reg_size, Arg arg) {
    return Run32Unary(reg_size, arg, mini_x86_int_perform_neg_32, mini_x86_int_perform_neg_64);
  }
  bool DoNOT(uint8_t reg_size, Arg arg) {
    return Run32Unary(reg_size, arg, mini_x86_int_perform_not_32, mini_x86_int_perform_not_64);
  }

  bool DoPUSHF() {
    // We only support 64-bit pushfq which is default in 64-bit mode
    // REX.W is not needed for 0x9c in 64-bit mode to push rflags (defaults to 64-bit)
    a_->perform_push(*a_->flags_place());
    return true;
  }
  bool DoPOPF() {
    uint64_t flags = a_->perform_pop();
    // Don't let userspace mess with reserved bits too much, although standard POPF allows some.
    // We just restore what we can.
    *a_->flags_place() = flags;
    return true;
  }

  bool DoPUSH(Arg arg) {
    a_->perform_push(arg.Load(64));
    return true;
  }

  bool DoPOP(Arg arg) {
    arg.Store(a_->perform_pop(), 64);
    return true;
  }

  bool DoJMP(Arg arg) {
    *a_->rip_place() = arg.Load(64);
    return true;
  }

  bool DoLEA(uint8_t reg_size, Arg dst, Arg src) {
    if (!src.HasEffectiveAddr()) {
      return false;
    }
    uint64_t effective_addr = src.EffectiveAddr();
    if (reg_size == 32) {
      dst.Store(effective_addr, 32);
    } else {
      dst.Store(effective_addr, 64);
    }
    return true;
  }

  bool DoJcc(uint8_t jcc_code, Arg arg) {
    /*
     * https://www.felixcloutier.com/x86/jcc
     *
     * We only handle the most straightforward subset.
     *
     * 70 cb     JO rel8   Jump short if overflow (OF=1).
     * 71 cb     JNO rel8  Jump short if not overflow (OF=0).
     * 72 cb     JB rel8   Jump short if below (CF=1).
     * 73 cb     JAE rel8  Jump short if above or equal (CF=0).
     * 74 cb     JE rel8   Jump short if equal (ZF=1).
     * 75 cb     JNE rel8  Jump short if not equal (ZF=0).
     * 76 cb     JBE rel8  Jump short if below or equal (CF=1 or ZF=1).
     * 77 cb     JA rel8   Jump short if above (CF=0 and ZF=0).
     * 78 cb     JS rel8   Jump short if sign (SF=1).
     * 79 cb     JNS rel8  Jump short if not sign (SF=0).
     * 7C cb     JL rel8   Jump short if less (SF≠ OF).
     * 7D cb     JGE rel8  Jump short if greater or equal (SF=OF).
     * 7E cb     JLE rel8  Jump short if less or equal (ZF=1 or SF≠ OF).
     * 7F cb     JG rel8   Jump short if greater (ZF=0 and SF=OF).
     */
    auto do_cond_jump = [&](bool take_jump) {
      if (take_jump) {
        *a_->rip_place() = arg.Load(64);
      }
      return true;
    };
    switch ((jcc_code & 0x0f)) {
      case 0x00:
        return do_cond_jump(a_->HasOF());
      case 0x01:
        return do_cond_jump(!a_->HasOF());
      case 0x02:
        return do_cond_jump(a_->HasCF());
      case 0x03:
        return do_cond_jump(!a_->HasCF());
      case 0x04:
        return do_cond_jump(a_->HasZF());
      case 0x05:
        return do_cond_jump(!a_->HasZF());
      case 0x06:
        return do_cond_jump(a_->HasCF() || a_->HasZF());
      case 0x07:
        return do_cond_jump(!a_->HasCF() && !a_->HasZF());
      case 0x08:
        return do_cond_jump(a_->HasSF());
      case 0x09:
        return do_cond_jump(!a_->HasSF());
      case 0x0c:
        return do_cond_jump(a_->HasSF() != a_->HasOF());
      case 0x0d:
        return do_cond_jump(a_->HasSF() == a_->HasOF());
      case 0x0e:
        return do_cond_jump(a_->HasZF() || a_->HasSF() != a_->HasOF());
      case 0x0f:
        return do_cond_jump(!a_->HasZF() && a_->HasSF() == a_->HasOF());
    }
    return false;
  }

  bool DoNOP() {
    return true;
  }

 private:
  Accessor* const a_;
};

// Result of decoding ModR/M byte. A pair of reg and modr/m arg. The latter can also be rip-relative memory location. To
// access the modrm arg invoke FinalizeMemArg to resolve rip-relative memory locations.
struct ModRMResult {
 public:
  uint8_t reg() const {
    return reg_;
  }

  ModRMResult(uint8_t reg, Arg rm_arg) : ModRMResult(reg, rm_arg, false) {
  }

  static ModRMResult MakeRipRelative(uint8_t reg, int64_t displacement) {
    // note, we typically don't invoke Arg constructors directly, but for now it'll do fine.
    return ModRMResult{reg, Accessor::Imm(displacement), true};
  }

  Arg FinalizeMemArg(uint8_t* at_rip) {
    if (is_rip_relative_) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(at_rip) + rm_arg_.Load(64);
      return Accessor::Mem(addr);
    }
    return rm_arg_;
  }

 private:
  const uint8_t reg_;
  const Arg rm_arg_;
  bool is_rip_relative_ = false;
  ModRMResult(uint8_t reg, Arg rm_arg, bool is_rip_relative)
      : reg_(reg), rm_arg_(rm_arg), is_rip_relative_(is_rip_relative) {
  }
};

struct Interpreter {
  uint8_t* at_rip;
  Accessor* const a;
  Executor* const e;

  const bool has_rex_b : 1;
  const bool has_rex_x : 1;
  const bool has_rex_r : 1;
  const bool has_rex_w : 1;
  const uint8_t opcode;

  Interpreter(uint8_t* at_rip, Accessor* accessor, Executor* executor)
      : Interpreter(OpcodeRexState{at_rip}, accessor, executor) {
  }

  struct OpcodeRexState {
    uint8_t* start_rip;
    bool has_rex_b : 1;
    bool has_rex_x : 1;
    bool has_rex_r : 1;
    bool has_rex_w : 1;
    uint8_t opcode;

    explicit OpcodeRexState(uint8_t* at_rip) {
      uint8_t maybe_rex = *at_rip;
      if ((maybe_rex & 0xf0) == 0x40) {
        at_rip++;
      } else {
        maybe_rex = 0;
      }
      has_rex_b = (maybe_rex & 0x1) != 0;
      has_rex_x = (maybe_rex & 0x2) != 0;
      has_rex_r = (maybe_rex & 0x4) != 0;
      has_rex_w = (maybe_rex & 0x8) != 0;
      opcode = *at_rip++;
      start_rip = at_rip;
    }
  };

  Interpreter(const OpcodeRexState& state, Accessor* accessor, Executor* executor)
      : at_rip(state.start_rip),
        a(accessor),
        e(executor),
        has_rex_b(state.has_rex_b),
        has_rex_x(state.has_rex_x),
        has_rex_r(state.has_rex_r),
        has_rex_w(state.has_rex_w),
        opcode(state.opcode) {
  }

  bool OpcodeSlashIs(uint8_t opcode_value, uint8_t slash) {
    return (opcode == opcode_value) && ((*at_rip >> 3) & 0x7) == slash;
  }

  bool OpcodeBetweenInclusive(uint8_t low, uint8_t high) {
    return (low <= opcode && opcode <= high);
  }

  Reg RegWithB(uint8_t reg) {
    if (has_rex_b) {
      return Reg(reg + 8);
    }
    return Reg(reg);
  }

  Reg RegWithX(uint8_t reg) {
    if (has_rex_x) {
      return Reg(reg + 8);
    }
    return Reg(reg);
  }

  bool TryRun() {
    uint8_t* orig_rip = at_rip;

    const uint8_t reg_size = has_rex_w ? 64 : 32;

    // MOV only has reg -> r/m and r/m -> reg. It encodes load of immediate differently.
    if (HandleBinaryOp(reg_size, 0x89, 0x8b, &Executor::DoMOV)) {
      return true;
    }

    if (opcode == 0x3d) {  // cmp imm32, {r,e}ax
      int32_t imm = LoadAdvanceRIP<int32_t>();
      return e->DoCMP(reg_size, a->Reg(Reg::AX), a->Imm(imm));
    }

    if (opcode == 0xa9) {  // test imm32, {r,e}ax
      int32_t imm = LoadAdvanceRIP<int32_t>();
      return e->DoTEST(reg_size, a->Reg(Reg::AX), a->Imm(imm));
    }

    if (opcode == 0x2d) {  // sub imm32, {r,e}ax
      int32_t imm = LoadAdvanceRIP<int32_t>();
      return e->DoSUB(reg_size, a->Reg(Reg::AX), a->Imm(imm));
    }

    if (OpcodeBetweenInclusive(0xb8, 0xbf)) {  // mov imm -> reg (movabs when doing 64-bit mode with rex.W)
      int64_t tmp;
      if (reg_size == 64) {
        tmp = LoadAdvanceRIP<int64_t>();
      } else {
        assert(reg_size == 32);
        tmp = LoadAdvanceRIP<int32_t>();
      }
      return e->DoMOV(reg_size, a->Reg(RegWithB(opcode - 0xb8)), a->Imm(tmp));
    }

    if (OpcodeSlashIs(0xc7, 0)) {  // mov is also available as imm32 -> r/m form
      ModRMResult res = DecodeRM();
      int32_t imm = LoadAdvanceRIP<int32_t>();
      return e->DoMOV(reg_size, res.FinalizeMemArg(at_rip), a->Imm(imm));
    }

    if (HandleNOP()) {
      return e->DoNOP();
    }

    if (opcode == 0x8d) {  // https://www.felixcloutier.com/x86/lea
      return DecodeSimpleBinaryOp([&](Arg mem_arg, Arg reg) -> bool {
        // DoLEA uses first arg as destination, so we pass reg as first arg
        return e->DoLEA(reg_size, reg, mem_arg);
      });
    }

    if (OpcodeSlashIs(0xff, 4)) {  // jmp *(r/m)
      ModRMResult res = DecodeRM();
      return !has_rex_w && e->DoJMP(res.FinalizeMemArg(at_rip));
    }

    if (opcode == 0xeb) {  // jmp with 8 bit offset
      int64_t value = LoadAdvanceRIP<int8_t>();
      return !has_rex_w && e->DoJMP(a->RipOffsetCareful(at_rip, value));
    }
    if (opcode == 0xe9) {  // jmp with 32 bit offset
      int64_t value = LoadAdvanceRIP<int32_t>();
      return !has_rex_w && e->DoJMP(a->RipOffsetCareful(at_rip, value));
    }

    // There are 2 encodings of conditional jumps. One with 8 bit offset and one with 32 bit offset.
    if (OpcodeBetweenInclusive(0x70, 0x7f)) {  // jmp imm8
      int64_t value = LoadAdvanceRIP<int8_t>();
      return !has_rex_w && e->DoJcc(opcode, a->RipOffsetCareful(at_rip, value));
    }
    if (opcode == 0x0f && 0x80 <= *at_rip && *at_rip <= 0x8f) {  // jmp imm32
      uint8_t opcode_2 = *at_rip++;  // important: we read opcode before advancing at_rip at load below
      int64_t value = LoadAdvanceRIP<int32_t>();
      return !has_rex_w && e->DoJcc(opcode_2, a->RipOffsetCareful(at_rip, value));
    }

    if (OpcodeBetweenInclusive(0x50, 0x57)) {  // push reg
      return !has_rex_w && e->DoPUSH(a->Reg(RegWithB(opcode - 0x50)));
    }
    if (OpcodeBetweenInclusive(0x58, 0x5f)) {  // pop reg
      return !has_rex_w && e->DoPOP(a->Reg(RegWithB(opcode - 0x58)));
    }

    if (opcode == 0xc3) {
      return !has_rex_w && e->DoRET();
    }
    if (opcode == 0xf3 && *at_rip == 0xc3) {  // rep ret
      at_rip++;
      return !has_rex_w && e->DoRET();
    }

    // call offset32
    if (opcode == 0xe8) {
      int64_t offset = LoadAdvanceRIP<int32_t>();
      return !has_rex_w && e->DoCALL(at_rip, a->RipOffsetCareful(at_rip, offset));
    }

    // call *(r/m)
    if (OpcodeSlashIs(0xff, 2)) {
      ModRMResult res = DecodeRM();
      return !has_rex_w && e->DoCALL(at_rip, res.FinalizeMemArg(at_rip));
    }

    static constexpr struct {
      uint8_t opcode_reg_to_rm;
      uint8_t opcode_rm_to_reg;
      uint8_t opcode_reg_to_imm8;
      uint8_t opcode_reg_to_imm32;
      uint8_t slash;
      bool (Executor::*op_fn)(uint8_t, Arg, Arg);
    } binary_ops[] = {
        {0x39, 0x3b, 0x83, 0x81, 7, &Executor::DoCMP},  // https://www.felixcloutier.com/x86/cmp
        {0x29, 0x2b, 0x83, 0x81, 5, &Executor::DoSUB},  // https://www.felixcloutier.com/x86/sub
        {0x01, 0x03, 0x83, 0x81, 0, &Executor::DoADD},  // https://www.felixcloutier.com/x86/add
        {0x21, 0x23, 0x83, 0x81, 4, &Executor::DoAND},  // https://www.felixcloutier.com/x86/and
        {0x09, 0x0b, 0x83, 0x81, 1, &Executor::DoOR},   // https://www.felixcloutier.com/x86/or
        {0x31, 0x33, 0x83, 0x81, 6, &Executor::DoXOR},  // https://www.felixcloutier.com/x86/xor
    };

    for (const auto& op : binary_ops) {
      if (HandleBinaryOp(reg_size, op.opcode_reg_to_rm, op.opcode_rm_to_reg, op.op_fn)) {
        return true;
      }
      if (HandleBinaryOpWithImm(reg_size, op.opcode_reg_to_imm8, op.opcode_reg_to_imm32, op.slash, op.op_fn)) {
        return true;
      }
    }

    assert(orig_rip == at_rip);

    // test only has one r/m and r form since order is irrelevant
    if (opcode == 0x85) {
      return DecodeSimpleBinaryOp(
          [&](Arg mem_arg, Arg reg_arg) -> bool { return e->DoTEST(reg_size, mem_arg, reg_arg); });
    }
    // test r/m and imm32. No version with imm8 and 32/64 bit register
    if (OpcodeSlashIs(0xf7, 0)) {
      // imm32 -> r/m
      ModRMResult res = DecodeRM();
      int32_t tmp = LoadAdvanceRIP<int32_t>();  // int32_t to ensure sign-extension
      return e->DoTEST(reg_size, res.FinalizeMemArg(at_rip), a->Imm(tmp));
    }

    if (OpcodeSlashIs(0xf7, 2) && HandleUnaryOp(reg_size, &Executor::DoNOT)) {
      return true;
    }
    if (OpcodeSlashIs(0xf7, 3) && HandleUnaryOp(reg_size, &Executor::DoNEG)) {
      return true;
    }

    // shift instructions
    if (TryHandleShift(reg_size)) {
      return true;
    }

    assert(orig_rip == at_rip);

    if (opcode == 0x9c) {  // pushfq
      return !has_rex_w && e->DoPUSHF();
    }
    if (opcode == 0x9d) {  // popfq
      return !has_rex_w && e->DoPOPF();
    }

    // Verify that various handle/try etc calls above didn't advance any state (when they return false).
    assert(orig_rip == at_rip);
    (void)orig_rip;  // Avoid unused variable warning.
    return false;
  }

  // DecodeSimplyBinaryOp handle decoding of the common type of instructions. Many instructions are "simple" binary
  // operation involving register and reg/mem (either register or memory location). They are encoded as opcode + modrm
  // byte (plus optional sib and displacement).
  //
  // op_fn takes mem_arg and reg_arg in this order.
  template <typename OpFn>
  bool DecodeSimpleBinaryOp(OpFn&& op_fn) {
    ModRMResult res = DecodeRM();
    return op_fn(res.FinalizeMemArg(at_rip), a->Reg(Reg{res.reg()}));
  }

  // Binary ops (such as add, or cmp) typically have 4 forms: reg -> r/m, r/m -> reg, imm8 -> r/m, imm32 -> r/m. This
  // function handles decoding and dispatching to the executor of the first two forms.
  bool HandleBinaryOp(uint8_t reg_size, uint8_t opcode_reg_to_rm, uint8_t opcode_rm_to_reg,
                      bool (Executor::*op_fn)(uint8_t, Arg, Arg)) {
    if (opcode == opcode_reg_to_rm) {
      // reg -> r/m
      return DecodeSimpleBinaryOp(
          [&](Arg mem_arg, Arg reg_arg) -> bool { return (e->*op_fn)(reg_size, mem_arg, reg_arg); });
    }
    if (opcode == opcode_rm_to_reg) {
      // r/m -> reg
      return DecodeSimpleBinaryOp(
          [&](Arg mem_arg, Arg reg_arg) -> bool { return (e->*op_fn)(reg_size, reg_arg, mem_arg); });
    }
    return false;
  }

  // HandleBinaryOpWithImm handles decoding and dispatching to the executor of the immediate to r/m forms of binary ops.
  bool HandleBinaryOpWithImm(uint8_t reg_size, uint8_t opcode_reg_to_imm8, uint8_t opcode_reg_to_imm32, uint8_t slash,
                             bool (Executor::*op_fn)(uint8_t, Arg, Arg)) {
    if (OpcodeSlashIs(opcode_reg_to_imm8, slash)) {
      // imm8 -> r/m
      ModRMResult res = DecodeRM();
      int8_t tmp = LoadAdvanceRIP<int8_t>();  // int8_t to ensure sign-extension
      return (e->*op_fn)(reg_size, res.FinalizeMemArg(at_rip), a->Imm(tmp));
    }
    if (OpcodeSlashIs(opcode_reg_to_imm32, slash)) {
      // imm32 -> r/m
      ModRMResult res = DecodeRM();
      int32_t tmp = LoadAdvanceRIP<int32_t>();  // int32_t to ensure sign-extension
      return (e->*op_fn)(reg_size, res.FinalizeMemArg(at_rip), a->Imm(tmp));
    }
    return false;
  }

  bool HandleNOP() {
    /*
     * 18a: 66 90                	xchg   %ax,%ax
     * 18c: 0f 1f 00             	nopl   (%rax)
     * 18f: 0f 1f 40 00          	nopl   0x0(%rax)
     * 193: 0f 1f 44 00 00       	nopl   0x0(%rax,%rax,1)
     * 198: 66 0f 1f 44 00 00    	nopw   0x0(%rax,%rax,1)
     * 19e: 0f 1f 80 00 00 00 00 	nopl   0x0(%rax)
     * 1a5: 0f 1f 84 00 00 00 00 	nopl   0x0(%rax,%rax,1)
     * 1ac: 00
     * 1ad: 66 0f 1f 84 00 00 00 	nopw   0x0(%rax,%rax,1)
     * 1b4: 00 00
     */
    auto n_zeros = [](uint8_t* at_rip, size_t n) -> bool {
      for (size_t i = 0; i < n; ++i) {
        if (at_rip[i] != 0) {
          return false;
        }
      }
      return true;
    };
    // note: 0x66 prefix is eaten above
    if (opcode == 0x90) {
      return true;
    }
    if (opcode != 0x0f) {
      return false;
    }
    if (*at_rip != 0x1f) {
      return false;
    }
    if (at_rip[1] == 0x00) {
      at_rip += 2;
      return true;
    }
    if (at_rip[1] == 0x40 && at_rip[2] == 0x00) {
      at_rip += 3;
      return true;
    }
    if (at_rip[1] == 0x44 && n_zeros(at_rip + 2, 2)) {
      at_rip += 4;
      return true;
    }
    if (at_rip[1] == 0x80 && n_zeros(at_rip + 2, 4)) {
      at_rip += 6;
      return true;
    }
    if (at_rip[1] == 0x84 && n_zeros(at_rip + 2, 5)) {
      at_rip += 7;
      return true;
    }
    return false;
  }

  bool TryHandleShift(uint8_t reg_size) {
    bool is_cl = (opcode == 0xd3);
    bool is_imm8 = (opcode == 0xc1);
    bool is_1 = (opcode == 0xd1);
    if (!is_cl && !is_imm8 && !is_1) {
      return false;
    }

    uint8_t modrm = at_rip[0];
    uint8_t reg = (modrm >> 3) & 0x7;

    // Type of operation is encoded in the reg field of the ModR/M byte.
    // We check if before making any state changes.
    using ShiftFn = bool (Executor::*)(uint8_t, Arg, Arg);
    ShiftFn shift_fn = (reg == 4)   ? &Executor::DoSHL
                       : (reg == 5) ? &Executor::DoSHR
                       : (reg == 7) ? &Executor::DoSAR
                                    : nullptr;
    if (!shift_fn) {
      return false;
    }

    // And at this point we "commit" to running the shift.

    ModRMResult res = DecodeRM();
    int32_t count = 0;

    if (is_cl) {
      count = a->Reg(Reg::CX).Load(64);
    } else if (is_imm8) {
      count = LoadAdvanceRIP<uint8_t>();
    } else if (is_1) {
      count = 1;
    }

    // Note, our asm uses shifts with %cl which implicitly masks to 5/6 bits for shl/shr/sar/sal/ror/rol.

    auto count_arg = a->Imm(count);

    bool ok = (e->*shift_fn)(reg_size, res.FinalizeMemArg(at_rip), count_arg);
    // at this point, we've advanced rip etc, so we commit to success. In practice our shift implementations are always
    // succeeding.
    if (!ok) {
      abort();
    }
    return true;
  }

  bool HandleUnaryOp(uint8_t reg_size, bool (Executor::*op_fn)(uint8_t, Arg)) {
    ModRMResult res = DecodeRM();
    return (e->*op_fn)(reg_size, res.FinalizeMemArg(at_rip));
  }

  // https://wiki.osdev.org/X86-64_Instruction_Encoding
  ModRMResult DecodeRM() {
    uint8_t modrm = *at_rip++;
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 0x7;

    uint8_t reg = (modrm >> 3) & 0x7;
    uint8_t result_reg = reg | (has_rex_r ? 8 : 0);

    // mod == 3 means that rm is a register (not memory location).
    //
    // Other values are in general about which displacement to use.
    // 0 -> no disp, 1 -> disp8, 2 -> disp32
    if (mod == 3) {
      return ModRMResult{result_reg, a->Reg(RegWithB(rm))};
    }

    // special case: mod == 0 && rm == 5 means that we use rip-relative addressing
    if (mod == 0 && rm == 5) {
      int64_t offset = LoadAdvanceRIP<int32_t>();
      return ModRMResult::MakeRipRelative(result_reg, offset);
    }

    if (rm != 4) {
      // simple indirect access possibly with displacement. E.g. [reg + disp]. Reg is encoded by rm field
      int64_t displacement = 0;
      if (mod == 1) {
        displacement = LoadAdvanceRIP<int8_t>();
      } else if (mod == 2) {
        displacement = LoadAdvanceRIP<int32_t>();
      }
      return ModRMResult{result_reg, a->Reg(RegWithB(rm)).Add(a->Imm(displacement)).Deref()};
    }

    // rm of 4 indicates that we use SIB
    uint8_t sib = *at_rip++;
    uint8_t scale = (sib >> 6) & 0x3;
    uint8_t raw_index = (sib >> 3) & 0x7;
    uint8_t raw_base = sib & 0x7;
    uint8_t index = raw_index | (has_rex_x ? 8 : 0);

    // SIB in general encodes: [base + (index << scale) + disp]
    // But there are some special cases.

    auto base = [&]() -> Arg {
      if (raw_base == 5 && mod == 0) {
        // Special case: base of 5 and mod of 0 indicates that base is 0 and displacement is 32-bit immediate.
        int32_t disp = LoadAdvanceRIP<int32_t>();
        return a->Imm(disp);
      } else {
        // Otherwise, base is the register indicated by base field.
        return a->Reg(RegWithB(raw_base));
      }
    };

    auto scaled_index = [&]() -> Arg {
      if (index == 4) {
        // index of 4 indicates that we don't use index register.
        return a->Imm(0);
      }
      return a->Reg(Reg{index}).ScaleBy(1 << scale);
    };

    auto displacement = [&]() -> Arg {
      if (mod == 1) {
        return a->Imm(LoadAdvanceRIP<int8_t>());
      } else if (mod == 2) {
        return a->Imm(LoadAdvanceRIP<int32_t>());
      }
      return a->Imm(0);
    };

    return ModRMResult{result_reg, base().Add(scaled_index()).Add(displacement()).Deref()};
  }

  template <typename T>
  T LoadAdvanceRIP() {
    T val;
    memcpy(&val, at_rip, sizeof(val));
    at_rip += sizeof(val);
    return val;
  }
};

bool mini_x86_int_try_advance(ucontext_t* uc) {
  Accessor accessor(uc);
  Executor executor(&accessor);
  reg_value_type* pc_loc = accessor.rip_place();
  auto old_pc_raw = *pc_loc;

  Interpreter interp(reinterpret_cast<uint8_t*>(old_pc_raw), &accessor, &executor);

  if (!interp.TryRun()) {
    return false;
  }

  auto new_pc_raw = *pc_loc;
  // jmp and call update rip directly, otherwise we need to advance to new
  // value of at_rip
  if (new_pc_raw == old_pc_raw) {
    *pc_loc = reinterpret_cast<uintptr_t>(interp.at_rip);
  }

  return true;
}
