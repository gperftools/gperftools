
        .text

#if AW_HIDDEN_VISIBILITY
#define HIDE(s) .hidden s
#else
#define HIDE(s)
#endif

#define DEF_FN(name) .globl name; HIDE(name)

        DEF_FN(mini_x86_int_perform_wrapper_64)
        DEF_FN(mini_x86_int_perform_wrapper_32)
        DEF_FN(mini_x86_int_perform_sub_32)
        DEF_FN(mini_x86_int_perform_sub_64)
        DEF_FN(mini_x86_int_perform_add_32)
        DEF_FN(mini_x86_int_perform_add_64)
        DEF_FN(mini_x86_int_perform_cmp_32)
        DEF_FN(mini_x86_int_perform_cmp_64)
        DEF_FN(mini_x86_int_perform_and_32)
        DEF_FN(mini_x86_int_perform_and_64)
        DEF_FN(mini_x86_int_perform_or_32)
        DEF_FN(mini_x86_int_perform_or_64)
        DEF_FN(mini_x86_int_perform_xor_32)
        DEF_FN(mini_x86_int_perform_xor_64)
        DEF_FN(mini_x86_int_perform_test_32)
        DEF_FN(mini_x86_int_perform_test_64)
        DEF_FN(mini_x86_int_perform_shl_32)
        DEF_FN(mini_x86_int_perform_shl_64)
        DEF_FN(mini_x86_int_perform_shr_32)
        DEF_FN(mini_x86_int_perform_shr_64)
        DEF_FN(mini_x86_int_perform_sar_32)
        DEF_FN(mini_x86_int_perform_sar_64)
        DEF_FN(mini_x86_int_perform_neg_32)
        DEF_FN(mini_x86_int_perform_neg_64)
        DEF_FN(mini_x86_int_perform_not_32)
        DEF_FN(mini_x86_int_perform_not_64)

#define USE_FLAGS ((1<<0) | (1<<2) | (1<<4) | (1<<6) | (1<<7) | (1<<11))
//  extern uint64_t mini_x86_int_perform_wrapper_64(uint64_t a, uint64_t b, reg_value_type* flags_loc, void (*fn)(void));
mini_x86_int_perform_wrapper_32:
mini_x86_int_perform_wrapper_64:
        .cfi_startproc
        pushfq
        .cfi_def_cfa_offset 16
        popq %r8
        .cfi_def_cfa_offset 8
        andq $(~USE_FLAGS), %r8

        movq (%rdx), %rax
        andq $(USE_FLAGS), %rax
        orq %r8, %rax

        pushq %rax
        .cfi_def_cfa_offset 16
        popfq
        .cfi_def_cfa_offset 8

        call *%rcx

        pushfq
        .cfi_def_cfa_offset 16
        popq %rax
        .cfi_def_cfa_offset 8
        mov (%rdx), %r8
        andq $(~USE_FLAGS), %r8

        and $(USE_FLAGS), %rax
        or %r8, %rax
        mov %rax, (%rdx)

        mov %rdi, %rax
        ret

        .cfi_endproc
        .size mini_x86_int_perform_wrapper_32, . - mini_x86_int_perform_wrapper_32
        .size mini_x86_int_perform_wrapper_64, . - mini_x86_int_perform_wrapper_64

mini_x86_int_perform_sub_32:
        .cfi_startproc
        sub %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_sub_32, . - mini_x86_int_perform_sub_32

mini_x86_int_perform_sub_64:
        .cfi_startproc
        sub %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_sub_64, . - mini_x86_int_perform_sub_64

mini_x86_int_perform_add_32:
        .cfi_startproc
        add %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_add_32, . - mini_x86_int_perform_add_32

mini_x86_int_perform_add_64:
        .cfi_startproc
        add %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_add_64, . - mini_x86_int_perform_add_64

mini_x86_int_perform_cmp_32:
        .cfi_startproc
        cmp %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_cmp_32, . - mini_x86_int_perform_cmp_32

mini_x86_int_perform_cmp_64:
        .cfi_startproc
        cmp %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_cmp_64, . - mini_x86_int_perform_cmp_64

mini_x86_int_perform_and_32:
        .cfi_startproc
        and %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_and_32, . - mini_x86_int_perform_and_32

mini_x86_int_perform_and_64:
        .cfi_startproc
        and %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_and_64, . - mini_x86_int_perform_and_64

mini_x86_int_perform_or_32:
        .cfi_startproc
        or %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_or_32, . - mini_x86_int_perform_or_32

mini_x86_int_perform_or_64:
        .cfi_startproc
        or %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_or_64, . - mini_x86_int_perform_or_64

mini_x86_int_perform_xor_32:
        .cfi_startproc
        xor %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_xor_32, . - mini_x86_int_perform_xor_32

mini_x86_int_perform_xor_64:
        .cfi_startproc
        xor %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_xor_64, . - mini_x86_int_perform_xor_64

mini_x86_int_perform_test_32:
        .cfi_startproc
        test %esi, %edi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_test_32, . - mini_x86_int_perform_test_32

mini_x86_int_perform_test_64:
        .cfi_startproc
        test %rsi, %rdi
        ret
        .cfi_endproc
        .size mini_x86_int_perform_test_64, . - mini_x86_int_perform_test_64

mini_x86_int_perform_shl_32:
        .cfi_startproc
        mov %rsi, %rcx  // shift amount is in second arg, move to cl
        shl %cl, %edi
        mov %edi, %eax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_shl_32, . - mini_x86_int_perform_shl_32

mini_x86_int_perform_shl_64:
        .cfi_startproc
        mov %rsi, %rcx
        shl %cl, %rdi
        mov %rdi, %rax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_shl_64, . - mini_x86_int_perform_shl_64

mini_x86_int_perform_shr_32:
        .cfi_startproc
        mov %rsi, %rcx
        shr %cl, %edi
        mov %edi, %eax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_shr_32, . - mini_x86_int_perform_shr_32

mini_x86_int_perform_shr_64:
        .cfi_startproc
        mov %rsi, %rcx
        shr %cl, %rdi
        mov %rdi, %rax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_shr_64, . - mini_x86_int_perform_shr_64

mini_x86_int_perform_sar_32:
        .cfi_startproc
        mov %rsi, %rcx
        sar %cl, %edi
        mov %edi, %eax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_sar_32, . - mini_x86_int_perform_sar_32

mini_x86_int_perform_sar_64:
        .cfi_startproc
        mov %rsi, %rcx
        sar %cl, %rdi
        mov %rdi, %rax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_sar_64, . - mini_x86_int_perform_sar_64

mini_x86_int_perform_neg_32:
        .cfi_startproc
        neg %edi
        mov %edi, %eax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_neg_32, . - mini_x86_int_perform_neg_32

mini_x86_int_perform_neg_64:
        .cfi_startproc
        neg %rdi
        mov %rdi, %rax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_neg_64, . - mini_x86_int_perform_neg_64

mini_x86_int_perform_not_32:
        .cfi_startproc
        not %edi
        mov %edi, %eax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_not_32, . - mini_x86_int_perform_not_32

mini_x86_int_perform_not_64:
        .cfi_startproc
        not %rdi
        mov %rdi, %rax
        ret
        .cfi_endproc
        .size mini_x86_int_perform_not_64, . - mini_x86_int_perform_not_64
