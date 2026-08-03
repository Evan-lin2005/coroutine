#include "coroutine_internal.h"
#include "co_context_win64.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

/* ------------------------------------------------------------------ *
 * context：欄位順序與 win64.S 的偏移量，用 _Static_assert 綁定
 * ------------------------------------------------------------------ */
_Static_assert(offsetof(struct co_context, rsp)   ==  0, "win64.S 假設 rsp @0");
_Static_assert(offsetof(struct co_context, rbx)   ==  8, "win64.S 假設 rbx @8");
_Static_assert(offsetof(struct co_context, rbp)   == 16, "win64.S 假設 rbp @16");
_Static_assert(offsetof(struct co_context, rdi)   == 24, "win64.S 假設 rdi @24");
_Static_assert(offsetof(struct co_context, rsi)   == 32, "win64.S 假設 rsi @32");
_Static_assert(offsetof(struct co_context, r12)   == 40, "win64.S 假設 r12 @40");
_Static_assert(offsetof(struct co_context, r13)   == 48, "win64.S 假設 r13 @48");
_Static_assert(offsetof(struct co_context, r14)   == 56, "win64.S 假設 r14 @56");
_Static_assert(offsetof(struct co_context, r15)   == 64, "win64.S 假設 r15 @64");
_Static_assert(offsetof(struct co_context, mxcsr) == 72, "win64.S 假設 mxcsr @72");
_Static_assert(offsetof(struct co_context, x87cw) == 76, "win64.S 假設 x87cw @76");
_Static_assert(offsetof(struct co_context, pad)   == 78, "win64.S 假設 pad @78");
_Static_assert(offsetof(struct co_context, xmm)   == 80, "win64.S 假設 xmm @80");
_Static_assert(sizeof(void (*)(void)) == 8, "assumes 64-bit flat function pointers");

static size_t page_size(void)
{
    static size_t ps;
    if (!ps) {
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        ps = (size_t)sysinfo.dwPageSize;
    }
    return ps;
}

int co_platform_initialize(void)
{
    /* 基本切換不需 altstack / VEH；溢位保護之後再加 */
    return 0;
}

int co_stack_create(struct co_stack *s, size_t want)
{
    const size_t ps     = page_size();
    const size_t usable = (want + ps - 1) & ~(ps - 1);
    /* 上下各留一頁邊界 */
    const size_t total  = usable + 2 * ps;
    void *base;
    //預留空間(不可用)
    base = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!base) return -1;

    DWORD old;
    //中間區域設為可讀寫
    if (!VirtualProtect((char *)base + ps, usable, PAGE_READWRITE, &old)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return -1;
    }

    s->base  = base;
    s->lo    = (char *)base + ps;
    s->hi    = (char *)base + ps + usable;
    s->total = total;
    return 0;
}

void co_stack_destroy(struct co_stack *s)
{
    if (!s || !s->base) return;
    VirtualFree(s->base, 0, MEM_RELEASE);
    memset(s, 0, sizeof *s);
}

void initialize_context(struct coroutine *co)
{
    uintptr_t  top = (uintptr_t)co->stack.hi;
    uintptr_t *frame;

    top &= ~(uintptr_t)0xF;
    frame = (uintptr_t *)top;

    /*
     *  高位址
     *  +------------------------+ top
     *  | co_bad_return          |  top-8
     *  +------------------------+
     *  | co_trampoline_entry    |  top-16  ← rsp；ret 後進 entry 時 rsp≡8(mod16)
     *  +------------------------+
     *  低位址
     *  shadow 32B 由 win64.S trampoline 的 sub $40 處理
     */
    *--frame = (uintptr_t)&co_bad_return;
    *--frame = (uintptr_t)&co_trampoline_entry;

    co->context.rsp   = frame;
    co->context.mxcsr = 0x1F80;
    co->context.x87cw = 0x037F;
}
