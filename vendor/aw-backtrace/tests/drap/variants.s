# see doc/amd64-drap-problem.adoc
	.file	"amd64-drap-variants-test.c"
# GNU C23 (Debian 16.2.0-4) version 16.2.0 (x86_64-linux-gnu)
#	compiled by GNU C version 16.2.0, GMP version 6.3.0, MPFR version 4.2.2, MPC version 1.3.1, isl version isl-0.28-GMP

# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -mforce-drap -mtune=generic -march=x86-64 -g0 -O2 -fasynchronous-unwind-tables
	.text
	.p2align 4
	.type	inner.0, @function
inner.0:
.LFB2:
	.cfi_startproc
	pushq	%r13	#
	.cfi_def_cfa_offset 16
	.cfi_offset 13, -16
	leaq	16(%rsp), %r13	#,
	.cfi_def_cfa 13, 0
	andq	$-4096, %rsp	#,
	pushq	-8(%r13)	#
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
# NOTE: This cfi is DW_CFA_expression: r6 (rbp) (DW_OP_breg6 (rbp): 0)
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r13	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -8; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x78,0x6
	pushq	%rbx	#
# amd64-drap-variants-test.c:31:     fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp101
# NOTE: This cfi is DW_CFA_expression: r3 (rbx) (DW_OP_breg6 (rbp): -16)
	.cfi_escape 0x10,0x3,0x2,0x76,0x70
# amd64-drap-variants-test.c:29:   __attribute__((noinline)) void inner(void) {
	movq	%r10, %rbx	# CHAIN.1, CHAIN.1
	subq	$8160, %rsp	#,
# amd64-drap-variants-test.c:31:     fn(aligned_array);
	call	*(%r10)	# CHAIN.1_2(D)->fn
# amd64-drap-variants-test.c:32:     result = aligned_array[0];
	movl	-8176(%rbp), %eax	# aligned_array[0], aligned_array[0]
	movl	%eax, 8(%rbx)	# aligned_array[0], CHAIN.1_2(D)->result
# amd64-drap-variants-test.c:33:   }
	addq	$8160, %rsp	#,
	popq	%rbx	#
	.cfi_restore 3
	popq	%r13	#
	.cfi_restore 13
	.cfi_def_cfa 13, 0
	popq	%rbp	#
	.cfi_restore 6
	leaq	-16(%r13), %rsp	#,
	.cfi_def_cfa 7, 16
	popq	%r13	#
	.cfi_restore 13
	.cfi_def_cfa_offset 8
	ret	
	.cfi_endproc
.LFE2:
	.size	inner.0, .-inner.0
	.p2align 4
	.globl	drap_many_regs
	.type	drap_many_regs, @function
drap_many_regs:
.LFB0:
	.cfi_startproc
	leaq	8(%rsp), %r10	#,
	.cfi_def_cfa 10, 0
	andq	$-4096, %rsp	#,
	pushq	-8(%r10)	#
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
# NOTE: This cfi is DW_CFA_expression: r6 (rbp) (DW_OP_breg6 (rbp): 0)
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r15	#
	pushq	%r14	#
	pushq	%r13	#
	pushq	%r12	#
# NOTE: This cfi is DW_CFA_expression: r15 (r15) (DW_OP_breg6 (rbp): -8)
	.cfi_escape 0x10,0xf,0x2,0x76,0x78
# NOTE: This cfi is DW_CFA_expression: r14 (r14) (DW_OP_breg6 (rbp): -16)
	.cfi_escape 0x10,0xe,0x2,0x76,0x70
# NOTE: This cfi is DW_CFA_expression: r13 (r13) (DW_OP_breg6 (rbp): -24)
	.cfi_escape 0x10,0xd,0x2,0x76,0x68
# NOTE: This cfi is DW_CFA_expression: r12 (r12) (DW_OP_breg6 (rbp): -32)
	.cfi_escape 0x10,0xc,0x2,0x76,0x60
	movq	%rdi, %r12	# fn, fn
# amd64-drap-variants-test.c:11:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp125
# amd64-drap-variants-test.c:9: int drap_many_regs(void (*fn)(int*)) {
	pushq	%r10	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -40; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x58,0x6
	pushq	%rbx	#
	subq	$12224, %rsp	#,
# NOTE: This cfi is DW_CFA_expression: r3 (rbx) (DW_OP_breg6 (rbp): -48)
	.cfi_escape 0x10,0x3,0x2,0x76,0x50
# amd64-drap-variants-test.c:11:   fn(aligned_array);
	call	*%r12	# fn
# amd64-drap-variants-test.c:12:   int r1 = aligned_array[0];
	movl	-8176(%rbp), %ebx	# aligned_array[0], r1
# amd64-drap-variants-test.c:13:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp126
	call	*%r12	# fn
# amd64-drap-variants-test.c:14:   int r2 = aligned_array[0];
	movl	-8176(%rbp), %r15d	# aligned_array[0], r2
# amd64-drap-variants-test.c:15:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp127
	call	*%r12	# fn
# amd64-drap-variants-test.c:16:   int r3 = aligned_array[0];
	movl	-8176(%rbp), %r14d	# aligned_array[0], r3
# amd64-drap-variants-test.c:17:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp128
	call	*%r12	# fn
# amd64-drap-variants-test.c:18:   int r4 = aligned_array[0];
	movl	-8176(%rbp), %r13d	# aligned_array[0], r4
# amd64-drap-variants-test.c:19:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp129
	call	*%r12	# fn
# amd64-drap-variants-test.c:20:   int r5 = aligned_array[0];
	movl	-8176(%rbp), %eax	# aligned_array[0], r5
# amd64-drap-variants-test.c:21:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp131
# amd64-drap-variants-test.c:20:   int r5 = aligned_array[0];
	movl	%eax, -8180(%rbp)	# r5, %sfp
# amd64-drap-variants-test.c:21:   fn(aligned_array);
	call	*%r12	# fn
# amd64-drap-variants-test.c:22:   return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
	leal	(%rbx,%r15), %eax	#, _1
# amd64-drap-variants-test.c:22:   return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
	addl	%r14d, %eax	# r3, _2
# amd64-drap-variants-test.c:22:   return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
	addl	%r13d, %eax	# r4, _3
# amd64-drap-variants-test.c:22:   return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
	addl	-8180(%rbp), %eax	# %sfp, _4
# amd64-drap-variants-test.c:22:   return r1 + r2 + r3 + r4 + r5 + aligned_array[0];
	addl	-8176(%rbp), %eax	# aligned_array[0], <retval>
# amd64-drap-variants-test.c:23: }
	addq	$12224, %rsp	#,
	popq	%rbx	#
	.cfi_restore 3
	popq	%r10	#
	.cfi_restore 10
	.cfi_def_cfa 10, 0
	popq	%r12	#
	.cfi_restore 12
	popq	%r13	#
	.cfi_restore 13
	popq	%r14	#
	.cfi_restore 14
	popq	%r15	#
	.cfi_restore 15
	popq	%rbp	#
	.cfi_restore 6
	leaq	-8(%r10), %rsp	#,
	.cfi_def_cfa 7, 8
	ret	
	.cfi_endproc
.LFE0:
	.size	drap_many_regs, .-drap_many_regs
	.p2align 4
	.globl	drap_r13_nested
	.type	drap_r13_nested, @function
drap_r13_nested:
.LFB1:
	.cfi_startproc
	subq	$40, %rsp	#,
	.cfi_def_cfa_offset 48
# amd64-drap-variants-test.c:27: int drap_r13_nested(void (*fn)(int*)) {
	leaq	48(%rsp), %rax	#, tmp106
# amd64-drap-variants-test.c:34:   inner();
	movq	%rsp, %r10	#,
# amd64-drap-variants-test.c:27: int drap_r13_nested(void (*fn)(int*)) {
	movq	%rdi, (%rsp)	# fn, FRAME.0.fn
	movq	%rax, 16(%rsp)	# tmp106, FRAME.0.FRAME_BASE.PARENT
# amd64-drap-variants-test.c:34:   inner();
	movl	$0, 8(%rsp)	#, MEM <int> [(void *)&FRAME.0 + 8B]
	call	inner.0	#
# amd64-drap-variants-test.c:36: }
	movl	8(%rsp), %eax	# MEM <int> [(void *)&FRAME.0 + 8B],
	addq	$40, %rsp	#,
	.cfi_def_cfa_offset 8
	ret	
	.cfi_endproc
.LFE1:
	.size	drap_r13_nested, .-drap_r13_nested
	.p2align 4
	.globl	drap_r13_ncsr
	.type	drap_r13_ncsr, @function
drap_r13_ncsr:
.LFB3:
	.cfi_startproc
	pushq	%r13	#
	.cfi_def_cfa_offset 16
	.cfi_offset 13, -16
	leaq	16(%rsp), %r13	#,
	.cfi_def_cfa 13, 0
	andq	$-4096, %rsp	#,
	pushq	-8(%r13)	#
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
# NOTE: This cfi is DW_CFA_expression: r6 (rbp) (DW_OP_breg6 (rbp): 0)
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r13	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -8; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x78,0x6
	pushq	%r11	#
	pushq	%r10	#
	pushq	%r9	#
	pushq	%r8	#
	pushq	%rdi	#
	pushq	%rsi	#
	pushq	%rbx	#
# NOTE: This cfi is DW_CFA_expression: r11 (r11) (DW_OP_breg6 (rbp): -16)
	.cfi_escape 0x10,0xb,0x2,0x76,0x70
# NOTE: This cfi is DW_CFA_expression: r10 (r10) (DW_OP_breg6 (rbp): -24)
	.cfi_escape 0x10,0xa,0x2,0x76,0x68
# NOTE: This cfi is DW_CFA_expression: r9 (r9) (DW_OP_breg6 (rbp): -32)
	.cfi_escape 0x10,0x9,0x2,0x76,0x60
# NOTE: This cfi is DW_CFA_expression: r8 (r8) (DW_OP_breg6 (rbp): -40)
	.cfi_escape 0x10,0x8,0x2,0x76,0x58
# NOTE: This cfi is DW_CFA_expression: r5 (rdi) (DW_OP_breg6 (rbp): -48)
	.cfi_escape 0x10,0x5,0x2,0x76,0x50
# NOTE: This cfi is DW_CFA_expression: r4 (rsi) (DW_OP_breg6 (rbp): -56)
	.cfi_escape 0x10,0x4,0x2,0x76,0x48
# NOTE: This cfi is DW_CFA_expression: r3 (rbx) (DW_OP_breg6 (rbp): -64)
	.cfi_escape 0x10,0x3,0x2,0x76,0x40
	movq	%rdi, %rbx	# fn, fn
# amd64-drap-variants-test.c:42:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp100
# amd64-drap-variants-test.c:40: __attribute__((no_caller_saved_registers, target("general-regs-only"))) int drap_r13_ncsr(void (*fn)(int*)) {
	pushq	%rcx	#
	pushq	%rdx	#
	subq	$8096, %rsp	#,
# NOTE: This cfi is DW_CFA_expression: r2 (rcx) (DW_OP_breg6 (rbp): -72)
	.cfi_escape 0x10,0x2,0x3,0x76,0xb8,0x7f
# NOTE: This cfi is DW_CFA_expression: r1 (rdx) (DW_OP_breg6 (rbp): -80)
	.cfi_escape 0x10,0x1,0x3,0x76,0xb0,0x7f
# amd64-drap-variants-test.c:42:   fn(aligned_array);
	call	*%rbx	# fn
# amd64-drap-variants-test.c:44: }
	movl	-8176(%rbp), %eax	# aligned_array[0],
	addq	$8096, %rsp	#,
	popq	%rdx	#
	.cfi_restore 1
	popq	%rcx	#
	.cfi_restore 2
	popq	%rbx	#
	.cfi_restore 3
	popq	%rsi	#
	.cfi_restore 4
	popq	%rdi	#
	.cfi_restore 5
	popq	%r8	#
	.cfi_restore 8
	popq	%r9	#
	.cfi_restore 9
	popq	%r10	#
	.cfi_restore 10
	popq	%r11	#
	.cfi_restore 11
	popq	%r13	#
	.cfi_restore 13
	.cfi_def_cfa 13, 0
	popq	%rbp	#
	.cfi_restore 6
	leaq	-16(%r13), %rsp	#,
	.cfi_def_cfa 7, 16
	popq	%r13	#
	.cfi_restore 13
	.cfi_def_cfa_offset 8
	ret	
	.cfi_endproc
.LFE3:
	.size	drap_r13_ncsr, .-drap_r13_ncsr
	.ident	"GCC: (Debian 16.2.0-4) 16.2.0"
	.section	.note.GNU-stack,"",@progbits
