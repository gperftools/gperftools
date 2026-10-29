/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// Unwinding through gcc's DRAP (stack-realigning) frames. See
// doc/amd64-drap-problem.adoc. The DRAP functions themselves are gcc output
// in tests/drap/*.s, plus minimal_drap_2 below.
#include <stdio.h>
#include <stdlib.h>

#include <array>
#include <string_view>

#include "aw-backtrace/aw-backtrace.h"
#include "backtrace-comparer.h"

extern "C" {
int minimal_drap(void (*fn)(int*));
int minimal_drap_2(void (*fn)(int*));
int drap_saved_regs(void (*fn)(int*));
int drap_many_regs(void (*fn)(int*));
int drap_r13_nested(void (*fn)(int*));
int drap_r13_ncsr(void (*fn)(int*));
int drap_test_trampoline(decltype(minimal_drap) test_fn, void (*arg)(int*));
// Return address of drap_test_trampoline's call into the DRAP function.
extern const char drap_test_trampoline_ret[];
}

static constexpr int kBtSize = 64;

// CheckDRAPBacktrace counts backtraces taken from inside a DRAP function's
// callee that did or did not get through the DRAP frame and reach
// drap_test_trampoline. Without this, a plain (non-stepped) run checks only
// that the DRAP functions return the right value, which they do even if we
// cannot unwind through them.
int drap_bt_good;
int drap_bt_bad;

void ResetDRAPCheck() {
  drap_bt_good = drap_bt_bad = 0;
}

bool DRAPCheckOk() {
  return drap_bt_good > 0 && drap_bt_bad == 0;
}

void CheckDRAPBacktrace() {
  std::array<void*, kBtSize> buf;
  int count = aw_backtrace(nullptr, buf.data(), buf.size(), 0);
  bool ok = false;
  for (int i = 0; i < count; i++) {
    if (buf[i] == static_cast<const void*>(drap_test_trampoline_ret)) {
      ok = true;
    }
  }
  if (ok) {
    drap_bt_good++;
    return;
  }
  if (drap_bt_bad++ == 0) {
    printf("DRAP backtrace (%d frames) did not reach drap_test_trampoline_ret = %p:\n", count,
           static_cast<const void*>(drap_test_trampoline_ret));
    for (int i = 0; i < count; i++) {
      printf("  %d: %p\n", i, buf[i]);
    }
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  {
    static aw_backtrace_ext::DebugExtensionV0::DiagOptions options{};
    aw_backtrace_ext::DebugExtensionV0::TryGet()->OverrideGlobalBacktracer(&options);
  }

  if (argc < 2 || std::string_view{argv[1]} != "--nocompare") {
    setenv("AW_BT_DIAG", "0", 0);
    StartBacktraceComparer();
  }

  bool all_ok = true;

  ResetDRAPCheck();
  int res = drap_test_trampoline(minimal_drap, [](int* p) -> void {
    *p = 41;
    *p += minimal_drap([](int* p) -> void {
      CheckDRAPBacktrace();
      *p = 1;
    });
  });
  printf("drap test result value %d (need 42), backtrace %s\n", res, DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  // Same as above but with minimal_drap_2, whose epilogue lacks the
  // .cfi_restore for %rbp, as gcc's does when the stack is realigned to
  // 64 bytes or less.
  ResetDRAPCheck();
  res = drap_test_trampoline(minimal_drap_2, [](int* p) -> void {
    *p = 41;
    *p += minimal_drap_2([](int* p) -> void {
      CheckDRAPBacktrace();
      *p = 1;
    });
  });
  printf("drap test 2 result value %d (need 42), backtrace %s\n", res, DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  // drap_saved_regs (tests/drap/regs.s) also saves callee-saved
  // registers in the DRAP frame, so %r10 lands at -16(%rbp), not -8, and
  // %rbx/%r12 get DW_CFA_expression rules. gperftools'
  // MallocBlock::ProcessFreeQueue has this prologue with -march=x86-64-v3
  // (the default on Ubuntu 26.04's amd64v3 variant), though its epilogue
  // lacks the .cfi_restore for %rbp, as it is realigned only to 32 bytes.
  ResetDRAPCheck();
  res = drap_test_trampoline(drap_saved_regs, [](int* p) -> void {
    CheckDRAPBacktrace();
    *p = 21;
  });
  printf("drap test 3 (saved regs) result value %d (need 42), backtrace %s\n", res, DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  // The rest are from tests/drap/variants.s.

  // All five callee-saved registers in the DRAP frame: %r10 at -40(%rbp).
  ResetDRAPCheck();
  res = drap_test_trampoline(drap_many_regs, [](int* p) -> void {
    CheckDRAPBacktrace();
    *p = 7;
  });
  printf("drap test 4 (many regs) result value %d (need 42), backtrace %s\n", res, DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  // %r13 as the DRAP register, in a nested function (the static chain
  // occupies %r10). The DRAP frame is the nested function, called from
  // drap_r13_nested, which is an ordinary frame.
  ResetDRAPCheck();
  res = drap_test_trampoline(drap_r13_nested, [](int* p) -> void {
    CheckDRAPBacktrace();
    *p = 42;
  });
  printf("drap test 5 (r13, nested function) result value %d (need 42), backtrace %s\n", res,
         DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  // %r13 as the DRAP register in a no_caller_saved_registers function, which
  // also saves %r10 and the caller-saved registers via DW_CFA_expression.
  ResetDRAPCheck();
  res = drap_test_trampoline(drap_r13_ncsr, [](int* p) -> void {
    CheckDRAPBacktrace();
    *p = 42;
  });
  printf("drap test 6 (r13, no_caller_saved_registers) result value %d (need 42), backtrace %s\n", res,
         DRAPCheckOk() ? "ok" : "BAD");
  all_ok = all_ok & (res == 42) & DRAPCheckOk();

  return all_ok ? 0 : 1;
}

// DRAP sets up "fake" stack frame, so general GuessUnwindInfo will be
// able to correctly restore caller's RIP and RBP (after fake frame is
// setup). But not RSP. Such mismatch will only fail tests if
// the DRAP function's caller is using RSP-offset-based frame layout. To
// force that we have special hand-coded asm. Otherwise main() could
// well be compiled with -fno-omit-frame-pointer and "pass" even with
// broken DRAP detection logic.
__attribute__((naked)) int drap_test_trampoline(decltype(minimal_drap) /* test_fn */, void (*)(int*)) {
  asm volatile(R"(
        push %rdi
        .cfi_def_cfa 7, 16
        push %rbp
        .cfi_def_cfa_offset 24
        push %rsi
        .cfi_def_cfa_offset 32
        xchg %rsi, %rdi
        // we also "corrupt" rbp to make it even more likely for naive frame-pointer-based backtracing to fail
        mov %rsi, %rbp
        .cfi_offset 6, -24
        call *%rsi
        .globl drap_test_trampoline_ret
        .hidden drap_test_trampoline_ret
drap_test_trampoline_ret:
        add $24, %rsp
        .cfi_def_cfa 7, 8
        mov -16(%rsp), %rbp
        ret
)");
}

// Same as tests/drap/minimal.s but without the .cfi_restore 6 in the
// epilogue. That is what gcc emits when the stack is realigned to 64 bytes or
// less (tests/drap/minimal.c realigns to 4096). See
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=128270
__attribute__((naked)) int minimal_drap_2(void (*)(int*)) {
  asm volatile(R"(
	leaq	8(%rsp), %r10	#,
	.cfi_def_cfa 10, 0
	andq	$-4096, %rsp	#,
	movq	%rdi, %rax	# fn, fn
	pushq	-8(%r10)	#
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
# NOTE: This cfi is DW_CFA_expression: r6 (rbp) (DW_OP_breg6 (rbp): 0)
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r10	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -8; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x78,0x6
	leaq	-8176(%rbp), %rdi	#, tmp100
	subq	$8168, %rsp	#,
	call	*%rax	# fn
	movq	-8(%rbp), %r10	#,
	.cfi_def_cfa 10, 0
	movl	-8176(%rbp), %eax	# aligned_array[0],
	leave
#	.cfi_restore 6
	leaq	-8(%r10), %rsp	#,
	.cfi_def_cfa 7, 8
	ret
)");
}
