/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef AW_BACKTRACE_FASTPATH_H_
#define AW_BACKTRACE_FASTPATH_H_

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>

#include "aw-arch.h"
#include "aw-structs.h"
#include "backtrace-core.h"
#include "dwarf-constants.h"
#include "utils.h"

namespace aw_backtrace_internal {

namespace fastpath {

// Address translator for the decoder's raw reads. Production uses
// IdentityXlate: the decoder's "ptr space" IS the process address space,
// so a read is a plain dereference and this compiles away entirely. Tests
// can substitute a translator that redirects reads into a private buffer
// and/or reports the accessed offsets back to a fuzzer -- see
// fuzz/fastpath-fuzz.cc.
struct IdentityXlate {
  static const void* Map(uintptr_t ptr, size_t /*size*/, size_t /*align*/) {
    return reinterpret_cast<const void*>(ptr);
  }
};

template <class Xlate = IdentityXlate>
struct Access {
  const uintptr_t safety_start;
  const uintptr_t safety_end;
  [[no_unique_address]] Xlate xlate;

  Access(uintptr_t safety_start, uintptr_t safety_end, Xlate xlate = {})
      : safety_start{safety_start}, safety_end{safety_end}, xlate{xlate} {
  }

  // Advances by this much or less cannot overflow ptr. This is key to
  // making various pointer operations fast and yet safe. Because we
  // can do ptr += sizeof(small-data) and only worry about ptr <=>
  // data_end and nothing else.
  static constexpr uintptr_t kSmallBump = 1 << 16;

  bool IsSafetyValid() const {
    if (safety_end + kSmallBump + 1 < safety_end)
      return false;
    // We also ensure that safety_are start isn't close to 0. See
    // bunches of AW_ASSUME below.
    if (safety_start < kSmallBump)
      return false;
    return (safety_start < safety_end && safety_end - safety_start > 32);
  }

  uintptr_t data_end = safety_start;

  [[nodiscard]] uintptr_t SetPtr(uintptr_t addr) {
    if (PREDICT_FALSE(!(safety_start <= addr && addr < safety_end))) {
      return 0;
    }
    data_end = safety_end;
    return addr;
  }

  [[nodiscard]] static bool IsAligned(uintptr_t value, uintptr_t by) {
    return (value & (by - 1)) == 0;
  }

  template <typename T>
  uintptr_t ReadAdvanceStructAligned(uintptr_t ptr, const T** out) {
    constexpr size_t size = sizeof(T);
    static_assert(size < kSmallBump);
    const T* ret = internal_ptr_as<T>(ptr);
    ptr += size;

    // This assume and others. Here is the logic. We know that
    // ptr+=size cannot overflow, because ptr is between
    // safety_{start,end} and because we just asserted
    // size<kSmallBump. But compiler is not aware, most likely. What
    // are we achieving. So we expect this and bunch of other
    // functions to be inlined. It is reasonable for PREDICT_FALSE
    // jump below to just go directly to return false in the
    // TryFastFrameInfo below (i.e. ReadAdvanceStructAligned is
    // wrapped in ASSUME). Most importantly we want _successful_
    // return to avoid check of ptr on 0. Which we know isn't
    // possible. I.e. we don't want the caller to check ptr at all and
    // just "inline" the jumps/straight code directly. In practice as
    // of this writing it doesn't seem to matter. So AW_ASSUME is just
    // assert and utils.h definition with actual [[assume(...)]] is
    // commented out.
    //
    // If we had nice way to return both ptr and "okay/fail"
    // indication separately and without ceremony (std::tie etc) we'd
    // probably just prefer that.

    AW_ASSUME(ptr != 0);  // by kSmallBump condition

    if (PREDICT_FALSE(ptr > data_end)) {
      return 0;
    }
    *out = ret;
    return ptr;
  }

  template <typename T>
  const T* internal_ptr_as(uintptr_t ptr, size_t align = alignof(T)) {
    return reinterpret_cast<const T*>(xlate.Map(ptr, sizeof(T), align));
  }

  // Accesses index item of array of T-s starting at ptr. To be used
  // after we've proved the data we're about to access is all within
  // bounds.
  template <typename T>
  [[nodiscard]] const T* UnsafeTableAccess(uintptr_t ptr, uintptr_t index) {
    constexpr size_t size = sizeof(T);
    uintptr_t offset = index * size;
    assert(!__builtin_mul_overflow(index, size, &offset));
    uintptr_t data_left = data_end - ptr;
    (void)data_left;
    assert(offset < data_left && offset + sizeof(T) <= data_left && ((offset | ptr) & (alignof(T) - 1)) == 0);
    return internal_ptr_as<T>(ptr + offset);
  }

  bool TruncateTo(uintptr_t ptr, uint32_t size) {
    if (PREDICT_FALSE(data_end - ptr < size)) {
      return false;
    }
    data_end = ptr + size;
    return true;
  }

  // Returns address of '\0' or value 0 if we haven't found it within
  // 8 bytes. Used in aug strings we expect to be shorter.
  uintptr_t StrLen8(uintptr_t ptr) {
    for (uint32_t i = 0; i < 8; i++) {
      const uint8_t* p = internal_ptr_as<uint8_t>(ptr + i);
      if (PREDICT_TRUE(*p == 0)) {
        ptr += i;
        AW_ASSUME(ptr != 0);
        return ptr;
      }
    }
    return 0;
  }

  // All of this is litte-endian, obviously. Who needs big-endian, anyways :) ?
  uint32_t ReadSmallInt(uintptr_t ptr, uint8_t bytes) {
    uint32_t res{};
    memcpy(&res, (char*)internal_ptr_as<uint32_t>(ptr, 1), bytes);
    return res;
  }
  uint8_t ReadByte(uintptr_t ptr) {
    return internal_ptr_as<uint8_t>(ptr)[0];
  }

  // We only support 2 byte ULEBs. Enough to cover real-world cases.
  ALWAYS_INLINE uintptr_t ReadULEB(uintptr_t ptr, uint32_t* out_res) {
    uint32_t res = ReadByte(ptr);
    if (PREDICT_TRUE(res < 128)) {
      *out_res = res;
      ptr++;
      AW_ASSUME(ptr != 0);
      return ptr;
    }
    uint32_t data = ReadSmallInt(ptr, 2);
    if (PREDICT_FALSE(data >= 0x8000)) {
      res = ReadByte(ptr + 2);
      if (res >= 128) {
        return 0;
      }
      // 3 byte uleb
      for (int i = 1; i >= 0; i--) {
        res <<= 7;
        res |= (ReadByte(ptr + (unsigned)i) & 0x7f);
      }
      *out_res = res;
      return ptr + 3;
    }
    res &= 0x7f;
    res = res | ((data & 0xff00) >> 1);
    *out_res = res;
    ptr += 2;
    AW_ASSUME(ptr != 0);
    return ptr;
  }

  ALWAYS_INLINE uintptr_t ReadSLEB(uintptr_t ptr, int32_t* out_res) {
    uint32_t u = 0;
    uintptr_t ptr_after = ReadULEB(ptr, &u);
    if (PREDICT_FALSE(ptr_after == 0)) {
      return 0;
    }
    int32_t consumed = static_cast<int32_t>(ptr_after - ptr);
    uint32_t shift = (uint32_t)(32 - consumed * 7);
    *out_res = ((int32_t)(u << shift)) >> shift;
    return ptr_after;
  }
};

struct FastPathFrame {
  // lowest bit is 0 if SP-based and 1 if FP-based. Positive. E.g. CFA
  // is %rsp + cfa_offset. Value of 0 is "impossible" used to indicate
  // failure or "end of chain" signal.
  uint32_t cfa_offset;
  // CFA offset where fp is saved. 0 when fp is not save (same as
  // caller). Negated. E.g. %rbp is CFA - fp_offset
  uint16_t fp_offset;
  // CFA offset where ra is saved. 0 if in RA register. Unused on
  // x86. Negative.
  uint16_t ra_offset;

  bool IsFPBased() const {
    assert(cfa_offset != 0);
    return (cfa_offset & 1) != 0;
  }

  uint32_t actual_cfa_offset() const {
    assert(cfa_offset != 0);
    return cfa_offset & ~uint32_t{1};
  }

  bool operator==(const FastPathFrame&) const = default;

  static constexpr FastPathFrame Failure() {
    return FastPathFrame{};
  }
  static constexpr FastPathFrame EndOfChain() {
    return FastPathFrame{.cfa_offset = 0, .fp_offset = 1, .ra_offset = 0};
  }
  constexpr FastPathFrame SwitchToSP() {
    return FastPathFrame{.cfa_offset = cfa_offset & ~uint32_t{1}, .fp_offset = fp_offset, .ra_offset = ra_offset};
  }
  constexpr FastPathFrame SwitchToFP() {
    return FastPathFrame{.cfa_offset = cfa_offset | uint32_t{1}, .fp_offset = fp_offset, .ra_offset = ra_offset};
  }
  static constexpr bool IsGoodOffset(int32_t offset) {
    if (PREDICT_FALSE(offset >= 0)) {
      return false;
    }
    if (PREDICT_FALSE((offset & ((int32_t)sizeof(uintptr_t) - 1)) != 0)) {
      return false;
    }
    if (PREDICT_FALSE(-offset > 0xFFFF)) {  // -offset fits into uint16_t
      return false;
    }
    return true;
  }
  static constexpr bool IsGoodCFAOffset(uint32_t offset) {
    if (PREDICT_FALSE((offset & (sizeof(uintptr_t) - 1)) != 0)) {
      return false;
    }
    if (PREDICT_FALSE(offset == 0)) {
      return false;
    }
    return true;
  }
  constexpr FastPathFrame SetCFAOffset(uint32_t offset) {
    assert(IsGoodCFAOffset(offset));
    return FastPathFrame{.cfa_offset = offset | (cfa_offset & 1), .fp_offset = fp_offset, .ra_offset = ra_offset};
  }
  constexpr FastPathFrame SetFPOffset(int32_t offset) {
    assert(offset == 0 || IsGoodOffset(offset));
    return FastPathFrame{.cfa_offset = cfa_offset, .fp_offset = static_cast<uint16_t>(-offset), .ra_offset = ra_offset};
  }

  bool ToFrameInfo(FrameInfo* info) const {
    if (*this == Failure() || (int32_t)cfa_offset < 0) {
      return false;
    }
    // Start from the architectural default row and overwrite what the
    // decoded CFI actually changed. Note *info is reused across frames
    // by the caller, so every field has to be written unconditionally.
    *info = FrameInfo{};
    if (*this == EndOfChain()) {
      info->ra = RegisterRule::Undefined();
      return true;
    }
    if (IsFPBased()) {
      info->cfa = CfaRule::FpRel((int32_t)actual_cfa_offset());
    } else {
      info->cfa = CfaRule::SpRel((int32_t)cfa_offset);
    }
    // fp_offset is the positive magnitude of the CFA-relative slot %rbp
    // was spilled to (0 means "not spilled", i.e. unchanged from the
    // caller). ra_offset stays unused on x86: the CFI loop already
    // verified RA sits at the architectural CFA - 8, which is what the
    // fresh FrameInfo{} above encodes.
    if (fp_offset != 0) {
      info->fp = RegisterRule::MemCfaRel(-static_cast<int32_t>(fp_offset));
    }
    return true;
  }

  static constexpr FastPathFrame ArchDefault() {
    // TODO: figure out arm64 and others. Right now cfa_offset = 0 is
    // okay for arms and risc-v-s and invalid for us.
#if __x86_64__
    return FastPathFrame{.cfa_offset = 8, .fp_offset = 0, .ra_offset = 0};
#else
    return FastPathFrame{};
#endif
  }
};

template <class Xlate = IdentityXlate>
NEVER_INLINE FastPathFrame TryFastFrameInfo(const uintptr_t eh_frame_start, const uintptr_t eh_frame_end,
                                            const uintptr_t eh_frame_hdr, const uintptr_t lookup_pc, Xlate xlate = {}) {
#ifndef RECKLESS_TESTING_ONLY_EMPTY_ASSURE
#define ASSURE(cond)                   \
  do {                                 \
    if (PREDICT_FALSE(!(cond)))        \
      return FastPathFrame::Failure(); \
  } while (0)
#else  // !RECKLESS_TESTING_ONLY_EMPTY_ASSURE
#define ASSURE(cond) \
  do {               \
    (void)(cond);    \
  } while (0)
#endif  // RECKLESS_TESTING_ONLY_EMPTY_ASSURE

  Access<Xlate> acc{eh_frame_start, eh_frame_end, xlate};
  ASSURE(acc.IsSafetyValid());

  const uintptr_t eh_frame_hdr_addr = eh_frame_hdr;
  uintptr_t ptr;
  ASSURE(ptr = acc.SetPtr(eh_frame_hdr_addr));
  ASSURE(acc.IsAligned(ptr, 4));

  // First we read eh_frame_hdr part: regular header and then 2
  // fields: table_offset and fde_count. The latter 2 are used to
  // binary search FDE we need.
  struct EHFrameHDR {
    uint8_t version;
    uint8_t eh_frame_ptr_enc;
    uint8_t fde_count_enc;
    uint8_t table_enc;
    int32_t assumed_table_offset;
    uint32_t assumed_fde_count;
  };
  const EHFrameHDR* hdr;
  ASSURE(ptr = acc.ReadAdvanceStructAligned(ptr, &hdr));
  ASSURE(hdr->version == 1 && hdr->eh_frame_ptr_enc == 0x1b && hdr->fde_count_enc == 0x03 && hdr->table_enc == 0x3b);
  const uint32_t fde_count = hdr->assumed_fde_count;  // we just checked above the encoding is right for this

  ASSURE(fde_count > 0);

  // The data after header (with table 'pointer' and fde_count), we
  // have binary search table.

  struct EHFrameHDREntry {
    int32_t start_ip_offset;
    int32_t fde_ptr_offset;
  };

  {
    uintptr_t table_size;
    ASSURE(!__builtin_mul_overflow(fde_count, sizeof(EHFrameHDREntry), &table_size));
    ASSURE(acc.data_end - ptr >= table_size);
  }
  // We proved that whole table is within bounds. Safe to access

  const EHFrameHDREntry* my_fde;
  {
    const intptr_t target_offset = static_cast<intptr_t>(lookup_pc - eh_frame_hdr_addr);
    uintptr_t n = fde_count;
    uintptr_t start = 0;

    while (PREDICT_TRUE(n > 1)) {
      uintptr_t half = n >> 1;
      const EHFrameHDREntry* e = acc.template UnsafeTableAccess<EHFrameHDREntry>(ptr, start + half);
      start = (e->start_ip_offset <= target_offset) ? start + half : start;
      n -= half;
    }

    const EHFrameHDREntry* e = acc.template UnsafeTableAccess<EHFrameHDREntry>(ptr, start);
    if (PREDICT_FALSE(e->start_ip_offset > target_offset)) {
      return FastPathFrame::Failure();  // All table FDEs are for PCs greater than LOOKUP_PC.
    }

    my_fde = e;
  }

  uintptr_t table_start_pc = eh_frame_hdr_addr + (uintptr_t)my_fde->start_ip_offset;  // sign-extend
  ASSURE(ptr = acc.SetPtr(eh_frame_hdr_addr + (uintptr_t)my_fde->fde_ptr_offset));    // sign-extend
  ASSURE(acc.IsAligned(ptr, 4));

  uintptr_t pc;
  uintptr_t pc_end;
  uintptr_t cie_start;

  {
    struct FDEStartHDR {
      uint32_t len;
      uint32_t cie_offset;
      int32_t assumed_start_pc;
      uint32_t assumed_size;
    };

    const FDEStartHDR* fde_start_hdr;
    ASSURE(ptr = acc.ReadAdvanceStructAligned(ptr, &fde_start_hdr));

    // Reading "initial location" field. It is nominally encoded with
    // encoding which is given by 'R' aug field. But in practice this
    // is 0x1b: pcrel|sdata4. We just assume it and check against FDE
    // PC start from binary search table. Kudos to Claude suggesting
    // this neat idea.

    pc = ptr - sizeof(FDEStartHDR) + offsetof(FDEStartHDR, assumed_start_pc) +
         (uintptr_t)fde_start_hdr->assumed_start_pc;
    ASSURE(pc == table_start_pc);
    ASSURE(!__builtin_add_overflow(pc, fde_start_hdr->assumed_size, &pc_end));
    if (PREDICT_FALSE(pc_end <= lookup_pc)) {
      return FastPathFrame::Failure();  // it was not our FDE after all.
    }

    uintptr_t cie_offset = fde_start_hdr->cie_offset;  // note: zero-extend
    // cie_offset is subtracted
    cie_start = ptr - sizeof(FDEStartHDR) + offsetof(FDEStartHDR, cie_offset) + ~cie_offset + 1;

    // Len is the size of FDE data right after len, but we just
    // consumed full header. So truncate to FDE size taking into
    // account header size.
    uint32_t len_without_header;
    ASSURE(!__builtin_sub_overflow(fde_start_hdr->len, sizeof(FDEStartHDR) - 4, &len_without_header));
    ASSURE(acc.TruncateTo(ptr, len_without_header));
  }

  bool truncated = false;
  bool has_remembered_state = false;
  FastPathFrame remembered_state;
  FastPathFrame info = FastPathFrame::ArchDefault();

  static constexpr uintptr_t kSlop = 8;
  // For speed and simplicity we allow our CFI decoding loop to exceed
  // data_end by up to kSlop bytes. So check if there is enough safety
  // buffer after FDEs end. If not, we deliberately truncate. Hitting
  // this truncation is unfortunate, but we'll simply fallback to
  // slow-path which handles it.
  if (PREDICT_FALSE(acc.data_end + kSlop > acc.safety_end)) {
    truncated = true;
    uintptr_t truncation = acc.data_end + kSlop - acc.safety_end;
    acc.data_end -= truncation;
    if (ptr >= acc.data_end) {
      return FastPathFrame::Failure();
    }
  }

  uintptr_t fde_cie = ptr;
  uintptr_t fde_cie_end = acc.data_end;

  // decode CIE first

  ASSURE(ptr = acc.SetPtr(cie_start));
  ASSURE(acc.IsAligned(ptr, 4));

  struct CIEHeaderStart {
    uint32_t len;
    uint32_t zero;
  };
  const CIEHeaderStart* cie_hdr;
  ASSURE(ptr = acc.ReadAdvanceStructAligned(ptr, &cie_hdr));

  {
    uint32_t len_without_header;
    ASSURE(!__builtin_sub_overflow(cie_hdr->len, sizeof(CIEHeaderStart) - 4, &len_without_header));
    ASSURE(acc.TruncateTo(ptr, len_without_header));
  }

  // field after len is CIE id which must be 0
  ASSURE(cie_hdr->zero == 0);

  constexpr uint32_t kMaxReasonableCIE = 40;
  if (PREDICT_FALSE(!(cie_hdr->len < kMaxReasonableCIE && acc.safety_end >= ptr + (kMaxReasonableCIE + kSlop * 2)))) {
    return FastPathFrame::Failure();  // not fast path
  }

  {
    // next field is one byte version
    ASSURE(acc.ReadByte(ptr) == 1);
    // aug starts with 'z'
    ASSURE(acc.ReadByte(ptr + 1) == 'z');
    // and doesn't end with 'z'
    ASSURE(acc.ReadByte(ptr + 2) != '\0');
    ASSURE(ptr = acc.StrLen8(ptr + 3));

    struct AfterAug {
      uint8_t zero;        // aug string's '\0'
      uint8_t code_align;  // code align. Nominally uleb but we check against single-byte constant
      uint8_t data_align;  // data align. Nominally sleb and usually negative. But check against single-byte constant.
      uint8_t ret_reg;     // return reg (16 for x86; we don't bother)
    };
    static_assert(sizeof(AfterAug) == sizeof(uint32_t));
    uint32_t after_aug_data = acc.ReadSmallInt(ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    const uint8_t kMinus8Sleb = -8 & 0x7f;
    constexpr AfterAug expected_after_aug = {
        .zero = 0, .code_align = 1, .data_align = kMinus8Sleb, .ret_reg = Arch::kRAReg};
    ASSURE(memcmp(&after_aug_data, &expected_after_aug, sizeof(AfterAug)) == 0);

    ASSURE(ptr < acc.data_end);
  }

decode_insn:
  // So we've reached CIE/FDE aug section
  {
    // aug len is nominally uleb but we only allow small values
    uint8_t aug_len = acc.ReadByte(ptr++);
    const uint8_t kMaxAugLen = 32;
    ASSURE(aug_len < kMaxAugLen);
    ptr += aug_len;
    ASSURE(ptr < acc.data_end);
  }

#define ADVANCE_LOC(by)                             \
  ({                                                \
    ASSURE(!__builtin_add_overflow(pc, (by), &pc)); \
    PREDICT_FALSE(pc > lookup_pc);                  \
  })

  assert(ptr != 0);

  while (PREDICT_TRUE(ptr < acc.data_end)) {
    uint8_t opcode = acc.ReadByte(ptr++);

    switch (__builtin_expect(opcode, DW_CFA_advance_loc)) {
      case 0x40 ... 0x7f: {
        static_assert(0x40 == DW_CFA_advance_loc);
        uint8_t small_operand = opcode & static_cast<uint8_t>(~0xc0);
        if (ADVANCE_LOC(small_operand)) {
          goto found;
        }
        if (PREDICT_TRUE(acc.ReadByte(ptr) == DW_CFA_def_cfa_offset)) {
          // common case is cfa_offset following advance_loc
          if (PREDICT_TRUE(ptr < acc.data_end)) {
            ptr = ptr + 1;
            goto cfa_offset;
          }
        }
        break;
      }

      case 0x80 ... 0xbf: {
        static_assert(0x80 == DW_CFA_offset);
        uint8_t small_operand = opcode & static_cast<uint8_t>(~0xc0);
        uint32_t offset_arg;
        ASSURE(ptr = acc.ReadULEB(ptr, &offset_arg));
        uint32_t reg = small_operand;
        int32_t offset;
        if (PREDICT_FALSE(__builtin_mul_overflow(offset_arg, -8, &offset) || !FastPathFrame::IsGoodOffset(offset))) {
          return FastPathFrame::Failure();
        }
        if (reg == Arch::kFPReg) {
          info = info.SetFPOffset(offset);
        } else if (reg == Arch::kRAReg) {
#if __x86_64__
          if (offset != -8)  // NOTE: this is x86-specific for now
            return FastPathFrame::Failure();
#else
          // FIXME. But at least don't assume that info already stores offset == -8
          return FastPathFrame::Failure();
#endif
        } else if (reg == Arch::kSPReg) {
          return FastPathFrame::Failure();  // too confusing
        }

        // other regs we ignore
        break;
      }

      case DW_CFA_nop: {
        break;
      }

      case DW_CFA_advance_loc1:
        ptr++;
        if (ADVANCE_LOC(acc.ReadSmallInt(ptr - 1, 1) * 1)) {
          goto found;
        }
        break;

      case DW_CFA_advance_loc2:
        ptr += 2;
        if (ADVANCE_LOC(acc.ReadSmallInt(ptr - 2, 2) * 1)) {
          goto found;
        }
        break;

      case DW_CFA_advance_loc4:
        ptr += 4;
        if (ADVANCE_LOC(acc.ReadSmallInt(ptr - 4, 4) * 1)) {
          goto found;
        }
        break;

      case DW_CFA_def_cfa: {
        // note: reg is nominally uleb but we only have small
        // registers we allow anyways.
        uint8_t reg = acc.ReadByte(ptr++);
        uint32_t offset;
        ASSURE(ptr = acc.ReadULEB(ptr, &offset));
        ASSURE(FastPathFrame::IsGoodCFAOffset(offset));
        if (reg == Arch::kFPReg) {
          info = info.SetCFAOffset(offset).SwitchToFP();
        } else if (reg == Arch::kSPReg) {
          info = info.SetCFAOffset(offset).SwitchToSP();
        } else
          return FastPathFrame::Failure();
        break;
      }

      case DW_CFA_def_cfa_register: {
        // Same as above, reg is nominally uleb.
        uint8_t reg = acc.ReadByte(ptr++);
        if (reg == Arch::kFPReg) {
          info = info.SwitchToFP();
        } else if (reg == Arch::kSPReg) {
          info = info.SwitchToSP();
        } else
          return FastPathFrame::Failure();
        break;
      }

      case DW_CFA_def_cfa_offset: {
      cfa_offset:
        uint32_t offset;
        ASSURE(ptr = acc.ReadULEB(ptr, &offset));
        ASSURE(FastPathFrame::IsGoodCFAOffset(offset));
        info = info.SetCFAOffset(offset);
        break;
      }

      case DW_CFA_def_cfa_offset_sf: {
        int32_t offset;
        ASSURE(ptr = acc.ReadSLEB(ptr, &offset));
        ASSURE(!__builtin_mul_overflow(offset, -8, &offset));
        ASSURE(offset > 0 && FastPathFrame::IsGoodCFAOffset(static_cast<uint32_t>(offset)));
        info = info.SetCFAOffset(static_cast<uint32_t>(offset));
        break;
      }

      case DW_CFA_register: {
        uint32_t caller_reg;
        ASSURE(ptr = acc.ReadULEB(ptr, &caller_reg));
        // reading another up to 4 bytes needs w.StartAccess, but lets
        // registers are usually small. So lets bail out of fast-path
        // in those cases.
        uint32_t stored_in_reg;
        ASSURE(ptr = acc.ReadULEB(ptr, &stored_in_reg));
        (void)stored_in_reg;
        if (caller_reg == Arch::kFPReg || caller_reg == Arch::kSPReg || caller_reg == Arch::kRAReg)
          return FastPathFrame::Failure();
        break;  // ignore all other CFA_register rules
      }

      case DW_CFA_remember_state:
        ASSURE(!has_remembered_state);
        remembered_state = info;
        has_remembered_state = true;
        break;

      case DW_CFA_restore_state:
        ASSURE(has_remembered_state);
        info = remembered_state;
        has_remembered_state = false;
        break;

      case DW_CFA_undefined:
        // We only handle "undefined" for return address. This is used
        // in practice as a special case saying "this is end of
        // chain". Formally, we're breaking the rule a bit, because
        // there is remote chance that subsequent CFI rows define
        // return address again, but this is not happening in
        // practice. And we can afford a tiny chance of producing
        // truncated backtrace for such unusual stack frames.
        if (acc.ReadByte(ptr++) == Arch::kRAReg) {
          info = FastPathFrame::EndOfChain();
          goto found;
        }
        return FastPathFrame::Failure();

      case DW_CFA_GNU_args_size: {
        uint32_t dummy;
        ASSURE(ptr = acc.ReadULEB(ptr, &dummy));
        // ignore
        break;
      }

      default:
        if (PREDICT_FALSE((opcode & 0xc0) == DW_CFA_restore)) {
          uint8_t small_operand = opcode & static_cast<uint8_t>(~0xc0);
          if (small_operand == Arch::kFPReg) {
            // note, by spec "restore" is to the snapshot state after
            // CIE. In practice CIEs don't touch frame pointer, but in the
            // "main"/non-fast-path unwinder we actually check. libgcc
            // never bothers and just resets to "same" (architectural
            // default). So a little nominal incorrectness is no big deal
            // IMO.
            info = info.SetFPOffset(0);
          } else if (small_operand == Arch::kRAReg || small_operand == Arch::kSPReg)
            return FastPathFrame::Failure();  // those regs are too weird to touch
          break;
        }
        return FastPathFrame::Failure();
    }
  }

  assert(ptr != 0);

  if (PREDICT_FALSE(ptr != acc.data_end)) {
    return FastPathFrame::Failure();
  }

  if (PREDICT_TRUE(fde_cie != 0)) {
    // this is end of CIE instructions
    ptr = fde_cie;
    acc.data_end = fde_cie_end;
    fde_cie = 0;
    goto decode_insn;
  }

  if (PREDICT_FALSE(truncated)) {
    return FastPathFrame::Failure();
  }

found:
  if (ptr > acc.data_end)
    return FastPathFrame::Failure();

  // rest of CFI rows are duplicated until the end of FDE range. So it is success.
  return info;
#undef ASSURE
#undef ADVANCE_LOC
}

}  // namespace fastpath

using fastpath::TryFastFrameInfo;

}  // namespace aw_backtrace_internal

#endif  // AW_BACKTRACE_FASTPATH_H_
