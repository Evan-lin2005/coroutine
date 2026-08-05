/*
 * Win64 co_context_switch / co_trampoline_entry
 *
 * co_context_switch(from, to): rcx = from, rdx = to
 *
 * Layout (must match co_context_win64.h / Win64.c asserts):
 *   +0   rsp
 *   +8   rbx
 *   +16  rbp
 *   +24  rdi
 *   +32  rsi
 *   +40  r12
 *   +48  r13
 *   +56  r14
 *   +64  r15
 *   +72  mxcsr
 *   +76  x87cw
 *   +78  pad
 *   +80  xmm6..xmm15  (10 * 16)
 */
    .text
    .globl co_context_switch
    .def   co_context_switch; .scl 2; .type 32; .endef
    .align 16

co_context_switch:
    /* ---- save from (rcx) ---- */
    movq  %rsp,  0(%rcx)
    movq  %rbx,  8(%rcx)
    movq  %rbp, 16(%rcx)
    movq  %rdi, 24(%rcx)
    movq  %rsi, 32(%rcx)
    movq  %r12, 40(%rcx)
    movq  %r13, 48(%rcx)
    movq  %r14, 56(%rcx)
    movq  %r15, 64(%rcx)

    stmxcsr 72(%rcx)
    fnstcw  76(%rcx)

    movaps %xmm6,  80(%rcx)
    movaps %xmm7,  96(%rcx)
    movaps %xmm8, 112(%rcx)
    movaps %xmm9, 128(%rcx)
    movaps %xmm10,144(%rcx)
    movaps %xmm11,160(%rcx)
    movaps %xmm12,176(%rcx)
    movaps %xmm13,192(%rcx)
    movaps %xmm14,208(%rcx)
    movaps %xmm15,224(%rcx)

    /* ---- restore to (rdx); rdx stays as to-pointer (not in co_context) ---- */
    movaps 80(%rdx),  %xmm6
    movaps 96(%rdx),  %xmm7
    movaps 112(%rdx), %xmm8
    movaps 128(%rdx), %xmm9
    movaps 144(%rdx), %xmm10
    movaps 160(%rdx), %xmm11
    movaps 176(%rdx), %xmm12
    movaps 192(%rdx), %xmm13
    movaps 208(%rdx), %xmm14
    movaps 224(%rdx), %xmm15

    ldmxcsr 72(%rdx)
    fldcw   76(%rdx)

    movq  8(%rdx),  %rbx
    movq 16(%rdx),  %rbp
    movq 24(%rdx),  %rdi
    movq 32(%rdx),  %rsi
    movq 40(%rdx),  %r12
    movq 48(%rdx),  %r13
    movq 56(%rdx),  %r14
    movq 64(%rdx),  %r15
    movq  0(%rdx),  %rsp

    ret

    .globl co_trampoline_entry
    .def   co_trampoline_entry; .scl 2; .type 32; .endef
    .align 16

co_trampoline_entry:
    xorq  %rbp, %rbp
    /* enter with rsp≡8(mod16); need shadow(32)+align(8) before call */
    subq  $40, %rsp
    call  co_trampoline_body
    ud2
