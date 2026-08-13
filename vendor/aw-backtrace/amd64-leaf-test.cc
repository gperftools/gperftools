/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <csignal>
#endif

#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "aw-backtrace/aw-backtrace.h"
#include "check.h"
#include "symbolize-backtrace.h"
#include "utils.h"

extern "C" {
int leaf_test_fn();
void null_call_test_fn();
// The instruction right after the `call *%rax` in null_call_test_fn, i.e. the
// return address the CPU pushes before the call faults.
extern const char null_call_return_site[];
}

__attribute__((naked)) int leaf_test_fn() {
  asm volatile(R"(
	xor %eax, %eax
	ud2
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	ud2
	pushq	%r12
	.cfi_def_cfa_offset 24
	.cfi_offset %r12, -24

	ud2

	popq	%r12
	.cfi_def_cfa_offset 16
	.cfi_restore %r12
	ud2
	popq	%rbp
	.cfi_def_cfa_offset 8
	ud2
	mov $42, %eax
	ret
)");
}

static bool in_leaf_test;
static const char* current_fn_name;
static uintptr_t current_fn_base;

NEVER_INLINE int call_leaf_test_fn() {
  CHECK(!in_leaf_test);
  current_fn_name = "leaf_test_fn";
  current_fn_base = reinterpret_cast<uintptr_t>(&leaf_test_fn);
  in_leaf_test = true;
  int ret = leaf_test_fn();
  in_leaf_test = false;
  return ret;
}

static void sigill_handler(int, siginfo_t*, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);
  auto& rip_loc = uc->uc_mcontext.gregs[REG_RIP];
  // check if we're in our leaf test function and we're at ud2 added manually
  if (!in_leaf_test || *reinterpret_cast<volatile uint16_t*>(rip_loc) != 0x0b0f) {
    // If this is not intentional we let the default SIGILL handling to crash the program
    signal(SIGILL, SIG_DFL);
    return;
  }

  // Skip the ud2 instruction (2 bytes)
  rip_loc += 2;

  printf("At %s + 0x%x\n", current_fn_name, (int)(static_cast<uintptr_t>(rip_loc) - current_fn_base));

  static void* aw_trace[16];
  static void* bt_trace[16];
  int aw_count = aw_backtrace(uc, aw_trace, 16, 0);
  int bt_count = backtrace(bt_trace, 16);
  CHECK(bt_count > 4);
  CHECK(aw_count > 2);

  // skip 2 top frames (this signal handler and signal trampoline)
  bt_count -= 2;
  memmove(bt_trace, bt_trace + 2, bt_count * sizeof(void*));

  printf("aw_backtrace top 3 entries of total %d\n", aw_count);
  DumpStackTraceToFD(STDERR_FILENO, aw_trace, 3, true);
  printf("\nglibc backtrace top 3 entries of total %d\n", bt_count);
  DumpStackTraceToFD(STDERR_FILENO, bt_trace, 3, true);

  CHECK(aw_trace[0] == bt_trace[0]);
  CHECK(aw_trace[1] == bt_trace[1]);
  CHECK(aw_trace[2] == bt_trace[2]);

  printf("---\n");
}

// Jumping through a null function pointer.
//
// `call *%rax` with %rax == 0 pushes the return address and only *then* faults,
// on the instruction fetch at 0. So the ucontext handed to the SIGSEGV handler
// has RIP == 0 with RSP pointing straight at the pushed return address. There
// is of course no unwind info for pc 0, so the entire backtrace hangs off
// Arch::GuessUnwindInfo recognizing that word as a return address -- which is
// exactly what it is there for.
//
// This is the case where a backtrace is worth the most, and it is one libgcc
// gives up on: glibc's backtrace() stops at the signal frame here, so there is
// no reference implementation to diff against the way the leaf test does. We
// check against the one address we know for certain instead.
//
// Note that aw_backtrace goes through UnwindLoopFastPath, which finds no
// .eh_frame for pc 0 and hands off to UnwindLoop -- so this covers both loops.

__attribute__((naked)) void null_call_test_fn() {
  asm volatile(R"(
	xor %eax, %eax
	call *%rax
	.globl null_call_return_site
null_call_return_site:
	ret
)");
}

static bool in_null_call_test;
static sigjmp_buf null_call_jmp;

static void sigsegv_handler(int, siginfo_t* si, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);

  if (!in_null_call_test || uc->uc_mcontext.gregs[REG_RIP] != 0) {
    // Not the fault we set up. Let the default handling crash the program.
    signal(SIGSEGV, SIG_DFL);
    return;
  }
  CHECK(si->si_addr == nullptr);

  static constexpr size_t kBTSize = 16;
  static void* bt[kBTSize];
  int aw_count = aw_backtrace(uc, bt, kBTSize, 0);

  static void* bt2[kBTSize];
  int count2 = aw_backtrace(nullptr, bt2, kBTSize, 0);

  printf("null call: aw_backtrace returned %d frames\n", aw_count);
  if (aw_count > 0) {
    DumpStackTraceToFD(STDOUT_FILENO, bt, std::min(aw_count, 4), true);
  }

  printf("null call: aw_backtrace with no ucontext returned %d frames\n", count2);
  if (count2 > 0) {
    DumpStackTraceToFD(STDOUT_FILENO, bt2, std::min(count2, 5), true);
  }

  // The frame below pc 0 must be the return address the `call` pushed, and the
  // walk must keep going past it rather than stopping on the recovered frame.
  CHECK(aw_count >= 4);
  CHECK(bt[0] == nullptr);
  CHECK(bt[1] == reinterpret_cast<const void*>(null_call_return_site));

  CHECK(count2 >= 6);
  CHECK(bt2[2] == nullptr);
  CHECK(bt2[3] == bt[1]);

  siglongjmp(null_call_jmp, 1);
}

NEVER_INLINE static void call_null_call_test_fn() {
  in_null_call_test = true;
  null_call_test_fn();
  asm volatile("" : : : "memory");  // prevent tail call above
}

static void run_null_call_test() {
  struct sigaction sa = {};
  sa.sa_sigaction = sigsegv_handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
    perror("sigaction");
    exit(1);
  }

  printf("Calling null_call_test_fn...\n");
  if (sigsetjmp(null_call_jmp, 1) == 0) {
    call_null_call_test_fn();
    CHECK(false);  // null_call_test_fn was supposed to fault
  }
  in_null_call_test = false;
  printf("---\n");
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  struct sigaction sa = {};
  sa.sa_sigaction = sigill_handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGILL, &sa, nullptr) != 0) {
    perror("sigaction");
    return 1;
  }

  printf("Calling leaf_test_fn...\n");
  int ret = call_leaf_test_fn();
  if (ret != 42) {
    printf("FAIL: ret is %d, expected 42\n", ret);
    return 1;
  }

  run_null_call_test();

  printf("PASSED\n");
}
