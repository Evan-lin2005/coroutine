#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#include "coroutine_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#endif

/* 考慮不同compiler的差異 */
#if defined(_MSC_VER) && !defined(__clang__)
#  define CO_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define CO_THREAD_LOCAL thread_local
#else
#  define CO_THREAD_LOCAL _Thread_local
#endif

/*
 * ASan fiber 切換註解
 * ----------------------------------------------------------------
 * 依 LLVM sanitizer 契約：start 在切換前呼叫（fake_stack 存於 from 協程），
 * finish 在目標 stack 開始執行時（trampoline）或切換返回 from 時呼叫。
 * - fake_stack_save 傳 NULL（目前不啟用 use-after-return fake stack 追蹤）。
 */
#if defined(__SANITIZE_ADDRESS__)
extern void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                           const void *bottom,
                                           size_t size);
extern void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                            const void **bottom_old,
                                            size_t *size_old);

static size_t co_asan_stack_bytes(const struct co_stack *st)
{
    if (!st || !st->lo || !st->hi || st->hi <= st->lo)
        return 0;
    return (size_t)(st->hi - st->lo);
}

static void co_asan_start_switch(struct coroutine *from_co,
                                 const struct coroutine *to_co)
{
    const void *bottom = NULL;
    size_t      size   = 0;
    (void)from_co;

    if (to_co && to_co->stack.lo)
        size = co_asan_stack_bytes(&to_co->stack);
    if (size)
        bottom = to_co->stack.lo;

    /* fake_stack_save=NULL：目前僅需 stack 邊界註解，不啟用 use-after-return fake stack */
    __sanitizer_start_switch_fiber(NULL, bottom, size);
}

static void co_asan_finish_switch(void)
{
    const void *bottom_old = NULL;
    size_t      size_old   = 0;
    __sanitizer_finish_switch_fiber(NULL, &bottom_old, &size_old);
}

static void co_do_switch(struct co_context *from, struct co_context *to,
                         struct coroutine *from_co, struct coroutine *to_co)
{
    (void)from_co;
    co_asan_start_switch(from_co, to_co);
    co_context_switch(from, to);
    co_asan_finish_switch();
}

static void co_asan_finish_on_enter(struct coroutine *from_co)
{
    (void)from_co;
    co_asan_finish_switch();
}
#else
static void co_do_switch(struct co_context *from, struct co_context *to,
                         struct coroutine *from_co, struct coroutine *to_co)
{
    (void)from_co;
    (void)to_co;
    co_context_switch(from, to);
}

static void co_asan_finish_on_enter(struct coroutine *from_co)
{
    (void)from_co;
}
#endif

/* ------------------------------------------------------------------ *
 * thread-local 狀態 / owner 身分（單調序號，不回收）
 * ------------------------------------------------------------------ */
#if defined(_WIN32) && (defined(_MSC_VER) && !defined(__clang__))
static volatile LONG64 g_thread_serial;
#else
static unsigned long long g_thread_serial; /* accessed via __atomic_* */
#endif

static CO_THREAD_LOCAL uint64_t          thread_id; /* 0 = 尚未配置 */
static CO_THREAD_LOCAL struct coroutine  main_coroutine;
static CO_THREAD_LOCAL struct coroutine *current_coroutine;
static CO_THREAD_LOCAL struct coroutine *g_live_head;

static uint64_t co_self_thread_id(void)
{
    if (thread_id == 0) {
#if defined(_WIN32) && (defined(_MSC_VER) && !defined(__clang__))
        thread_id = (uint64_t)InterlockedIncrement64(&g_thread_serial);
#else
        thread_id = (uint64_t)__atomic_fetch_add(&g_thread_serial, 1ull,
                                                 __ATOMIC_RELAXED) + 1ull;
#endif
    }
    return thread_id;
}

static void ensure_initialized(void);
static void co_live_unlink(struct coroutine *co);
static void co_internal_reclaim_orphan(struct coroutine *co);

/* ------------------------------------------------------------------ *
 * thread-exit 安全網：owner 結束時 reclaim 庫資源（非公開 kill 語意）
 * ------------------------------------------------------------------ */
static void co_orphan_reclaim_all(void)
{
    size_t n = 0;

    while (g_live_head) {
        co_internal_reclaim_orphan(g_live_head);
        n++;
    }
    if (n > 0) {
        fprintf(stderr,
                "coroutine orphan: owner thread exited with %zu suspended "
                "coroutine(s); library resources reclaimed\n",
                n);
    }
}

#if defined(_WIN32)
static DWORD g_orphan_fls = FLS_OUT_OF_INDEXES;
static LONG  g_orphan_fls_once;

static void NTAPI co_orphan_fls_dtor(void *p)
{
    (void)p;
    co_orphan_reclaim_all();
}

static void co_orphan_arm(void)
{
    if (InterlockedCompareExchange(&g_orphan_fls_once, 1, 0) == 0)
        g_orphan_fls = FlsAlloc(co_orphan_fls_dtor);
    if (g_orphan_fls != FLS_OUT_OF_INDEXES)
        FlsSetValue(g_orphan_fls, (void *)(uintptr_t)1);
}

static void co_orphan_disarm(void)
{
    if (g_orphan_fls != FLS_OUT_OF_INDEXES)
        FlsSetValue(g_orphan_fls, NULL);
}
#else
static pthread_key_t  g_orphan_key;
static pthread_once_t g_orphan_once = PTHREAD_ONCE_INIT;

static void co_orphan_key_dtor(void *p)
{
    (void)p;
    co_orphan_reclaim_all();
}

static void co_orphan_key_create(void)
{
    (void)pthread_key_create(&g_orphan_key, co_orphan_key_dtor);
}

static void co_orphan_arm(void)
{
    pthread_once(&g_orphan_once, co_orphan_key_create);
    (void)pthread_setspecific(g_orphan_key, (void *)(uintptr_t)1);
}

static void co_orphan_disarm(void)
{
    pthread_once(&g_orphan_once, co_orphan_key_create);
    (void)pthread_setspecific(g_orphan_key, NULL);
}
#endif

static void co_live_link(struct coroutine *co)
{
    co->live_next = g_live_head;
    g_live_head   = co;
    co_orphan_arm();
}

static void co_live_unlink(struct coroutine *co)
{
    struct coroutine **pp = &g_live_head;

    while (*pp) {
        if (*pp == co) {
            *pp = co->live_next;
            co->live_next = NULL;
            break;
        }
        pp = &(*pp)->live_next;
    }
    if (!g_live_head)
        co_orphan_disarm();
}

static void ensure_initialized(void)
{
    co_platform_initialize();
    if (!current_coroutine) {
        current_coroutine       = &main_coroutine;
        main_coroutine.state    = CO_RUNNING;
        main_coroutine.owner_id = co_self_thread_id();
    }
}

static void *co_exchange_and_switch(struct coroutine *from,
                                    struct coroutine *to,
                                    void *outgoing);

/* ------------------------------------------------------------------ *
 * allocator — 僅 struct coroutine；g_allocator 為 co_set_allocator 的副本
 * ------------------------------------------------------------------ */
static co_allocator g_allocator;

_Static_assert(_Alignof(struct coroutine) <= CO_ALLOC_ALIGN,
               "struct coroutine alignment exceeds CO_ALLOC_ALIGN");

enum { co_obj_align = _Alignof(struct coroutine) };

static int co_ptr_aligned(const void *p)
{
    const uintptr_t mask = (uintptr_t)co_obj_align - 1u;
    return ((uintptr_t)p & mask) == 0;
}

static co_result co_mem_alloc(size_t n, void **out)
{
    void *p;

    if (!out)
        return CO_RESULT_INVALID_ARGUMENT;
    *out = NULL;

    if (!g_allocator.alloc) {
        p = calloc(1, n);
        if (!p)
            return CO_RESULT_OUT_OF_MEMORY;
        *out = p;
        return CO_RESULT_OK;
    }

    p = g_allocator.alloc(n, g_allocator.userdata);
    if (!p)
        return CO_RESULT_OUT_OF_MEMORY;
    if (!co_ptr_aligned(p)) {
        if (g_allocator.free)
            g_allocator.free(p, n, g_allocator.userdata);
        return CO_RESULT_INVALID_ARGUMENT;
    }
    *out = p;
    return CO_RESULT_OK;
}

/* 以 create 時快照的 allocator 釋放，避免與當前 g_allocator mismatch */
static void co_mem_free_with(const co_allocator *a, void *p, size_t n)
{
    if (!p)
        return;
    if (!a || !a->alloc) {
        free(p);
        return;
    }
    if (a->free)
        a->free(p, n, a->userdata);
}

/* 僅釋放庫資源；不 resume、不跑 callback、不經公開 co_destroy 狀態檢查 */
static void co_internal_reclaim_orphan(struct coroutine *co)
{
    co_allocator snap;

    if (!co)
        return;
    snap = co->allocator;
    co_live_unlink(co);
    co_stack_destroy(&co->stack);
    co->owner_id = 0;
    co_mem_free_with(&snap, co, sizeof *co);
}

void co_set_allocator(const co_allocator *a)
{
    if (!a || (!a->alloc && !a->free)) {
        memset(&g_allocator, 0, sizeof g_allocator);
        return;
    }
    if (!a->alloc) {
        memset(&g_allocator, 0, sizeof g_allocator);
        return;
    }
    g_allocator = *a;
}

/* ------------------------------------------------------------------ *
 * CLS — process-global key + per-coroutine value slots
 * ------------------------------------------------------------------ */
#if defined(_MSC_VER) && !defined(__clang__)
#  include <windows.h>

static unsigned co_atomic_load_relaxed(unsigned *p)
{
    return (unsigned)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
}

static int co_atomic_compare_exchange_relaxed(unsigned *p,
                                              unsigned *expected,
                                              unsigned desired)
{
    LONG prev = InterlockedCompareExchange((volatile LONG *)p,
                                           (LONG)desired,
                                           (LONG)*expected);
    if ((unsigned)prev == *expected)
        return 1;
    *expected = (unsigned)prev;
    return 0;
}
#else
static unsigned co_atomic_load_relaxed(unsigned *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static int co_atomic_compare_exchange_relaxed(unsigned *p,
                                              unsigned *expected,
                                              unsigned desired)
{
    return __atomic_compare_exchange_n(p, expected, desired, 1,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}
#endif

static unsigned g_cls_next;

co_cls_key co_cls_alloc(void)
{
    unsigned current;

    for (;;) {
        current = co_atomic_load_relaxed(&g_cls_next);
        if (current >= CO_CLS_SLOTS)
            return CO_CLS_KEY_INVALID;
        if (co_atomic_compare_exchange_relaxed(&g_cls_next, &current, current + 1))
            return (co_cls_key)current;
    }
}

co_result co_cls_set(co_cls_key key, void *value)
{
    struct coroutine *self;

    if (key < 0 || key >= CO_CLS_SLOTS)
        return CO_RESULT_INVALID_ARGUMENT;

    self = co_current();
    self->cls[key] = value;
    return CO_RESULT_OK;
}

void *co_cls_get(co_cls_key key)
{
    struct coroutine *self;

    if (key < 0 || key >= CO_CLS_SLOTS)
        return NULL;

    self = co_current();
    return self->cls[key];
}

/* ------------------------------------------------------------------ *
 * trampoline
 * ------------------------------------------------------------------ */
void co_trampoline_body(void)
{
    struct coroutine *self = current_coroutine;
    struct coroutine *caller;
    void             *initial_input;

    /* P0：首次進入 fiber 時 finish 來自 caller 的 start_switch 狀態 */
    co_asan_finish_on_enter(self->caller);

    /* 首次 resume 的 input：消費 mailbox 後交給 callback */
    initial_input   = self->mailbox;
    self->mailbox   = NULL;
    self->function(self, self->userdata, initial_input); /* 契約見標頭 */

    self->state       = CO_DONE;
    caller            = self->caller;
    current_coroutine = caller;

    /* 正常結束：無 output；避免 co_resume 讀到過期 mailbox */
    caller->mailbox = NULL;

    co_do_switch(&self->context, &caller->context, self, caller);
    abort();    /* 已完成的協程不該被再次進入 */
}

void co_bad_return(void) { abort(); }

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */
co_result co_create_ex(size_t stack_size, co_function function, void *userdata,
                      coroutine **out)
{
    struct coroutine *co;

    if (!out) return CO_RESULT_INVALID_ARGUMENT;
    *out = NULL;

    ensure_initialized();

    if (!function || stack_size < CO_MIN_STACK_SIZE ||
        stack_size > CO_MAX_STACK_SIZE)
        return CO_RESULT_INVALID_ARGUMENT;

    co_result ar;

    ar = co_mem_alloc(sizeof *co, (void **)&co);
    if (ar != CO_RESULT_OK)
        return ar;
    if (g_allocator.alloc)
        memset(co, 0, sizeof *co);

    /* 快照當前 allocator；destroy 永遠用此副本，不依賴之後的 co_set_allocator */
    co->allocator = g_allocator;

    if (co_stack_create(&co->stack, stack_size) != 0) {
        co_allocator snap = co->allocator;
        co_mem_free_with(&snap, co, sizeof *co);
        return CO_RESULT_OUT_OF_MEMORY;
    }
    co->function  = function;
    co->userdata  = userdata;
    co->state     = CO_READY;
    co->owner_id  = co_self_thread_id();

    initialize_context(co);
    co_live_link(co);
    *out = co;
    return CO_RESULT_OK;
}

coroutine *co_create(size_t stack_size, co_function function, void *userdata)
{
    coroutine *co = NULL;

    if (co_create_ex(stack_size, function, userdata, &co) != CO_RESULT_OK)
        return NULL;
    return co;
}

co_result co_resume(coroutine *target, void *input, void **output)
{
    struct coroutine *caller;

    if (output)
        *output = NULL;

    ensure_initialized();

    if (!target)                                    return CO_RESULT_INVALID_ARGUMENT;
    if (target->owner_id != co_self_thread_id())    return CO_RESULT_WRONG_THREAD;
    if (target->state == CO_RUNNING)           return CO_RESULT_ALREADY_RUNNING;
    if (target->state == CO_DONE)              return CO_RESULT_FINISHED;
    if (target->state == CO_WAITING)           return CO_RESULT_INVALID_STATE;
    if (target->state != CO_READY &&
        target->state != CO_SUSPENDED)         return CO_RESULT_INVALID_STATE;

    caller            = current_coroutine;
    caller->state     = CO_WAITING;
    target->caller    = caller;
    target->state     = CO_RUNNING;
    current_coroutine = target;

    void *got = co_exchange_and_switch(caller, target, input);

    if (output)
        *output = (target->state == CO_DONE) ? NULL : got;
    /* target yield 或結束後，控制流恢復 */
    current_coroutine = caller;
    caller->state     = CO_RUNNING;
    target->caller    = NULL;

    return CO_RESULT_OK;
}

co_result co_yield_now(void *output, void **next_input)
{
    struct coroutine *self;
    struct coroutine *caller;

    if (next_input)
        *next_input = NULL;

    ensure_initialized();
    self = current_coroutine;

    if (!self)         return CO_RESULT_INVALID_STATE;
    if (!self->caller) return CO_RESULT_NO_CALLER;

    caller            = self->caller;
    self->state       = CO_SUSPENDED;
    current_coroutine = caller;

    void *got = co_exchange_and_switch(self, caller, output);
    if (next_input)
        *next_input = got;

    current_coroutine = self;
    self->state       = CO_RUNNING;
    return CO_RESULT_OK;
}

static void *co_exchange_and_switch(struct coroutine *from,
                                    struct coroutine *to,
                                    void *outgoing)
{
    void *incoming;

    to->mailbox = outgoing;
    co_do_switch(&from->context, &to->context, from, to);
    incoming      = from->mailbox;
    from->mailbox = NULL; /* mailbox = transient message */
    return incoming;
}

coroutine *co_current(void)
{
    ensure_initialized();
    return current_coroutine;
}

void *co_userdata(const coroutine *co)
{
    if (!co)
        return NULL;
    return co->userdata;
}

co_result co_destroy(coroutine *co)
{
    if (!co)                                    return CO_RESULT_INVALID_ARGUMENT;
    /* 只有 owner thread 能釋放 coroutine */
    if (co->owner_id != co_self_thread_id())    return CO_RESULT_WRONG_THREAD;
    if (co->state == CO_RUNNING)                return CO_RESULT_ALREADY_RUNNING;
    /* 掛起中的協程堆疊上可能有未釋放的資源；採「禁止銷毀」語意（無法 kill 一條 coroutine） */
    if (co->state == CO_SUSPENDED ||
        co->state == CO_WAITING)                return CO_RESULT_INVALID_STATE;

    {
        co_allocator snap = co->allocator;
        co_live_unlink(co);
        co_stack_destroy(&co->stack);
        co->owner_id = 0; /* 降低 UAF 誤判為同 owner 的機率（非完整 poison） */
        co_mem_free_with(&snap, co, sizeof *co);
    }
    return CO_RESULT_OK;
}

co_result co_thread_shutdown(size_t *leaked_count)
{
    struct coroutine *co;
    size_t            leaked = 0;

    ensure_initialized();

    co = g_live_head;
    while (co) {
        struct coroutine *next = co->live_next;

        if (co->owner_id == co_self_thread_id()) {
            if (co->state == CO_DONE || co->state == CO_READY) {
                (void)co_destroy(co);
            } else {
                /* SUSPENDED / WAITING / RUNNING：契約禁止 destroy，計入 leaked */
                leaked++;
            }
        }
        co = next;
    }

    if (leaked_count)
        *leaked_count = leaked;
    return leaked ? CO_RESULT_INVALID_STATE : CO_RESULT_OK;
}

int co_finished(const coroutine *co)
{
    if (!co) return 1;
    return co->state == CO_DONE;
}

size_t co_stack_peak(const coroutine *co)
{
#ifdef CO_DEBUG_STACK_USAGE
    const unsigned char *p, *e;
    if (!co || !co->stack.lo) return 0;
    p = (const unsigned char *)co->stack.lo;
    e = (const unsigned char *)co->stack.hi;
    while (p < e && *p == 0xCD) p++;     /* 從低位往上找第一個被動過的 byte */
    return (size_t)(e - p);
#else
    (void)co;
    return 0;
#endif
}

co_result co_set_storage(coroutine *co, void *buf, size_t cap)
{
    if (!co) return CO_RESULT_INVALID_ARGUMENT;
    if (co->owner_id != co_self_thread_id()) return CO_RESULT_WRONG_THREAD;
    if (co->state != CO_READY) return CO_RESULT_INVALID_STATE;
    if (buf && cap == 0) return CO_RESULT_INVALID_ARGUMENT;
    if (!buf && cap > 0) return CO_RESULT_INVALID_ARGUMENT;

    co->storage_buffer = buf;
    co->st_cap         = cap;
    return CO_RESULT_OK;
}

void *co_storage(coroutine *co)
{
    if (!co || co->st_cap == 0) return NULL;
    return co->storage_buffer;
}

size_t co_storage_size(coroutine *co)
{
    if (!co) return 0;
    return co->st_cap;
}