/*
 * SPDX-License-Identifier: 0BSD
 *
 * pstepper_trampoline (arm64): what the plugin jumps PC to on every trap --
 * the start of a UXI upcall. Entry state, arranged by the plugin: SP =
 * upcall_stack_top - 16, with [SP] = trapped PC and [SP+8] = trapped SP
 * (the guest's real stack pointer at the trap -- untouched, never written).
 *
 * Sequence:
 *   1. Open the frame (sub sp) and save x0..x30 into it -- plain stores, no
 *      scratch register needed, and none of x0..x30 is clobbered until they
 *      are all saved. (x30/LR is live guest state and the internal `bl`
 *      below will overwrite it, so it must be saved up front like any GPR.)
 *   2. Copy the floating [PC][SP] words (now at sp+FRAME_SIZE{,+8}) into the
 *      frame's gpr.pc / gpr.sp so the handler can read them.
 *   3. Save NZCV (mrs), NEON (V0..V31 + FPSR + FPCR), and -- if the CPU has
 *      SVE -- Z0..Z31 + P0..P15 + FFR. Real instructions, so no lazy-flag
 *      hazard. If SME is present and PSTATE says streaming mode or ZA is
 *      active, bail: that state is not saved.
 *   4. bl pstepper_invoke_handler with x0 = &frame (== &gpr).
 *   5. On return: restore SVE, NEON, NZCV, x0..x30 from the frame (frame
 *      memory is otherwise untouched), close the frame (add sp) -- landing
 *      exactly on the still-intact [PC][SP] pair.
 *   6. Execute the resume marker (brk). The plugin recognizes this address,
 *      reads [SP]/[SP+8], and installs them as the new guest PC/SP.
 *
 * SP is the only addressing base; it moves only at the sub/add accounted
 * for above. Nothing between `msr nzcv` and the resume marker sets flags.
 */
	.arch armv8-a+sve
#include "pstepper_frame_arm64.h"

/* arm64 raw syscall numbers (asm-generic/unistd.h) -- only needed by the
 * noreturn helpers here; the C side uses glibc SYS_* from <sys/syscall.h>. */
#define PSTEPPER_NR_CLONE  220
#define PSTEPPER_NR_MUNMAP 215
#define PSTEPPER_NR_EXIT    93

/* Set once by pstepper_arch_prepare() before stepping is ever armed. Hidden,
 * so the adrp/ldr below resolves directly under -pie with no GOT bounce.
 * pstepper_sve_vl: 0 if no SVE, else the vector length in bytes (also the
 * MUL VL stride). pstepper_sme_present: nonzero iff FEAT_SME. */
	.extern pstepper_sve_vl
	.extern pstepper_sme_present

/*
 * CFI so gdb (and any .eh_frame unwinder) can walk through an in-progress
 * upcall into the interrupted guest code. CFA is pinned to the upcall stack
 * top -- the fixed point just above the floating [PC][SP] pair -- so every
 * register rule is a plain constant displacement.
 *
 *     [CFA - 16] = trapped PC   (floating word, written by the plugin)
 *     [CFA -  8] = trapped SP   (floating word, written by the plugin)
 *     trapped x_n -> CFA + (PSTEPPER_OFF_X0 + 8*n - PSTEPPER_FRAME_SIZE - 16)
 *
 * .cfi_signal_frame: take the trapped PC as exact (no "return address - 1"
 * nudge) and skip the caller-SP-must-exceed-callee-SP check (the guest
 * stack and the mmap'd upcall stack have no address ordering).
 *
 * DWARF regs: x0..x30 = 0..30, SP = 31, PC = 32. Vector CFI (V/Z/P) is
 * omitted -- an unwinder walking a backtrace never needs it.
 */
	.macro guest_gpr n
	.cfi_offset \n, (PSTEPPER_OFF_X0 + 8*\n) - PSTEPPER_FRAME_SIZE - 16
	.endm

	.text
	.global pstepper_trampoline
	.type pstepper_trampoline, %function
pstepper_trampoline:
	.cfi_startproc
	.cfi_signal_frame
	.cfi_def_cfa 31, 16		/* entry SP = upcall_top - 16 */
	.cfi_offset 32, -16		/* trapped PC at [CFA-16] */
	.cfi_offset 31, -8		/* trapped SP at [CFA-8]  */
	/* x0..x30 still live in their real registers here. */
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.cfi_same_value \r
	.endr

	/* Two instructions: the frame is larger than a single `sub sp` imm.
	 * The CFA offset is only declared after both, so an unwinder that lands
	 * on the exact boundary between them sees a stale CFA -- a one-insn
	 * window in a signal-frame trampoline nobody breakpoints; not worth a
	 * throwaway intermediate .cfi_def_cfa_offset. */
	sub	sp, sp, #(PSTEPPER_FRAME_SIZE >> 12), lsl #12
	sub	sp, sp, #(PSTEPPER_FRAME_SIZE & 0xfff)
	.cfi_def_cfa_offset PSTEPPER_FRAME_SIZE + 16

	stp	x0,  x1,  [sp, #PSTEPPER_OFF_X0 + 0]
	stp	x2,  x3,  [sp, #PSTEPPER_OFF_X0 + 16]
	stp	x4,  x5,  [sp, #PSTEPPER_OFF_X0 + 32]
	stp	x6,  x7,  [sp, #PSTEPPER_OFF_X0 + 48]
	stp	x8,  x9,  [sp, #PSTEPPER_OFF_X0 + 64]
	stp	x10, x11, [sp, #PSTEPPER_OFF_X0 + 80]
	stp	x12, x13, [sp, #PSTEPPER_OFF_X0 + 96]
	stp	x14, x15, [sp, #PSTEPPER_OFF_X0 + 112]
	stp	x16, x17, [sp, #PSTEPPER_OFF_X0 + 128]
	stp	x18, x19, [sp, #PSTEPPER_OFF_X0 + 144]
	stp	x20, x21, [sp, #PSTEPPER_OFF_X0 + 160]
	stp	x22, x23, [sp, #PSTEPPER_OFF_X0 + 176]
	stp	x24, x25, [sp, #PSTEPPER_OFF_X0 + 192]
	stp	x26, x27, [sp, #PSTEPPER_OFF_X0 + 208]
	stp	x28, x29, [sp, #PSTEPPER_OFF_X0 + 224]
	str	x30,      [sp, #PSTEPPER_OFF_X0 + 240]

	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	guest_gpr \r
	.endr

	/* floating [PC][SP] -> frame gpr.pc / gpr.sp */
	ldr	x9, [sp, #PSTEPPER_FRAME_SIZE + 0]
	str	x9, [sp, #PSTEPPER_OFF_PC]
	ldr	x9, [sp, #PSTEPPER_FRAME_SIZE + 8]
	str	x9, [sp, #PSTEPPER_OFF_SP]

	/* NZCV */
	mrs	x9, nzcv
	str	x9, [sp, #PSTEPPER_OFF_NZCV]

	/* NEON: V0..V31, then FPSR/FPCR */
	stp	q0,  q1,  [sp, #PSTEPPER_OFF_VREGS + 0]
	stp	q2,  q3,  [sp, #PSTEPPER_OFF_VREGS + 32]
	stp	q4,  q5,  [sp, #PSTEPPER_OFF_VREGS + 64]
	stp	q6,  q7,  [sp, #PSTEPPER_OFF_VREGS + 96]
	stp	q8,  q9,  [sp, #PSTEPPER_OFF_VREGS + 128]
	stp	q10, q11, [sp, #PSTEPPER_OFF_VREGS + 160]
	stp	q12, q13, [sp, #PSTEPPER_OFF_VREGS + 192]
	stp	q14, q15, [sp, #PSTEPPER_OFF_VREGS + 224]
	stp	q16, q17, [sp, #PSTEPPER_OFF_VREGS + 256]
	stp	q18, q19, [sp, #PSTEPPER_OFF_VREGS + 288]
	stp	q20, q21, [sp, #PSTEPPER_OFF_VREGS + 320]
	stp	q22, q23, [sp, #PSTEPPER_OFF_VREGS + 352]
	stp	q24, q25, [sp, #PSTEPPER_OFF_VREGS + 384]
	stp	q26, q27, [sp, #PSTEPPER_OFF_VREGS + 416]
	stp	q28, q29, [sp, #PSTEPPER_OFF_VREGS + 448]
	stp	q30, q31, [sp, #PSTEPPER_OFF_VREGS + 480]
	mrs	x9, fpsr
	str	x9, [sp, #PSTEPPER_OFF_FPSR]
	mrs	x9, fpcr
	str	x9, [sp, #PSTEPPER_OFF_FPCR]

	/* SVE: Z0..Z31 + P0..P15 + FFR, only if the CPU has it. */
	adrp	x9, pstepper_sve_vl
	ldr	x9, [x9, #:lo12:pstepper_sve_vl]
	cbz	x9, .Lsve_save_done
	add	x10, sp, #PSTEPPER_OFF_SVE
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	str	z\r, [x10, #\r, mul vl]
	.endr
	addvl	x11, x10, #16
	addvl	x11, x11, #16		/* P area base = SVE base + 32*VL */
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	str	p\r, [x11, #\r, mul vl]
	.endr
	rdffr	p0.b			/* P0 <- FFR (real P0 already stored) */
	addvl	x12, x11, #2		/* FFR slot = P base + 2*VL */
	str	p0,  [x12, #0, mul vl]
.Lsve_save_done:

	/* SME: streaming mode / ZA are not saved -- bail loudly if active. */
	adrp	x9, pstepper_sme_present
	ldr	x9, [x9, #:lo12:pstepper_sme_present]
	cbz	x9, .Lsme_check_done
	mrs	x9, S3_3_C4_C2_2	/* SVCR: bit0 = SM, bit1 = ZA */
	tst	x9, #3
	b.eq	.Lsme_check_done
	adrp	x0, .Lsme_msg
	add	x0, x0, #:lo12:.Lsme_msg
	bl	pstepper_die		/* noreturn */
.Lsme_check_done:

	mov	x0, sp
	bl	pstepper_invoke_handler
	/*
	 * pstepper_invoke_handler either returns normally (ordinary step --
	 * fall through) or diverts internally and never returns (a trap that
	 * turned out to be a CLONE_THREAD clone or a thread exit, handled on
	 * the C side). Either way SP is unchanged since the bl.
	 */
pstepper_trampoline_epilogue:
	ldp	q0,  q1,  [sp, #PSTEPPER_OFF_VREGS + 0]
	ldp	q2,  q3,  [sp, #PSTEPPER_OFF_VREGS + 32]
	ldp	q4,  q5,  [sp, #PSTEPPER_OFF_VREGS + 64]
	ldp	q6,  q7,  [sp, #PSTEPPER_OFF_VREGS + 96]
	ldp	q8,  q9,  [sp, #PSTEPPER_OFF_VREGS + 128]
	ldp	q10, q11, [sp, #PSTEPPER_OFF_VREGS + 160]
	ldp	q12, q13, [sp, #PSTEPPER_OFF_VREGS + 192]
	ldp	q14, q15, [sp, #PSTEPPER_OFF_VREGS + 224]
	ldp	q16, q17, [sp, #PSTEPPER_OFF_VREGS + 256]
	ldp	q18, q19, [sp, #PSTEPPER_OFF_VREGS + 288]
	ldp	q20, q21, [sp, #PSTEPPER_OFF_VREGS + 320]
	ldp	q22, q23, [sp, #PSTEPPER_OFF_VREGS + 352]
	ldp	q24, q25, [sp, #PSTEPPER_OFF_VREGS + 384]
	ldp	q26, q27, [sp, #PSTEPPER_OFF_VREGS + 416]
	ldp	q28, q29, [sp, #PSTEPPER_OFF_VREGS + 448]
	ldp	q30, q31, [sp, #PSTEPPER_OFF_VREGS + 480]
	ldr	x9, [sp, #PSTEPPER_OFF_FPCR]
	msr	fpcr, x9
	ldr	x9, [sp, #PSTEPPER_OFF_FPSR]
	msr	fpsr, x9

	/* SVE restore (mirror of the save; scratch x9..x12 are restored from
	 * the frame with the other GPRs below). */
	adrp	x9, pstepper_sve_vl
	ldr	x9, [x9, #:lo12:pstepper_sve_vl]
	cbz	x9, .Lsve_restore_done
	add	x10, sp, #PSTEPPER_OFF_SVE
	addvl	x11, x10, #16
	addvl	x11, x11, #16
	addvl	x12, x11, #2
	ldr	p0,  [x12, #0, mul vl]	/* FFR value */
	wrffr	p0.b
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	ldr	p\r, [x11, #\r, mul vl]
	.endr
	.irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ldr	z\r, [x10, #\r, mul vl]
	.endr
.Lsve_restore_done:

	ldr	x9, [sp, #PSTEPPER_OFF_NZCV]
	msr	nzcv, x9

	ldp	x0,  x1,  [sp, #PSTEPPER_OFF_X0 + 0]
	ldp	x2,  x3,  [sp, #PSTEPPER_OFF_X0 + 16]
	ldp	x4,  x5,  [sp, #PSTEPPER_OFF_X0 + 32]
	ldp	x6,  x7,  [sp, #PSTEPPER_OFF_X0 + 48]
	ldp	x8,  x9,  [sp, #PSTEPPER_OFF_X0 + 64]
	ldp	x10, x11, [sp, #PSTEPPER_OFF_X0 + 80]
	ldp	x12, x13, [sp, #PSTEPPER_OFF_X0 + 96]
	ldp	x14, x15, [sp, #PSTEPPER_OFF_X0 + 112]
	ldp	x16, x17, [sp, #PSTEPPER_OFF_X0 + 128]
	ldp	x18, x19, [sp, #PSTEPPER_OFF_X0 + 144]
	ldp	x20, x21, [sp, #PSTEPPER_OFF_X0 + 160]
	ldp	x22, x23, [sp, #PSTEPPER_OFF_X0 + 176]
	ldp	x24, x25, [sp, #PSTEPPER_OFF_X0 + 192]
	ldp	x26, x27, [sp, #PSTEPPER_OFF_X0 + 208]
	ldp	x28, x29, [sp, #PSTEPPER_OFF_X0 + 224]
	ldr	x30,      [sp, #PSTEPPER_OFF_X0 + 240]

	add	sp, sp, #(PSTEPPER_FRAME_SIZE >> 12), lsl #12
	add	sp, sp, #(PSTEPPER_FRAME_SIZE & 0xfff)
	.cfi_def_cfa_offset 16		/* CFA still = upcall_top; [CFA-16]/[CFA-8]
					 * is the [PC][SP] pair the plugin consumes */
	.global pstepper_resume_marker
	.type pstepper_resume_marker, %function
pstepper_resume_marker:
	brk	#0x5453
	.cfi_endproc
	.size pstepper_trampoline, . - pstepper_trampoline
	.size pstepper_resume_marker, . - pstepper_resume_marker

/*
 * pstepper_sve_read_vl(): `rdvl` -- the SVE vector length in bytes. Must
 * only be called after pstepper_arch_prepare() has confirmed HWCAP_SVE;
 * `rdvl` is UNDEFINED on a CPU without SVE.
 */
	.global pstepper_sve_read_vl
	.type pstepper_sve_read_vl, %function
pstepper_sve_read_vl:
	.cfi_startproc
	rdvl	x0, #1
	ret
	.cfi_endproc
	.size pstepper_sve_read_vl, . - pstepper_sve_read_vl

/*
 * pstepper_resume_via_frame(frame_base): resume through a ready-made
 * trampoline frame -- point SP at it and fall into the shared epilogue,
 * which restores the full GPR + SVE/NEON + NZCV file and hands the resume
 * marker the [PC][SP] pair staged just above it. Never returns. Used only
 * for a clone-bootstrapped child's first resume (frame built pre-fork by
 * pstepper_do_clone_intercept). Entered with SP already inside the child's
 * upcall stack, so neither instruction here traps.
 */
	.global pstepper_resume_via_frame
	.type pstepper_resume_via_frame, %function
pstepper_resume_via_frame:
	.cfi_startproc
	.cfi_signal_frame
	.cfi_undefined 32
	mov	sp, x0
	b	pstepper_trampoline_epilogue
	.cfi_endproc
	.size pstepper_resume_via_frame, . - pstepper_resume_via_frame

/*
 * pstepper_clone_helper(frame, continue_pc, child_upcall_base,
 * child_frame_base): a real noreturn call from
 * pstepper_arm64.c's pstepper_do_clone_intercept, which has already decided
 * the trapped instruction is a CLONE_THREAD clone(2) about to happen and
 * done all the pre-fork prep -- mmapping the child's own upcall stack,
 * computing continue_pc (= trapped PC + 4), and building the child a
 * complete ready-to-resume trampoline frame at child_frame_base. This
 * issues the real clone and completes the trap for both threads.
 *
 * The syscall has to be raw and inline here (not a C clone() wrapper):
 * when a real child_stack is given, the CHILD resumes with SP already
 * pointing at that stack, and a normal function's trailing `ret` would pop
 * garbage. Branch on the result immediately, same instruction stream.
 */
	.global pstepper_clone_helper
	.type pstepper_clone_helper, %function
pstepper_clone_helper:
	.cfi_startproc
	.cfi_signal_frame
	.cfi_undefined 32
	mov	x19, x0			/* parent frame pointer (== &gpr) */
	mov	x20, x1			/* continue_pc (already +4) */
	mov	x21, x2			/* child_upcall_base */
	mov	x22, x3			/* child_frame_base */

	/* raw clone(2) args from the parent's saved register file */
	ldr	x0, [x19, #PSTEPPER_OFF_X0 + 0]	/* flags      (guest x0) */
	ldr	x1, [x19, #PSTEPPER_OFF_X0 + 8]	/* newsp      (guest x1) */
	ldr	x2, [x19, #PSTEPPER_OFF_X0 + 16]	/* parent_tid (guest x2) */
	ldr	x3, [x19, #PSTEPPER_OFF_X0 + 24]	/* tls        (guest x3) */
	ldr	x4, [x19, #PSTEPPER_OFF_X0 + 32]	/* child_tid  (guest x4) */
	mov	x8, #PSTEPPER_NR_CLONE
	svc	#0
	cbz	x0, pstepper_clone_child

	/*
	 * Parent: x0 is the real return (child tid or -errno). Put it where
	 * the trapped instruction's own x0 would land, and set the floating
	 * PC word to continue_pc (past the 4-byte svc -- otherwise resume
	 * lands on it and runs a second, real clone). The floating word at
	 * [x19 + FRAME_SIZE] is what the epilogue's add-sp/brk sequence reads
	 * back; gpr.pc inside the frame is only the handler's informational
	 * copy.
	 */
	str	x0, [x19, #PSTEPPER_OFF_X0 + 0]
	str	x20, [x19, #PSTEPPER_FRAME_SIZE + 0]
	mov	sp, x19
	b	pstepper_trampoline_epilogue

pstepper_clone_child:
	/*
	 * Child: brand-new thread, SP = newsp (kernel-set). Park SP just below
	 * the pre-built frame so pstepper_child_bootstrap's scratch writes land
	 * neither on the pristine child stack nor on the frame (it only writes
	 * below its entry SP). The plugin traps none of this -- the child vCPU
	 * is unarmed until bootstrap's own PSTEPPER_CMD_ENABLE (resuming=1).
	 */
	.cfi_undefined 32
	sub	x9, x22, #64
	and	x9, x9, #0xfffffffffffffff0
	mov	sp, x9
	mov	x0, x21			/* pstepper_child_bootstrap: upcall_stack_base */
	mov	x1, x22			/*                          child_frame_base  */
	bl	pstepper_child_bootstrap
	brk	#0			/* unreachable */
	.cfi_endproc
	.size pstepper_clone_helper, . - pstepper_clone_helper

/*
 * pstepper_thread_exit(resume_sp, upcall_base, upcall_size, status): the
 * noreturn tail of pstepper_do_exit_intercept. Entered still on this
 * thread's upcall stack with stepping already DISABLEd, so nothing re-traps
 * wherever SP goes. Switch onto the thread's real stack (its value at the
 * exit(2) site), munmap the upcall stack -- impossible while still on it --
 * then issue the real per-thread exit(2). Only raw syscalls and moves past
 * the switch; nothing is pushed.
 */
	.global pstepper_thread_exit
	.type pstepper_thread_exit, %function
pstepper_thread_exit:
	.cfi_startproc
	.cfi_undefined 32
	mov	x19, x3			/* status -- syscall clobbers x0/x8 */
	mov	sp, x0			/* onto the real stack (already 16-aligned) */
	mov	x0, x1			/* munmap(upcall_base, upcall_size) */
	mov	x1, x2
	mov	x8, #PSTEPPER_NR_MUNMAP
	svc	#0
	mov	x0, x19			/* exit(status) */
	mov	x8, #PSTEPPER_NR_EXIT
	svc	#0
	brk	#0			/* unreachable */
	.cfi_endproc
	.size pstepper_thread_exit, . - pstepper_thread_exit

	.section .rodata
.Lsme_msg:
	.asciz "SVE streaming mode / ZA is active at a trap -- unsupported by the trampoline"

/* Non-executable stack. */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif
