# see doc/amd64-drap-problem.adoc
	.file	"amd64-drap-regs-test.c"
# GNU C23 (Debian 16.2.0-4) version 16.2.0 (x86_64-linux-gnu)
#	compiled by GNU C version 16.2.0, GMP version 6.3.0, MPFR version 4.2.2, MPC version 1.3.1, isl version isl-0.28-GMP

# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -mforce-drap -mtune=generic -march=x86-64 -g0 -O2 -fasynchronous-unwind-tables
	.text
	.p2align 4
	.globl	drap_saved_regs
	.type	drap_saved_regs, @function
drap_saved_regs:
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
	pushq	%r12	#
	pushq	%r10	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -16; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x70,0x6
# NOTE: This cfi is DW_CFA_expression: r12 (r12) (DW_OP_breg6 (rbp): -8)
	.cfi_escape 0x10,0xc,0x2,0x76,0x78
	pushq	%rbx	#
# NOTE: This cfi is DW_CFA_expression: r3 (rbx) (DW_OP_breg6 (rbp): -24)
	.cfi_escape 0x10,0x3,0x2,0x76,0x68
	movq	%rdi, %rbx	# fn, fn
# amd64-drap-regs-test.c:9:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp109
# amd64-drap-regs-test.c:7: int drap_saved_regs(void (*fn)(int*)) {
	subq	$8152, %rsp	#,
# amd64-drap-regs-test.c:9:   fn(aligned_array);
	call	*%rbx	# fn
# amd64-drap-regs-test.c:10:   int first = aligned_array[0];
	movl	-8176(%rbp), %r12d	# aligned_array[0], first
# amd64-drap-regs-test.c:11:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp110
	call	*%rbx	# fn
# amd64-drap-regs-test.c:12:   return first + aligned_array[0];
	movl	-8176(%rbp), %eax	# aligned_array[0], first
# amd64-drap-regs-test.c:13: }
	addq	$8152, %rsp	#,
	popq	%rbx	#
	.cfi_restore 3
	popq	%r10	#
	.cfi_restore 10
	.cfi_def_cfa 10, 0
# amd64-drap-regs-test.c:12:   return first + aligned_array[0];
	addl	%r12d, %eax	# first, first
# amd64-drap-regs-test.c:13: }
	popq	%r12	#
	.cfi_restore 12
	popq	%rbp	#
	.cfi_restore 6
	leaq	-8(%r10), %rsp	#,
	.cfi_def_cfa 7, 8
	ret	
	.cfi_endproc
.LFE0:
	.size	drap_saved_regs, .-drap_saved_regs
	.ident	"GCC: (Debian 16.2.0-4) 16.2.0"
	.section	.note.GNU-stack,"",@progbits
