// SPDX-License-Identifier: 0BSD
/*
 * pstepper: the "magic syscall" wire protocol between a guest program and
 * the pstepper QEMU plugin -- the syscall number, the command enum, and the
 * UXI kind. Nothing arch-specific and nothing guest-side.
 *
 * Split out of pstepper.h so the plugin can include just this. The plugin
 * is host-native and target-neutral (one build serves whichever
 * qemu-<target> loads it), so it must not pull in pstepper.h's guest-facing,
 * target-gated register-snapshot machinery (pstepper_<arch>.h and the
 * #error for an unported arch) -- it only ever needs the constants here.
 */
#ifndef PSTEPPER_ABI_H_
#define PSTEPPER_ABI_H_

/*
 * ASCII "PSTE" -- comfortably outside every real linux-user syscall table
 * (they top out in the few hundreds), so it can never collide with a real
 * syscall. Must fit in a 32-bit int: linux-user's do_syscall() takes `num`
 * as a plain `int`, so anything wider gets silently truncated before a
 * plugin ever sees it.
 */
#define PSTEPPER_SYSCALL_NR 0x50535445ULL

enum pstepper_cmd {
	/*
	 * arg1 = trampoline entry PC (pstepper_trampoline)
	 * arg2 = upcall stack top (guest vaddr): the high end of the upcall
	 *        stack. x86_64: 64-byte aligned (the trampoline's xsave area
	 *        alignment derives from this -- see pstepper_frame_amd64.h);
	 *        aarch64: 16-byte aligned (AAPCS64). The trampoline is entered
	 *        with SP = top - 16 and the trapped [PC][SP] pair staged there.
	 *        Only the top matters to the plugin -- it never needs the base
	 *        or the size (the guest side keeps those, for munmap).
	 * arg3 = resuming: 0 for a normal arm from ordinary code; nonzero if
	 *        the calling thread is already running on the upcall stack and
	 *        is about to resume through a pre-built trampoline frame (only
	 *        the clone hook's bootstrap does this). When nonzero the plugin
	 *        starts the vCPU in its "trap parked" state, so the bootstrap
	 *        tail and first resume are not treated as steppable.
	 * arg4 = resume marker PC (pstepper_resume_marker: a `ud2` on x86_64, a
	 *        `brk`/`udf` on aarch64, inside the trampoline) -- the plugin
	 *        recognizes execution reaching this exact address as "pop
	 *        [SP]/[SP+8] into PC/SP and resume", rather than as a real
	 *        illegal-instruction fault.
	 * arg5 = pstepper_user_handler's own entry PC. Used only for the
	 *        acceleration hook (PSTEPPER_CMD_ENABLE_ACCELERATION): the
	 *        plugin recognizes execution reaching this exact address as "the
	 *        handler is about to run" and, if acceleration is enabled,
	 *        invokes it as a native host call instead of single-stepping
	 *        through it.
	 *
	 * Arms single-stepping for the calling thread. The upcall stack must be
	 * sized generously -- as with a POSIX sigaltstack, the entire handler
	 * call chain (trampoline, the user handler, anything it calls) runs on
	 * it.
	 */
	PSTEPPER_CMD_ENABLE = 1,

	/* Disarms single-stepping for the calling thread. */
	PSTEPPER_CMD_DISABLE = 3,

	/*
	 * arg1 = 0 or 1: disable/enable the acceleration hook (see
	 *        PSTEPPER_CMD_ENABLE's arg5). Process-wide, not per-thread.
	 * arg2 = asciiz contents of guest's /proc/self/maps
	 *
	 */
	PSTEPPER_CMD_ENABLE_ACCELERATION = 4,
};

/* The (currently only) kind of UXI, handed to the handler in its first
 * argument -- shaped like a real sa_sigaction(int sig, siginfo_t *info,
 * void *ucontext) handler for familiarity, but this is a UXI kind, not a
 * signal number. The second argument (siginfo_t*) is always NULL -- nothing
 * populates it in this first cut. The third argument is a "fake"-ish
 * ucontext_t (see pstepper_handler_fn in pstepper.h). */
#define PSTEPPER_UXI_STEP 1

#endif /* PSTEPPER_ABI_H_ */
