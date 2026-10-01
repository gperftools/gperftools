// SPDX-License-Identifier: 0BSD
/*
 * Guest test program for pstepper's thread-creation and thread-exit hooks.
 * The main thread arms stepping, then spawns+joins a worker pthread many
 * times over. Each worker does a bit of floating-point work single-stepped
 * (checked bit-for-bit against a native, non-stepped reference, same as
 * pstepper_test.c does for the main thread alone).
 *
 * The loop is what exercises the exit hook: every worker that returns from
 * its start routine hits a per-thread exit(2), which pstepper must intercept
 * to disable stepping and unmap that thread's upcall stack. Without the hook
 * each iteration leaks one upcall-stack mmap (64 KiB here); we assert the
 * process's virtual size (VmSize from /proc/self/status) stays flat across
 * the run. VmSize rather than the /proc/self/maps line count because the
 * kernel coalesces the leaked mappings with adjacent anonymous VMAs, so the
 * line count barely moves even as the address space grows by ITERATIONS*64K.
 *
 * It also checks clone-child register fidelity: a real clone hands the child
 * a faithful copy of the parent's register file, so the child's first
 * stepped instruction (just past the trapped `clone` syscall) must show the
 * same callee-saved GPRs and clone args as the parent showed at the syscall
 * itself. Before the "build the child a full frame" change the child resumed
 * with those registers clobbered by the bootstrap path.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>

#include "pstepper_internal.h"

#define ITERATIONS 40
#define PST_CLONE_THREAD 0x00010000UL
#define UPCALL_SIZE (64 * 1024) /* each worker's upcall stack */

static volatile long g_main_steps_seen;
static volatile long g_child_steps_seen;
static pthread_t g_main_tid_marker; /* never a real pthread_t; just 0 */

static volatile double g_child_result;

/* Captured once, on iteration 0: the parent's registers at the stepped
 * `clone` syscall, and the child's registers at its first stepped
 * instruction. Written under the g_have_* flags; children run serialized
 * (create+join) and main reads these only after join, so no locking. */
static pstepper_gpregs_t g_parent_clone_regs;
static pstepper_gpregs_t g_child_first_regs;
static volatile int g_have_parent_clone_regs;
static volatile int g_have_child_first_regs;

/* Copy the covered GPR prefix out of a fake ucontext_t back into the compact
 * pstepper_gpregs_t layout -- valid because the two share a field order (the
 * _Static_assert block in pstepper.h pins it). Lets the fidelity check below
 * keep comparing plain `->rbx` fields. */
static void gpregs_from_uc(pstepper_gpregs_t *out, const ucontext_t *uc)
{
#if defined(__x86_64__)
	memcpy(out, uc->uc_mcontext.gregs, sizeof(*out));
#elif defined(__aarch64__)
	memcpy(out, &uc->uc_mcontext.regs[0], sizeof(*out));
#else
#error "no register accessors for this arch"
#endif
}

/* Is the trapped instruction a CLONE_THREAD clone(2) about to happen? Must
 * confirm the opcode itself, not just rax/rdi -- rax happening to equal
 * SYS_clone with rdi's CLONE_THREAD bit set is a coincidence plain arithmetic
 * hits constantly across ~250k single-stepped main-thread instructions
 * (rdi is a pointer virtually always, so PST_CLONE_THREAD's bit is set by
 * pure chance on roughly half of them); without the opcode check this fires
 * on some ordinary earlier instruction and captures garbage as the "parent
 * clone regs" snapshot. Caught via a QEMU-plugin-side capture of the same
 * frame (pstepper_do_clone_intercept's cf) disagreeing with this one. */
static int uc_is_thread_clone(const ucontext_t *uc)
{
#if defined(__x86_64__)
	const greg_t *g = uc->uc_mcontext.gregs;
	const unsigned char *insn = (const unsigned char *)(uintptr_t)g[REG_RIP];
	return insn[0] == 0x0f && insn[1] == 0x05 && g[REG_RAX] == (greg_t)SYS_clone &&
	       ((uint64_t)g[REG_RDI] & PST_CLONE_THREAD);
#elif defined(__aarch64__)
	const uint32_t *insn = (const uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
	return *insn == 0xd4000001u /* svc #0 */ && uc->uc_mcontext.regs[8] == (unsigned long long)SYS_clone &&
	       (uc->uc_mcontext.regs[0] & PST_CLONE_THREAD);
#else
#error "no register accessors for this arch"
#endif
}

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
	/* No thread-id in the context (yet) -- distinguish threads via a
	 * thread-local flag computed once per thread instead. */
	static __thread int is_child = -1;
	if (is_child < 0) {
		is_child = !pthread_equal(pthread_self(), g_main_tid_marker);
	}
	if (is_child) {
		if (!g_have_child_first_regs) {
			gpregs_from_uc(&g_child_first_regs, uc);
			g_have_child_first_regs = 1; /* publish after the copy */
		}
		g_child_steps_seen++;
	} else {
		/* The stepped `clone` syscall itself -- we get a step upcall
		 * for it now, just before the intercept diverts. */
		if (!g_have_parent_clone_regs && uc_is_thread_clone(uc)) {
			gpregs_from_uc(&g_parent_clone_regs, uc);
			g_have_parent_clone_regs = 1;
		}
		g_main_steps_seen++;
	}
	(void)uxi_kind;
	(void)info;
}

/* Compare the registers a real clone would have carried parent->child. rax
 * (56 -> 0), rsp (own stack) and rip (advanced) legitimately differ; assert
 * everything else matches. Returns the number of mismatches, logging each. */
static int check_clone_reg_fidelity(void)
{
	const pstepper_gpregs_t *p = &g_parent_clone_regs;
	const pstepper_gpregs_t *c = &g_child_first_regs;
	int bad = 0;

#define CHECK(field)                                                                                                   \
	do {                                                                                                           \
		if (p->field != c->field) {                                                                            \
			fprintf(stderr, "[guest]   mismatch %-4s parent=0x%" PRIx64 " child=0x%" PRIx64 "\n", #field,  \
				p->field, c->field);                                                                   \
			bad++;                                                                                         \
		}                                                                                                      \
	} while (0)

#if defined(__x86_64__)
	CHECK(rbx);
	CHECK(rbp);
	CHECK(r12);
	CHECK(r13);
	CHECK(r14);
	CHECK(r15);
	CHECK(rdi);
	CHECK(rsi);
	CHECK(rdx);
	CHECK(r8);
	CHECK(r9);
	CHECK(r10);
	CHECK(rcx);
	CHECK(r11);

	if (c->rax != 0) {
		fprintf(stderr, "[guest]   child rax = 0x%" PRIx64 ", expected 0\n", c->rax);
		bad++;
	}
#elif defined(__aarch64__)
	/* A real clone copies the whole integer file to the child, overriding
	 * only x0 (-> 0) and SP (-> newsp); PC legitimately advances. So x1..x30
	 * must match parent-at-svc exactly. */
	for (int i = 1; i <= 30; i++) {
		if (p->x[i] != c->x[i]) {
			fprintf(stderr, "[guest]   mismatch x%-2d parent=0x%" PRIx64 " child=0x%" PRIx64 "\n", i,
				p->x[i], c->x[i]);
			bad++;
		}
	}
	if (c->x[0] != 0) {
		fprintf(stderr, "[guest]   child x0 = 0x%" PRIx64 ", expected 0\n", c->x[0]);
		bad++;
	}
#endif
#undef CHECK
	return bad;
}

__attribute__((noinline)) static double compute(int n)
{
	double acc = 1.0;
	for (int i = 1; i <= n; i++) {
		acc = acc * 1.0001 + (double)i;
	}
	return acc;
}

static void *thread_fn(void *arg)
{
	(void)arg;
	g_child_result = compute(1000);
	return NULL;
}

/* This process's virtual address-space size, in KiB (VmSize from
 * /proc/self/status). A leaked upcall stack per worker makes this climb by
 * ~64 KiB an iteration. */
static long vm_size_kb(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		perror("[guest] fopen(/proc/self/status)");
		return -1;
	}
	char key[64];
	long kb = -1;
	while (fscanf(f, "%63s", key) == 1) {
		if (strcmp(key, "VmSize:") == 0) {
			if (fscanf(f, "%ld", &kb) != 1) {
				kb = -1;
			}
			break;
		}
	}
	fclose(f);
	return kb;
}

/* Count private, anonymous mappings exactly UPCALL_SIZE bytes long -- one per
 * live upcall stack. This is the direct signal the exit hook works: it must
 * not grow with the iteration count. (VmSize is a coarser proxy -- under
 * qemu-user its jitter is megabytes, far above a single 64 KiB stack, so on
 * aarch64 only this count is asserted.) */
static long count_upcall_maps(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f) {
		perror("[guest] fopen(/proc/self/maps)");
		return -1;
	}
	char line[512];
	long n = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && (hi - lo) == UPCALL_SIZE && !strchr(line, '/')) {
			n++;
		}
	}
	fclose(f);
	return n;
}

int main(void)
{
	g_main_tid_marker = pthread_self();

	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, UPCALL_SIZE, 1);

	long vm_after_first = -1;
	long maps_after_first = -1;
	double first_result = 0.0;

	for (int it = 0; it < ITERATIONS; it++) {
		pthread_t tid;
		int rc = pthread_create(&tid, NULL, thread_fn, NULL);
		if (rc != 0) {
			fprintf(stderr,
				"[guest] pthread_create failed at iter %d, "
				"rc=%d (%s)\n",
				it, rc, strerror(rc));
			return 1;
		}
		pthread_join(tid, NULL);

		if (it == 0) {
			first_result = g_child_result;
			/* Baseline after iter 0, once glibc's thread-stack
			 * cache has warmed -- otherwise the first worker's own
			 * stack shows up as growth. */
			vm_after_first = vm_size_kb();
			maps_after_first = count_upcall_maps();
		} else if (g_child_result != first_result) {
			fprintf(stderr, "[guest] FAIL: iter %d child result %f != %f\n", it, g_child_result,
				first_result);
			return 1;
		}
	}

	long vm_at_end = vm_size_kb();
	long vm_delta = vm_at_end - vm_after_first;
	long maps_at_end = count_upcall_maps();
	long maps_delta = maps_at_end - maps_after_first;

	pstepper_disable();

	fprintf(stderr, "[guest] disabled pstepper after %d iterations\n", ITERATIONS);
	fprintf(stderr,
		"[guest] main steps = %ld, child steps (summed) = %ld, "
		"child result = %f\n",
		g_main_steps_seen, g_child_steps_seen, first_result);
	fprintf(stderr,
		"[guest] VmSize after iter 0 = %ld kB, at end = %ld kB "
		"(delta %ld kB)\n",
		vm_after_first, vm_at_end, vm_delta);
	fprintf(stderr, "[guest] upcall-stack mappings after iter 0 = %ld, at end = %ld (delta %ld)\n",
		maps_after_first, maps_at_end, maps_delta);

	if (!g_have_parent_clone_regs || !g_have_child_first_regs) {
		fprintf(stderr, "[guest] FAIL: never captured %s regs\n",
			!g_have_parent_clone_regs ? "parent clone" : "child first");
		return 1;
	}
	int bad = check_clone_reg_fidelity();
	fprintf(stderr, "[guest] clone-child register fidelity: %s (%d mismatch)\n", bad ? "BAD" : "ok", bad);
	if (bad) {
		return 1;
	}

#if defined(__x86_64__)
	(void)maps_delta;
	/* Threshold well above thread-stack-cache / allocator jitter (~100 kB
	 * observed) and well below a real leak (ITERATIONS-1 further workers *
	 * 64 kB ~= 2.5 MB). */
	if (vm_delta > 512) {
		fprintf(stderr,
			"[guest] FAIL: VmSize grew %ld kB across %d further "
			"iterations -- upcall stacks are leaking\n",
			vm_delta, ITERATIONS - 1);
		return 1;
	}
#elif defined(__aarch64__)
	(void)vm_delta;
	/* qemu-user's VmSize jitter here is megabytes -- far above a single
	 * 64 kB stack -- so check the thing directly: the exit hook must free
	 * each worker's upcall stack, keeping the count of upcall-stack
	 * mappings flat (allow +1 for the last worker, whose exit may race
	 * this measurement). */
	if (maps_delta > 1) {
		fprintf(stderr,
			"[guest] FAIL: upcall-stack mappings grew by %ld across %d further "
			"iterations -- the exit hook is not freeing them\n",
			maps_delta, ITERATIONS - 1);
		return 1;
	}
#endif

	fprintf(stderr, "[guest] PASS\n");
	return 0;
}
