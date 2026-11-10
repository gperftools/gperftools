/* SPDX-License-Identifier: 0BSD */
/*
 * The library's assembly, x86-64. Every .S here ends by including
 * aw-asm-notes.h, and has to meet the requirements stated there.
 *
 * void aw_capture_caller_cursor(Cursor* out): see its declaration in
 * aw-arch.h for what it is for.
 *
 * Writes the *caller's* Cursor as of the call: pc = the return address into
 * the caller, sp = the caller's sp once we return (just above that return
 * address), fp = %rbp, which this leaf never touches.
 *
 * Compiled on every architecture; only the notes survive elsewhere.
 */
#include "renamings.h"

#if defined(__x86_64__)
	.text
	.p2align 4
	.globl	aw_capture_caller_cursor
	.hidden	aw_capture_caller_cursor
	.type	aw_capture_caller_cursor, @function
aw_capture_caller_cursor:
	.cfi_startproc
	endbr64				/* always; see aw-asm-notes.h */
	movq	(%rsp), %rax		/* Cursor::pc */
	movq	%rax, (%rdi)
	leaq	8(%rsp), %rax		/* Cursor::sp */
	movq	%rax, 8(%rdi)
	movq	%rbp, 16(%rdi)		/* Cursor::fp */
	ret
	.cfi_endproc
	.size	aw_capture_caller_cursor, . - aw_capture_caller_cursor
#endif  /* __x86_64__ */

#include "aw-asm-notes.h"
