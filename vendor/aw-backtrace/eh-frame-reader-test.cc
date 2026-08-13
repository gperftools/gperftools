/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "eh-frame-reader.h"

#include <gtest/gtest.h>
#include <stdarg.h>
#include <stdio.h>

#include <array>
#include <bit>
#include <functional>
#include <optional>
#include <span>

namespace aw_backtrace_internal {
namespace {

struct NoOpVisitor : public UnwindVisitor {
  bool StartFDE(uintptr_t) {
    return true;
  }
  bool HandleAugS() {
    return true;
  }
  bool AfterCIE() {
    return true;
  }
  bool HandleSetLoc(uintptr_t) {
    return true;
  }
  bool HandleDefCFA(uintptr_t, uintptr_t) {
    return true;
  }
  bool HandleDefCFAReg(uintptr_t) {
    return true;
  }
  bool HandleDefCFAOffset(uintptr_t) {
    return true;
  }
  bool HandleDefCFAExpression(std::span<const uint8_t>) {
    return true;
  }
  bool HandleOffset(uintptr_t, uintptr_t) {
    return true;
  }
  bool HandleRegister(uintptr_t, uintptr_t) {
    return true;
  }
  bool HandleRestore(uintptr_t) {
    return true;
  }
  bool HandleSameValue(uintptr_t) {
    return true;
  }
  bool HandleUndefined(uintptr_t) {
    return true;
  }
  bool HandleValOffset(uintptr_t, uintptr_t) {
    return true;
  }
  bool HandleExpression(uintptr_t, std::span<const uint8_t>) {
    return true;
  }
  bool HandleValExpression(uintptr_t, std::span<const uint8_t>) {
    return true;
  }
  bool HandleRememberState() {
    return true;
  }
  bool HandleRestoreState() {
    return true;
  }

  bool ReportErrorVA(const char* fmt, va_list va) {
    vprintf(fmt, va);
    putchar('\n');
    return false;
  }
};

class ReadOpsTest : public testing::Test, private NoOpVisitor, protected eh_frame_internal::Decoder<NoOpVisitor> {
 public:
  using D = eh_frame_internal::Decoder<NoOpVisitor>;

  using D::ReadEncodedPtr;
  using D::ReadObject;
  using D::ReadSleb128;
  using D::ReadUleb128;

  ReadOpsTest() : eh_frame_internal::Decoder<NoOpVisitor>{EHReaderInputs{}, static_cast<NoOpVisitor*>(this)} {
  }

 protected:
  // Optionalize result of b() (empty option if b use D::exit non-local exit)
  template <typename Body>
  auto Optionalize(const Body& b) -> std::optional<decltype((*(Body*)0)())> {
    std::optional<decltype((*(Body*)0)())> ret;
    (void)WithExit::Run([&ret, &b, this](ExitCookie cookie) -> void {
      D::exit = cookie;
      ret = b();
    });
    D::exit = kInvalidExit;
    return ret;
  }

  template <typename Fn, typename... Args>
  auto Call(Fn&& fn, Args&&... args) {
    return Optionalize([&]() { return (this->*fn)(std::forward<Args>(args)...); });
  }
};

TEST_F(ReadOpsTest, Basic) {
  auto data = std::to_array<uint8_t>({0x01, 0x02, 0x03, 0x04});
  byte_slice slice{data};
  auto maybe_i32 = Optionalize([&]() { return ReadObject<uint32_t>(&slice); });
  EXPECT_EQ(std::endian::native == std::endian::little ? 0x04030201 : 0x01020304, maybe_i32);
  EXPECT_TRUE(slice.empty());
}

TEST_F(ReadOpsTest, BasicFailure) {
  auto data = std::to_array<uint8_t>({0x01, 0x02});
  byte_slice slice{data};
  std::optional<uint32_t> maybe_i32 = Call(&ReadOpsTest::ReadObject<uint32_t>, &slice);
  EXPECT_EQ(std::nullopt, maybe_i32);
}

TEST_F(ReadOpsTest, ReadUleb128) {
  auto data = std::to_array<uint8_t>({0xe5, 0x8e, 0x26, 0x01});  // 624485 and 1
  byte_slice slice{data};
  std::optional<std::pair<uintptr_t, uintptr_t>> res = Optionalize([&]() {
    uintptr_t first = ReadUleb128(&slice);
    uintptr_t second = ReadUleb128(&slice);
    return std::make_pair(first, second);
  });
  EXPECT_EQ(std::make_pair(uintptr_t{624485}, uintptr_t{1}), res);
}

TEST_F(ReadOpsTest, ReadSleb128) {
  auto data = std::to_array<uint8_t>({
      0x00,  // 0
      0x7f,  // -1
      0x40,  // -64
      0x9b,
      0xf1,
      0x59,  // -624485
  });
  byte_slice slice{data};
  EXPECT_EQ(0, Call(&ReadOpsTest::ReadSleb128, &slice));
  EXPECT_EQ(-1, Call(&ReadOpsTest::ReadSleb128, &slice));
  EXPECT_EQ(-64, Call(&ReadOpsTest::ReadSleb128, &slice));
  EXPECT_EQ(-624485, Call(&ReadOpsTest::ReadSleb128, &slice));
}

TEST_F(ReadOpsTest, ReadUleb128Overflow) {
  // 11 continuation bytes
  auto long_leb = std::to_array<uint8_t>({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  byte_slice slice1{long_leb};
  EXPECT_EQ(std::nullopt, Call(&ReadOpsTest::ReadUleb128, &slice1));

  // 10th byte has payload > 1 for 64-bit unsigned int (shift = 63)
  auto overflow_bits = std::to_array<uint8_t>({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02});
  byte_slice slice2{overflow_bits};
  EXPECT_EQ(std::nullopt, Call(&ReadOpsTest::ReadUleb128, &slice2));
}

TEST_F(ReadOpsTest, ReadSleb128Overflow) {
  // 11 continuation bytes
  auto long_leb = std::to_array<uint8_t>({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  byte_slice slice1{long_leb};
  EXPECT_EQ(std::nullopt, Call(&ReadOpsTest::ReadSleb128, &slice1));

  // 10th byte has inconsistent sign extension bits
  auto bad_sign_bits = std::to_array<uint8_t>({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02});
  byte_slice slice2{bad_sign_bits};
  EXPECT_EQ(std::nullopt, Call(&ReadOpsTest::ReadSleb128, &slice2));
}

TEST_F(ReadOpsTest, ReadEncodedPointerAbs) {
  uint64_t val = 0x1234567890abcdef;
  byte_slice slice{reinterpret_cast<const uint8_t*>(&val), sizeof(val)};
  std::optional<uintptr_t> maybe_res = Call(&ReadOpsTest::ReadEncodedPtr, &slice, DW_EH_PE_absptr);
  EXPECT_EQ(val, maybe_res);
}

TEST_F(ReadOpsTest, TruncateSlice) {
  auto data = std::to_array<uint8_t>({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});
  byte_slice slice{data};

  // Advance by 2 bytes
  (void)Call(&ReadOpsTest::ReadObject<uint16_t>, &slice);
  EXPECT_EQ(4u, slice.size());

  // Truncate to size 2
  TruncateSlice(&slice, 2);
  EXPECT_EQ(2u, slice.size());

  // Trying to read 4 bytes should fail
  std::optional<uint32_t> maybe_res = Call(&ReadOpsTest::ReadObject<uint32_t>, &slice);
  EXPECT_EQ(std::nullopt, maybe_res);
}

TEST_F(ReadOpsTest, ReadEncodedPointerSupportedFormats) {
  // 1. DW_EH_PE_udata4
  uint32_t val32 = 0x12345678;
  byte_slice s1{reinterpret_cast<const uint8_t*>(&val32), sizeof(val32)};
  EXPECT_EQ(0x12345678u, Call(&ReadOpsTest::ReadEncodedPtr, &s1, DW_EH_PE_udata4));

  // 2. DW_EH_PE_sdata4 (negative value, sign-extended)
  int32_t neg32 = -100;
  byte_slice s2{reinterpret_cast<const uint8_t*>(&neg32), sizeof(neg32)};
  EXPECT_EQ(static_cast<uintptr_t>(static_cast<intptr_t>(-100)),
            Call(&ReadOpsTest::ReadEncodedPtr, &s2, DW_EH_PE_sdata4));

  // 3. DW_EH_PE_udata8 / DW_EH_PE_sdata8
  uint64_t val64 = 0x1234567890abcdefULL;
  byte_slice s3{reinterpret_cast<const uint8_t*>(&val64), sizeof(val64)};
  EXPECT_EQ(val64, Call(&ReadOpsTest::ReadEncodedPtr, &s3, DW_EH_PE_udata8));

  // 4. DW_EH_PE_pcrel | DW_EH_PE_sdata4 (PC-relative with negative offset)
  int32_t pcrel_off = -0x10;
  std::array<uint8_t, 16> buf{};
  memcpy(buf.data() + 4, &pcrel_off, sizeof(pcrel_off));
  byte_slice s4 = byte_slice{buf}.subspan(4);
  uintptr_t expected_addr = reinterpret_cast<uintptr_t>(buf.data() + 4) - 0x10;
  EXPECT_EQ(expected_addr,
            Call(&ReadOpsTest::ReadEncodedPtr, &s4, static_cast<uint8_t>(DW_EH_PE_pcrel | DW_EH_PE_sdata4)));
}

TEST_F(ReadOpsTest, ReadEncodedPointerUnsupportedEncodings) {
  std::array<uint8_t, 8> dummy{};

  const uint8_t unsupported_encodings[] = {
      DW_EH_PE_uleb128, DW_EH_PE_udata2, 0x08, DW_EH_PE_omitted, DW_EH_PE_datarel | DW_EH_PE_sdata4,
  };

  for (uint8_t enc : unsupported_encodings) {
    byte_slice slice{dummy};
    std::optional<uintptr_t> res = Call(&ReadOpsTest::ReadEncodedPtr, &slice, enc);
    EXPECT_EQ(std::nullopt, res) << "Encoding 0x" << std::hex << (int)enc << " should set error";
  }
}

TEST_F(ReadOpsTest, ReadEncodedPointerBoundsFailure) {
  auto short_buf = std::to_array<uint8_t>({0x01, 0x02});
  byte_slice slice{short_buf};

  std::optional<uintptr_t> res = Call(&ReadOpsTest::ReadEncodedPtr, &slice, DW_EH_PE_udata4);
  EXPECT_EQ(std::nullopt, res);
}

TEST_F(ReadOpsTest, AArch64NegateRAStateAndArgsSize) {
  auto data = std::to_array<uint8_t>({
      DW_CFA_AARCH64_negate_ra_state,
      DW_CFA_GNU_args_size,
      0x10,  // uleb128 args size = 16
      DW_CFA_nop,
  });
  byte_slice slice{data};
  std::optional<bool> res = Optionalize([&]() { return RunInstructions(slice, true); });
  EXPECT_EQ(true, res);
}

}  // namespace
}  // namespace aw_backtrace_internal
