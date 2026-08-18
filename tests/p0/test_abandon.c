/*
 * D-11: co_abandon — public reclaim for READY/DONE/SUSPENDED.
 * co_destroy still refuses SUSPENDED/WAITING/RUNNING.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <pthread.h>
#include <stdint.h>

static int g_abandon_runs;
static int g_abandon_order[8];
static int g_abandon_order_n;

static void abandon_mark(void *p)
{
    int *ran = p;

    if (ran)
        (*ran)++;
    g_abandon_runs++;
}

static void abandon_record(void *p)
{
    if (g_abandon_order_n < (int)(sizeof g_abandon_order / sizeof g_abandon_order[0]))
        g_abandon_order[g_abandon_order_n++] = (int)(intptr_t)p;
    g_abandon_runs++;
}

static void fn_never_run(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_p0_failures++;
}

static void fn_done_quick(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

static void fn_suspend_once(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "yield", P0_YIELD(), CO_RESULT_OK);
}

static void fn_ignore_cancel(coroutine *self, void *ud, void *in)
{
    void *cmd = in;

    (void)self;
    (void)ud;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (!co_is_cancel(cmd)) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "ignore yield", co_yield_now(NULL, &cmd), CO_RESULT_OK);
}

void test_abandon_null(void)
{
    p0_expect(__LINE__, "abandon null", co_abandon(NULL),
              CO_RESULT_INVALID_ARGUMENT);
}

void test_abandon_ready(void)
{
    int ran = 0;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "ready defer",
              co_defer(co, abandon_mark, &ran), CO_RESULT_OK);
    p0_expect(__LINE__, "abandon ready", co_abandon(co), CO_RESULT_OK);
    p0_expect(__LINE__, "ready defer ran", ran, 1);
}

void test_abandon_done(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_done_quick, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume done", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "abandon done", co_abandon(co), CO_RESULT_OK);
}

void test_abandon_suspended(void)
{
    size_t leaked = 99;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_suspend_once, NULL);

    g_abandon_runs = 0;
    g_abandon_order_n = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "d1",
              co_defer(co, abandon_record, (void *)(intptr_t)1), CO_RESULT_OK);
    p0_expect(__LINE__, "d2",
              co_defer(co, abandon_record, (void *)(intptr_t)2), CO_RESULT_OK);
    p0_expect(__LINE__, "d3",
              co_defer(co, abandon_record, (void *)(intptr_t)3), CO_RESULT_OK);
    p0_expect(__LINE__, "resume suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "defer not run yet", g_abandon_runs, 0);
    p0_expect(__LINE__, "abandon suspended", co_abandon(co), CO_RESULT_OK);
    p0_expect(__LINE__, "lifo 0", g_abandon_order[0], 3);
    p0_expect(__LINE__, "lifo 1", g_abandon_order[1], 2);
    p0_expect(__LINE__, "lifo 2", g_abandon_order[2], 1);
    p0_expect(__LINE__, "defer once", g_abandon_runs, 3);
    p0_expect(__LINE__, "shutdown clean",
              co_thread_shutdown(&leaked), CO_RESULT_OK);
    p0_expect(__LINE__, "leaked 0", (int)leaked, 0);
}

void test_abandon_cancel_ignored(void)
{
    int ran = 0;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_ignore_cancel, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "pre-register",
              co_defer(co, abandon_mark, &ran), CO_RESULT_OK);
    p0_expect(__LINE__, "resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "destroy still blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "defer held", ran, 0);
    p0_expect(__LINE__, "abandon ignored", co_abandon(co), CO_RESULT_OK);
    p0_expect(__LINE__, "defer ran on abandon", ran, 1);
}

static void fn_abandon_self(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "abandon self",
              co_abandon(self), CO_RESULT_ALREADY_RUNNING);
}

void test_abandon_running(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_abandon_self, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume running", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after run", co_destroy(co), CO_RESULT_OK);
}

static coroutine *g_abandon_wait_outer;

static void fn_abandon_waiting_inner(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "abandon waiting outer",
              co_abandon(g_abandon_wait_outer), CO_RESULT_INVALID_STATE);
}

static void fn_abandon_waiting_outer(coroutine *self, void *ud, void *in)
{
    coroutine *inner;

    (void)self;
    (void)ud;
    (void)in;
    inner = co_create(CO_MIN_STACK_SIZE, fn_abandon_waiting_inner, NULL);
    if (!inner) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume inner", P0_RESUME(inner), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy inner", co_destroy(inner), CO_RESULT_OK);
}

void test_abandon_waiting(void)
{
    g_abandon_wait_outer = co_create(CO_MIN_STACK_SIZE,
                                     fn_abandon_waiting_outer, NULL);
    if (!g_abandon_wait_outer) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume outer",
              P0_RESUME(g_abandon_wait_outer), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy outer",
              co_destroy(g_abandon_wait_outer), CO_RESULT_OK);
}

static int g_defer_abandon_rc;

static void defer_abandon_self_slot(void *p)
{
    coroutine *co = p;

    g_defer_abandon_rc = (int)co_abandon(co);
}

void test_abandon_during_defer(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);

    g_defer_abandon_rc = -999;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "register abandon-self",
              co_defer(co, defer_abandon_self_slot, co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy runs defer", co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "abandon in defer blocked",
              g_defer_abandon_rc, CO_RESULT_INVALID_STATE);
}

#if defined(__linux__) || defined(__APPLE__)
static coroutine *g_abandon_wrong_co;
static pthread_barrier_t g_abandon_wrong_barrier;

static void *abandon_wrong_worker(void *arg)
{
    co_result r;

    (void)arg;
    pthread_barrier_wait(&g_abandon_wrong_barrier);
    r = co_abandon(g_abandon_wrong_co);
    pthread_barrier_wait(&g_abandon_wrong_barrier);
    return (void *)(intptr_t)r;
}

void test_abandon_wrong_thread(void)
{
    pthread_t t;
    void     *ret;
    co_result r;

    g_abandon_wrong_co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);
    if (!g_abandon_wrong_co) {
        g_p0_failures++;
        return;
    }
    if (pthread_barrier_init(&g_abandon_wrong_barrier, NULL, 2) != 0) {
        g_p0_failures++;
        (void)co_destroy(g_abandon_wrong_co);
        return;
    }
    if (pthread_create(&t, NULL, abandon_wrong_worker, NULL) != 0) {
        g_p0_failures++;
        pthread_barrier_destroy(&g_abandon_wrong_barrier);
        (void)co_destroy(g_abandon_wrong_co);
        return;
    }
    pthread_barrier_wait(&g_abandon_wrong_barrier);
    pthread_barrier_wait(&g_abandon_wrong_barrier);
    pthread_join(t, &ret);
    pthread_barrier_destroy(&g_abandon_wrong_barrier);

    r = (co_result)(intptr_t)ret;
    p0_expect(__LINE__, "wrong thread", r, CO_RESULT_WRONG_THREAD);
    p0_expect(__LINE__, "destroy after wrong thread",
              co_destroy(g_abandon_wrong_co), CO_RESULT_OK);
}
#else
void test_abandon_wrong_thread(void)
{
    p0_log("SKIP", "test_abandon.c:test_abandon_wrong_thread",
           "pthread barrier test skipped", "{}");
}
#endif
