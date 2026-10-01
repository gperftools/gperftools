// SPDX-License-Identifier: 0BSD
/*
 * pstepper: arm64-specific guest-side plumbing. Split out of pstepper.c the
 * same way pstepper_amd64.c is. What lives here:
 *
 *   - recognizing the trapped instruction as `svc #0` and classifying it
 *     (CLONE_THREAD clone / per-thread exit);
 *   - the clone and exit intercepts, which build/patch a pstepper_frame_t
 *     (layout in pstepper_frame_arm64.h) and hand off to the asm helpers in
 *     pstepper_trampoline_arm64.S;
 *   - pstepper_invoke_handler: the trampoline's C entry point;
 *   - pstepper_build_fake_ucontext: the one-way trapped-regs -> ucontext_t
 *     conversion the handler is given.
 *
 * No XSAVE analogue: NEON is a fixed-size save the trampoline does directly,
 * with no runtime mask, so pstepper_arch_prepare() has nothing to compute
 * (see the SVE/SME note there). No knowledge of QEMU.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <ucontext.h>

#include "pstepper.h"
#include "pstepper_internal.h"
#include "pstepper_frame_arm64.h" /* pstepper_frame_t + its _Static_assert cross-checks */

/* CLONE_THREAD from <linux/sched.h> -- hardcoded rather than pulling that
 * header in next to glibc's <sched.h> (a known conflict). Stable kernel ABI. */
#define PSTEPPER_CLONE_THREAD 0x00010000UL

/* `svc #0` -- the only SVC encoding linux-user uses for a syscall. */
#define PSTEPPER_SVC0_INSN 0xd4000001u

#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif

/*
 * Read by pstepper_trampoline_arm64.S on every trap. Hidden so its adrp/ldr
 * resolves directly under -pie (no GOT bounce).
 *   pstepper_sve_vl        -- 0 if the CPU has no SVE, else VL in bytes
 *                             (also the MUL VL stride for the Z/P save).
 *   pstepper_sme_present   -- nonzero iff FEAT_SME; the trampoline then
 *                             checks PSTATE for streaming mode / ZA.
 */
__attribute__((visibility("hidden"))) uint64_t pstepper_sve_vl;
__attribute__((visibility("hidden"))) uint64_t pstepper_sme_present;

/* `rdvl x0, #1`, defined in the .S -- UNDEFINED without SVE, so call only
 * after the HWCAP_SVE check below. */
extern uint64_t pstepper_sve_read_vl(void);

/*
 * Arch half of pstepper_enable(), run before stepping is armed (the
 * trampoline reads the globals above on the very first trap). On amd64 this
 * computes the xsave mask; here it decides whether SVE state has to be
 * saved and at what vector length.
 */
void pstepper_arch_prepare(void)
{
	/* The upcall stack must hold at least one trampoline frame with room to
	 * spare for the handler chain. pstepper_upcall_stack_size is set by
	 * pstepper_enable() just before this runs. */
	if (pstepper_upcall_stack_size < PSTEPPER_FRAME_SIZE + 4096) {
		pstepper_die("upcall stack is smaller than one trampoline frame -- pass a larger size to pstepper_enable()");
	}

	unsigned long hwcap = getauxval(AT_HWCAP);
	if (hwcap & HWCAP_SVE) {
		uint64_t vl = pstepper_sve_read_vl();
		if (vl == 0 || vl % 16 != 0 || vl > PSTEPPER_SVE_VL_MAX) {
			pstepper_die("SVE vector length out of the range the trampoline frame reserves");
		}
		pstepper_sve_vl = vl;
	}
	pstepper_sme_present = (getauxval(AT_HWCAP2) & HWCAP2_SME) ? 1 : 0;
}

/* Defined in pstepper_trampoline_arm64.S. */
extern void pstepper_clone_helper(pstepper_gpregs_t *frame, uint64_t continue_pc, uint64_t child_upcall_base,
				  uint64_t child_frame_base) __attribute__((noreturn));
extern void pstepper_thread_exit(uint64_t resume_sp, uint64_t upcall_base, uint64_t upcall_size, uint64_t status)
	__attribute__((noreturn));

/* True iff the trapped instruction is `svc #0`. regs->pc points at it, on an
 * executable page. x8 alone is a weak signal (any instruction may hold a
 * small integer there), so anything keying off a syscall number confirms
 * the opcode first. */
static int pstepper_insn_is_syscall(const pstepper_gpregs_t *regs)
{
	uint32_t insn;
	memcpy(&insn, (const void *)(uintptr_t)regs->pc, sizeof(insn));
	return insn == PSTEPPER_SVC0_INSN;
}

static int pstepper_is_thread_clone(const pstepper_gpregs_t *regs)
{
	/* qemu-user ENOSYSes clone3, so glibc falls back to legacy clone. */
	return pstepper_insn_is_syscall(regs) && regs->x[8] == (uint64_t)SYS_clone &&
	       (regs->x[0] & PSTEPPER_CLONE_THREAD) != 0;
}

/* A plain per-thread exit(2) (SYS_exit, not SYS_exit_group -- the latter is a
 * whole-process teardown the kernel handles). This is how glibc ends a
 * pthread that returns from its start routine. */
static int pstepper_is_thread_exit(const pstepper_gpregs_t *regs)
{
	return pstepper_insn_is_syscall(regs) && regs->x[8] == (uint64_t)SYS_exit;
}

/*
 * Runs on the parent's vCPU, before the real clone(2) -- still
 * single-threaded -- once pstepper_is_thread_clone() has recognized a
 * CLONE_THREAD clone about to happen. Mmaps the child's upcall stack,
 * computes continue_pc, and lays a complete ready-to-resume trampoline
 * frame on that stack, positioned exactly where pstepper_trampoline would
 * have put one. All handed to pstepper_clone_helper as arguments / in the
 * child's own memory, so neither post-syscall branch reads back the
 * parent's frame (which races the parent's next trap reusing that memory).
 *
 * arm64 raw clone(2) args are (flags, newsp, parent_tid, tls, child_tid) in
 * x0..x4 -- tls and child_tid are swapped vs x86-64 (see man 2 clone). The
 * child resumes past a 4-byte svc, so continue_pc = pc + 4.
 */
__attribute__((noreturn)) static void pstepper_do_clone_intercept(pstepper_gpregs_t *regs)
{
	pstepper_frame_t *pframe = (pstepper_frame_t *)regs;

	void *child_mem = pstepper_alloc_upcall_stack(pstepper_upcall_stack_size);
	uint64_t child_base = (uint64_t)(uintptr_t)child_mem;

	uint64_t continue_pc = pframe->gpr.pc + 4;
	uint64_t child_stack_top = pframe->gpr.x[1]; /* raw clone(2) newsp */

	/*
	 * Frame geometry: pstepper_trampoline is entered with SP =
	 * child_top - 16 (the [PC][SP] pair staged there) and immediately does
	 * `sub sp, sp, #FRAME_SIZE`. So the frame proper sits at child_entry -
	 * FRAME_SIZE, and the floating [PC][SP] pair right above it at
	 * child_frame + FRAME_SIZE (== child_entry). No flags word floats here
	 * (unlike amd64).
	 *
	 * A real clone gives the child a faithful copy of the parent's whole
	 * register file, overriding only x0 = 0 and sp = newsp. Copy the
	 * parent frame verbatim (GPRs + NEON), then patch.
	 */
	uint64_t child_top = pstepper_upcall_top(child_base);
	uint64_t child_entry = child_top - 16;
	uint64_t child_frame = child_entry - PSTEPPER_FRAME_SIZE;

	pstepper_frame_t *cf = (pstepper_frame_t *)(uintptr_t)child_frame;
	*cf = *pframe;
	cf->gpr.x[0] = 0;	    /* the child's clone(2) return */
	cf->gpr.pc = continue_pc;   /* informational copies only */
	cf->gpr.sp = child_stack_top;

	uint64_t *cfw = (uint64_t *)(uintptr_t)(child_frame + PSTEPPER_FRAME_SIZE);
	cfw[0] = continue_pc;	   /* floating PC the epilogue resumes to */
	cfw[1] = child_stack_top;   /* floating SP */

	pstepper_clone_helper(regs, continue_pc, child_base, child_frame);
}

/*
 * Runs when a stepping-armed thread is about to exit(2). We're on this
 * thread's upcall stack, so we can't munmap it from under ourselves:
 *   1. PSTEPPER_CMD_DISABLE first, while still on the upcall stack -- once
 *      SP moves off it every instruction would otherwise re-trap into the
 *      trampoline and scribble on the stack we're about to free.
 *   2. Hand off to pstepper_thread_exit (asm): switch SP to the thread's
 *      real stack (its value at the exit syscall site, in the frame),
 *      munmap the upcall stack, issue the real exit(2).
 */
__attribute__((noreturn)) static void pstepper_do_exit_intercept(pstepper_gpregs_t *frame)
{
	uint64_t status = frame->x[0];
	uint64_t resume_sp = frame->sp;
	uint64_t base = (uint64_t)(uintptr_t)pstepper_own_upcall_stack;
	uint64_t size = pstepper_upcall_stack_size;

	pstepper_syscall(PSTEPPER_CMD_DISABLE, 0, 0, 0, 0, 0);
	pstepper_thread_exit(resume_sp, base, size, status); /* noreturn */
}

/*
 * Baseline conversion of the trapped register snapshot into the "fake"-ish
 * ucontext_t the handler gets (contract: pstepper_handler_fn in pstepper.h).
 * Built fresh on the upcall stack each upcall, thrown away on return -- the
 * conversion is strictly one-way.
 *
 * pstepper_gpregs_t is laid out as the {regs[31], sp, pc} run of
 * mcontext_t (asserted in pstepper_arm64.h), so the integer file goes in
 * with one memcpy. Everything else is deliberately fake:
 *  - uc_mcontext.pstate stays 0 (NZCV *is* captured in the frame, so this is
 *    a deferral, not an oversight -- a backtrace consumer does not need it;
 *    same call as amd64 leaving REG_EFL at 0). fault_address stays 0.
 *  - the FP state chain in __reserved is left zeroed -- an immediate
 *    _aarch64_ctx terminator, the analogue of amd64's fpregs = NULL. NEON
 *    state is preserved transparently by the trampoline, never surfaced.
 *  - uc_stack / uc_sigmask / uc_flags / uc_link are zeroed.
 */
static void pstepper_build_fake_ucontext(ucontext_t *uc, const pstepper_gpregs_t *regs)
{
	_Static_assert(sizeof(*regs) == 33 * sizeof(uint64_t), "pstepper_gpregs_t must be x[31] + sp + pc");

	memset(uc, 0, sizeof(*uc));
	/* regs[31], then sp, then pc -- contiguous in mcontext_t. */
	memcpy(&uc->uc_mcontext.regs[0], regs, sizeof(*regs));
}

void pstepper_invoke_handler(pstepper_gpregs_t *regs)
{
	/*
	 * Deliver the ordinary per-instruction step upcall first, even when
	 * this instruction turns out to be a CLONE_THREAD svc we're about to
	 * intercept -- from the consumer's point of view the clone is then
	 * just another stepped `svc` (pc here, then pc+4 with x0 = the new
	 * tid). The child never gets a step upcall for this instruction: it
	 * resumes past the svc at continue_pc.
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
