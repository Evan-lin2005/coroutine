#include "coroutine_internal.h"
#include "co_context_win64.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
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

#if defined(__SANITIZE_ADDRESS__)
#  define CO_ASAN_BUILD 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CO_ASAN_BUILD 1
#  endif
#endif

#ifndef CO_ASAN_BUILD
#  define CO_ASAN_BUILD 0
#endif

#ifndef CO_MAX_TRACKED_STACKS
#  define CO_MAX_TRACKED_STACKS 4096 //堆疊大小
#endif

struct guard_entry {
    _Atomic(void *) base;
    _Atomic size_t  total;
};
static struct guard_entry g_guards[CO_MAX_TRACKED_STACKS];
static atomic_int g_guard_full_warned;

static void guard_register(const struct co_stack *s)
{
    for (size_t i = 0; i < CO_MAX_TRACKED_STACKS; i++) {
        void *expect = NULL;
        //比較內存值以決定是否寫入新值
        if (atomic_compare_exchange_strong(&g_guards[i].base, &expect, s->base)) {
            //以不可分割的原子操作將值寫入共用變數
            atomic_store(&g_guards[i].total, s->total);
            return;
        }
    }
    /* 表滿：不置換既有登記；新堆疊無法診斷溢位 */
    if (atomic_exchange(&g_guard_full_warned, 1) == 0) {
        static const char msg[] =
            "*** coroutine guard tracking table full; "
            "new stacks may lack overflow diagnosis\n";
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE) {
            DWORD n;
            WriteFile(err, msg, (DWORD)(sizeof msg - 1), &n, NULL);
        }
    }
}

//註銷一個協程堆疊。
static void guard_unregister(const struct co_stack *s)
{
    for (size_t i = 0; i < CO_MAX_TRACKED_STACKS; i++)
        //找出指定堆疊地址
        if (atomic_load(&g_guards[i].base) == s->base) {
            atomic_store(&g_guards[i].base, NULL);
            return;
        }
}

static size_t page_size(void)
{
    static _Atomic size_t ps;
    size_t v = atomic_load_explicit(&ps, memory_order_relaxed);
    if (!v) {
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        v = (size_t)sysinfo.dwPageSize;
        atomic_store_explicit(&ps, v, memory_order_relaxed);
    }
    return v;
}

#if CO_ASAN_BUILD
int co_platform_install_crash_handler(void) { return 0; }
int co_platform_initialize(void) { return 0; }
#else
//檢查記憶體地址，是否落在任何目前正受到保護的協程堆疊範圍內
static int addr_in_guard(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    for (size_t i = 0; i < CO_MAX_TRACKED_STACKS; i++) {
        void *b = atomic_load(&g_guards[i].base);
        if (!b) continue;
        uintptr_t lo = (uintptr_t)b;
        size_t    n  = atomic_load(&g_guards[i].total);
        if (a >= lo && a < lo + n) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * VEH crash handler（opt-in；見 co_install_crash_handler）
 * 無 sigaltstack；CONTINUE_SEARCH 已鏈到後續 handler
 * ------------------------------------------------------------------ */
static volatile LONG g_veh_once;
static volatile LONG g_crash_handler_wanted;

static LONG WINAPI co_veh(EXCEPTION_POINTERS *info){
    EXCEPTION_RECORD *er = info->ExceptionRecord;

    if(er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION){
        return EXCEPTION_CONTINUE_SEARCH;
    }

    //故障位址
    void* addr = (void *)er->ExceptionInformation[1];
    if(!addr_in_guard(addr)){
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static const char msg[] =
        "*** coroutine stack overflow (guard page hit)\n";
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        DWORD n;
        WriteFile(err, msg, (DWORD)(sizeof msg - 1), &n, NULL);
    }
    ExitProcess(139);          /* 結束；不要 CONTINUE_EXECUTION */
    return EXCEPTION_CONTINUE_SEARCH; /* 不可達 */
}

static void co_platform_enable_crash_handler_local(void)
{
    InterlockedExchange(&g_crash_handler_wanted, 1);
    if (InterlockedCompareExchange(&g_veh_once, 1, 0) == 0)
        AddVectoredExceptionHandler(1, co_veh);
}

int co_platform_install_crash_handler(void)
{
    co_platform_enable_crash_handler_local();
    return 0;
}

int co_platform_initialize(void)
{
#if defined(CO_INSTALL_SIGSEGV_HANDLER)
    co_platform_enable_crash_handler_local();
#else
    /* 若他執行緒已 opt-in，本執行緒／init 補上 VEH 註冊 */
    if (InterlockedCompareExchange(&g_crash_handler_wanted, 0, 0) != 0)
        co_platform_enable_crash_handler_local();
#endif
    return 0;
}
#endif

int co_platform_query_thread_stack(const void **bottom, size_t *size)
{
    ULONG_PTR low = 0, high = 0;
    if (!bottom || !size) return -1;
    GetCurrentThreadStackLimits(&low, &high);
    if (high <= low) return -1;
    *bottom = (const void *)(uintptr_t)low;
    *size = (size_t)(high - low);
    return 0;
}

int co_stack_create(struct co_stack *s, size_t want)
{
    const size_t ps = page_size();
    size_t usable, total;
    void *base;
    DWORD old;

    if (want == 0 || want > SIZE_MAX - (ps - 1))
        return -1;
    usable = (want + ps - 1) & ~(ps - 1);
    if (usable < CO_MIN_STACK_SIZE || usable > SIZE_MAX - 2 * ps)
        return -1;
    total = usable + 2 * ps;

    /* 上下各留一頁邊界 */
    base = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    if (!base) return -1;

    if (!VirtualProtect((char *)base + ps, usable, PAGE_READWRITE, &old)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return -1;
    }

    s->base     = base;
    s->lo       = (char *)base + ps;
    s->hi       = (char *)base + ps + usable;
    s->total    = total;
    s->external = 0;

    guard_register(s);

    return 0;
}

int co_stack_create_from(struct co_stack *s, void *base, size_t total)
{
    if (!s || !base || total < CO_MIN_STACK_SIZE)
        return -1;

    s->base     = base;
    s->lo       = base;
    s->hi       = (char *)base + total;
    s->total    = total;
    s->external = 1;
    return 0;
}

void co_stack_destroy(struct co_stack *s)
{
    if (!s || !s->base)
        return;
    if (s->external) {
        memset(s, 0, sizeof *s);
        return;
    }
    guard_unregister(s);
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
