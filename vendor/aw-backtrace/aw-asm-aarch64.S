/* SPDX-License-Identifier: 0BSD */
/*
 * The library's assembly, aarch64. Every .S here ends by including
 * aw-asm-notes.h, and has to meet the requirements stated there.
 *
 * void aw_capture_caller_cursor(Cursor* out): see its declaration in
 * aw-arch.h for what it is for.
 *
 * Writes the *caller's* Cursor as of the call: pc = the return address into
 * the caller, sp and fp = the caller's own. bl writes x30 and touches no
 * stack, and this leaf saves nothing, so all three are just the registers at
 * entry. x30 comes straight from bl, so it is never PAC-signed.
 *
 * Compiled on every architecture; only the notes survive elsewhere.
 */
#include "renamings.h"

#if defined(__aarch64__)
	.text
	.p2align 2
	.globl	aw_capture_caller_cursor
	.hidden	aw_capture_caller_cursor
	.type	aw_capture_caller_cursor, %function
aw_capture_caller_cursor:
	.cfi_startproc
	bti	c			/* always; see aw-asm-notes.h */
	mov	x1, sp
	stp	x30, x1, [x0]		/* Cursor::pc, Cursor::sp */
	str	x29, [x0, #16]		/* Cursor::fp */
	ret
	.cfi_endproc
	.size	aw_capture_caller_cursor, . - aw_capture_caller_cursor
#endif  /* __aarch64__ */

#include "aw-asm-notes.h"
