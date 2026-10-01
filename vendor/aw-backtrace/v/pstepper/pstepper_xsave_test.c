// SPDX-License-Identifier: 0BSD
/*
 * Guest test program: proves the trampoline round-trips the *full* vector
 * register state (YMM upper halves included) across an upcall, not just the
 * legacy x87+SSE area an `fxsave` would cover.
 *
 * ymm_roundtrip() loads a known 256-byte pattern into ymm0..ymm7, spins
 * briefly (so the step handler fires many times mid-sequence), then stores
 * ymm0..ymm7 back out. The handler deliberately trashes all of ymm0..ymm7
 * -- full 256 bits -- on every single step. If the trampoline only saved
 * the legacy 512-byte area (old `fxsave`/`fxrstor`), the upper 128 bits of
 * each caller ymm register are lost across every trap and the readback will
 * not match the input. With `xsave`/`xrstor` over an FP|SSE|YMM mask it
 * matches byte-for-byte.
 *
 * The load / spin / store all live in one asm block so the compiler cannot
 * insert a spill/reload of ymm state that would paper over a broken save.
 */
#include <stdio.h>
#include <string.h>

#include "pstepper.h"

static volatile long g_steps;

static void handler(int uxi_kind, void *info, ucontext_t *uc)
{
	(void)uxi_kind;
	(void)info;
	(void)uc;
	g_steps++;
	/*
	 * Trash the full 256 bits of ymm0..ymm7 (vpcmpeqd sets all bits of the
	 * whole destination register). This is what a real handler chain does
	 * implicitly the moment it touches an AVX-optimized libc routine.
	 */
	__asm__ volatile("vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\t"
			 "vpcmpeqd %%ymm1, %%ymm1, %%ymm1\n\t"
			 "vpcmpeqd %%ymm2, %%ymm2, %%ymm2\n\t"
			 "vpcmpeqd %%ymm3, %%ymm3, %%ymm3\n\t"
			 "vpcmpeqd %%ymm4, %%ymm4, %%ymm4\n\t"
			 "vpcmpeqd %%ymm5, %%ymm5, %%ymm5\n\t"
			 "vpcmpeqd %%ymm6, %%ymm6, %%ymm6\n\t"
			 "vpcmpeqd %%ymm7, %%ymm7, %%ymm7\n\t" ::
				 : "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7");
}

__attribute__((noinline)) static void ymm_roundtrip(const unsigned char *in, unsigned char *out)
{
	__asm__ volatile("vmovdqu   0(%0), %%ymm0\n\t"
			 "vmovdqu  32(%0), %%ymm1\n\t"
			 "vmovdqu  64(%0), %%ymm2\n\t"
			 "vmovdqu  96(%0), %%ymm3\n\t"
			 "vmovdqu 128(%0), %%ymm4\n\t"
			 "vmovdqu 160(%0), %%ymm5\n\t"
			 "vmovdqu 192(%0), %%ymm6\n\t"
			 "vmovdqu 224(%0), %%ymm7\n\t"
			 "mov $200, %%ecx\n\t"
			 "1:\n\t"
			 "dec %%ecx\n\t"
			 "jnz 1b\n\t"
			 "vmovdqu %%ymm0,   0(%1)\n\t"
			 "vmovdqu %%ymm1,  32(%1)\n\t"
			 "vmovdqu %%ymm2,  64(%1)\n\t"
			 "vmovdqu %%ymm3,  96(%1)\n\t"
			 "vmovdqu %%ymm4, 128(%1)\n\t"
			 "vmovdqu %%ymm5, 160(%1)\n\t"
			 "vmovdqu %%ymm6, 192(%1)\n\t"
			 "vmovdqu %%ymm7, 224(%1)\n\t"
			 :
			 : "r"(in), "r"(out)
			 : "rcx", "cc", "memory", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7");
}

int main(void)
{
	unsigned char in[256], out[256];
	for (int i = 0; i < 256; i++) {
		in[i] = (unsigned char)(i * 7 + 1);
		out[i] = 0;
	}

	fprintf(stderr, "[guest] arming pstepper\n");
	pstepper_enable_or_die(handler, 65536, 0);

	ymm_roundtrip(in, out);

	pstepper_disable();
	fprintf(stderr, "[guest] disabled pstepper, steps = %ld\n", g_steps);

	if (memcmp(in, out, 256) != 0) {
		int first = 0;
		while (first < 256 && in[first] == out[first]) {
			first++;
		}
		fprintf(stderr,
			"[guest] FAIL: ymm state not preserved across upcall; first mismatch at byte %d"
			" (ymm%d lane %s): in=0x%02x out=0x%02x\n",
			first, first / 32, (first % 32) < 16 ? "low" : "high", in[first], out[first]);
		return 1;
	}

	fprintf(stderr, "[guest] PASS\n");
	return 0;
}
