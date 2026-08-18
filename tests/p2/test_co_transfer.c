/*
 * P2: co_transfer as the shared switch primitive.
 * Sibling hop, first-entry via transfer, rejects, no-caller yield.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <pthread.h>
#include <stdint.h>

static int g_order[8];
static int g_norder;
static int g_a_continued;

static coroutine *g_main;
static coroutine *g_peer_a;
static coroutine *g_peer_b;

static char msg_ab;
static char msg_bm;
static char msg_initial;
static char msg_wake;

static void order_push(int v)
{
    if (g_norder < (int)(sizeof g_order / sizeof g_order[0]))
        g_order[g_norder++] = v;
}

static void fn_sibling_b(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;

    (void)ud;
    p0_expect_ptr(__LINE__, "B initial from A", in, &msg_ab);
    p0_expect_ptr(__LINE__, "B is current", self, co_current());
    order_push(2);
    p0_expect(__LINE__, "B transfer main",
              co_transfer(g_main, &msg_bm, &t), CO_RESULT_OK);
    order_push(9); /* must not run before main's resume returns */
}

static void fn_sibling_a(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;

    (void)self;
    (void)ud;
    (void)in;
    order_push(1);
    p0_expect(__LINE__, "A transfer B",
              co_transfer(g_peer_b, &msg_ab, &t), CO_RESULT_OK);
    g_a_continued = 1;
    order_push(8);
}

void test_sibling_hop(void)
{
    void *output = (void *)(uintptr_t)0xdead;

    g_norder = 0;
    g_a_continued = 0;
    g_main = co_current();
    g_peer_a = co_create(CO_MIN_STACK_SIZE, fn_sibling_a, NULL);
    g_peer_b = co_create(CO_MIN_STACK_SIZE, fn_sibling_b, NULL);
    if (!g_peer_a || !g_peer_b) {
        g_p0_failures++;
        if (g_peer_a)
            (void)co_abandon(g_peer_a);
        if (g_peer_b)
            (void)co_abandon(g_peer_b);
        return;
    }

    p0_expect(__LINE__, "resume A",
              co_resume(g_peer_a, NULL, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "A did not continue", g_a_continued, 0);
    p0_expect(__LINE__, "order len", g_norder, 2);
    p0_expect(__LINE__, "order0 A", g_order[0], 1);
    p0_expect(__LINE__, "order1 B", g_order[1], 2);
    p0_expect_ptr(__LINE__, "output from B", output, &msg_bm);
    p0_expect(__LINE__, "A not finished", co_finished(g_peer_a), 0);
    p0_expect(__LINE__, "B not finished", co_finished(g_peer_b), 0);

    p0_expect(__LINE__, "abandon A", co_abandon(g_peer_a), CO_RESULT_OK);
    p0_expect(__LINE__, "abandon B", co_abandon(g_peer_b), CO_RESULT_OK);
}

static void fn_first_entry(coroutine *self, void *ud, void *in)
{
    (void)ud;
    p0_expect_ptr(__LINE__, "self current", self, co_current());
    p0_expect_ptr(__LINE__, "initial via transfer", in, &msg_initial);
}

void test_first_entry_transfer(void)
{
    co_transfer_t t = { .prev = (coroutine *)(uintptr_t)1,
                        .data = (void *)(uintptr_t)2 };
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_first_entry, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "transfer enter",
              co_transfer(co, &msg_initial, &t), CO_RESULT_OK);
    p0_expect(__LINE__, "finished", co_finished(co), 1);
    p0_expect_ptr(__LINE__, "prev is fiber", t.prev, co);
    p0_expect_ptr(__LINE__, "finish data null", t.data, NULL);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_no_caller_yield(coroutine *self, void *ud, void *in)
{
    coroutine *main_co = ud;
    co_transfer_t t;

    (void)self;
    (void)in;
    p0_expect(__LINE__, "yield without caller",
              co_yield_now(NULL, NULL), CO_RESULT_NO_CALLER);
    p0_expect(__LINE__, "transfer back to main",
              co_transfer(main_co, NULL, &t), CO_RESULT_OK);
}

void test_transfer_no_caller_yield(void)
{
    co_transfer_t t;
    coroutine *main_co = co_current();
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_no_caller_yield, main_co);

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "transfer start",
              co_transfer(co, NULL, &t), CO_RESULT_OK);
    p0_expect(__LINE__, "still suspended in transfer", co_finished(co), 0);
    p0_expect(__LINE__, "transfer back to finish",
              co_transfer(co, NULL, &t), CO_RESULT_OK);
    p0_expect(__LINE__, "finished after transfer home", co_finished(co), 1);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_xfer_to_waiting_main(coroutine *self, void *ud, void *in)
{
    coroutine *main_co = ud;
    co_transfer_t t;

    (void)self;
    (void)in;
    p0_expect(__LINE__, "transfer to WAITING main",
              co_transfer(main_co, &msg_wake, &t), CO_RESULT_OK);
}

void test_transfer_to_waiting(void)
{
    void *output = NULL;
    coroutine *main_co = co_current();
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_xfer_to_waiting_main,
                              main_co);

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume then transfer home",
              co_resume(co, NULL, &output), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "mailbox from transfer", output, &msg_wake);
    p0_expect(__LINE__, "not finished", co_finished(co), 0);

    p0_expect(__LINE__, "resume finish",
              co_resume(co, NULL, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "finished", co_finished(co), 1);
    p0_expect_ptr(__LINE__, "done output null", output, NULL);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_instant_done(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

static void fn_transfer_self(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "transfer self",
              co_transfer(self, NULL, NULL), CO_RESULT_ALREADY_RUNNING);
}

static int g_defer_xfer_rc = -999;

static void defer_try_transfer(void *p)
{
    g_defer_xfer_rc = (int)co_transfer(p, NULL, NULL);
}

void test_transfer_rejects(void)
{
    co_transfer_t stale = { .prev = (coroutine *)(uintptr_t)1,
                            .data = (void *)(uintptr_t)2 };
    coroutine *co;
    coroutine *done;

    p0_expect(__LINE__, "NULL to",
              co_transfer(NULL, NULL, &stale), CO_RESULT_INVALID_ARGUMENT);
    p0_expect_ptr(__LINE__, "stale prev cleared", stale.prev, NULL);
    p0_expect_ptr(__LINE__, "stale data cleared", stale.data, NULL);

    p0_expect(__LINE__, "transfer self main",
              co_transfer(co_current(), NULL, NULL), CO_RESULT_ALREADY_RUNNING);

    co = co_create(CO_MIN_STACK_SIZE, fn_transfer_self, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume self-transfer",
              co_resume(co, NULL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy self-transfer", co_destroy(co), CO_RESULT_OK);

    done = co_create(CO_MIN_STACK_SIZE, fn_instant_done, NULL);
    if (!done) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "transfer to finish",
              co_transfer(done, NULL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "transfer DONE",
              co_transfer(done, NULL, NULL), CO_RESULT_FINISHED);
    p0_expect(__LINE__, "destroy done", co_destroy(done), CO_RESULT_OK);

    co = co_create(CO_MIN_STACK_SIZE, fn_instant_done, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }
    g_defer_xfer_rc = -999;
    p0_expect(__LINE__, "defer register",
              co_defer(co, defer_try_transfer, co_current()), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy runs defer", co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "transfer in defer",
              g_defer_xfer_rc, (int)CO_RESULT_INVALID_STATE);
}

#if defined(__linux__) || defined(__APPLE__)
static coroutine *g_wrong_co;
static pthread_barrier_t g_wrong_barrier;

static void *wrong_thread_worker(void *arg)
{
    co_result r;

    (void)arg;
    pthread_barrier_wait(&g_wrong_barrier);
    r = co_transfer(g_wrong_co, NULL, NULL);
    pthread_barrier_wait(&g_wrong_barrier);
    return (void *)(intptr_t)r;
}

void test_transfer_wrong_thread(void)
{
    pthread_t t;
    void     *ret;
    co_result r;

    g_wrong_co = co_create(CO_MIN_STACK_SIZE, fn_instant_done, NULL);
    if (!g_wrong_co) {
        g_p0_failures++;
        return;
    }
    if (pthread_barrier_init(&g_wrong_barrier, NULL, 2) != 0) {
        g_p0_failures++;
        (void)co_destroy(g_wrong_co);
        return;
    }
    if (pthread_create(&t, NULL, wrong_thread_worker, NULL) != 0) {
        g_p0_failures++;
        pthread_barrier_destroy(&g_wrong_barrier);
        (void)co_destroy(g_wrong_co);
        return;
    }
    pthread_barrier_wait(&g_wrong_barrier);
    pthread_barrier_wait(&g_wrong_barrier);
    pthread_join(t, &ret);
    pthread_barrier_destroy(&g_wrong_barrier);

    r = (co_result)(intptr_t)ret;
    p0_expect(__LINE__, "wrong thread", r, CO_RESULT_WRONG_THREAD);
    p0_expect(__LINE__, "destroy after wrong thread",
              co_destroy(g_wrong_co), CO_RESULT_OK);
}
#else
void test_transfer_wrong_thread(void)
{
}
#endif
