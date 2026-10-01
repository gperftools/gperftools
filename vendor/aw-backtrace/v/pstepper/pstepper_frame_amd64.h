// SPDX-License-Identifier: 0BSD
/*
 * pstepper_trampoline's stack frame layout (amd64) -- internal to the
 * pstepper implementation, not part of the guest-facing ABI in pstepper.h.
 *
 * Shared between pstepper_trampoline_amd64.S (which addresses fields by these
 * plain numeric offsets -- a .S file is only preprocessed, never compiled,
 * so it can use a #define directly but could never evaluate offsetof()
 * itself) and pstepper.c (which cross-checks them against the real struct
 * layout via _Static_assert, so a layout change that isn't mirrored here
 * fails the build instead of silently corrupting register state at
 * runtime). The __ASSEMBLER__ guard is the standard idiom for this split --
 * gcc predefines it while preprocessing a .S file for the assembler.
 */
#ifndef PSTEPPER_FRAME_AMD64_H_
#define PSTEPPER_FRAME_AMD64_H_

/*
 * Field order mirrors x86-64 glibc's gregset_t -- the REG_* indices in
 * <sys/ucontext.h> -- so a "fake"-ish ucontext_t built from this frame is a
 * straight index mapping. Keep pstepper_gpregs_t in pstepper.h in lockstep;
 * the _Static_assert block below is the enforcement.
 */
#define PSTEPPER_OFF_R8 0
#define PSTEPPER_OFF_R9 8
#define PSTEPPER_OFF_R10 16
#define PSTEPPER_OFF_R11 24
#define PSTEPPER_OFF_R12 32
#define PSTEPPER_OFF_R13 40
#define PSTEPPER_OFF_R14 48
#define PSTEPPER_OFF_R15 56
#define PSTEPPER_OFF_RDI 64
#define PSTEPPER_OFF_RSI 72
#define PSTEPPER_OFF_RBP 80
#define PSTEPPER_OFF_RBX 88
#define PSTEPPER_OFF_RDX 96
#define PSTEPPER_OFF_RAX 104
#define PSTEPPER_OFF_RCX 112
#define PSTEPPER_OFF_RSP 120
#define PSTEPPER_OFF_RIP 128

/*
 * The XSAVE save area. Must be 64-byte aligned both within the frame and at
 * runtime -- `xsave`/`xrstor` #GP on a misaligned operand (unlike `fxsave`,
 * which only needed 16). The trampoline saves FP + SSE + AVX (and any wider
 * vector state the host has) here around the handler call, via `xsave` with
 * a runtime mask (pstepper_xsave_mask, computed once in pstepper_enable()).
 * See pstepper_trampoline_amd64.S for the alignment argument tying OFF_XSAVE,
 * FRAME_SIZE and the 64-byte-aligned upcall-stack top together.
 */
#define PSTEPPER_OFF_XSAVE 192

/*
 * Bytes reserved for the XSAVE area. Generously oversized on purpose: the
 * upcall stack is 64 KiB, so the cost of over-reserving is nil, and this
 * keeps sizeof(pstepper_frame_t) a compile-time constant (the clone hook
 * copies a whole frame with a plain struct assignment). 3072 covers
 * x87+SSE+AVX (832), the full AVX-512 component set (opmask + ZMM_Hi256 +
 * Hi16_ZMM, ~2560 total in standard format) and leaves headroom for PKRU /
 * APX. It deliberately does NOT cover AMX tile data (+8 KiB); the mask
 * excludes AMX and pstepper_enable() aborts loudly if CPUID ever reports a
 * required area larger than this.
 */
#define PSTEPPER_XSAVE_AREA_SIZE 3072

/*
 * Bytes reserved on the stack for the frame (the trampoline's `sub`/`add`
 * amount) -- deliberately NOT sizeof(pstepper_frame_t): picking it
 * independent of sizeof() (just required to be big enough) sidesteps the
 * compiler's forced struct-alignment padding, and lets us satisfy two
 * runtime alignment constraints at once:
 *
 *   - The trampoline's first instruction is `pushfq` (8 bytes, before RSP
 *     is touched for the frame), and entry RSP is the 64-byte-aligned
 *     upcall-stack top minus 16. So after pushfq (-8) and `sub $FRAME_SIZE`,
 *     RSP_frame = top - 24 - FRAME_SIZE. For the internal `call` to be
 *     SysV-legal RSP_frame must be 16-aligned  =>  FRAME_SIZE % 16 == 8.
 *   - The `xsave` operand is RSP_frame + OFF_XSAVE and must be 64-aligned.
 *     With `top` 64-aligned that needs (FRAME_SIZE + 24 - OFF_XSAVE) % 64 == 0.
 *
 * FRAME_SIZE % 64 == 40 satisfies both (40 % 16 == 8, and 40 + 24 == 64).
 */
#define PSTEPPER_FRAME_SIZE 3304

#ifndef __ASSEMBLER__

#include <stddef.h>

#include "pstepper_internal.h"

typedef struct pstepper_frame {
	pstepper_gpregs_t gpr;
	/* Padding so xsave_area lands on a 64-byte boundary -- xsave/xrstor
	 * fault on a misaligned operand. */
	unsigned char _pad_to_xsave[PSTEPPER_OFF_XSAVE - sizeof(pstepper_gpregs_t)];
	unsigned char xsave_area[PSTEPPER_XSAVE_AREA_SIZE] __attribute__((aligned(64)));
} pstepper_frame_t;

_Static_assert(offsetof(pstepper_frame_t, gpr.r8) == PSTEPPER_OFF_R8, "offset drift: r8");
_Static_assert(offsetof(pstepper_frame_t, gpr.r9) == PSTEPPER_OFF_R9, "offset drift: r9");
_Static_assert(offsetof(pstepper_frame_t, gpr.r10) == PSTEPPER_OFF_R10, "offset drift: r10");
_Static_assert(offsetof(pstepper_frame_t, gpr.r11) == PSTEPPER_OFF_R11, "offset drift: r11");
_Static_assert(offsetof(pstepper_frame_t, gpr.r12) == PSTEPPER_OFF_R12, "offset drift: r12");
_Static_assert(offsetof(pstepper_frame_t, gpr.r13) == PSTEPPER_OFF_R13, "offset drift: r13");
_Static_assert(offsetof(pstepper_frame_t, gpr.r14) == PSTEPPER_OFF_R14, "offset drift: r14");
_Static_assert(offsetof(pstepper_frame_t, gpr.r15) == PSTEPPER_OFF_R15, "offset drift: r15");
_Static_assert(offsetof(pstepper_frame_t, gpr.rdi) == PSTEPPER_OFF_RDI, "offset drift: rdi");
_Static_assert(offsetof(pstepper_frame_t, gpr.rsi) == PSTEPPER_OFF_RSI, "offset drift: rsi");
_Static_assert(offsetof(pstepper_frame_t, gpr.rbp) == PSTEPPER_OFF_RBP, "offset drift: rbp");
_Static_assert(offsetof(pstepper_frame_t, gpr.rbx) == PSTEPPER_OFF_RBX, "offset drift: rbx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rdx) == PSTEPPER_OFF_RDX, "offset drift: rdx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rax) == PSTEPPER_OFF_RAX, "offset drift: rax");
_Static_assert(offsetof(pstepper_frame_t, gpr.rcx) == PSTEPPER_OFF_RCX, "offset drift: rcx");
_Static_assert(offsetof(pstepper_frame_t, gpr.rsp) == PSTEPPER_OFF_RSP, "offset drift: rsp");
_Static_assert(offsetof(pstepper_frame_t, gpr.rip) == PSTEPPER_OFF_RIP, "offset drift: rip");
_Static_assert(offsetof(pstepper_frame_t, xsave_area) == PSTEPPER_OFF_XSAVE, "offset drift: xsave_area");
_Static_assert(PSTEPPER_OFF_XSAVE % 64 == 0, "xsave_area must be 64-byte aligned within the frame");
_Static_assert(PSTEPPER_XSAVE_AREA_SIZE >= 576, "xsave area must fit at least the legacy 512 bytes + 64-byte header");
_Static_assert(PSTEPPER_FRAME_SIZE >= sizeof(pstepper_frame_t), "frame size constant must fit the real struct");
_Static_assert(PSTEPPER_FRAME_SIZE % 16 == 8, "frame size + the leading pushfq must total a 16-byte "
					      "multiple, for the internal call's stack alignment");
_Static_assert((PSTEPPER_FRAME_SIZE + 24 - PSTEPPER_OFF_XSAVE) % 64 == 0,
	       "at runtime the xsave operand is (upcall_top - 24 - FRAME_SIZE) "
	       "+ OFF_XSAVE, with upcall_top 64-byte aligned; this must be a "
	       "multiple of 64 or xsave/xrstor #GP");

#endif /* __ASSEMBLER__ */

#endif /* PSTEPPER_FRAME_AMD64_H_ */
