// SPDX-License-Identifier: 0BSD
/*
 * pstepper: amd64-specific guest-side plumbing. Split out of pstepper.c so
 * the portable core there (upcall-stack management, arming/disarming,
 * enable/disable, the clone-child bootstrap) carries no x86 detail. What
 * lives here:
 *
 *   - the xsave/xrstor request-feature mask the trampoline uses around every
 *     handler call, and its host-capability checks;
 *   - recognizing the trapped instruction as `syscall` and classifying it
 *     (CLONE_THREAD clone / per-thread exit);
 *   - the clone and exit intercepts themselves, which build/patch a
 *     pstepper_frame_t (layout in pstepper_frame_amd64.h) and hand off to the
 *     asm helpers in pstepper_trampoline_amd64.S;
 *   - pstepper_invoke_handler: the trampoline's C entry point -- deliver the
 *     step upcall, then run an intercept if this instruction warrants one.
 *
 * No knowledge of QEMU. See pstepper.h for the overall design.
 */

#include <cpuid.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>

#include "pstepper.h"
#include "pstepper_frame_amd64.h" /* pstepper_frame_t (clone-child frame building)
                                   * + its _Static_assert offset cross-checks */
#include "pstepper_internal.h"

/* CLONE_THREAD from <linux/sched.h> -- hardcoded rather than including that
 * header directly, since mixing it with glibc's own <sched.h> has a history
 * of definition conflicts. This bit is part of the stable kernel ABI. */
#define PSTEPPER_CLONE_THREAD 0x00010000UL

/*
 * xsave/xrstor request-feature bitmap (edx:eax) used by
 * pstepper_trampoline_amd64.S around every handler call. Process-wide:
 * computed once by the first pstepper_enable() (well before any clone), read
 * directly by the asm. Hidden visibility so the trampoline's RIP-relative
 * load resolves without a GOT indirection under -pie.
 *
 * Contents: (host XCR0) & (FP | SSE | all vector-state components). The
 * floor is FP|SSE|YMM = 0x7 (unlike glibc's _dl_runtime_resolve, which drops
 * x87 -- it interposes at a call boundary where the x87 stack is ABI-dead;
 * pstepper interposes at an arbitrary instruction boundary where it can be
 * live). AVX-512 opmask/ZMM bits are included so the mask is already correct
 * on a real AVX-512 host (the eventual aw-backtrace meld); they just mask
 * away to nothing under QEMU TCG, which caps at AVX2. Deliberately excluded:
 * MPX BND* (dead ISA), PKRU (a backtrace handler never executes WRPKRU), and
 * AMX tile data (huge; enforced by the size check below).
 */
__attribute__((visibility("hidden"))) uint64_t pstepper_xsave_mask;

#define PSTEPPER_XSTATE_X87 (1ull << 0)
#define PSTEPPER_XSTATE_SSE (1ull << 1)
#define PSTEPPER_XSTATE_YMM (1ull << 2)
#define PSTEPPER_XSTATE_OPMASK (1ull << 5)
#define PSTEPPER_XSTATE_ZMM_HI256 (1ull << 6)
#define PSTEPPER_XSTATE_HI16_ZMM (1ull << 7)

#define PSTEPPER_XSAVE_WANTED                                                                                          \
	(PSTEPPER_XSTATE_X87 | PSTEPPER_XSTATE_SSE | PSTEPPER_XSTATE_YMM | PSTEPPER_XSTATE_OPMASK |                    \
	 PSTEPPER_XSTATE_ZMM_HI256 | PSTEPPER_XSTATE_HI16_ZMM)

/* Compute pstepper_xsave_mask from the live XCR0. Runs in the guest, so the
 * cpuid / xgetbv below see the guest CPU (qemu -cpu max). */
static uint64_t pstepper_compute_xsave_mask(void)
{
	unsigned int eax, ebx, ecx, edx;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & (1u << 27))) {
		pstepper_die("CPU lacks OSXSAVE -- xsave-based state save unavailable");
	}

	uint32_t xcr0_lo, xcr0_hi;
	__asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
	uint64_t xcr0 = ((uint64_t)xcr0_hi << 32) | xcr0_lo;

	/* Force FP|SSE on unconditionally: it is the floor the trampoline asm
	 * assumes (a zero mask would make its xsave/xrstor silent no-ops), and
	 * SSE must be set whenever YMM is or the RFBM is invalid. */
	return (xcr0 & PSTEPPER_XSAVE_WANTED) | PSTEPPER_XSTATE_X87 | PSTEPPER_XSTATE_SSE;
}

/* Abort if the standard-format XSAVE area for `mask` would not fit the
 * compile-time reservation in pstepper_frame_amd64.h. Precise per-component
 * walk (CPUID.0Dh.n) rather than CPUID.0Dh.0:EBX, which counts the whole
 * enabled XCR0 and would over-abort on, e.g., an AMX-capable host. */
static void pstepper_check_xsave_area_fits(uint64_t mask)
{
	uint32_t need = 512 + 64; /* legacy area + XSAVE header, always present */
	for (int i = 2; i < 63; i++) {
		if (!(mask & (1ull << i))) {
			continue;
		}
		unsigned int eax, ebx, ecx, edx;
		__cpuid_count(0x0d, i, eax, ebx, ecx, edx);
		/* standard format: ebx = byte offset in the area, eax = size */
		if (ebx + eax > need) {
			need = ebx + eax;
		}
	}
	if (need > PSTEPPER_XSAVE_AREA_SIZE) {
		fprintf(stderr,
			"pstepper: xsave area for mask 0x%" PRIx64
			" needs %u bytes, compiled reservation PSTEPPER_XSAVE_AREA_SIZE is %d -- rebuild with a larger "
			"value\n",
			mask, need, PSTEPPER_XSAVE_AREA_SIZE);
		abort();
	}
}

/*
 * Arch half of pstepper_enable(): decide the trampoline's xsave/xrstor mask
 * and confirm the host's state area fits the compiled reservation, before
 * stepping is armed (the asm reads pstepper_xsave_mask on the very first
 * trap). Called from the portable pstepper_enable() in pstepper.c.
 */
void pstepper_arch_prepare(void)
{
	pstepper_xsave_mask = pstepper_compute_xsave_mask();
	pstepper_check_xsave_area_fits(pstepper_xsave_mask);
}

/* Defined in pstepper_trampoline_amd64.S. */
extern void pstepper_clone_helper(pstepper_gpregs_t *frame, uint64_t continue_rip, uint64_t child_upcall_base,
				  uint64_t child_frame_base) __attribute__((noreturn));
extern void pstepper_thread_exit(uint64_t resume_rsp, uint64_t upcall_base, uint64_t upcall_size, uint64_t status)
	__attribute__((noreturn));

/* True iff the trapped instruction is `syscall` (opcode 0f 05). rax alone is
 * a weak signal for the checks below -- the trampoline captures it for every
 * instruction, and plenty of them transiently hold a small integer there --
 * so anything keying off a syscall number must confirm the opcode first.
 * regs->rip points at the trapped instruction, on an executable page. */
static int pstepper_insn_is_syscall(const pstepper_gpregs_t *regs)
{
	const unsigned char *p = (const unsigned char *)(uintptr_t)regs->rip;
	return p[0] == 0x0f && p[1] == 0x05;
}

static int pstepper_is_thread_clone(const pstepper_gpregs_t *regs)
{
	// TODO: qemu user currently ENOSYSes clone3, so we're good at least for now.
	return pstepper_insn_is_syscall(regs) && regs->rax == SYS_clone && (regs->rdi & PSTEPPER_CLONE_THREAD) != 0;
}

/* A plain per-thread exit(2) (SYS_exit, not SYS_exit_group -- the latter is a
 * whole-process teardown the kernel handles for us). This is how glibc ends a
 * pthread that returns from its start routine. */
static int pstepper_is_thread_exit(const pstepper_gpregs_t *regs)
{
	return pstepper_insn_is_syscall(regs) && regs->rax == SYS_exit;
}

/*
 * Runs on the *parent's* vCPU, before the real clone(2) syscall -- still
 * single-threaded, so no race with anything yet -- once
 * pstepper_is_thread_clone() has recognized a CLONE_THREAD clone about to
 * happen. Does all the prep the not-yet-existing child will need: mmaps its
 * upcall stack, computes the post-syscall continue_rip, and lays down a
 * complete ready-to-resume trampoline frame on that stack (see below). All
 * of it is handed to pstepper_clone_helper as plain arguments / lives in
 * the child's own memory, so neither branch coming out of the real syscall
 * ever needs to read anything back from the parent's frame -- which would
 * otherwise race against the parent's own very next single-stepped trap
 * reusing the same upcall stack memory.
 */
__attribute__((noreturn)) static void pstepper_do_clone_intercept(pstepper_gpregs_t *regs)
{
	/* regs points at the base of the trampoline's full pstepper_frame_t
	 * (its gpr member is first) -- we need the xsave area and the frame
	 * geometry too, so view the whole thing. */
	pstepper_frame_t *pframe = (pstepper_frame_t *)regs;

	void *child_mem = pstepper_alloc_upcall_stack(pstepper_upcall_stack_size);
	uint64_t child_base = (uint64_t)(uintptr_t)child_mem;

	/* +2: past the 2-byte `syscall` opcode we intercepted -- the kernel
	 * resumes both parent and child right after it, never on it. */
	uint64_t continue_rip = pframe->gpr.rip + 2;
	uint64_t child_stack_top = pframe->gpr.rsi; /* raw clone(2)'s child_stack */

	/*
	 * Lay the child a complete trampoline frame on its own upcall stack,
	 * positioned exactly where pstepper_trampoline would have put one: the
	 * frame proper at child_frame, then the floating [eflags][rip][rsp]
	 * triplet immediately above it (child_frame + FRAME_SIZE{,+8,+16}).
	 * pstepper_clone_helper's child branch runs bootstrap without ever
	 * disturbing this region, then pstepper_resume_via_frame points RSP
	 * here and lets the shared trampoline epilogue restore everything.
	 *
	 * A real clone/clone3 gives the child a faithful copy of the parent's
	 * whole register file (GPRs + RFLAGS + FP/vector state), overriding
	 * only rax = 0 and rsp = child_stack. So copy the parent frame verbatim
	 * -- GPRs and the xsave area both -- then patch. rcx/r11, which a real
	 * `syscall` would also overwrite (with continue_rip and RFLAGS), are
	 * left as the parent's values, matching the parent-resume path; nothing
	 * downstream reads them. Safe to read pframe here: pre-fork,
	 * single-threaded, the parent's own memory that the child never
	 * touches.
	 */
	uint64_t child_top = pstepper_upcall_top(child_base);
	uint64_t child_entry = child_top - 16; /* == pstepper_trampoline entry RSP */
	uint64_t child_frame = child_entry - 8 - PSTEPPER_FRAME_SIZE;

	pstepper_frame_t *cf = (pstepper_frame_t *)(uintptr_t)child_frame;
	*cf = *pframe;		    /* GPRs + xsave area, verbatim */
	cf->gpr.rax = 0;	    /* the child's clone(2) return */
	cf->gpr.rip = continue_rip; /* informational copies only */
	cf->gpr.rsp = child_stack_top;

	unsigned char *cfw = (unsigned char *)(uintptr_t)child_frame + PSTEPPER_FRAME_SIZE;
	unsigned char *pfw = (unsigned char *)pframe + PSTEPPER_FRAME_SIZE;
	*(uint64_t *)(cfw + 0) = *(uint64_t *)(pfw + 0); /* eflags: the parent's */
	*(uint64_t *)(cfw + 8) = continue_rip;
	*(uint64_t *)(cfw + 16) = child_stack_top;

	pstepper_clone_helper(regs, continue_rip, child_base, child_frame);
}

/*
 * Runs when a stepping-armed thread is about to exit(2). We're on this
 * thread's upcall stack (the trampoline put us here, same as any trap), so
 * we can't just munmap it from under ourselves. Sequence:
 *   1. PSTEPPER_CMD_DISABLE *first*, while still on the upcall stack -- once
 *      RSP moves off it every instruction would otherwise re-trap into the
 *      trampoline and scribble on the stack we're about to free.
 *   2. Hand off to pstepper_thread_exit (asm): switch RSP to the thread's
 *      real stack (its value at the exit syscall site, captured in the
 *      frame), munmap the upcall stack, then issue the real exit(2).
 * If stepping was never armed for this thread none of this runs -- we'd
 * never have been called.
 */
__attribute__((noreturn)) static void pstepper_do_exit_intercept(pstepper_gpregs_t *frame)
{
	uint64_t status = frame->rdi;
	uint64_t resume_rsp = frame->rsp;
	uint64_t base = (uint64_t)(uintptr_t)pstepper_own_upcall_stack;
	uint64_t size = pstepper_upcall_stack_size;

	pstepper_syscall(PSTEPPER_CMD_DISABLE, 0, 0, 0, 0, 0);
	pstepper_thread_exit(resume_rsp, base, size, status); /* noreturn */
}

/*
 * Baseline conversion of a trapped-instruction register snapshot into the
 * "fake"-ish ucontext_t the handler is given (see pstepper_handler_fn in
 * pstepper.h for the contract). Built fresh on the upcall stack for each
 * upcall and thrown away when the handler returns -- the conversion is
 * strictly one-way, nothing here is ever read back to drive the resume
 * (the trampoline epilogue restores state from the frame, which this never
 * aliases).
 *
 * Only the integer register file is populated. pstepper_gpregs_t's field
 * order is pinned to gregset_t's REG_* indices by the _Static_assert block
 * in pstepper_amd64.h, so the GPRs go in with a single prefix memcpy rather
 * than a field-by-field shuffle.
 *
 * Everything else is deliberately fake:
 *  - fpregs = NULL, not &uc->__fpregs_mem. No FP/SIMD state is exposed here
 *    (it is preserved transparently by the trampoline's xsave/xrstor, just
 *    never surfaced). NULL makes a consumer that dereferences it fault
 *    loudly instead of reading an uninitialised buffer -- do not "fix" this
 *    to point at __fpregs_mem.
 *  - gregs[REG_EFL] stays zero. The real eflags *is* available (the
 *    trampoline captures it one word above the frame), so this is a
 *    deliberate deferral, not an oversight -- a backtrace consumer does not
 *    need it. REG_CSGSFS / REG_ERR / REG_TRAPNO / REG_OLDMASK / REG_CR2
 *    likewise stay zero.
 *  - uc_stack is a zeroed stack_t, not the real upcall stack geometry;
 *    uc_sigmask is empty; uc_flags / uc_link are zero.
 */
static void pstepper_build_fake_ucontext(ucontext_t *uc, const pstepper_gpregs_t *regs)
{
	_Static_assert(sizeof(regs->r8) == sizeof(greg_t) && sizeof(*regs) <= sizeof(uc->uc_mcontext.gregs),
		       "pstepper_gpregs_t does not fit gregset_t");

	memset(uc, 0, sizeof(*uc));
	memcpy(uc->uc_mcontext.gregs, regs, sizeof(*regs));
	uc->uc_mcontext.fpregs = NULL;
}

void pstepper_invoke_handler(pstepper_gpregs_t *regs)
{
	/*
	 * Deliver the ordinary per-instruction step upcall first, even when
	 * this instruction turns out to be a CLONE_THREAD syscall we're about
	 * to intercept. From the consumer's point of view the clone is then
	 * just another stepped `syscall` (it sees rip at the syscall here, then
	 * next at rip+2 with rax holding the new tid) -- the trap logically
	 * happens in the parent, before the child exists. The child never gets
	 * a step upcall for this instruction: it resumes past the syscall at
	 * continue_rip.
	 */
	if (pstepper_user_handler) {
		ucontext_t uc;
		pstepper_build_fake_ucontext(&uc, regs);
		pstepper_user_handler(PSTEPPER_UXI_STEP, NULL, &uc);
	}
	if (pstepper_is_thread_clone(regs)) {
		pstepper_do_clone_intercept(regs); /* noreturn */
	}
	if (pstepper_is_thread_exit(regs)) {
		pstepper_do_exit_intercept(regs); /* noreturn */
	}
}
