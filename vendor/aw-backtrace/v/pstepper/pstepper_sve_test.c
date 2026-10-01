// SPDX-License-Identifier: 0BSD
/*
 * Guest test for SVE state round-tripping through the pstepper trampoline
 * (arm64 only). Analogous to pstepper_xsave_test.c on x86.
 *
 * The trampoline saves NEON always and -- when the CPU has SVE -- Z0..Z31 +
 * P0..P15 + FFR (see pstepper_trampoline_arm64.S / pstepper_arch_prepare).
 * This test proves it: it loads all 32 Z registers and all 16 P registers
 * with sentinels, single-steps a loop that keeps them live (and steps SVE
 * instructions), has the step handler *deliberately trash every Z and P
 * register on every single step*, then reads the sentinels back and checks
 * they are byte-for-byte intact.
 *
 * Run once on the main thread and once on a spawned pthread, so the
 * clone-bootstrapped-thread resume path (which restores the full frame,
 * SVE area included, via pstepper_resume_via_frame) is covered too.
 * (Faithful copy of the *parent's live* SVE state at the clone instruction
 * is not separately asserted -- the child overwrites its inherited vector
 * state with its own sve_load before any observation point.)
 *
 * Built with -march=armv8-a+sve (per-file, see genbuild.rb). If the runtime
 * CPU has no SVE the test prints "skip" and passes.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

#include "pstepper.h"

#ifndef HWCAP_SVE
#define HWCAP_SVE (1UL << 22)
#endif

#define VL_MAX 256                 /* architectural max, bytes */
#define ZBYTES (32 * VL_MAX)       /* Z0..Z31 */
#define PBYTES (16 * (VL_MAX / 8)) /* P0..P15 */

static unsigned long g_vl;
static volatile long g_steps;

static unsigned char g_zin[ZBYTES], g_pin[PBYTES];

/* rdvl in its own asm so it is reached only after the HWCAP_SVE check. */
static unsigned long read_vl(void)
{
	unsigned long v;
	__asm__(".arch_extension sve\n\trdvl %0, #1" : "=r"(v));
	return v;
}

/* Load Z0..Z31 from z[], P0..P15 from p[], packed at the runtime VL. */
static void sve_load(const void *z, const void *p)
{
	__asm__ volatile(".arch_extension sve\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n\t"
			 "ldr z\\i, [%[z], #\\i, mul vl]\n\t"
			 ".endr\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
			 "ldr p\\i, [%[p], #\\i, mul vl]\n\t"
			 ".endr\n\t"
			 :
			 : [z] "r"(z), [p] "r"(p)
			 : "memory");
}

/* Store Z0..Z31 to z[], P0..P15 to p[]. */
static void sve_store(void *z, void *p)
{
	__asm__ volatile(".arch_extension sve\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n\t"
			 "str z\\i, [%[z], #\\i, mul vl]\n\t"
			 ".endr\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
			 "str p\\i, [%[p], #\\i, mul vl]\n\t"
			 ".endr\n\t"
			 :
			 : [z] "r"(z), [p] "r"(p)
			 : "memory");
}

/* Trash every Z and P register (Z <- 0xAA bytes, P <- all-true), so a
 * missing save/restore corrupts the guest's sentinels. \i\() -- the \()
 * ends the .irp symbol so "z\i.b" parses as "z<n>.b". */
static void sve_scramble(void)
{
	__asm__ volatile(".arch_extension sve\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n\t"
			 "dup z\\i\\().b, #-86\n\t" /* 0xAA */
			 ".endr\n\t"
			 ".irp i,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
			 "ptrue p\\i\\().b\n\t"
			 ".endr\n\t"
			 :
			 :
			 : "memory");
}

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
	(void)uxi_kind;
	(void)info;
	(void)uc;
	sve_scramble();
	g_steps++;
}

/* A stepped loop that keeps the vector file live -- identity ops on a
 * spread of Z registers plus a scalar countdown. */
static void sve_keepalive(long iters)
{
	__asm__ volatile(".arch_extension sve\n\t"
			 "1:\n\t"
			 "orr z0.d, z0.d, z0.d\n\t"
			 "orr z11.d, z11.d, z11.d\n\t"
			 "orr z23.d, z23.d, z23.d\n\t"
			 "orr z31.d, z31.d, z31.d\n\t"
			 "subs %[n], %[n], #1\n\t"
			 "b.ne 1b\n\t"
			 : [n] "+r"(iters)
			 :
			 : "cc", "memory");
}

/* Load sentinels, keep them live through a single-stepped loop while the
 * handler scrambles the vector file on every step, read them back, compare.
 * Returns 0 on success. */
static int run_check(const char *who)
{
	unsigned char zout[ZBYTES], pout[PBYTES];
	memset(zout, 0, sizeof(zout));
	memset(pout, 0, sizeof(pout));

	sve_load(g_zin, g_pin);
	sve_keepalive(3000);
	sve_store(zout, pout);

	size_t zn = 32 * g_vl, pn = 16 * (g_vl / 8);
	int zbad = memcmp(g_zin, zout, zn) != 0;
	int pbad = memcmp(g_pin, pout, pn) != 0;

	fprintf(stderr, "[guest] %-6s Z regs: %s  P regs: %s\n", who, zbad ? "CORRUPT" : "intact",
		pbad ? "CORRUPT" : "intact");

	if (zbad) {
		for (size_t r = 0; r < 32; r++) {
			if (memcmp(g_zin + r * g_vl, zout + r * g_vl, g_vl) != 0) {
				fprintf(stderr, "[guest]   %s: first corrupt register Z%zu\n", who, r);
				break;
			}
		}
	}
	return zbad || pbad;
}

static void *worker(void *arg)
{
	*(int *)arg = run_check("worker");
	return NULL;
}

int main(void)
{
	if (!(getauxval(AT_HWCAP) & HWCAP_SVE)) {
		fprintf(stderr, "[guest] no SVE on this CPU -- skip\n");
		return 0;
	}
	g_vl = read_vl();
	fprintf(stderr, "[guest] SVE VL = %lu bytes (%lu bits)\n", g_vl, g_vl * 8);
	if (g_vl == 0 || g_vl > VL_MAX || g_vl % 16 != 0) {
		fprintf(stderr, "[guest] FAIL: implausible VL\n");
		return 1;
	}

	/* Distinct, non-trivial pattern per byte so a misplaced register or a
	 * partial save shows up as a mismatch. */
	for (size_t i = 0; i < sizeof(g_zin); i++) {
		g_zin[i] = (unsigned char)(0x40 + (i * 7 + (i >> 5) * 3));
	}
	for (size_t i = 0; i < sizeof(g_pin); i++) {
		g_pin[i] = (unsigned char)(i * 5 + 1);
	}

	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, 1 << 16, 0);

	int main_bad = run_check("main");

	int worker_bad = 1;
	pthread_t th;
	pthread_create(&th, NULL, worker, &worker_bad);
	pthread_join(th, NULL);

	pstepper_disable();

	fprintf(stderr, "[guest] steps=%ld\n", g_steps);
	if (g_steps < 200) {
		fprintf(stderr, "[guest] FAIL: only %ld steps -- loops were not single-stepped\n", g_steps);
		return 1;
	}
	if (main_bad || worker_bad) {
		fprintf(stderr, "[guest] FAIL\n");
		return 1;
	}

	fprintf(stderr, "[guest] PASS\n");
	return 0;
}
