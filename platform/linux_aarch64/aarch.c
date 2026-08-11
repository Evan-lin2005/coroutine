#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "co_context_aarch64.h"
#include "coroutine_internal.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

/* C23/C11 版本差異 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define CO_THREAD_LOCAL thread_local
#else
#  define CO_THREAD_LOCAL _Thread_local
#endif

/* ------------------------------------------------------------------ *
 * context：欄位順序與 aarch.S 的偏移量，用 _Static_assert 綁定
 * ------------------------------------------------------------------ */

//確認順序
_Static_assert(offsetof(struct co_context, sp)   ==   0, "aarch.S 假設 sp @0");
_Static_assert(offsetof(struct co_context, x19)   ==   8, "aarch.S 假設 x19 @8");
_Static_assert(offsetof(struct co_context, x20)   ==  16, "aarch.S 假設 x20 @16");
_Static_assert(offsetof(struct co_context, x21)   ==  24, "aarch.S 假設 x21 @24");
_Static_assert(offsetof(struct co_context, x22)   ==  32, "aarch.S 假設 x22 @32");
_Static_assert(offsetof(struct co_context, x23)   ==  40, "aarch.S 假設 x23 @40");
_Static_assert(offsetof(struct co_context, x24)   ==  48, "aarch.S 假設 x24 @48");
_Static_assert(offsetof(struct co_context, x25)   ==  56, "aarch.S 假設 x25 @56");
_Static_assert(offsetof(struct co_context, x26)   ==  64, "aarch.S 假設 x26 @64");
_Static_assert(offsetof(struct co_context, x27)   ==  72, "aarch.S 假設 x27 @72");
_Static_assert(offsetof(struct co_context, x28)   ==  80, "aarch.S 假設 x28 @80");
_Static_assert(offsetof(struct co_context, fp)   ==  88, "aarch.S 假設 fp @88");
_Static_assert(offsetof(struct co_context, lr)   ==  96, "aarch.S 假設 lr @96");
_Static_assert(offsetof(struct co_context, d)   == 104, "aarch.S 假設 d @104");

/* 這份程式碼假設 AArch64：平坦位址、lr 為返回目標、bl 不壓 stack return address */
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
    /* 表滿：以 hash slot 強制登記，讓 guard page 溢位仍能辨識 */
    {
        size_t i = ((uintptr_t)s->base >> 12) % CO_MAX_TRACKED_STACKS;
        atomic_store(&g_guards[i].base, s->base);
        atomic_store(&g_guards[i].total, s->total);
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
        v = (size_t)sysconf(_SC_PAGESIZE);
        atomic_store_explicit(&ps, v, memory_order_relaxed);
    }
    return v;
}

#if CO_ASAN_BUILD
int co_platform_install_crash_handler(void) { return 0; }
int co_platform_initialize(void) { return 0; }
#else
/* ------------------------------------------------------------------ *
 * SIGSEGV handler + sigaltstack（opt-in；見 co_install_crash_handler）
 * ------------------------------------------------------------------ */
static CO_THREAD_LOCAL int altstack_ready;

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

static struct sigaction g_prev_segv;
static struct sigaction g_prev_bus;
static pthread_once_t   g_handler_once = PTHREAD_ONCE_INIT;
static atomic_int       g_crash_handler_wanted;

static void co_chain_prev(int sig, siginfo_t *si, void *uctx,
                          const struct sigaction *prev)
{
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction)
            prev->sa_sigaction(sig, si, uctx);
        return;
    }
    if (prev->sa_handler == SIG_IGN)
        return;
    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_ERR) {
        prev->sa_handler(sig);
        return;
    }
    {
        static const char msg_seg[] =
            "*** SIGSEGV outside any coroutine guard page\n";
        (void)!write(STDERR_FILENO, msg_seg, sizeof msg_seg - 1);
    }
    _exit(128 + sig);
}

static void co_segv_handler(int sig, siginfo_t *si, void *uctx)
{
    static const char msg_ovf[] =
        "*** coroutine stack overflow (guard page hit)\n";
    (void)uctx;

    if (addr_in_guard(si->si_addr)) {
        (void)!write(STDERR_FILENO, msg_ovf, sizeof msg_ovf - 1);
        _exit(128 + sig);
    }

    if (sig == SIGBUS)
        co_chain_prev(sig, si, uctx, &g_prev_bus);
    else
        co_chain_prev(sig, si, uctx, &g_prev_segv);
}

static void install_altstack_for_thread(void)
{
    static CO_THREAD_LOCAL char alt[64 * 1024];
    stack_t old, ss;

    if (altstack_ready)
        return;
    altstack_ready = 1;

    if (sigaltstack(NULL, &old) == 0 && old.ss_sp &&
        !(old.ss_flags & SS_DISABLE))
        return; /* 宿主已有 altstack，不動它 */

    ss.ss_sp    = alt;
    ss.ss_flags = 0;
    ss.ss_size  = sizeof alt;
    (void)sigaltstack(&ss, NULL);
}

static void install_handlers_process(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = co_segv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS,  &sa, &g_prev_bus);
}

static void co_platform_enable_crash_handler_local(void)
{
    atomic_store(&g_crash_handler_wanted, 1);
    pthread_once(&g_handler_once, install_handlers_process);
    install_altstack_for_thread();
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
    /* 若他執行緒已 opt-in，本執行緒補上 altstack（handler 為 process 級） */
    if (atomic_load(&g_crash_handler_wanted))
        co_platform_enable_crash_handler_local();
#endif
    return 0;
}
#endif

int co_platform_query_thread_stack(const void **bottom, size_t *size)
{
    pthread_attr_t attr;
    void *base = NULL;
    size_t sz = 0;
    if (!bottom || !size) return -1;
    *bottom = NULL; *size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return -1;
    if (pthread_attr_getstack(&attr, &base, &sz) != 0) {
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);
    *bottom = base; *size = sz;
    return 0;
}

/* ------------------------------------------------------------------ *
 * 堆疊配置：上下各一頁 PROT_NONE
 *   低位址 [ guard ][ usable stack ][ guard ] 高位址
 * ------------------------------------------------------------------ */
int co_stack_create(struct co_stack *s, size_t want)
{
    const size_t ps = page_size();
    size_t usable, total;
    void *base;

    if (want == 0 || want > SIZE_MAX - (ps - 1))
        return -1;
    usable = (want + ps - 1) & ~(ps - 1);
    if (usable < CO_MIN_STACK_SIZE || usable > SIZE_MAX - 2 * ps)
        return -1;
    total = usable + 2 * ps;

    base = mmap(NULL, total, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return -1;

    if (mprotect((char *)base + ps, usable, PROT_READ | PROT_WRITE) != 0) {
        munmap(base, total);
        return -1;
    }

    s->base     = base;
    s->lo       = (char *)base + ps;
    s->hi       = (char *)base + ps + usable;
    s->total    = total;
    s->external = 0;

#ifdef CO_DEBUG_STACK_USAGE
    memset(s->lo, 0xCD, usable);
#endif
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
    munmap(s->base, s->total);
    memset(s, 0, sizeof *s);
}

void initialize_context(struct coroutine *co)
{
    uintptr_t top = (uintptr_t)co->stack.hi;

    memset(&co->context, 0, sizeof co->context);

    /*
     * AAPCS64：SP  16-byte 對齊。
     * 首次切入時 co_context_switch 會 restore sp/lr 並 ret；
     * 不需要在 stack 上放 return address。
     */
    top &= ~(uintptr_t)0xF;

    co->context.sp = (void *)top;
    co->context.lr = (uint64_t)(uintptr_t)&co_trampoline_entry;
}
