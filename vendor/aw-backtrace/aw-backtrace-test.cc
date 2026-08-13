/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/types.h>
#include <ucontext.h>

#include <array>
#include <span>
#include <string>

#include "aw-arch.h"
#include "check.h"
#include "utils.h"

#if !BT_USE_SIMPLE
#include "aw-backtrace/aw-backtrace.h"
#if defined(__x86_64__)
#include "backtrace-comparer.h"
#endif
#else
#include "simple-fp-backtrace.h"
#endif

#include "symbolize-backtrace.h"

#if defined(__x86_64__)
#define DISTINCT_TRAP() asm volatile(".byte 0x66; .byte 0x40; .byte 0x0f; .byte 0x0b")  // data16 rex ud2
static std::array kTrapInsn = std::to_array<uint8_t>({0x66, 0x40, 0x0f, 0x0b});
#define DISTINCT_TRAP_SIZE 4
auto PCLocation(ucontext_t* uc) {
  return &uc->uc_mcontext.gregs[REG_RIP];
}
#elif defined(__aarch64__)
#define DISTINCT_TRAP() asm volatile(".inst 0x00000007")
static std::array kTrapInsn = std::to_array<uint8_t>({0x07, 0x00, 0x00, 0x00});
#define DISTINCT_TRAP_SIZE 4
auto PCLocation(ucontext_t* uc) {
  return &uc->uc_mcontext.pc;
}
extern "C" int pac_test_trampoline(int (*test_fn)());
#endif

static bool IsDistinctTrap(uintptr_t pc) {
  return memcmp(reinterpret_cast<void*>(pc), kTrapInsn.data(), sizeof(kTrapInsn)) == 0;
}

// We use gcc section attribute to place test functions. It then lets
// us more easily check if the backtrace actually contains those
// functions.
#define IN_SECTION(a) NEVER_INLINE __attribute__((section(#a)))

#define DEFINE_IS_IN_SECTION(name, section) \
  extern "C" char __start_##section[];      \
  extern "C" char __stop_##section[];       \
  static bool name(void* addr) {            \
    auto start = __start_##section;         \
    auto stop = __stop_##section;           \
    return start <= addr && addr < stop;    \
  }

extern "C" {
IN_SECTION(foo_sec) int foo(int a, int b);
IN_SECTION(bar_sec) int bar(int a);
};

volatile int g_var = 42;
IN_SECTION(foo_sec) int foo(int a, int b) {
#if BT_USE_SIMPLE
  // This forces frame pointer in practice. Needed while we do frame-pointer
  // based backtrace
  (void)*const_cast<void* volatile*>(reinterpret_cast<void**>(__builtin_frame_address(0)));
#endif

  int v = g_var;
  (void)v;
  DISTINCT_TRAP();
  return a + b;
}

DEFINE_IS_IN_SECTION(is_in_foo_sec, foo_sec);

IN_SECTION(bar_sec) int bar(int a) {
  return foo(g_var, a) + g_var;
}

DEFINE_IS_IN_SECTION(is_in_bar_sec, bar_sec);

static void SetupSignal(int signo, void (*handler)(int, siginfo_t*, void*)) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
  if (sigaction(signo, &sa, nullptr) != 0) {
    abort();
  }
}

static constexpr int kBtSize = 64;

std::array<void*, kBtSize> sigill_backtrace_buf;
std::array<void*, kBtSize> raw_sigill_backtrace_buf;
std::span<void*> sigill_backtrace;
std::span<void*> raw_sigill_backtrace;

bool seen_sigill;

extern "C" {
int minimal_drap(void (*fn)(int*));
int minimal_drap_2(void (*fn)(int*));
int drap_test_trampoline(decltype(minimal_drap) test_fn, void (*arg)(int*));
}

// macro because we cannot afford an extra function frame
#define DO_BT(fn, uc, buf, res, ...)                                     \
  do {                                                                   \
    auto& b = (buf);                                                     \
    int count = fn((uc), b.data(), b.size() __VA_OPT__(, ) __VA_ARGS__); \
    *(res) = std::span{b}.subspan(0, count);                             \
  } while (0)

void sigill_handler(int signo, siginfo_t*, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);
  if (!IsDistinctTrap(*PCLocation(uc))) {
    signal(signo, SIG_DFL);
    return;  // crash for real
  }

  seen_sigill = true;
#if BT_USE_SIMPLE
  DO_BT(simple_backtrace, uc, sigill_backtrace_buf, &sigill_backtrace);
  DO_BT(simple_backtrace, nullptr, raw_sigill_backtrace_buf, &raw_sigill_backtrace);
#else
  DO_BT(aw_backtrace, uc, sigill_backtrace_buf, &sigill_backtrace, 0);
  DO_BT(aw_backtrace, nullptr, raw_sigill_backtrace_buf, &raw_sigill_backtrace, 0);
#endif

  *PCLocation(uc) += DISTINCT_TRAP_SIZE;
}

#if !BT_USE_SIMPLE
// Note this prints info->ra as well; the old aw_check_frame_for() helper
// dropped it, which was unfortunate given the RA rule is the one that
// actually drives unwinding.
static void print_frame_info(const aw_backtrace_internal::FrameInfo* info, void*) {
  printf("info: cfa_kind: %d, cfa_offset: %d, fp_kind: %d, fp_offset: %d, ra_kind: %d, ra_offset: %d\n",
         static_cast<int>(info->cfa.kind), info->cfa.offset, static_cast<int>(info->fp.kind), info->fp.offset,
         static_cast<int>(info->ra.kind), info->ra.offset);
}
#endif

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  setvbuf(stdout, nullptr, _IONBF, 0);

#if !BT_USE_SIMPLE
  {
    // Smoke test and human-readable dump of the decoded unwind rules for a
    // known non-trivial function.
    auto* ext = aw_backtrace_ext::DebugExtensionV0::TryGet();
    CHECK(ext != nullptr);

    if (!ext->LookupFrameInfo(reinterpret_cast<uintptr_t>(&WithSymbolizer), print_frame_info, nullptr)) {
      printf("FAIL: no unwind info for WithSymbolizer\n");
      return 1;
    }

    static aw_backtrace_ext::DebugExtensionV0::DiagOptions options{};
    ext->OverrideGlobalBacktracer(&options);

    (void)atexit([]() { aw_backtrace_ext::DebugExtensionV0::TryGet()->PrintStats(); });
  }

#if defined(__x86_64__)
  if (argc < 2 || std::string_view{argv[1]} != "--nocompare") {
    setenv("AW_BT_DIAG", "0", 0);
    setenv("AW_BT_DIAG_VIA_CORE", "1", 0);
    StartBacktraceComparer();
  }
#endif
#endif  // !BT_USE_SIMPLE

  SetupSignal(SIGILL, sigill_handler);

  int value = bar(3);
  printf("bar value: %d\n", value);

  if (!seen_sigill) {
    printf("expected to see sigill but haven't\n");
    return 1;
  }

  DumpStackTraceToFD(STDERR_FILENO, sigill_backtrace.data(), sigill_backtrace.size(), true);

  bool all_ok = true;

  bool ok = is_in_foo_sec(sigill_backtrace[0]);
  printf("foo in sigill_backtrace[0] = %s\n", ok ? "true" : "false");
  all_ok = all_ok && ok;

  ok = is_in_bar_sec(sigill_backtrace[1]);
  printf("bar in sigill_backtrace[1] = %s\n", ok ? "true" : "false");
  all_ok = all_ok && ok;

#if !BT_USE_SIMPLE
  printf("\nraw_sigill_backtrace:\n");
  DumpStackTraceToFD(STDERR_FILENO, raw_sigill_backtrace.data(), raw_sigill_backtrace.size(), true);
  printf("\n\n");

  ok = (raw_sigill_backtrace[2] == sigill_backtrace[0]);
  ok = ok && (raw_sigill_backtrace[3] == sigill_backtrace[1]);
  if (!ok) {
    printf("raw_sigill_backtrace doesn't match sigill_backtrace\n");
  }
  all_ok = all_ok && ok;

#if __x86_64__
  // test drap bits. See doc/amd64-drap-problem.adoc.
  {
    int res = drap_test_trampoline(minimal_drap, [](int* p) -> void {
      *p = 41;
      *p += minimal_drap([](int* p) -> void { *p = 1; });
    });
    printf("drap test result value %d (need 42)\n", res);
    all_ok = all_ok & (res == 42);

    // Same as above but exercises minimal_drap_2 doesn't have which does not have
    //  .cfi_restore fix at the epilogue (it was fixed in gcc 16)
    res = drap_test_trampoline(minimal_drap_2, [](int* p) -> void {
      *p = 41;
      *p += minimal_drap_2([](int* p) -> void { *p = 1; });
    });
    printf("drap test 2 result value %d (need 42)\n", res);
    all_ok = all_ok & (res == 42);
  }
#endif  // __x86_64__
#endif  // !BT_USE_SIMPLE

#if defined(__aarch64__)
  {
    auto test_leaf = []() -> int {
      void* frames[16];
#if !BT_USE_SIMPLE
      int count = aw_backtrace(nullptr, frames, 16, 0);
#else
      int count = simple_backtrace(nullptr, frames, 16);
#endif
      return count;
    };
    int count = pac_test_trampoline(test_leaf);
    printf("aarch64 pac unwind test: captured %d frames (need >= 3)\n", count);
    all_ok = all_ok && (count >= 3);
  }
#endif  // __aarch64__

  return all_ok ? 0 : 1;
}

#if __x86_64__

// DRAP sets up "fake" stack frame, so general GuessUnwindInfo will be
// able to correctly restore caller's RIP and RBP (after fake frame is
// setup). But not RSP. Such mismatch will only fail tests if
// minimal_drap{,_2} caller is using RSP-offset-based frame layout. To
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
        add $24, %rsp
        .cfi_def_cfa 7, 8
        mov -16(%rsp), %rbp
        ret
)");
}
// same as amd64-drap-test.s but without .cfi_restore 6 fix from gcc 16
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
#endif

#if defined(__aarch64__)
// Note: GCC on AArch64 does not support __attribute__((naked)) on C functions
// (it ignores the attribute and emits standard function prologue/epilogue), so
// we define pac_test_trampoline via top-level assembly instead.
asm(R"(
  .global pac_test_trampoline
  .type pac_test_trampoline, %function
pac_test_trampoline:
  .cfi_startproc
  hint 25 // paciasp
  .cfi_negate_ra_state
  stp x29, x30, [sp, -16]!
  .cfi_def_cfa_offset 16
  .cfi_offset 29, -16
  .cfi_offset 30, -8
  mov x29, sp
  blr x0
  ldp x29, x30, [sp], 16
  .cfi_restore 30
  .cfi_restore 29
  .cfi_def_cfa_offset 0
  hint 29 // autiasp
  .cfi_negate_ra_state
  ret
  .cfi_endproc
  .size pac_test_trampoline, .-pac_test_trampoline
)");
#endif
