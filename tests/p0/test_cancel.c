/*
 * P0: co_cancel + CO_CANCEL sentinel
 */

#include "p0_common.h"

#include <pthread.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * co_is_cancel
 * ------------------------------------------------------------------ */
void test_cancel_sentinel(void)
{
    p0_expect(__LINE__, "is_cancel NULL", co_is_cancel(NULL), 0);
    p0_expect(__LINE__, "is_cancel CO_CANCEL", co_is_cancel(CO_CANCEL), 1);
}

/* ------------------------------------------------------------------ *
 * READY / DONE
 * ------------------------------------------------------------------ */
static void fn_never_run(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_p0_failures++;
}

void test_cancel_ready(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "cancel ready", co_cancel(co), CO_RESULT_OK);
}

static void fn_done_quick(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

void test_cancel_done(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_done_quick, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to done", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel done", co_cancel(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * SUSPENDED — cooperative cancel
 * ------------------------------------------------------------------ */
typedef struct {
    int cleaned;
    int cancel_pass;
} cancel_ctx_t;

static void fn_cooperative_cancel(coroutine *self, void *ud, void *in)
{
    cancel_ctx_t *ctx = ud;
    void         *cmd = in;

    (void)self;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd)) {
        ctx->cleaned = 1;
        return;
    }
    g_p0_failures++;
}

void test_cancel_suspended_ok(void)
{
    cancel_ctx_t ctx = {0};
    coroutine   *co  = co_create(CO_MIN_STACK_SIZE, fn_cooperative_cancel, &ctx);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel suspended", co_cancel(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cleanup ran", ctx.cleaned, 1);
}

/* ------------------------------------------------------------------ *
 * SUSPENDED — ignored cancel (yield after sentinel)
 * ------------------------------------------------------------------ */
static void fn_ignore_cancel(coroutine *self, void *ud, void *in)
{
    void *cmd = in;

    (void)self;
    (void)ud;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd))
        (void)co_yield_now(NULL, &cmd); /* 違約：看到 cancel 後再 yield */
    else
        g_p0_failures++;
}

void test_cancel_ignored(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_ignore_cancel, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "destroy still blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "resume finish", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after finish", co_destroy(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * Second cancel after ignore — cooperative on retry
 * ------------------------------------------------------------------ */
static void fn_retry_cancel(coroutine *self, void *ud, void *in)
{
    cancel_ctx_t *ctx = ud;
    void         *cmd = in;

    (void)self;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    for (;;) {
        if (!co_is_cancel(cmd))
            break;
        if (ctx->cancel_pass == 0) {
            ctx->cancel_pass = 1;
            p0_expect(__LINE__, "ignore yield",
                      co_yield_now(NULL, &cmd), CO_RESULT_OK);
            continue;
        }
        ctx->cleaned = 1;
        return;
    }
    g_p0_failures++;
}

void test_cancel_retry_after_ignore(void)
{
    cancel_ctx_t ctx = {0};
    coroutine   *co  = co_create(CO_MIN_STACK_SIZE, fn_retry_cancel, &ctx);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "first cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "second cancel ok", co_cancel(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cleanup ran", ctx.cleaned, 1);
}

/* ------------------------------------------------------------------ *
 * RUNNING — cancel self inside callback
 * ------------------------------------------------------------------ */
static void fn_cancel_self(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "cancel self",
              co_cancel(self), CO_RESULT_ALREADY_RUNNING);
}

void test_cancel_running(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_cancel_self, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume running", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after run", co_destroy(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * WAITING — inner tries to cancel outer
 * ------------------------------------------------------------------ */
static coroutine *g_cancel_wait_outer;

static void fn_cancel_waiting_inner(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "cancel waiting outer",
              co_cancel(g_cancel_wait_outer), CO_RESULT_INVALID_STATE);
}

static void fn_cancel_waiting_outer(coroutine *self, void *ud, void *in)
{
    coroutine *inner;

    (void)self;
    (void)ud;
    (void)in;
    inner = co_create(CO_MIN_STACK_SIZE, fn_cancel_waiting_inner, NULL);
    if (!inner) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume inner", P0_RESUME(inner), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy inner", co_destroy(inner), CO_RESULT_OK);
}

void test_cancel_waiting(void)
{
    g_cancel_wait_outer = co_create(CO_MIN_STACK_SIZE, fn_cancel_waiting_outer, NULL);
    if (!g_cancel_wait_outer) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume outer", P0_RESUME(g_cancel_wait_outer), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy outer", co_destroy(g_cancel_wait_outer), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * NULL / wrong thread
 * ------------------------------------------------------------------ */
void test_cancel_null(void)
{
    p0_expect(__LINE__, "cancel null", co_cancel(NULL), CO_RESULT_INVALID_ARGUMENT);
}

#if defined(__linux__) || defined(__APPLE__)
static coroutine *g_cancel_wrong_co;
static pthread_barrier_t g_cancel_wrong_barrier;

static void *cancel_wrong_worker(void *arg)
{
    co_result r;

    (void)arg;
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    r = co_cancel(g_cancel_wrong_co);
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    return (void *)(intptr_t)r;
}

void test_cancel_wrong_thread(void)
{
    pthread_t t;
    void     *ret;
    co_result r;

    g_cancel_wrong_co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);
    if (!g_cancel_wrong_co) {
        g_p0_failures++;
        return;
    }
    if (pthread_barrier_init(&g_cancel_wrong_barrier, NULL, 2) != 0) {
        g_p0_failures++;
        (void)co_destroy(g_cancel_wrong_co);
        return;
    }
    if (pthread_create(&t, NULL, cancel_wrong_worker, NULL) != 0) {
        g_p0_failures++;
        pthread_barrier_destroy(&g_cancel_wrong_barrier);
        (void)co_destroy(g_cancel_wrong_co);
        return;
    }
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    pthread_join(t, &ret);
    pthread_barrier_destroy(&g_cancel_wrong_barrier);

    r = (co_result)(intptr_t)ret;
    p0_expect(__LINE__, "wrong thread", r, CO_RESULT_WRONG_THREAD);
    p0_expect(__LINE__, "destroy after wrong thread",
              co_destroy(g_cancel_wrong_co), CO_RESULT_OK);
}
#else
void test_cancel_wrong_thread(void)
{
    p0_log("SKIP", "test_cancel.c:test_cancel_wrong_thread",
           "pthread barrier test skipped", "{}");
}
#endif
