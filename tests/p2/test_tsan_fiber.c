/*
 * P2: TSan fiber annotation smoke — lifecycle + happens-before via transfer.
 * Non-TSan builds SKIP (still link / run as no-op pass).
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdio.h>
#include <stdint.h>

#ifdef __SANITIZE_THREAD__
#  define CO_TEST_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CO_TEST_TSAN 1
#  endif
#endif
#ifndef CO_TEST_TSAN
#  define CO_TEST_TSAN 0
#endif

#if CO_TEST_TSAN

static int g_shared;
static int g_hb_seen;
static coroutine *g_peer_b;
static coroutine *g_main;

static void fn_hb_b(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;

    (void)self;
    (void)ud;
    (void)in;
    /* switch_to_fiber(flags=0) 應建立 HB：讀 A 寫入的 g_shared 不應誤報 race */
    g_hb_seen = g_shared;
    p0_expect(__LINE__, "B transfer main",
              co_transfer(g_main, NULL, &t), CO_RESULT_OK);
}

static void fn_hb_a(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;

    (void)self;
    (void)ud;
    (void)in;
    g_shared = 0xC0FFEE;
    p0_expect(__LINE__, "A transfer B",
              co_transfer(g_peer_b, NULL, &t), CO_RESULT_OK);
}

void test_tsan_fiber_hb(void)
{
    void *output = NULL;

    g_shared  = 0;
    g_hb_seen = 0;
    g_main    = co_current();
    g_peer_b  = co_create(CO_MIN_STACK_SIZE, fn_hb_b, NULL);
    if (!g_peer_b) {
        g_p0_failures++;
        return;
    }
    {
        coroutine *a = co_create(CO_MIN_STACK_SIZE, fn_hb_a, NULL);
        if (!a) {
            g_p0_failures++;
            (void)co_abandon(g_peer_b);
            return;
        }
        p0_expect(__LINE__, "resume A",
                  co_resume(a, NULL, &output), CO_RESULT_OK);
        p0_expect(__LINE__, "hb value", g_hb_seen, 0xC0FFEE);
        p0_expect(__LINE__, "abandon A", co_abandon(a), CO_RESULT_OK);
        p0_expect(__LINE__, "abandon B", co_abandon(g_peer_b), CO_RESULT_OK);
    }
}

static void fn_life_yield(coroutine *self, void *ud, void *in)
{
    void *next = NULL;

    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "yield", co_yield_now((void *)(uintptr_t)1, &next),
              CO_RESULT_OK);
}

static void fn_life_hop(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;
    coroutine    *peer = (coroutine *)ud;

    (void)self;
    (void)in;
    p0_expect(__LINE__, "hop peer", co_transfer(peer, NULL, &t), CO_RESULT_OK);
}

static void fn_life_sink(coroutine *self, void *ud, void *in)
{
    co_transfer_t t;

    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "sink->main",
              co_transfer(g_main, NULL, &t), CO_RESULT_OK);
}

void test_tsan_fiber_lifecycle(void)
{
    int i;

    g_main = co_current();
    for (i = 0; i < 64; i++) {
        coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_life_yield, NULL);
        void      *out = NULL;

        if (!co) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "resume yield",
                  co_resume(co, NULL, &out), CO_RESULT_OK);
        p0_expect(__LINE__, "resume finish",
                  co_resume(co, NULL, &out), CO_RESULT_OK);
        p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
    }

    for (i = 0; i < 32; i++) {
        coroutine *sink = co_create(CO_MIN_STACK_SIZE, fn_life_sink, NULL);
        coroutine *hop;
        void      *out = NULL;

        if (!sink) {
            g_p0_failures++;
            return;
        }
        hop = co_create(CO_MIN_STACK_SIZE, fn_life_hop, sink);
        if (!hop) {
            g_p0_failures++;
            (void)co_abandon(sink);
            return;
        }
        p0_expect(__LINE__, "resume hop",
                  co_resume(hop, NULL, &out), CO_RESULT_OK);
        p0_expect(__LINE__, "abandon hop", co_abandon(hop), CO_RESULT_OK);
        p0_expect(__LINE__, "abandon sink", co_abandon(sink), CO_RESULT_OK);
    }

    {
        size_t leaked = 0;
        p0_expect(__LINE__, "shutdown",
                  co_thread_shutdown(&leaked), CO_RESULT_OK);
        p0_expect(__LINE__, "no leak", (int)leaked, 0);
    }
}

#else /* !CO_TEST_TSAN */

void test_tsan_fiber_hb(void)
{
    fprintf(stderr, "SKIP tsan-fiber-hb: needs -fsanitize=thread\n");
}

void test_tsan_fiber_lifecycle(void)
{
    fprintf(stderr, "SKIP tsan-fiber-lifecycle: needs -fsanitize=thread\n");
}

#endif
