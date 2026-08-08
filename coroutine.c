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
 * thread-local 狀態
 * ------------------------------------------------------------------ */
static CO_THREAD_LOCAL unsigned char   thread_token;
static CO_THREAD_LOCAL struct coroutine main_coroutine;
static CO_THREAD_LOCAL struct coroutine *current_coroutine;

static void ensure_initialized(void)
{
    co_platform_initialize();
    if (!current_coroutine) {
        current_coroutine          = &main_coroutine;
        main_coroutine.state       = CO_RUNNING;
        main_coroutine.owner_token = &thread_token;
    }
}

static void *co_exchange_and_switch(struct coroutine *from,
                                    struct coroutine *to,
                                    void *data);

/* ------------------------------------------------------------------ *
 * trampoline
 * ------------------------------------------------------------------ */
void co_trampoline_body(void)
{
    struct coroutine *self = current_coroutine;
    struct coroutine *caller;

    /* P0：首次進入 fiber 時 finish 來自 caller 的 start_switch 狀態 */
    co_asan_finish_on_enter(self->caller);

    self->function(self->transfer);   /* C 沒有例外可攔截；契約見標頭 */

    self->state       = CO_DONE;
    caller            = self->caller;
    current_coroutine = caller;

    /* 結束切回 caller 前清空其信箱，避免 co_resume 讀到過期 transfer */
    caller->transfer = NULL;

    co_do_switch(&self->context, &caller->context, self, caller);
    abort();    /* 已完成的協程不該被再次進入 */
}

void co_bad_return(void) { abort(); }

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */
co_result co_create_ex(size_t stack_size, co_function function, void *argument,
                      coroutine **out)
{
    struct coroutine *co;

    if (!out) return CO_RESULT_INVALID_ARGUMENT;
    *out = NULL;

    ensure_initialized();

    if (!function || stack_size < CO_MIN_STACK_SIZE)
        return CO_RESULT_INVALID_ARGUMENT;

    co = calloc(1, sizeof *co);
    if (!co) return CO_RESULT_OUT_OF_MEMORY;

    if (co_stack_create(&co->stack, stack_size) != 0) {
        free(co);
        return CO_RESULT_OUT_OF_MEMORY;
    }

    co->function    = function;
    co->argument    = argument;
    co->state       = CO_READY;
    co->owner_token = &thread_token;

    initialize_context(co);
    *out = co;
    return CO_RESULT_OK;
}

coroutine *co_create(size_t stack_size, co_function function, void *argument)
{
    coroutine *co = NULL;

    if (co_create_ex(stack_size, function, argument, &co) != CO_RESULT_OK)
        return NULL;
    return co;
}

co_result co_resume(coroutine *target,void *arg,void **out)
{
    struct coroutine *caller;

    ensure_initialized();

    if (!target)                               return CO_RESULT_INVALID_ARGUMENT;
    if (target->owner_token != &thread_token)  return CO_RESULT_WRONG_THREAD;
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

    void *got = co_exchange_and_switch(caller, target, arg);

    if (out)
        *out = (target->state == CO_DONE) ? NULL : got;
    /* target yield 或結束後，控制流恢復 */
    current_coroutine = caller;
    caller->state     = CO_RUNNING;
    target->caller    = NULL;

    return CO_RESULT_OK;
}

co_result co_yield_now(void* arg,void **out)
{
    struct coroutine *self = current_coroutine;
    struct coroutine *caller;

    //確保回傳結果不產生錯誤的失敗訊號
    ensure_initialized();

    if (!self)         return CO_RESULT_INVALID_STATE;
    if (!self->caller) return CO_RESULT_NO_CALLER;

    caller            = self->caller;
    self->state       = CO_SUSPENDED;
    current_coroutine = caller;

    void *got = co_exchange_and_switch(self, caller, arg);
    if (out)
        *out = got;

    current_coroutine = self;
    self->state       = CO_RUNNING;
    return CO_RESULT_OK;
}

static void *co_exchange_and_switch(struct coroutine *from,
    struct coroutine *to,
    void *data)
{
    to->transfer = data;
    co_do_switch(&from->context, &to->context, from, to);
    return from->transfer;
}

co_result co_destroy(coroutine *co)
{
    if (!co)                                return CO_RESULT_INVALID_ARGUMENT;
    //只有owner thread能釋放coroutine
    if (co->owner_token != &thread_token)   return CO_RESULT_WRONG_THREAD;
    if (co->state == CO_RUNNING)            return CO_RESULT_ALREADY_RUNNING;
    /* 掛起中的協程堆疊上可能有未釋放的資源；採「禁止銷毀」語意（無法 kill 一條 coroutine） */
    if (co->state == CO_SUSPENDED ||
        co->state == CO_WAITING)            return CO_RESULT_INVALID_STATE;

    co_stack_destroy(&co->stack);
    free(co);
    return CO_RESULT_OK;
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
    if (co->owner_token != &thread_token) return CO_RESULT_WRONG_THREAD;
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