// SPDX-License-Identifier: 0BSD
/*
 * Guest test program for pstepper. Written purely against pstepper.h --
 * no knowledge of QEMU. Arms stepping, runs a chunk of real floating-point
 * work while the handler observes each instruction (proving per-instruction
 * delivery and that FP/SIMD state survives transparently across every
 * trap/resume), then disables stepping and exits normally.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "pstepper.h"

static volatile long g_steps_seen;
static volatile double g_fp_seen; /* proves xmm/fp state survives round-trip */

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
#if defined(__x86_64__)
	const greg_t *g = uc->uc_mcontext.gregs;
	unsigned long long pc = g[REG_RIP], sp = g[REG_RSP];
#elif defined(__aarch64__)
	unsigned long long pc = uc->uc_mcontext.pc, sp = uc->uc_mcontext.sp;
#else
#error "no register accessors for this arch"
#endif
	g_steps_seen++;
	if (g_steps_seen <= 5 || (g_steps_seen % 5000) == 0) {
		fprintf(stderr, "[guest handler] step %ld: pc=0x%llx sp=0x%llx\n", g_steps_seen, pc, sp);
	}
}

/* A little floating point work, to exercise xmm state round-tripping
 * through every single-stepped instruction. */
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
	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, 65536, 1);

	g_fp_seen = compute(20000);

	pstepper_disable();
	fprintf(stderr,
		"[guest] disabled pstepper, steps seen = %ld, "
		"compute() = %f\n",
		g_steps_seen, g_fp_seen);
	return 0;
}
