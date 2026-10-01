// SPDX-License-Identifier: 0BSD
/*
 * pstepper_trampoline's stack frame layout (arm64) -- internal to the
 * pstepper implementation, not part of the guest-facing ABI in pstepper.h.
 *
 * Shared between pstepper_trampoline_arm64.S (which addresses fields by these
 * plain numeric offsets -- a .S file is only preprocessed, never compiled,
 * so it cannot evaluate offsetof() itself) and pstepper_arm64.c (which
 * cross-checks them against the real struct layout via _Static_assert, so a
 * layout change that isn't mirrored here fails the build instead of
 * silently corrupting register state at runtime). The __ASSEMBLER__ guard
 * is the standard idiom -- gcc predefines it while preprocessing a .S file.
 *
 * Unlike the amd64 frame there is no floating flags word: aarch64 has no
 * push-flags instruction, so NZCV is saved into the frame like any other
 * register (see the trampoline). Only the [PC][SP] pair the plugin stages
 * floats above the frame.
 *
 * FP/SIMD: NEON (V0..V31 + FPSR + FPCR) is always saved -- a fixed 528
 * bytes. If the CPU has SVE (decided once in pstepper_arch_prepare(), key
 * pstepper_sve_vl), Z0..Z31 + P0..P15 + FFR are saved too, into sve_area,
 * packed at the runtime vector length. SME streaming mode / ZA are not
 * supported -- the trampoline die()s if PSTATE says either is active.
 */
#ifndef PSTEPPER_FRAME_ARM64_H_
#define PSTEPPER_FRAME_ARM64_H_

/*
 * gpr area: exactly pstepper_gpregs_t == { x[31], sp, pc } (see
 * pstepper_arm64.h), the natural-order image of glibc mcontext_t's regs..pc.
 * x_n lives at PSTEPPER_OFF_X0 + 8*n.
 */
#define PSTEPPER_OFF_X0 0
#define PSTEPPER_OFF_SP 248   /* x[31] ends at 248 */
#define PSTEPPER_OFF_PC 256
#define PSTEPPER_OFF_NZCV 264  /* mrs/msr NZCV; low 32 bits meaningful */
#define PSTEPPER_OFF_VREGS 272 /* V0..V31, 16 bytes each -> 512 bytes */
#define PSTEPPER_OFF_FPSR 784  /* low 32 bits */
#define PSTEPPER_OFF_FPCR 792  /* low 32 bits */

/*
 * SVE save area. Contents when pstepper_sve_vl != 0, packed at the runtime
 * VL (bytes):
 *     Z0..Z31   at  n * VL                 (32 * VL bytes)
 *     P0..P15   at  32*VL + n * (VL/8)      (16 * VL/8 == 2*VL bytes)
 *     FFR       at  34*VL                   (VL/8 bytes)
 * This mirrors the kernel's SVE signal-frame layout. The reservation is the
 * architectural worst case (VL = 256 bytes = 2048 bits); pstepper_arch_prepare()
 * aborts if the host VL somehow exceeds it.
 */
#define PSTEPPER_SVE_VL_MAX 256
#define PSTEPPER_SVE_AREA_MAX (32 * PSTEPPER_SVE_VL_MAX + 16 * (PSTEPPER_SVE_VL_MAX / 8) + (PSTEPPER_SVE_VL_MAX / 8))
#define PSTEPPER_OFF_SVE 800

/*
 * Bytes the trampoline sub/adds off SP for the frame. Must be a multiple of
 * 16 (AArch64 requires SP 16-aligned whenever it is used to access memory);
 * entry SP = upcall_top - 16 is already 16-aligned. Too large for a single
 * `sub sp, sp, #imm`, so the trampoline splits it: `#(SIZE>>12), lsl #12`
 * plus `#(SIZE & 0xfff)` -- hence the (SIZE >> 12) <= 0xfff constraint.
 */
#define PSTEPPER_FRAME_SIZE 9536 /* 0x2540 -> sub #2,lsl#12 (0x2000) + sub #0x540 */

#ifndef __ASSEMBLER__

#include <stddef.h>

#include "pstepper.h"

typedef struct pstepper_frame {
	pstepper_gpregs_t gpr; /* x[31], sp, pc -- 264 bytes */
	uint64_t nzcv;
	unsigned char vregs[512] __attribute__((aligned(16))); /* V0..V31 */
	uint64_t fpsr;
	uint64_t fpcr;
	unsigned char sve_area[PSTEPPER_SVE_AREA_MAX] __attribute__((aligned(16)));
} pstepper_frame_t;

_Static_assert(offsetof(pstepper_frame_t, gpr) == PSTEPPER_OFF_X0, "offset drift: gpr");
_Static_assert(offsetof(pstepper_frame_t, gpr.sp) == PSTEPPER_OFF_SP, "offset drift: sp");
_Static_assert(offsetof(pstepper_frame_t, gpr.pc) == PSTEPPER_OFF_PC, "offset drift: pc");
_Static_assert(offsetof(pstepper_frame_t, nzcv) == PSTEPPER_OFF_NZCV, "offset drift: nzcv");
_Static_assert(offsetof(pstepper_frame_t, vregs) == PSTEPPER_OFF_VREGS, "offset drift: vregs");
_Static_assert(offsetof(pstepper_frame_t, fpsr) == PSTEPPER_OFF_FPSR, "offset drift: fpsr");
_Static_assert(offsetof(pstepper_frame_t, fpcr) == PSTEPPER_OFF_FPCR, "offset drift: fpcr");
_Static_assert(offsetof(pstepper_frame_t, sve_area) == PSTEPPER_OFF_SVE, "offset drift: sve_area");
_Static_assert(PSTEPPER_OFF_VREGS % 16 == 0, "vregs must be 16-byte aligned within the frame");
_Static_assert(PSTEPPER_OFF_SVE % 16 == 0, "sve_area must be 16-byte aligned within the frame");
_Static_assert(PSTEPPER_FRAME_SIZE % 16 == 0, "frame size must be a multiple of 16 (SP alignment)");
_Static_assert((PSTEPPER_FRAME_SIZE >> 12) <= 0xfff, "frame size too large for the split sub sp sequence");
_Static_assert(PSTEPPER_FRAME_SIZE >= sizeof(pstepper_frame_t), "frame size constant must fit the real struct");

#endif /* __ASSEMBLER__ */

#endif /* PSTEPPER_FRAME_ARM64_H_ */
