/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_STRUCTS_H_
#define AW_STRUCTS_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <ucontext.h>

#include <limits>
#include <optional>
#include <type_traits>

namespace aw_backtrace_internal {

struct CfaRule {
  enum class Kind : uint8_t {
    Unsupported = 0,
    SpRel,
    FpRel,
    RegRel,
    DerefFpRel,
  };

  Kind kind{Kind::Unsupported};
  int8_t reg{-1};
  int32_t offset{0};

  bool operator==(const CfaRule&) const = default;

  static constexpr CfaRule Unsupported() {
    return {Kind::Unsupported};
  }
  static constexpr CfaRule SpRel(int32_t off) {
    return {Kind::SpRel, -1, off};
  }
  static constexpr CfaRule FpRel(int32_t off) {
    return {Kind::FpRel, -1, off};
  }
  static constexpr CfaRule RegRel(int8_t r, int32_t off) {
    return {Kind::RegRel, r, off};
  }
  static constexpr CfaRule DerefFpRel(int32_t off) {
    return {Kind::DerefFpRel, -1, off};
  }
};

struct RegisterRule {
  enum class Kind : uint8_t {
    Unsupported = 0,
    SameValue,
    MemCfaRel,
    MemFpRel,
    InReg,
    Undefined,
  };

  Kind kind{Kind::SameValue};
  int8_t reg{-1};
  int32_t offset{0};

  bool operator==(const RegisterRule&) const = default;

  static constexpr RegisterRule Unsupported() {
    return {Kind::Unsupported};
  }
  static constexpr RegisterRule SameValue() {
    return {Kind::SameValue};
  }
  static constexpr RegisterRule MemCfaRel(int32_t off) {
    return {Kind::MemCfaRel, -1, off};
  }
  static constexpr RegisterRule MemFpRel(int32_t off) {
    return {Kind::MemFpRel, -1, off};
  }
  static constexpr RegisterRule InReg(int8_t r) {
    return {Kind::InReg, r, 0};
  }
  static constexpr RegisterRule Undefined() {
    return {Kind::Undefined};
  }
};

struct FrameInfo {
  CfaRule cfa{CfaRule::SpRel(8)};
  RegisterRule fp{RegisterRule::SameValue()};
  RegisterRule ra{RegisterRule::MemCfaRel(-8)};

  bool operator==(const FrameInfo&) const = default;
};

// Compressed form of FrameInfo for dense caching. 8 bytes, no padding.
// Conversions are fallible since not all FrameInfo values are representable.
struct CompressedFrameInfo {
  // CFA = (use_fp ? fp : sp) + (frame_offset << 3). Covers frames up to 512KB.
  uint16_t frame_offset;
  // >= 0: RA is live in this DWARF register number.
  // <  0: RA is on the stack at CFA + ra_combined.
  int16_t ra_combined;
  // >= 0: FP is live in this DWARF register number.
  // == -1: FP unchanged from caller (same value).
  // < -1: FP is on the stack at CFA + fp_combined.
  int16_t fp_combined;
  bool use_fp;  // true: CFA based on FP; false: CFA based on SP
  // note: this field is unused but important. CompressedFrameInfo is
  // used for CAS in unwind info cache. So it must have no
  // padding. Thus has_unique_object_representations_v check below.
  uint8_t unused;

  bool operator==(const CompressedFrameInfo&) const = default;
};
static_assert(sizeof(CompressedFrameInfo) == 8, "");
static_assert(std::has_unique_object_representations_v<CompressedFrameInfo>);

// Returns nullopt if f cannot be represented in compressed form.
inline std::optional<CompressedFrameInfo> CompressFrameInfo(const FrameInfo& f) {
  if (f.cfa.kind != CfaRule::Kind::SpRel && f.cfa.kind != CfaRule::Kind::FpRel) {
    return std::nullopt;
  }
  if (f.cfa.offset < 0 || (f.cfa.offset & 7) != 0) {
    return std::nullopt;
  }
  size_t shifted = static_cast<size_t>(f.cfa.offset) >> 3;
  if (shifted > std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }

  CompressedFrameInfo c{};
  c.frame_offset = static_cast<uint16_t>(shifted);
  c.use_fp = (f.cfa.kind == CfaRule::Kind::FpRel);

  if (f.ra.kind == RegisterRule::Kind::InReg) {
    c.ra_combined = f.ra.reg;
  } else if (f.ra.kind == RegisterRule::Kind::MemCfaRel) {
    if (f.ra.offset >= 0 || f.ra.offset < std::numeric_limits<int16_t>::min()) {
      return std::nullopt;
    }
    c.ra_combined = static_cast<int16_t>(f.ra.offset);
  } else {
    return std::nullopt;
  }

  if (f.fp.kind == RegisterRule::Kind::InReg) {
    c.fp_combined = f.fp.reg;
  } else if (f.fp.kind == RegisterRule::Kind::SameValue) {
    c.fp_combined = -1;
  } else if (f.fp.kind == RegisterRule::Kind::MemCfaRel) {
    if (f.fp.offset >= 0 || f.fp.offset < std::numeric_limits<int16_t>::min()) {
      return std::nullopt;
    }
    c.fp_combined = static_cast<int16_t>(f.fp.offset);
  } else {
    return std::nullopt;
  }

  return c;
}

inline FrameInfo DecompressFrameInfo(const CompressedFrameInfo& c) {
  FrameInfo f;
  int32_t offset = static_cast<int32_t>(c.frame_offset) << 3;
  f.cfa = c.use_fp ? CfaRule::FpRel(offset) : CfaRule::SpRel(offset);

  if (c.ra_combined >= 0) {
    f.ra = RegisterRule::InReg(static_cast<int8_t>(c.ra_combined));
  } else {
    f.ra = RegisterRule::MemCfaRel(c.ra_combined);
  }

  if (c.fp_combined >= 0) {
    f.fp = RegisterRule::InReg(static_cast<int8_t>(c.fp_combined));
  } else if (c.fp_combined == -1) {
    f.fp = RegisterRule::SameValue();
  } else {
    f.fp = RegisterRule::MemCfaRel(c.fp_combined);
  }

  return f;
}

struct Cursor {
  uintptr_t pc;
  uintptr_t sp;
  uintptr_t fp;
};

}  // namespace aw_backtrace_internal

#endif  // AW_STRUCTS_H_
