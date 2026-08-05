#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#include "coroutine_internal.h"
#include "co_context_sys.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

/* C23/C11 版本差異 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define CO_THREAD_LOCAL thread_local
#else
#  define CO_THREAD_LOCAL _Thread_local
#endif



/* ------------------------------------------------------------------ *
 * context：欄位順序與 sysv.S 的偏移量，用 _Static_assert 綁定
 * ------------------------------------------------------------------ */

//確認順序
_Static_assert(offsetof(struct co_context, rsp)   ==  0, "sysv.S 假設 rsp @0");
_Static_assert(offsetof(struct co_context, rbx)   ==  8, "sysv.S 假設 rbx @8");
_Static_assert(offsetof(struct co_context, rbp)   == 16, "sysv.S 假設 rbp @16");
_Static_assert(offsetof(struct co_context, r12)   == 24, "sysv.S 假設 r12 @24");
_Static_assert(offsetof(struct co_context, r13)   == 32, "sysv.S 假設 r13 @32");
_Static_assert(offsetof(struct co_context, r14)   == 40, "sysv.S 假設 r14 @40");
_Static_assert(offsetof(struct co_context, r15)   == 48, "sysv.S 假設 r15 @48");
_Static_assert(offsetof(struct co_context, mxcsr) == 56, "sysv.S 假設 mxcsr @56");
_Static_assert(offsetof(struct co_context, x87cw) == 60, "sysv.S 假設 x87cw @60");

/* 這份程式碼假設 SysV AMD64：平坦位址、函式指標即進入點、ret 從堆疊取位址 */
_Static_assert(sizeof(void (*)(void)) == 8, "assumes 64-bit flat function pointers");

static CO_THREAD_LOCAL int altstack_ready;

/* ------------------------------------------------------------------ *
 * guard page 註冊表：SIGSEGV handler 用它判斷是否為協程堆疊溢位。
 * 全域（handler 可能在任一執行緒觸發）且 async-signal-safe。
 * ------------------------------------------------------------------ */
#ifndef CO_MAX_TRACKED_STACKS
#  define CO_MAX_TRACKED_STACKS 4096 //堆疊大小
#endif

struct guard_entry {
    _Atomic(void *) base;
    _Atomic size_t  total;
};
static struct guard_entry g_guards[CO_MAX_TRACKED_STACKS];

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
    /* 表滿：溢位仍會被 guard page 擋下，只是訊息會退化成一般 SIGSEGV */
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

static size_t page_size(void)
{
    static size_t ps;
    //算出記憶體分頁大小
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
}

/* ------------------------------------------------------------------ *
 * SIGSEGV handler + sigaltstack
 * 沒有 altstack，堆疊爆掉時 handler 無處可跑 → double fault → 靜默消失
 * ------------------------------------------------------------------ */
static void co_segv_handler(int sig, siginfo_t *si, void *uctx)
{
    static const char msg_ovf[] = "*** coroutine stack overflow (guard page hit)\n";
    static const char msg_seg[] = "*** SIGSEGV outside any coroutine guard page\n";
    (void)uctx;

    /* 只能用 async-signal-safe 函式：不可 printf、不可 malloc */
    //取得記憶體錯誤位置，確認是否是在保護區部分
    if (addr_in_guard(si->si_addr))
        //溢位
        (void)!write(STDERR_FILENO, msg_ovf, sizeof msg_ovf - 1);
    else
        //一般錯誤
        (void)!write(STDERR_FILENO, msg_seg, sizeof msg_seg - 1);

    _exit(128 + sig);
}

//利用備用堆疊承接溢位堆疊，使co_segv_handler 能成功地執行完畢
static void install_altstack(void)
{
    static CO_THREAD_LOCAL char alt[64 * 1024];
    stack_t ss;
    struct sigaction sa;

    //避免重複設定
    if (altstack_ready) return;

    //配置備用堆疊
    ss.ss_sp    = alt;
    ss.ss_flags = 0;
    ss.ss_size  = sizeof alt;
    sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = co_segv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    altstack_ready = 1;
}

int co_platform_initialize(void)
{
    install_altstack();
    return 0;
}

/* ------------------------------------------------------------------ *
 * 堆疊配置：上下各一頁 PROT_NONE
 *   低位址 [ guard ][ usable stack ][ guard ] 高位址
 * 下方擋溢位；上方擋 setup 程式碼自己算錯偏移量
 * ------------------------------------------------------------------ */
int co_stack_create(struct co_stack *s, size_t want)
{
    const size_t ps     = page_size();
    const size_t usable = (want + ps - 1) & ~(ps - 1);
    const size_t total  = usable + 2 * ps;
    void *base;

    base = mmap(NULL, total, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return -1;

    if (mprotect((char *)base + ps, usable, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, total);
        return -1;
    }

    s->base  = base;
    s->lo    = (char *)base + ps;
    s->hi    = (char *)base + ps + usable;
    s->total = total;

#ifdef CO_DEBUG_STACK_USAGE
    memset(s->lo, 0xCD, usable);   /* 塗色以便事後掃描高水位 */
#endif
    guard_register(s);
    return 0;
}

void co_stack_destroy(struct co_stack *s)
{
    if (!s->base) return;
    guard_unregister(s);
    munmap(s->base, s->total);
    memset(s, 0, sizeof *s);
}

void initialize_context(struct coroutine *co)
{
    uintptr_t  top = (uintptr_t)co->stack.hi;
    uintptr_t *frame;

    top &= ~(uintptr_t)0xF;
    frame = (uintptr_t *)top;

    /*  高位址
     *  +------------------------+ top
     *  | co_bad_return          |  top-8   ← trampoline 的「回傳位址」
     *  +------------------------+
     *  | co_trampoline_entry    |  top-16  ← ret 會彈出這個
     *  +------------------------+ rsp
     *  低位址
     *
     *  top 為 16 對齊 → rsp = top-16 也是 16 對齊；
     *  co_context_switch 的 ret 彈掉 8 bytes 後，
     *  進入 trampoline 時 rsp ≡ 8 (mod 16)，符合 SysV 規定的函式進入。
     */
    *--frame = (uintptr_t)&co_bad_return;
    *--frame = (uintptr_t)&co_trampoline_entry;

    co->context.rsp   = frame;
    co->context.mxcsr = 0x1F80;   /* 全例外遮罩 & round-to-nearest */
    co->context.x87cw = 0x037F;
}
