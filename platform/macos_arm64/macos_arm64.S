/*
 * macOS arm64 co_context_switch / co_trampoline_entry  (Mach-O)
 *
 * ABI: Apple arm64 (AAPCS64 相容)
 *   - x0 = from, x1 = to at entry
 *   - callee-saved GPRs: x19-x28, x29 (fp)
 *   - callee-saved SIMD/FP: v8-v15 bottom 64 bits (d8-d15)
 *   - SP must be 16-byte aligned at public interfaces
 *   - return via x30 (lr); bl does not push a stack return address
 *
 * Mach-O: C 可見符號需加前導 '_'（_co_context_switch 等）。
 *
 * Layout (must match co_context_macos.h / macos_aarch64.c _Static_assert):
 *   +0    sp
 *   +8    x19
 *   +16   x20
 *   +24   x21
 *   +32   x22
 *   +40   x23
 *   +48   x24
 *   +56   x25
 *   +64   x26
 *   +72   x27
 *   +80   x28
 *   +88   fp  (x29)
 *   +96   lr  (x30)
 *   +104  d8..d15  (8 * 8 bytes)
 */
    .text
    .align 4
    .globl _co_context_switch

_co_context_switch:
    .cfi_startproc

    /* ---- save current context to from (x0) ---- */
    mov  x9, sp
    str  x9, [x0]

    stp  x19, x20, [x0, #8]
    stp  x21, x22, [x0, #24]
    stp  x23, x24, [x0, #40]
    stp  x25, x26, [x0, #56]
    stp  x27, x28, [x0, #72]
    stp  x29, x30, [x0, #88]

    stp  d8,  d9,  [x0, #104]
    stp  d10, d11, [x0, #120]
    stp  d12, d13, [x0, #136]
    stp  d14, d15, [x0, #152]

    /* ---- restore target context from to (x1) ---- */
    ldp  d8,  d9,  [x1, #104]
    ldp  d10, d11, [x1, #120]
    ldp  d12, d13, [x1, #136]
    ldp  d14, d15, [x1, #152]

    ldp  x19, x20, [x1, #8]
    ldp  x21, x22, [x1, #24]
    ldp  x23, x24, [x1, #40]
    ldp  x25, x26, [x1, #56]
    ldp  x27, x28, [x1, #72]
    ldp  x29, x30, [x1, #88]

    ldr  x9, [x1]
    mov  sp, x9

    ret
    .cfi_endproc

/*
 * First entry into a fresh coroutine stack.
 * initialize_context() sets sp (16-byte aligned) and lr = co_trampoline_entry.
 * co_context_switch() restores those values and ret lands here.
 */
    .align 4
    .globl _co_trampoline_entry

_co_trampoline_entry:
    .cfi_startproc
    .cfi_undefined x30
    mov  x29, xzr                 /* mark stack bottom for unwinder */
    bl   _co_trampoline_body
    brk  #0                       /* body is noreturn */
    .cfi_endproc
