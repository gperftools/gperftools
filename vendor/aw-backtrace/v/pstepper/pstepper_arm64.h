// SPDX-License-Identifier: 0BSD
/*
 * pstepper: arm64-specific slice of the guest-facing ABI.
 *
 * Picked by the arch #ifdef block in pstepper.h; also included directly by
 * pstepper_frame_arm64.h, which needs only pstepper_gpregs_t. Self-contained
 * (pulls its own <stddef.h>/<stdint.h>/<ucontext.h>). Everything here is
 * AArch64 detail that the x86-64 port keeps in pstepper_amd64.h: the
 * portable parts of the ABI (the syscall interface, the handler signature,
 * the entry points) stay in pstepper.h.
 *
 * The one type this defines, pstepper_gpregs_t, is what pstepper.h's
 * pstepper_handler_fn / pstepper_invoke_handler are expressed in terms of,
 * so the include has to precede those declarations.
 */
#ifndef PSTEPPER_ARM64_H_
#define PSTEPPER_ARM64_H_

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

/*
 * General-purpose register snapshot handed to the pstepper handler. As on
 * x86-64 this is only the integer file -- x0..x30, SP and PC -- since that
 * is all a backtracer needs. NEON/FP state is preserved transparently by
 * the trampoline around the handler call but never exposed here; NZCV
 * likewise (PSTATE is fake-zeroed in the ucontext, mirroring x86 leaving
 * REG_EFL at 0). There is no promise that edits to this struct are
 * reflected back on resume.
 *
 * The layout is exactly the {regs[31], sp, pc} run of AArch64 glibc's
 * mcontext_t (natural register order, no permutation like x86's gregset_t),
 * so building the "fake"-ish ucontext_t is a single memcpy onto
 * uc->uc_mcontext.regs[0]. The _Static_assert block enforces that against
 * the real header.
 */
typedef struct pstepper_gpregs {
	uint64_t x[31]; /* x0 .. x30 (x30 = LR) */
	uint64_t sp;
	uint64_t pc;
} pstepper_gpregs_t;

_Static_assert(sizeof(uint64_t) == sizeof(((mcontext_t *)0)->regs[0]), "mcontext_t register width is not 64-bit");
_Static_assert(sizeof(pstepper_gpregs_t) == 33 * sizeof(uint64_t), "pstepper_gpregs_t must be x[31] + sp + pc, gap-free");
/* sp and pc must directly follow regs[31] in mcontext_t, so the memcpy that
 * fills uc->uc_mcontext.regs[0..] also fills .sp and .pc. */
_Static_assert(offsetof(mcontext_t, sp) - offsetof(mcontext_t, regs) == 31 * sizeof(uint64_t),
	       "mcontext_t.sp does not immediately follow regs[31]");
_Static_assert(offsetof(mcontext_t, pc) - offsetof(mcontext_t, regs) == 32 * sizeof(uint64_t),
	       "mcontext_t.pc does not immediately follow .sp");

#endif /* PSTEPPER_ARM64_H_ */
