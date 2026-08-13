        .data

        .globl test_scratch_space
        .globl test_scratch_space_end
test_scratch_space:
        .space 1024
test_scratch_space_end:
        .byte 0

        .text

#define ENDOF(TC) \
        ud2; \
        .long 0xdefeb144; \
        .size TC, . - TC; \
        .type TC, @function; \
        .section .rodata.td,"aMS",@progbits,1; \
555: .string #TC; \
        .section test_case_pointers, "awR"; \
        .align 8; \
        .quad TC; \
        .quad 555b; \
        .text

initialize_flags:
        .cfi_startproc
        xor %rax, %rax
        pushfq
        .cfi_adjust_cfa_offset 8
        pop %rax
        .cfi_adjust_cfa_offset -8
        ret
        .cfi_endproc
.globl initialize_flags
.type initialize_flags, @function
.size initialize_flags, . - initialize_flags

simple_mov:
        mov %rcx, %rax
        mov %rax, %rdx
        mov %rdx, %rsi
        mov %rsp, %rdi
        lea 1f(%rip), %rbx
1:
ENDOF(simple_mov)

push_pop_64:
        push %rax
        push %rbx
        push %rcx
        push %rdx
        push %rsi
        push %rdi
        push %rbp

        //push %r8
        .byte 0x43 // same as %r8 but with unused rex.X bit
        push %rax

        push %r9
        push %r10
        push %r11
        push %r12
        push %r13
        push %r14
        push %r15
        pop %rax
        pop %rbx
        pop %rcx
        pop %rdx
        pop %rsi
        pop %rdi
        pop %rbp
        pop %r8
        pop %r9
        pop %r10
        pop %r11
        pop %r12
        pop %r13
        pop %r14
        pop %r15
ENDOF(push_pop_64)

imm_moves:
        movabs $-1, %rax

        mov $2, %ebx
        mov $3, %ecx
        mov $4, %edx
        mov $5, %esi
        mov $6, %edi
        mov $7, %ebp
        mov $8, %r8d
        mov $9, %r9d
        mov $10, %r10d
        mov $11, %r11d
        mov $12, %r12d
        mov $13, %r13d
        mov $14, %r14d
        mov $15, %r15d
ENDOF(imm_moves)

imm_moves64:
        mov $1, %rax
        mov $2, %rbx
        mov $3, %rcx
        mov $4, %rdx
        mov $5, %rsi
        mov $6, %rdi
        mov $7, %rbp
        mov $8, %r8
        mov $9, %r9
        mov $10, %r10
        mov $11, %r11
        mov $12, %r12
        mov $13, %r13
        mov $14, %r14
        mov $15, %r15
ENDOF(imm_moves64)

imm_moves64neg:
        mov $-1, %rax
        mov $-2, %rbx
        mov $-3, %rcx
        mov $-4, %rdx
        mov $-5, %rsi
        mov $-6, %rdi
        mov $-7, %rbp
        mov $-8, %r8
        mov $-9, %r9
        mov $-10, %r10
        mov $-11, %r11
        mov $-12, %r12
        mov $-13, %r13
        mov $-14, %r14
        mov $-15, %r15
ENDOF(imm_moves64neg)

add1_64:
        add $1, %rax
ENDOF(add1_64)

add1_32:
        add $1, %eax
ENDOF(add1_32)

add2_64:
        add $-1, %rax
ENDOF(add2_64)

add2_32:
        add $-1, %eax
ENDOF(add2_32)

xor_32:
        xor %ecx, %ecx
        xor %edx, %ebx
ENDOF(xor_32)

xor_64:
        xor %rcx, %rcx
        sub $0x100, %rdx
        xor %rdx, %rbx
ENDOF(xor_64)

sub_64:
        sub $0x100, %rdx
ENDOF(sub_64)

sub_32:
        sub $0x100, %edx
ENDOF(sub_32)

sub_32_overflow:
        mov $(1<<31), %edx
        sub $2, %edx
ENDOF(sub_32_overflow)

sub_64_no_overflow:
        mov $((1<<31)-1), %edx
        sub $2, %rdx
ENDOF(sub_64_no_overflow)

or_32:
        or %esp, %ebp
ENDOF(or_32)

or_64:
        or %rsp, %rbp
ENDOF(or_64)

cmp_32_0:
        cmp $0, %ecx
ENDOF(cmp_32_0)

cmp_64_0:
        cmp $0, %rcx
ENDOF(cmp_64_0)

cmp_32_1:
        cmp $1, %eax
ENDOF(cmp_32_1)

cmp_64_1:
        cmp $1, %rax
ENDOF(cmp_64_1)

cmp_32_2:
        sub %rcx, %rax // %rax is now uint64_t{-1}
        mov %eax, %edx // %rdx is now uint32_t{-1}
        xor %rdx, %rbx
        xor %rax, %rbx // %rbx has upper half set to 1s lower unchanged (flipped twice)
        cmp %r8d, %ebx
ENDOF(cmp_32_2)

cmp_32_2_64:
        sub %rcx, %rax // %rax is now uint64_t{-1}
        mov %eax, %edx // %rdx is now uint32_t{-1}
        xor %rdx, %rbx
        xor %rax, %rbx // %rbx has upper half set to 1s lower unchanged (flipped twice)
        cmp %r8, %rbx
ENDOF(cmp_32_2_64)

test_32_1:
        xor %ecx, %ecx
        test $-1, %ecx
ENDOF(test_32_1)

test_64_1:
        xor %ecx, %ecx
        test $-1, %rcx
ENDOF(test_64_1)

test_32_2:
        test $-1, %ecx
ENDOF(test_32_2)

test_64_2:
        test $-1, %rcx
ENDOF(test_64_2)

simple_jmp:
        jmp 1f
        mov $1, %rax
1:
        mov $0xff, %rcx
ENDOF(simple_jmp)

at_call_location:
        mov $42, %rax
        ret
.type at_call_location, @function
.size at_call_location, . - at_call_location

call_neg_offset:
        call at_call_location
        mov $-42, %ecx
ENDOF(call_neg_offset)

call_indirect:
        lea at_call_location(%rip), %rax
        call *%rax
        mov $-42, %ecx
ENDOF(call_indirect)

trivial_jmp_if_signed:
        mov %eax, %edx
        cmp $1, %edx
        js 1f
        mov $42, %ecx
1:
        mov $-42, %ebx
ENDOF(trivial_jmp_if_signed)

trivial_jmp_if_signed2:
        mov %eax, %edx
        cmp %edx, %ecx
        js 1f
        mov $42, %ecx
1:
        mov $-42, %ebx
ENDOF(trivial_jmp_if_signed2)

trivial_jmp_if_signed3:
        mov %eax, %edx
        cmp $1, %edx
        jns 1f
        mov $42, %ecx
1:
        mov $-42, %ebx
ENDOF(trivial_jmp_if_signed3)

cmp_imm_eax:
        cmp $0, %eax
        jz 1f
        mov $42, %ecx
1:
        mov $-42, %ebx
ENDOF(cmp_imm_eax)

test_imm_eax:
        test $-1, %eax
        jz 1f
        mov $42, %ecx
1:
        mov $-42, %ebx
ENDOF(test_imm_eax)

lea_32:
        sub $0x100, %rax
        lea 0x42(%rax), %ebx
ENDOF(lea_32)

lea_64:
        sub $0x100, %rcx
        lea 0x42(%rcx), %rbx
ENDOF(lea_64)

push_rsp:
        push %rsp
        mov %rsp, %rcx
        mov (%rcx), %rax
ENDOF(push_rsp)

pop_rsp:
        mov %rsp, %rbp
        push %rbx
        pop %rsp
        mov %rsp, %rax
        mov %rbp, %rsp
ENDOF(pop_rsp)

simple_store_and_load:
        movl $11, test_scratch_space+4(%rip)
        lea test_scratch_space+4(%rip), %rsi
        movl (%rsi), %esi

        lea test_scratch_space(%rip), %rax
        movq $42, (%rax)
        mov (%rax), %rax

        lea test_scratch_space-8(%rip), %r8
        movq $-42, 8(%r8)
        movq 8(%r8), %rcx
        movl 8(%r8), %edx

        movl $33, %ebx
        movl %ebx, 8(%r8)
        movq 8(%r8), %r9
        movl 8(%r8), %r10d

        movl $11, test_scratch_space+4(%rip)
        movq 8(%r8), %rdi
ENDOF(simple_store_and_load)

sib_test:
        xor %rax, %rax
        lea test_scratch_space(%rip), %rbx
        // simple scale
        lea (%rbx, %rax, 1), %rcx

        // scale * 2
        lea (%rbx, %rax, 2), %rdx

        // different index
        mov $1, %rax
        lea (%rbx, %rax, 1), %rsi

        // base + index * scale + disp8
        lea 1(%rbx, %rax, 2), %rdi

        // no base, only index * scale + disp32
        mov %rbx, %r13
        lea 0x12345678(,%r13, 8), %r14
ENDOF(sib_test)

shift_test:
        // Test 1: SHL by 1 (OF check)
        // Set up bit 62 set
        mov $1, %rax
        shl $62, %rax
        mov %rax, %rbx // Save initial value
        // Shift left by 1 (0xd1 opcode likely)
        shl $1, %rax
        // Result should have bit 63 set (negative). OF should be set because sign changed.
        mov %rax, %rcx // Save result
        pushfq          // Save flags

        // Test 2: SHL by CL (multiple bits)
        mov $1, %rax
        mov $4, %rcx    // cl = 4
        shl %cl, %rax   // Result: 16
        mov %rax, %rdx  // Save result
        pushfq          // Save flags

        // Test 3: SHR by imm8 (ZF check)
        xor %rax, %rax
        mov $10, %rax
        shr $4, %rax    // Result: 0, ZF should be set.
        mov %rax, %rsi  // Save result
        pushfq          // Save flags

        // Test 4: SAR by 1 (CF check)
        // Set up negative number
        mov $-1, %rax
        sar $1, %rax    // Result: -1 (all ones)
        mov %rax, %rdi  // Save result
        pushfq          // Save flags

        // Test 5: SHL by large immediate (OF undefined, but consistent with CPU)
        mov $1, %rax
        shl $2, %rax    // Result: 4
        mov %rax, %r12  // Save result
        pushfq          // Save flags
ENDOF(shift_test)

neg_not_test:
        // Test NOT (no flag changes)
        mov $0x0F, %rax
        not %rax        // Result: 0xFF....F0
        mov %rax, %rbx  // Save result
        pushfq          // Save flags

        // Test NEG (updates flags)
        mov $1, %rax
        neg %rax        // Result: -1 (0xFF...FF). CF set (borrow). SF set.
        mov %rax, %rdx  // Save result
        pushfq          // save flags

        // Test NEG 0 (ZF set, CF clear)
        xor %rax, %rax
        neg %rax
ENDOF(neg_not_test)

r12_index_test:
        // Test SIB with R12 index (requires REX.X)
        // lea (%rbx, %r12, 1), %rax
        mov $10, %rbx
        mov $20, %r12
        lea (%rbx, %r12, 1), %rax
        // Expect RAX = 30. If R12 ignored, RAX = 10.
ENDOF(r12_index_test)

r13_base_test:
        // Test R13 base (requires REX.B)
        // lea 0(%r13), %rax
        mov $40, %r13
        lea 0(%r13), %rax
        // Expect RAX = 40.
ENDOF(r13_base_test)

shift_rex_r_test:
        mov $1, %eax
        // Test SHL with unused REX.R prefix (0x44).
        //
        // We manually encode it.
        // REX.R (0x4) | REX (0x40) = 0x44.
        .byte 0x44
        shl $1, %eax
ENDOF(shift_rex_r_test)

mov_ordering_test:
        // Test mov reg -> mem
        lea test_scratch_space(%rip), %r8
        mov $0x12345678, %rax
        mov %rax, (%r8)

        // Test mov mem -> reg
        mov (%r8), %rbx
ENDOF(mov_ordering_test)

add_ordering_test:
        // Test add reg -> mem
        lea test_scratch_space+16(%rip), %r8
        movq $10, (%r8)
        mov $20, %rax
        add %rax, (%r8) // Mem should be 30

        // Test add mem -> reg
        mov $5, %rbx
        add (%r8), %rbx // rbx should be 35
ENDOF(add_ordering_test)

rex_ignored_test:
        // Test 1: Mod=0, RM=5 (R13) with REX.B
        // Standard says REX.B is ignored => RIP Relative.
        // We will execute `lea (%r13), %rax` encoded manually as Mod=0.
        // If it obeys R13, RAX = R13.
        // If it ignores REX.B, RAX = RIP + Disp32.

        mov $0xDEADBEEF, %r13
        // REX.W(48) + REX.B(01) = 49. LEA(8d). Mod=00 Reg=000(RAX) RM=101. Disp32=0.
        .byte 0x49, 0x8d, 0x05, 0x00, 0x00, 0x00, 0x00
        // Next instruction address is here.
        // RAX should be equal to RIP (address of this instruction).
        // R13 is DEADBEEF.

        // Check if RAX == R13. If so, fail.
        cmp %r13, %rax
        je 1f

        // Success for Test 1

        // Test 2: SIB Base=5 (R13) with Mod=0
        // Standard says REX.B ignored => Disp32 only.
        // `lea 0(,%r12,1), %rax` but with Base=5 and REX.B.
        // REX.W(48) + REX.B(01) + REX.X(02) = 4B. LEA(8d). Mod=00 Reg=000 RM=100(SIB). SIB: Scale=00 Ind=100(R12) Base=101(R13). Disp32.
        // Byte sequence: 4B 8d 04 25 00 00 00 00
        // Calculation: [Base + Index*1 + Disp32].
        // If Base is R13, result = R13 + R12.
        // If REX.B ignored (No Base), result = R12 + Disp32 (which is 0).

        mov $100, %r13
        mov $200, %r12
        .byte 0x4b, 0x8d, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00

        // If correct (No Base), RAX = 200.
        // If incorrect (Base=R13), RAX = 300.
        mov $200, %rcx
        cmp %rcx, %rax
        jne 1f

        mov $1, %rax
        jmp 2f
1:      mov $0, %rax
2:      mov %rax, %rbx
        pushfq
        pop %rcx
ENDOF(rex_ignored_test)

unsupported_rol:
        // Test ROL $1, %eax (opcode 0xd1 /0)
        // This is not supported by our interpreter (only SHL/SHR/SAR are).
        // It should cause TryHandleShift to return false.
        mov $1, %eax
        rol $1, %eax
ENDOF(unsupported_rol)

unsupported_test_const_bl:
        test $0x48, %bl
ENDOF(unsupported_test_const_bl)

unsupported_add_const_bl:
        add $0x48, %bl
ENDOF(unsupported_add_const_bl)
