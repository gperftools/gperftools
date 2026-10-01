// SPDX-License-Identifier: 0BSD
/*
 * pstepper: portable guest-side plumbing -- upcall-stack management,
 * arming/disarming, enable/disable, and the clone-child bootstrap. Written
 * purely against the imagined pstepper kernel facility declared in
 * pstepper.h; no knowledge of QEMU, and no x86 detail (that lives in
 * pstepper_amd64.c / pstepper_trampoline_amd64.S -- this file reaches it only
 * through the portable-named symbols pstepper_trampoline,
 * pstepper_resume_marker, pstepper_resume_via_frame, pstepper_invoke_handler
 * and the pstepper_arch_prepare() hook).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "pstepper.h"
#include "pstepper_internal.h"

/* Set by pstepper_enable(); consulted by pstepper_invoke_handler()
 * (pstepper_amd64.c). */
pstepper_handler_fn pstepper_user_handler;

__attribute__((noreturn)) void pstepper_die(const char *msg)
{
	fprintf(stderr, "pstepper: %s\n", msg);
	abort();
}

int pstepper_syscall(uint64_t cmd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	return syscall(PSTEPPER_SYSCALL_NR, cmd, arg1, arg2, arg3, arg4, arg5);
}

/*
 * The size every thread's upcall stack gets mmapped at -- set once by
 * whichever thread first calls pstepper_enable() (normally the main
 * thread), and reused automatically for every thread the clone hook
 * bootstraps later, so a newly created thread needs no guidance of its own.
 */
uint64_t pstepper_upcall_stack_size;

/* This thread's own upcall stack, remembered so pstepper_disable() can free
 * it. Per-thread: each thread mmaps and owns its own.
 *
 * initial-exec, not the default global-dynamic: pstepper_child_bootstrap()
 * writes this on a clone-bootstrapped thread *before* glibc's start_thread
 * runs, and a global-dynamic access there compiles to __tls_get_addr, which
 * can call malloc on a half-initialized thread -- on the upcall stack.
 * initial-exec resolves to a fixed offset from the thread pointer with no
 * call. Fine today (this is linked into a -no-pie executable, so local-exec
 * anyway); load-bearing once this melds into aw-backtrace as a preloaded
 * .so, which is why the existing stepper annotates every TLS var this way.
 * pstepper_internal.h's extern declaration must carry the same tls_model. */
__thread void *pstepper_own_upcall_stack __attribute__((tls_model("initial-exec")));

/* Defined in pstepper_trampoline_amd64.S. */
extern void pstepper_resume_via_frame(uint64_t frame_base) __attribute__((noreturn));

/* The high end of an upcall stack that starts at `base`, rounded down to a
 * 64-byte boundary. pstepper_trampoline is entered with RSP = top - 16, and
 * its xsave area alignment is derived statically from `top` being 64-aligned
 * (see pstepper_frame_amd64.h). This is the only stack geometry the plugin is
 * told (PSTEPPER_CMD_ENABLE arg2); the base and raw size stay on this side,
 * for munmap. */
uint64_t pstepper_upcall_top(uint64_t base)
{
	return (base + pstepper_upcall_stack_size) & ~(uint64_t)63;
}

void *pstepper_alloc_upcall_stack(uint64_t size)
{
	void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (mem == MAP_FAILED) {
		perror("pstepper: mmap(upcall stack)");
		abort();
	}
	return mem;
}

/* Arms stepping for the calling thread over an already-allocated upcall
 * stack, given by its 16-byte-aligned top. `resuming` is 0 for a normal arm
 * from ordinary code (pstepper_enable(), the first thread) and 1 for a
 * clone-bootstrapped thread arming while already on its upcall stack and
 * about to resume through a pre-built frame (pstepper_child_bootstrap()) --
 * see PSTEPPER_CMD_ENABLE in pstepper.h. */
static int pstepper_arm_stepping(uint64_t upcall_top, int resuming)
{
	return pstepper_syscall(PSTEPPER_CMD_ENABLE, (uint64_t)(uintptr_t)pstepper_trampoline, upcall_top,
				(uint64_t)resuming, (uint64_t)(uintptr_t)pstepper_resume_marker,
				(uint64_t)(uintptr_t)pstepper_user_handler);
}

/*
 * Called only from pstepper_clone_helper's child branch, on the brand-new
 * thread, with RSP already parked on this thread's own upcall stack (just
 * below child_frame_base -- see that code for why it must not run on the
 * real thread stack). upcall_stack_base was mmapped by the parent pre-fork;
 * child_frame_base is the ready-to-resume trampoline frame the parent built
 * there. Arm stepping for this thread (resuming = 1: we are already on the
 * upcall stack), then resume through that frame -- the shared epilogue
 * restores the full register file (rax = 0, rsp = child_stack, everything
 * else = the parent's) and jumps the resume marker to continue_rip.
 */
__attribute__((noreturn)) void pstepper_child_bootstrap(uint64_t upcall_stack_base, uint64_t child_frame_base)
{
	pstepper_own_upcall_stack = (void *)(uintptr_t)upcall_stack_base;
	if (pstepper_arm_stepping(pstepper_upcall_top(upcall_stack_base), 1) < 0) {
		pstepper_die("failed to arm the stepping in the child");
	}
	pstepper_resume_via_frame(child_frame_base); /* noreturn */
}

static char *pstepper_read_proc_maps(void)
{
	/*
	 * Report every range this process itself considers executable, from
	 * its own /proc/self/maps -- QEMU synthesizes this per-guest from its
	 * own guest-page bookkeeping, so it lists only this process's own
	 * mappings (never QEMU's own unrelated host libraries) with accurate
	 * r-xp flags. The plugin has no other way to find pstepper_user_handler's
	 * transitive call graph (whatever library code it ends up calling into)
	 * to make host-executable.
	 *
	 * Guest vaddrs are handed to the plugin as-is; it treats them as host
	 * pointers directly (no guest_base translation), which is exact only
	 * when QEMU picked an identity mapping.
	 */
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) {
		perror("fopen");
		return NULL;
	}
	size_t sz = 16 << 10;
	char *mem = malloc(sz);
	size_t eaten = 0;
	for (;;) {
		int readen = fread(mem + eaten, 1, sz - 1 - eaten, f);
		if (readen <= 0) {
			/* silently assume eof */
			break;
		}
		eaten += (size_t)readen;
		if (eaten >= sz - 1) {
			sz *= 2;
			mem = realloc(mem, sz);
		}
	}
	mem[eaten] = 0;
	fprintf(stderr, "[pstepper plumbing]: feeding proc maps:\n%.*s\n\n", (int)eaten, mem);
	return mem;
}

int pstepper_enable(pstepper_handler_fn handler, uint64_t upcall_stack_size, int enable_acceleration)
{
	pstepper_user_handler = handler;
	pstepper_upcall_stack_size = upcall_stack_size;

	/* Arch-specific setup the trampoline depends on (on amd64: the
	 * xsave/xrstor mask, read by the asm on the very first trap), done
	 * before stepping is armed. */
	pstepper_arch_prepare();

	char* maps = enable_acceleration ? pstepper_read_proc_maps() : NULL;

	/* Mmap this (the first / calling) thread's own upcall stack and arm
	 * stepping for it. Every thread the clone hook bootstraps afterward
	 * instead gets its stack pre-mmapped by the parent (see
	 * pstepper_do_clone_intercept), so this is the first thread's path
	 * only. */
	pstepper_own_upcall_stack = pstepper_alloc_upcall_stack(pstepper_upcall_stack_size);
	uint64_t base = (uint64_t)(uintptr_t)pstepper_own_upcall_stack;

	int ret = 0;
	if (pstepper_arm_stepping(pstepper_upcall_top(base), 0) < 0) {
		ret = errno;
		// silently assuming ENOSYS
		munmap(pstepper_own_upcall_stack, pstepper_upcall_stack_size);
		pstepper_own_upcall_stack = NULL;
	}
	if (enable_acceleration && ret == 0) {
		pstepper_syscall(PSTEPPER_CMD_ENABLE_ACCELERATION, 1, (uintptr_t)maps, 0, 0, 0);
	}
	free(maps);
	return ret;
}

void pstepper_enable_or_die(pstepper_handler_fn handler, uint64_t upcall_stack_size, int enable_acceleration)
{
	int ret = pstepper_enable(handler, upcall_stack_size, enable_acceleration);
	if (ret != 0) {
		errno = ret;
		perror("pstepper_enable");
		pstepper_die("pstepper-enable failed.");
	}
}

void pstepper_disable(void)
{
	if (pstepper_syscall(PSTEPPER_CMD_DISABLE, 0, 0, 0, 0, 0) < 0) {
		pstepper_die("PSTEPPER_CMD_DISABLE failed");
	}
	if (pstepper_own_upcall_stack) {
		munmap(pstepper_own_upcall_stack, pstepper_upcall_stack_size);
		pstepper_own_upcall_stack = NULL;
	}
}
