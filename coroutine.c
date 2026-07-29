#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#include "coroutine_internal.h"

#include <stdlib.h>

/* C23/C11 版本差異 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#  define CO_THREAD_LOCAL thread_local
#else
#  define CO_THREAD_LOCAL _Thread_local
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

/* ------------------------------------------------------------------ *
 * trampoline
 * ------------------------------------------------------------------ */
void co_trampoline_body(void)
{
    struct coroutine *self = current_coroutine;
    struct coroutine *caller;

    self->function(self->argument);   /* C 沒有例外可攔截；契約見標頭 */

    self->state       = CO_DONE;
    caller            = self->caller;
    current_coroutine = caller;

    co_context_switch(&self->context, &caller->context);
    abort();    /* 已完成的協程不該被再次進入 */
}

void co_bad_return(void) { abort(); }

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */
coroutine *co_create(size_t stack_size, co_function function, void *argument)
{
    struct coroutine *co;

    ensure_initialized();

    if (!function || stack_size < CO_MIN_STACK_SIZE) return NULL;

    co = calloc(1, sizeof *co);
    if (!co) return NULL;

    if (co_stack_create(&co->stack, stack_size) != 0) {
        free(co);
        return NULL;
    }

    co->function    = function;
    co->argument    = argument;
    co->state       = CO_READY;
    co->owner_token = &thread_token;

    initialize_context(co);
    return co;
}

co_result co_resume(coroutine *target)
{
    struct coroutine *caller;

    ensure_initialized();

    if (!target)                               return CO_RESULT_INVALID_ARGUMENT;
    if (target->owner_token != &thread_token)  return CO_RESULT_WRONG_THREAD;
    if (target->state == CO_RUNNING)           return CO_RESULT_ALREADY_RUNNING;
    if (target->state == CO_DONE)              return CO_RESULT_FINISHED;
    if (target->state != CO_READY &&
        target->state != CO_SUSPENDED)         return CO_RESULT_INVALID_STATE;

    caller            = current_coroutine;
    caller->state     = CO_SUSPENDED;
    target->caller    = caller;
    target->state     = CO_RUNNING;
    current_coroutine = target;

    co_context_switch(&caller->context, &target->context);

    /* target yield 或結束後，控制流恢復 */
    current_coroutine = caller;
    caller->state     = CO_RUNNING;
    target->caller    = NULL;
    return CO_RESULT_OK;
}

co_result co_yield_now(void)
{
    struct coroutine *self = current_coroutine;
    struct coroutine *caller;

    if (!self)         return CO_RESULT_INVALID_STATE;
    if (!self->caller) return CO_RESULT_NO_CALLER;

    caller            = self->caller;
    self->state       = CO_SUSPENDED;
    current_coroutine = caller;

    co_context_switch(&self->context, &caller->context);

    current_coroutine = self;
    self->state       = CO_RUNNING;
    return CO_RESULT_OK;
}

co_result co_destroy(coroutine *co)
{
    if (!co)                        return CO_RESULT_INVALID_ARGUMENT;
    if (co->state == CO_RUNNING)    return CO_RESULT_ALREADY_RUNNING;
    /* 掛起中的協程堆疊上可能有未釋放的資源；採「禁止銷毀」語意（無法 kill 一條 coroutine） */
    if (co->state == CO_SUSPENDED)  return CO_RESULT_INVALID_STATE;

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