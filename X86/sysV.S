    .text
    .globl co_context_switch
    .type  co_context_switch, @function
    .align 16

co_context_switch:
    .cfi_startproc

    movq %rsp,  0(%rdi)
    movq %rbx,  8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    /* SysV 規定 MXCSR 控制位元與 x87 control word 是 callee-saved */
    stmxcsr 56(%rdi)
    fnstcw  60(%rdi)

    movq  0(%rsi), %rsp
    movq  8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15

    ldmxcsr 56(%rsi)
    fldcw   60(%rsi)

    ret
    .cfi_endproc
    .size co_context_switch, .-co_context_switch

/* trampoline 的進入點：告訴 unwinder 這裡是堆疊底部，停止回溯 */
    .globl co_trampoline_entry
    .type  co_trampoline_entry, @function
    .align 16
co_trampoline_entry:
    .cfi_startproc
    .cfi_undefined rip
    xorq %rbp, %rbp           /* 標記堆疊底部，協助 unwinder 停止 */
    subq $8, %rsp             /* 進入時 rsp≡8(mod16)，call 前必須 ≡0 */
    call co_trampoline_body
    ud2                       /* body 是 noreturn，走到這裡代表出事了 */
    .cfi_endproc
    .size co_trampoline_entry, .-co_trampoline_entry

    .section .note.GNU-stack,"",@progbits