// SPDX-License-Identifier: 0BSD
/*
 * Guest test for the baseline pstepper_gpregs -> ucontext_t conversion (the
 * "fake"-ish context the upcall handler is handed -- see pstepper_handler_fn
 * in pstepper.h).
 *
 * What it pins down:
 *
 *  1. Every general-purpose register lands in the right gregs[] slot. The
 *     _Static_asserts in pstepper_amd64.h only prove pstepper_gpregs_t's *layout*
 *     matches gregset_t's; nothing static checks that the trampoline stores
 *     each machine register into the offset it claims to. So: set distinct
 *     64-bit sentinels in the callee-saved registers, call marker() with
 *     three more sentinels in rdi/rsi/rdx, and have the handler snapshot the
 *     context at marker()'s entry instruction (rip == &marker, before the
 *     prologue moves anything). Then assert each named greg holds its
 *     sentinel, rip == &marker, and rsp is on the real thread stack. A
 *     register shuffle -- in the asm or in the REG_* mapping -- shows up as
 *     a specific slot holding the wrong sentinel.
 *
 *  2. The conversion is one-way. The handler scribbles garbage into
 *     gregs[REG_RIP] / gregs[REG_RSP] on *every* step; the single-stepped
 *     floating-point result must still match a native reference bit-for-bit.
 *     If handler edits ever started feeding back into the resume, this
 *     faults or diverges immediately. (Safe to do unconditionally: the
 *     clone / thread-exit intercepts read the frame, never the ucontext.)
 *
 *  3. The fake fields stay fake: uc_mcontext.fpregs == NULL, gregs[REG_EFL]
 *     == 0, and uc_stack / uc_flags / uc_link all zeroed.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

#include "pstepper.h"

#define SENT_RBX 0x1111111111111111ULL
#define SENT_R12 0x2222222222222222ULL
#define SENT_R13 0x3333333333333333ULL
#define SENT_R14 0x4444444444444444ULL
#define SENT_R15 0x5555555555555555ULL
#define SENT_RDI 0xD1D1D1D1D1D1D1D1ULL
#define SENT_RSI 0x5252525252525252ULL
#define SENT_RDX 0xD3D3D3D3D3D3D3D3ULL

static volatile int g_have_marker_regs;
static gregset_t g_marker_gregs;

static volatile long g_steps;
static volatile long g_fake_field_bad;

/* Rough bounds of main()'s stack, filled in from a local's address. */
static uintptr_t g_sp_approx;

/* The instruction whose entry the handler recognizes. Address-taken and
 * marked used so the compiler keeps a real symbol called `marker` (rather
 * than a constprop clone) for the rip comparison to match. */
__attribute__((noinline, used)) static void marker(uint64_t a, uint64_t b, uint64_t c)
{
	__asm__ volatile("" : : "r"(a), "r"(b), "r"(c) : "memory");
}

static void call_marker_with_sentinels(void)
{
	/* `call marker` is a direct relative call even under -pie: marker is
	 * static, so the R_X86_64_PLT32 relocation resolves to a plain
	 * rip-relative call with no PLT stub in between -- the register setup
	 * above therefore reaches marker's entry instruction untouched. If
	 * GUEST_CFLAGS ever makes marker externally visible this would route
	 * through the PLT and the "registers exactly as set" claim weakens. */
	__asm__ volatile("movq $0x1111111111111111, %%rbx\n\t"
			 "movq $0x2222222222222222, %%r12\n\t"
			 "movq $0x3333333333333333, %%r13\n\t"
			 "movq $0x4444444444444444, %%r14\n\t"
			 "movq $0x5555555555555555, %%r15\n\t"
			 "call marker\n\t"
			 :
			 : "D"((uint64_t)SENT_RDI), "S"((uint64_t)SENT_RSI), "d"((uint64_t)SENT_RDX)
			 : "rbx", "r12", "r13", "r14", "r15", "rax", "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
}

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
	(void)uxi_kind;
	(void)info;
	greg_t *g = uc->uc_mcontext.gregs;
	g_steps++;

	/* Fake-field contract (checked every step -- cheap, and catches a
	 * regression wherever it first shows up). */
	if (uc->uc_mcontext.fpregs != NULL) {
		g_fake_field_bad++;
	}
	if (g[REG_EFL] != 0) {
		g_fake_field_bad++;
	}
	if (uc->uc_stack.ss_sp != NULL || uc->uc_stack.ss_size != 0 || uc->uc_stack.ss_flags != 0) {
		g_fake_field_bad++;
	}
	if (uc->uc_flags != 0 || uc->uc_link != NULL) {
		g_fake_field_bad++;
	}

	if (!g_have_marker_regs && (uintptr_t)g[REG_RIP] == (uintptr_t)&marker) {
		memcpy(g_marker_gregs, g, sizeof(g_marker_gregs));
		g_have_marker_regs = 1; /* publish after the copy */
	}

	/*
	 * One-way: trashing the fake context must not affect the resume. Hit
	 * both the words the resume reads (RIP/RSP -- though those actually
	 * come from the floating words above the frame, not the gpregs copy)
	 * and a couple the trampoline epilogue genuinely restores from the
	 * frame (RBX, R12): if pstepper_build_fake_ucontext ever grew a
	 * write-back path, the single-stepped FP loop's callee-saved registers
	 * would be destroyed and `got != want` fires immediately.
	 */
	g[REG_RIP] = (greg_t)0xdead000000000000ULL;
	g[REG_RSP] = (greg_t)0xdead000000000000ULL;
	g[REG_RBX] = (greg_t)0xdead000000000000ULL;
	g[REG_R12] = (greg_t)0xdead000000000000ULL;
}

__attribute__((noinline)) static double compute(int n)
{
	double acc = 1.0;
	for (int i = 1; i <= n; i++) {
		acc = acc * 1.0001 + (double)i;
	}
	return acc;
}

int main(void)
{
	int stack_anchor = 0;
	g_sp_approx = (uintptr_t)&stack_anchor;

	const int n = 4000;
	double want = compute(n); /* native reference, before stepping */

	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, 65536, 0);

	call_marker_with_sentinels();
	double got = compute(n);

	pstepper_disable();

	fprintf(stderr, "[guest] steps=%ld  fake-field violations=%ld\n", g_steps, g_fake_field_bad);
	fprintf(stderr, "[guest] compute()=%a  reference=%a\n", got, want);

	int fail = 0;

#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "[guest] FAIL: " __VA_ARGS__);                                                 \
			fprintf(stderr, "\n");                                                                         \
			fail = 1;                                                                                      \
		}                                                                                                      \
	} while (0)

	CHECK(g_have_marker_regs, "handler never saw rip == &marker");

	if (g_have_marker_regs) {
		const greg_t *g = g_marker_gregs;
#define CHECK_GREG(slot, sent)                                                                                         \
	CHECK((uint64_t)g[slot] == (sent), #slot " = 0x%" PRIx64 ", expected 0x%" PRIx64, (uint64_t)g[slot],           \
	      (uint64_t)(sent))

		CHECK_GREG(REG_RDI, SENT_RDI);
		CHECK_GREG(REG_RSI, SENT_RSI);
		CHECK_GREG(REG_RDX, SENT_RDX);
		CHECK_GREG(REG_RBX, SENT_RBX);
		CHECK_GREG(REG_R12, SENT_R12);
		CHECK_GREG(REG_R13, SENT_R13);
		CHECK_GREG(REG_R14, SENT_R14);
		CHECK_GREG(REG_R15, SENT_R15);
#undef CHECK_GREG

		CHECK((uintptr_t)g[REG_RIP] == (uintptr_t)&marker,
		      "REG_RIP = 0x%" PRIx64 ", expected &marker = 0x%" PRIx64, (uint64_t)g[REG_RIP],
		      (uint64_t)(uintptr_t)&marker);

		uint64_t rsp = (uint64_t)g[REG_RSP];
		CHECK(rsp < g_sp_approx && rsp > g_sp_approx - 0x100000,
		      "REG_RSP = 0x%" PRIx64 " not on the main thread stack "
		      "(anchor 0x%" PRIx64 ")",
		      rsp, (uint64_t)g_sp_approx);
	}

	CHECK(g_fake_field_bad == 0, "%ld fake-field violation(s)", g_fake_field_bad);
	CHECK(got == want, "single-stepped result != native reference -- "
			   "handler edits to the context leaked into the resume");

#undef CHECK

	fprintf(stderr, "[guest] %s\n", fail ? "FAIL" : "PASS");
	return fail;
}
