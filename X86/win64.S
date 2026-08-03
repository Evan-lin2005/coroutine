/*
 * Win64 co_context_switch / co_trampoline_entry
 *
 * co_context_switch(from, to): rcx = from, rdx = to
 *
 * Layout (match co_context_win64.h / Window64.c asserts):
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


    /* ---- restore to (r8) ---- */
    movaps 80(%r8),  %xmm6
    movaps 96(%r8),  %xmm7
    movaps 112(%r8), %xmm8
    movaps 128(%r8), %xmm9
    movaps 144(%r8), %xmm10
    movaps 160(%r8), %xmm11
    movaps 176(%r8), %xmm12
    movaps 192(%r8), %xmm13
    movaps 208(%r8), %xmm14
    movaps 224(%r8), %xmm15

    ldmxcsr 72(%r8)
    fldcw   76(%r8)

    movq  8(%r8),  %rbx
    movq 16(%r8),  %rbp
    movq 24(%r8),  %rdi
    movq 32(%r8),  %rsi
    movq 40(%r8),  %r12
    movq 48(%r8),  %r13
    movq 56(%r8),  %r14
    movq 64(%r8),  %r15
    movq  0(%r8),  %rsp

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
