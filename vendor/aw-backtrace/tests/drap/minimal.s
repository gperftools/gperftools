# see doc/amd64-drap-problem.adoc
	.file	"amd64-drap-test.c"
# GNU C23 (Debian 16.1.0-3) version 16.1.0 (x86_64-linux-gnu)
#	compiled by GNU C version 16.1.0, GMP version 6.3.0, MPFR version 4.2.2, MPC version 1.3.1, isl version isl-0.28-GMP

# GGC heuristics: --param ggc-min-expand=100 --param ggc-min-heapsize=131072
# options passed: -mforce-drap -mtune=generic -march=x86-64 -g0 -O2 -fasynchronous-unwind-tables
	.text
	.p2align 4
	.globl	minimal_drap
	.type	minimal_drap, @function
minimal_drap:
.LFB0:
	.cfi_startproc
	leaq	8(%rsp), %r10	#,
	.cfi_def_cfa 10, 0
	andq	$-4096, %rsp	#,
	movq	%rdi, %rax	# fn, fn
	pushq	-8(%r10)	#
	pushq	%rbp	#
	movq	%rsp, %rbp	#,
# NOTE: This cfi is DW_CFA_expression: r6 (rbp) (DW_OP_breg6 (rbp): 0)
	.cfi_escape 0x10,0x6,0x2,0x76,0
	pushq	%r10	#
# NOTE: This cfi is DW_CFA_def_cfa_expression (DW_OP_breg6 (rbp): -8; DW_OP_deref)
	.cfi_escape 0xf,0x3,0x76,0x78,0x6
# amd64-drap-test.c:5:   fn(aligned_array);
	leaq	-8176(%rbp), %rdi	#, tmp100
# amd64-drap-test.c:3: int minimal_drap(void (*fn)(int*)) {
	subq	$8168, %rsp	#,
# amd64-drap-test.c:5:   fn(aligned_array);
	call	*%rax	# fn
# amd64-drap-test.c:7: }
	movq	-8(%rbp), %r10	#,
	.cfi_def_cfa 10, 0
	movl	-8176(%rbp), %eax	# aligned_array[0],
	leave
	.cfi_restore 6
	leaq	-8(%r10), %rsp	#,
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE0:
	.size	minimal_drap, .-minimal_drap
	.ident	"GCC: (Debian 16.1.0-3) 16.1.0"
	.section	.note.GNU-stack,"",@progbits
