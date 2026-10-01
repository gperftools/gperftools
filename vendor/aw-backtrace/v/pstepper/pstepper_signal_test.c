// SPDX-License-Identifier: 0BSD
/*
 * Guest test for pstepper's behaviour when POSIX signals arrive mid-step.
 *
 * A real signal (SIGALRM, handler registered SA_ONSTACK with an active
 * sigaltstack) delivered while a step trap is in flight used to be able to:
 *   (a) let the signal handler's own instructions -- running off the upcall
 *       stack -- take genuine traps that overwrote the plugin's single fixed
 *       trap slot, so the eventual resume landed at the wrong RIP/RSP; and
 *   (b) have the handler's first instruction consume the one-shot resume
 *       suppression meant for the trapped instruction, delivering that
 *       instruction to the step handler twice.
 * The plugin now parks an `in_trap` flag across the whole upcall and leaves
 * any signal handler QEMU interposes there completely unstepped.
 *
 * This drives a single-stepped, call-heavy FP computation with an itimer
 * firing continuously and checks:
 *   - the FP result is bit-for-bit identical to a native run (a wrong-RIP /
 *     wrong-register resume corrupts it);
 *   - the run terminates (a wrong resume tends to wedge or fault);
 *   - no instruction is ever delivered to the step handler twice in a row
 *     (the old double-delivery);
 *   - signals actually landed while an upcall was in flight (g_sig_in_upcall
 *     > 0), so the fixed path was genuinely exercised -- the itimer covers
 *     the asynchronous case, and the upcall handler also raises a bounded few
 *     itself so this does not depend on winning a race;
 *   - any step upcalls seen for code on the altstack (a signal that landed
 *     at the one boundary per step with no trap parked -- harmless, handler
 *     just runs stepped) never break the above.
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "pstepper.h"

#define ALT_SIZE (64 * 1024)
static char g_altstack[ALT_SIZE] __attribute__((aligned(16)));

static volatile long g_steps;
static volatile long g_steps_on_altstack;
static volatile long g_double_deliveries;
static volatile sig_atomic_t g_in_upcall;
static volatile long g_sig_total;
static volatile long g_sig_in_upcall;

static int on_altstack(uint64_t sp)
{
	uint64_t lo = (uint64_t)(uintptr_t)g_altstack;
	return sp >= lo && sp < lo + ALT_SIZE;
}

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
	(void)uxi_kind;
	(void)info;
#if defined(__x86_64__)
	uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
	uint64_t rsp = uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
	uint64_t rip = uc->uc_mcontext.pc;
	uint64_t rsp = uc->uc_mcontext.sp;
#else
#error "no register accessors for this arch"
#endif
	g_in_upcall = 1;
	g_steps++;

	static uint64_t last_rip, last_rsp;
	static int have_last;
	if (have_last && rip == last_rip && rsp == last_rsp && !on_altstack(rsp)) {
		g_double_deliveries++;
	}
	last_rip = rip;
	last_rsp = rsp;
	have_last = 1;

	if (on_altstack(rsp)) {
		g_steps_on_altstack++;
	}

	/*
	 * Force a handful of deliveries into the parked window directly. The
	 * itimer below covers the genuinely asynchronous case, but whether any
	 * of its ticks lands *inside* an upcall is a race whose odds are
	 * (upcall time / total time) -- and that ratio is not a constant of the
	 * design: it collapsed once the plugin stopped delivering callbacks
	 * while a trap is parked, which left the coverage assertion at the
	 * bottom of this file failing on a run that was otherwise perfect. A
	 * signal raised from here is pending immediately and QEMU delivers it
	 * at the next instruction boundary, which is still inside this upcall,
	 * so the interposed-handler path is exercised on purpose rather than by
	 * luck. Bounded, so the async ticks still dominate the run.
	 */
	static int forced;
	if ((g_steps & 0x3ff) == 0 && forced < 8) {
		forced++;
		raise(SIGALRM);
	}

	g_in_upcall = 0;
}

static volatile double g_sig_work = 1.5;

__attribute__((noinline)) static double sig_leaf(double x)
{
	return x * 3.0 + 1.0;
}

static void on_alarm(int sig)
{
	(void)sig;
	int saved = errno;
	g_sig_total++;
	if (g_in_upcall) {
		g_sig_in_upcall++;
	}
	double d = g_sig_work;
	for (int i = 0; i < 15; i++) {
		d = sig_leaf(d);
	}
	g_sig_work = d;
	errno = saved;
}

/* Call-heavy so the step stream has a dense call/return structure for the
 * double-delivery check to bite on, and so a wrong-RIP resume is likely to
 * land somewhere that faults. */
__attribute__((noinline)) static double leaf(double x, int i)
{
	return x * 1.0001 + (double)i;
}

__attribute__((noinline)) static double compute(int n)
{
	double acc = 1.0;
	for (int i = 1; i <= n; i++) {
		acc = leaf(acc, i);
	}
	return acc;
}

static double reference(int n)
{
	double acc = 1.0;
	for (int i = 1; i <= n; i++) {
		acc = acc * 1.0001 + (double)i;
	}
	return acc;
}

int main(void)
{
	stack_t ss = {.ss_sp = g_altstack, .ss_size = ALT_SIZE, .ss_flags = 0};
	if (sigaltstack(&ss, NULL) != 0) {
		perror("[guest] sigaltstack");
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;
	sa.sa_flags = SA_ONSTACK | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) != 0) {
		perror("[guest] sigaction");
		return 1;
	}

	struct itimerval it = {
		.it_interval = {.tv_sec = 0, .tv_usec = 1000},
		.it_value = {.tv_sec = 0, .tv_usec = 1000},
	};
	if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
		perror("[guest] setitimer");
		return 1;
	}

	const int n = 3000;
	double want = reference(n);

	/*
	 * Coverage needs a few ticks, one of them landing inside an upcall, and
	 * how many a fixed amount of work collects depends on host load. So keep
	 * running rounds of work until the conditions are met, capped so a
	 * genuinely broken run still terminates.
	 */
	const double kMaxSeconds = 10.0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	long rounds = 0;
	int mismatches = 0;

	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, 65536, 0);
	for (;;) {
		if (compute(n) != want) {
			mismatches++;
		}
		rounds++;
		if (mismatches || (g_sig_total >= 3 && g_sig_in_upcall >= 1)) {
			break;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if ((double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9 > kMaxSeconds) {
			break;
		}
	}
	pstepper_disable();

	struct itimerval off = {{0, 0}, {0, 0}};
	setitimer(ITIMER_REAL, &off, NULL);

	fprintf(stderr, "[guest] steps=%ld  altstack steps=%ld  double deliveries=%ld\n", g_steps, g_steps_on_altstack,
		g_double_deliveries);
	fprintf(stderr, "[guest] signals: total=%ld  during upcall=%ld\n", g_sig_total, g_sig_in_upcall);
	fprintf(stderr, "[guest] rounds=%ld  mismatches=%d\n", rounds, mismatches);

	int fail = 0;
	if (mismatches) {
		fprintf(stderr, "[guest] FAIL: stepped result != native reference\n");
		fail = 1;
	}
	if (g_double_deliveries != 0) {
		fprintf(stderr, "[guest] FAIL: %ld instruction(s) delivered twice\n", g_double_deliveries);
		fail = 1;
	}
	if (g_sig_total < 3) {
		fprintf(stderr, "[guest] FAIL: only %ld signals -- test proved nothing\n", g_sig_total);
		fail = 1;
	}
	if (g_sig_in_upcall < 1) {
		fprintf(stderr,
			"[guest] FAIL: no signal landed during an upcall -- the parked-trap path was not exercised\n");
		fail = 1;
	}

	fprintf(stderr, "[guest] %s\n", fail ? "FAIL" : "PASS");
	return fail;
}
