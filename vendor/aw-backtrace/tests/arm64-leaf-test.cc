/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// The aarch64 companion of amd64-leaf-test.cc: hand-written asm with
// deliberately *absent* CFI, so that every unwind step out of it has to come
// from Arch::GuessUnwindInfo. None of the cfiless_* functions below carry
// .cfi_startproc, so the assembler emits no FDE for them and DoUnwindLookup
// fails on every pc inside them.
//
// The exception is case 6, PLT0, which is also CFI-less but is recognised by
// Arch::DetectPLTEntry before the guess ever runs.
//
// glibc's backtrace() is not a usable reference here (libgcc gives up at the
// first frame without CFI -- which is the whole point of these fixtures), so
// each case checks against addresses exported from the asm itself.
//
// Like amd64-leaf-test, this binary never starts the comparer, so it is the
// gdb-friendly one.

#include <execinfo.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>

#include "aw-backtrace/aw-backtrace.h"
#include "check.h"
#include "symbolize-backtrace.h"
#include "utils.h"

extern "C" {
int call_cfiless_leaf();
int call_cfiless_framed();
int call_cfiless_pac();
int call_cfiless_subsp();
void null_call_test_fn();

// Return sites, i.e. the addresses x30 holds while the corresponding cfiless_*
// function runs. These are what the guess has to recover.
extern const char cfiless_leaf_ret_site[];
extern const char cfiless_framed_ret_site[];
extern const char cfiless_pac_ret_site[];
extern const char cfiless_subsp_ret_site[];
// Inside cfiless_framed, just past its own `bl`: the value a *stale* x30 has.
extern const char cfiless_stale_lr_site[];
// The instruction after `blr` on a null register, i.e. what x30 holds when the
// fetch at 0 faults.
extern const char null_call_return_site[];

void plt0_bti_fixture();
void plt0_plain_fixture();
void call_plt0_outer(void (*plt0)());
// The ldr in each PLT0 fixture that faults on plt0_got.
extern const char plt0_bti_fault_site[];
extern const char plt0_plain_fault_site[];
extern const char plt0_call_ret_site[];
extern const char plt0_outer_ret_site[];
extern char plt0_got[];
}

// All the fixtures. `udf #0` raises SIGILL and, unlike x86's ud2, is exactly
// one instruction wide, so the handler resumes with pc += 4.
//
// gcc on aarch64 ignores __attribute__((naked)) on C functions, so everything
// here is top-level asm (same reason aw-backtrace-test.cc defines
// pac_test_trampoline this way).
asm(R"(
	.text

// ---- case 1: CFI-less leaf. x30 live, sp and fp untouched. ----
	.global cfiless_leaf
	.type cfiless_leaf, %function
	.balign 16
cfiless_leaf:
	bti	c
	udf	#0
	ret
	.size cfiless_leaf, .-cfiless_leaf

	.global call_cfiless_leaf
	.type call_cfiless_leaf, %function
call_cfiless_leaf:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	bl	cfiless_leaf
	.global cfiless_leaf_ret_site
cfiless_leaf_ret_site:
	mov	w0, #1
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_cfiless_leaf, .-call_cfiless_leaf

// ---- case 2: CFI-less function with a frame record, trapping *after* its
// own call, so x30 is stale and points back into itself. ----
	.global cfiless_helper
	.type cfiless_helper, %function
cfiless_helper:
	.cfi_startproc
	bti	c
	ret
	.cfi_endproc
	.size cfiless_helper, .-cfiless_helper

	.global cfiless_framed
	.type cfiless_framed, %function
	.balign 16
cfiless_framed:
	bti	c
	stp	x29, x30, [sp, #-16]!
	mov	x29, sp
	bl	cfiless_helper
	.global cfiless_stale_lr_site
cfiless_stale_lr_site:
	nop
	udf	#0
	ldp	x29, x30, [sp], #16
	ret
	.size cfiless_framed, .-cfiless_framed

	.global call_cfiless_framed
	.type call_cfiless_framed, %function
call_cfiless_framed:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	bl	cfiless_framed
	.global cfiless_framed_ret_site
cfiless_framed_ret_site:
	mov	w0, #2
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_cfiless_framed, .-call_cfiless_framed

// ---- case 3: CFI-less leaf that signs x30. Inert without FEAT_PAuth (the
// hint decodes as a NOP), a real test of CleanReturnAddress on hardware that
// has it. ----
	.global cfiless_pac
	.type cfiless_pac, %function
	.balign 16
cfiless_pac:
	bti	c
	paciasp
	udf	#0
	autiasp
	ret
	.size cfiless_pac, .-cfiless_pac

	.global call_cfiless_pac
	.type call_cfiless_pac, %function
call_cfiless_pac:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	bl	cfiless_pac
	.global cfiless_pac_ret_site
cfiless_pac_ret_site:
	mov	w0, #3
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_cfiless_pac, .-call_cfiless_pac

// ---- case 4: CFI-less leaf that moves sp without building a frame record.
// x30 is live, so the caller's pc comes back right, but CFA == sp is wrong by
// the size of the scratch area. Known limitation, asserted only as far as it
// is actually correct. ----
	.global cfiless_subsp
	.type cfiless_subsp, %function
	.balign 16
cfiless_subsp:
	bti	c
	sub	sp, sp, #64
	udf	#0
	add	sp, sp, #64
	ret
	.size cfiless_subsp, .-cfiless_subsp

	.global call_cfiless_subsp
	.type call_cfiless_subsp, %function
call_cfiless_subsp:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	bl	cfiless_subsp
	.global cfiless_subsp_ret_site
cfiless_subsp_ret_site:
	mov	w0, #4
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_cfiless_subsp, .-call_cfiless_subsp

// ---- case 5: branch-and-link through a null register. ----
	.global null_call_test_fn
	.type null_call_test_fn, %function
null_call_test_fn:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	mov	x8, xzr
	blr	x8
	.global null_call_return_site
null_call_return_site:
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size null_call_test_fn, .-null_call_test_fn

// ---- case 6: PLT0, the lazy-binding header, in both its BTI and plain
// shapes. The GOT load faults (main makes plt0_got PROT_NONE), so the capture
// happens after the stp has already moved sp by 16 -- the window where CFA ==
// sp is wrong and DetectPLTEntry has to recognise the stub. ----
	.global plt0_bti_fixture
	.type plt0_bti_fixture, %function
	.balign 16
plt0_bti_fixture:
	bti	c
	stp	x16, x30, [sp, #-16]!
	adrp	x16, plt0_got
	.global plt0_bti_fault_site
plt0_bti_fault_site:
	ldr	x17, [x16, #:lo12:plt0_got]
	add	x16, x16, #:lo12:plt0_got
	br	x17
	nop
	nop
	.size plt0_bti_fixture, .-plt0_bti_fixture

	.global plt0_plain_fixture
	.type plt0_plain_fixture, %function
	.balign 16
plt0_plain_fixture:
	stp	x16, x30, [sp, #-16]!
	adrp	x16, plt0_got
	.global plt0_plain_fault_site
plt0_plain_fault_site:
	ldr	x17, [x16, #:lo12:plt0_got]
	add	x16, x16, #:lo12:plt0_got
	br	x17
	nop
	nop
	nop
	.size plt0_plain_fixture, .-plt0_plain_fixture

// x0 = the fixture. Two CFI'd frames above it, so that the frame *after* the
// one x30 recovers is checkable too: that is where a wrong CFA shows, as
// call_plt0 repeated.
	.global call_plt0
	.type call_plt0, %function
call_plt0:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	blr	x0
	.global plt0_call_ret_site
plt0_call_ret_site:
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_plt0, .-call_plt0

	.global call_plt0_outer
	.type call_plt0_outer, %function
call_plt0_outer:
	.cfi_startproc
	bti	c
	stp	x29, x30, [sp, #-16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	bl	call_plt0
	.global plt0_outer_ret_site
plt0_outer_ret_site:
	ldp	x29, x30, [sp], #16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
	.size call_plt0_outer, .-call_plt0_outer

// A whole 64K aligned block of its own, so that protecting its first page
// touches nothing else at any page size up to 64K.
	.section .bss.plt0_got, "aw", %nobits
	.balign 65536
	.global plt0_got
plt0_got:
	.skip 65536
	.text
)");

namespace {

constexpr size_t kBTSize = 32;

struct Fixture {
  const char* name;
  int (*fn)();
  int expected_result;
  // The address x30 holds inside the fixture: what the guess must recover.
  const char* ret_site;
  // Non-null for case 2 only: the spurious frame a stale x30 produces before
  // the walk self-corrects.
  const char* stale_site;
  // False for the fixture whose CFA we knowingly get wrong, where everything
  // below the recovered frame is garbage by construction.
  bool walk_continues;
};

const Fixture* current_case;
bool in_test;

void* aw_trace[kBTSize];
void* bt_trace[kBTSize];

void Dump(const char* what, void** trace, int count) {
  printf("%s: %d frames\n", what, count);
  if (count > 0) {
    DumpStackTraceToFD(STDOUT_FILENO, trace, std::min(count, 5), true);
  }
}

void sigill_handler(int, siginfo_t*, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);

  // Only ours: a udf #0 (all-zero word) inside a fixture we are running.
  if (!in_test || (uc->uc_mcontext.pc & 3) != 0 || *reinterpret_cast<volatile uint32_t*>(uc->uc_mcontext.pc) != 0) {
    signal(SIGILL, SIG_DFL);
    return;
  }

  printf("=== %s\n", current_case->name);

  int aw_count = aw_backtrace(uc, aw_trace, kBTSize, 0);
  int bt_count = backtrace(bt_trace, kBTSize);

  Dump("aw_backtrace", aw_trace, aw_count);
  // Only informative: libgcc stops at the first frame with no CFI, which is
  // frame 0 of every fixture here.
  printf("glibc backtrace: %d frames (not a reference, it gives up on these)\n", bt_count);

  // Frame 0 is the trapping instruction itself, straight out of the ucontext.
  CHECK(aw_trace[0] == reinterpret_cast<void*>(uc->uc_mcontext.pc));

  int next = 1;
  if (current_case->stale_site != nullptr) {
    // A stale x30 buys one spurious frame pointing back into the fixture; the
    // walk then recovers through the frame record on the following step, since
    // that step is no longer a leaf and so cannot consult x30 again. This is
    // the accepted cost of preferring x30 on leaf frames -- repeating a callee
    // rather than deleting a caller.
    CHECK(aw_trace[next] == reinterpret_cast<const void*>(current_case->stale_site));
    next++;
  }
  CHECK(aw_trace[next] == reinterpret_cast<const void*>(current_case->ret_site));

  if (current_case->walk_continues) {
    // The walk has to keep going past the recovered frame, not stop on it.
    CHECK(aw_count >= next + 3);
  } else {
    printf("(known limitation: CFA is wrong here, frames past %d are not trusted)\n", next);
  }

  // Skip the udf.
  uc->uc_mcontext.pc += 4;
  printf("---\n");
}

NEVER_INLINE void RunCase(const Fixture* fixture) {
  current_case = fixture;
  in_test = true;
  int res = fixture->fn();
  in_test = false;
  CHECK(res == fixture->expected_result);
}

// ---- the null-call case, which needs SIGSEGV and a way out ----

bool in_null_call_test;
sigjmp_buf null_call_jmp;
void* aw_trace2[kBTSize];

// ---- the PLT0 cases, which fault on the GOT load ----

struct Plt0Case {
  const char* name;
  void (*fn)();
  const char* fault_site;
};

const Plt0Case* current_plt0;
sigjmp_buf plt0_jmp;

void HandlePlt0Fault(siginfo_t* si, ucontext_t* uc) {
  CHECK(si->si_addr == plt0_got);

  printf("=== %s\n", current_plt0->name);

  int aw_count = aw_backtrace(uc, aw_trace, kBTSize, 0);
  Dump("aw_backtrace", aw_trace, aw_count);

  CHECK(aw_count >= 4);
  CHECK(aw_trace[0] == reinterpret_cast<void*>(uc->uc_mcontext.pc));
  // x30, untouched by the stp.
  CHECK(aw_trace[1] == reinterpret_cast<const void*>(plt0_call_ret_site));
  // Taking CFA as sp here would make call_plt0's sp-based CFI read its return
  // address from the slot PLT0's stp just filled, and repeat frame 1.
  CHECK(aw_trace[2] == reinterpret_cast<const void*>(plt0_outer_ret_site));

  siglongjmp(plt0_jmp, 1);
}

NEVER_INLINE void RunPlt0Case(const Plt0Case* plt0_case) {
  current_plt0 = plt0_case;
  if (sigsetjmp(plt0_jmp, 1) == 0) {
    call_plt0_outer(plt0_case->fn);
    CHECK(false);  // the GOT load was supposed to fault
  }
  current_plt0 = nullptr;
  printf("---\n");
}

void sigsegv_handler(int, siginfo_t* si, void* _uc) {
  ucontext_t* uc = static_cast<ucontext_t*>(_uc);

  if (current_plt0 != nullptr && uc->uc_mcontext.pc == reinterpret_cast<uintptr_t>(current_plt0->fault_site)) {
    HandlePlt0Fault(si, uc);  // noreturn
  }

  if (!in_null_call_test || uc->uc_mcontext.pc != 0) {
    signal(SIGSEGV, SIG_DFL);
    return;
  }
  CHECK(si->si_addr == nullptr);

  printf("=== null call\n");

  int aw_count = aw_backtrace(uc, aw_trace, kBTSize, 0);
  int count2 = aw_backtrace(nullptr, aw_trace2, kBTSize, 0);

  Dump("aw_backtrace (from ucontext)", aw_trace, aw_count);
  Dump("aw_backtrace (no ucontext)", aw_trace2, count2);

  // `blr` writes x30 before the fetch at 0 faults, so the caller is recovered
  // from the register file, not from the stack -- the aarch64 counterpart of
  // amd64-leaf-test's pushed-return-address case.
  CHECK(aw_count >= 4);
  CHECK(aw_trace[0] == nullptr);
  CHECK(aw_trace[1] == reinterpret_cast<const void*>(null_call_return_site));

  // Same thing reached by walking *through* the signal frame instead of being
  // handed the ucontext: find pc 0 and check what follows it.
  int zero_at = -1;
  for (int i = 0; i < count2; i++) {
    if (aw_trace2[i] == nullptr) {
      zero_at = i;
      break;
    }
  }
  CHECK(zero_at > 0);
  CHECK(zero_at + 1 < count2);
  CHECK(aw_trace2[zero_at + 1] == reinterpret_cast<const void*>(null_call_return_site));

  siglongjmp(null_call_jmp, 1);
}

NEVER_INLINE void call_null_call_test_fn() {
  in_null_call_test = true;
  null_call_test_fn();
  asm volatile("" : : : "memory");  // prevent tail call above
}

void RunNullCallCase() {
  if (sigsetjmp(null_call_jmp, 1) == 0) {
    call_null_call_test_fn();
    CHECK(false);  // null_call_test_fn was supposed to fault
  }
  in_null_call_test = false;
  printf("---\n");
}

void InstallHandler(int signo, void (*handler)(int, siginfo_t*, void*)) {
  struct sigaction sa = {};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(signo, &sa, nullptr) != 0) {
    perror("sigaction");
    exit(1);
  }
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  InstallHandler(SIGILL, sigill_handler);
  InstallHandler(SIGSEGV, sigsegv_handler);

  static const Fixture kFixtures[] = {
      {"cfi-less leaf (x30 live)", call_cfiless_leaf, 1, cfiless_leaf_ret_site, nullptr, true},
      {"cfi-less framed, stale x30", call_cfiless_framed, 2, cfiless_framed_ret_site, cfiless_stale_lr_site, true},
      {"cfi-less leaf with paciasp", call_cfiless_pac, 3, cfiless_pac_ret_site, nullptr, true},
      // Known limitation. x30 is live, so the caller's pc comes back right,
      // but CFA == sp is wrong by the 64 bytes this one carves off, and the
      // caller's own CFI is sp-based -- so the frame after the recovered one
      // is read from the wrong place. Nothing here can detect that without
      // reading code, which the guess deliberately never does.
      {"cfi-less leaf that moves sp", call_cfiless_subsp, 4, cfiless_subsp_ret_site, nullptr, false},
  };
  for (const Fixture& f : kFixtures) {
    RunCase(&f);
  }

  RunNullCallCase();

  long page_size = sysconf(_SC_PAGESIZE);
  CHECK(page_size > 0 && page_size <= 65536);
  CHECK(mprotect(plt0_got, page_size, PROT_NONE) == 0);
  static const Plt0Case kPlt0Cases[] = {
      {"PLT0 with bti, past the stp", plt0_bti_fixture, plt0_bti_fault_site},
      {"PLT0 without bti, past the stp", plt0_plain_fixture, plt0_plain_fault_site},
  };
  for (const Plt0Case& c : kPlt0Cases) {
    RunPlt0Case(&c);
  }

  printf("PASSED\n");
}
