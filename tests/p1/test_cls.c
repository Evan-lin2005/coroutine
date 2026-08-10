/*
 * P1: Coroutine Local Storage (CLS).
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p1_common.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static co_cls_key g_cls_keys[CO_CLS_SLOTS];
static int        g_cls_keys_ready;

static void ensure_cls_keys(void)
{
    int i;

    if (g_cls_keys_ready)
        return;

    for (i = 0; i < CO_CLS_SLOTS; ++i) {
        co_cls_key k = co_cls_alloc();
        p0_expect(__LINE__, "cls alloc key", k, i);
        g_cls_keys[i] = k;
    }
    p0_expect(__LINE__, "cls alloc exhausted",
              co_cls_alloc(), CO_CLS_KEY_INVALID);
    g_cls_keys_ready = 1;
}

void test_cls_allocation(void)
{
    int i;

    if (g_cls_keys_ready) {
        fprintf(stderr, "SKIP cls-allocation: keys already allocated in this process\n");
        return;
    }

    for (i = 0; i < CO_CLS_SLOTS; ++i) {
        co_cls_key k = co_cls_alloc();
        p0_expect(__LINE__, "alloc sequential", k, i);
        g_cls_keys[i] = k;
    }
    p0_expect(__LINE__, "alloc exhausted",
              co_cls_alloc(), CO_CLS_KEY_INVALID);
    g_cls_keys_ready = 1;
}

void test_cls_main(void)
{
    int value = 42;
    co_cls_key key;

    ensure_cls_keys();
    key = g_cls_keys[0];

    p0_expect_ptr(__LINE__, "main cls unset", co_cls_get(key), NULL);
    p0_expect(__LINE__, "main cls set",
              co_cls_set(key, &value), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "main cls get", co_cls_get(key), &value);
}

typedef struct {
    co_cls_key key;
    int        tag;
} cls_fiber_ctx_t;

static int g_cls_main_val;
static int g_cls_a_val;
static int g_cls_b_val;

static void fn_cls_fiber_b(coroutine *self, void *userdata, void *initial_input)
{
    cls_fiber_ctx_t *ctx = userdata;
    int             *pv  = (initial_input == &g_cls_a_val) ? &g_cls_b_val : NULL;

    (void)self;
    p0_expect_ptr(__LINE__, "fiber B initial", initial_input, &g_cls_a_val);
    p0_expect(__LINE__, "fiber B set",
              co_cls_set(ctx->key, pv), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "fiber B get", co_cls_get(ctx->key), pv);
    P1_YIELD();
    p0_expect_ptr(__LINE__, "fiber B get after yield", co_cls_get(ctx->key), pv);
}

static void fn_cls_fiber_a(coroutine *self, void *userdata, void *initial_input)
{
    cls_fiber_ctx_t *ctx = userdata;

    (void)self;
    p0_expect_ptr(__LINE__, "fiber A initial", initial_input, &g_cls_main_val);
    p0_expect(__LINE__, "fiber A set",
              co_cls_set(ctx->key, &g_cls_a_val), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "fiber A get", co_cls_get(ctx->key), &g_cls_a_val);

    coroutine *child = co_create(CO_MIN_STACK_SIZE, fn_cls_fiber_b, ctx);
    if (!child) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume B",
              co_resume(child, &g_cls_a_val, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "fiber A after B", co_cls_get(ctx->key), &g_cls_a_val);
    p0_expect(__LINE__, "resume B finish",
              co_resume(child, NULL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy B", co_destroy(child), CO_RESULT_OK);
    P1_YIELD();
    p0_expect_ptr(__LINE__, "fiber A after yield", co_cls_get(ctx->key), &g_cls_a_val);
}

static void fn_cls_fiber_simple(coroutine *self, void *userdata, void *initial_input)
{
    cls_fiber_ctx_t *ctx = userdata;
    int             *pv  = (int *)initial_input;

    (void)self;
    p0_expect(__LINE__, "fiber set",
              co_cls_set(ctx->key, pv), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "fiber get", co_cls_get(ctx->key), pv);
    P1_YIELD();
    p0_expect_ptr(__LINE__, "fiber get after yield", co_cls_get(ctx->key), pv);
}

void test_cls_fiber_isolation(void)
{
    cls_fiber_ctx_t ctx_a = { 0, 1 };
    cls_fiber_ctx_t ctx_b = { 0, 2 };
    coroutine      *co_a;
    coroutine      *co_b;

    ensure_cls_keys();
    ctx_a.key = g_cls_keys[1];
    ctx_b.key = g_cls_keys[1];

    g_cls_main_val = 1000;
    g_cls_a_val    = 2000;
    g_cls_b_val    = 3000;

    p0_expect(__LINE__, "main set",
              co_cls_set(ctx_a.key, &g_cls_main_val), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "main get", co_cls_get(ctx_a.key), &g_cls_main_val);

    co_a = co_create(CO_MIN_STACK_SIZE, fn_cls_fiber_simple, &ctx_a);
    co_b = co_create(CO_MIN_STACK_SIZE, fn_cls_fiber_simple, &ctx_b);
    if (!co_a || !co_b) {
        g_p0_failures++;
        if (co_a) co_destroy(co_a);
        if (co_b) co_destroy(co_b);
        return;
    }

    p0_expect(__LINE__, "resume A",
              co_resume(co_a, &g_cls_a_val, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "main after A", co_cls_get(ctx_a.key), &g_cls_main_val);

    p0_expect(__LINE__, "resume B",
              co_resume(co_b, &g_cls_b_val, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "main after B", co_cls_get(ctx_a.key), &g_cls_main_val);

    p0_expect(__LINE__, "resume A again",
              co_resume(co_a, NULL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "resume B finish",
              co_resume(co_b, NULL, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "main final", co_cls_get(ctx_a.key), &g_cls_main_val);

    p0_expect(__LINE__, "destroy A", co_destroy(co_a), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy B", co_destroy(co_b), CO_RESULT_OK);
}

void test_cls_nested(void)
{
    cls_fiber_ctx_t ctx = { 0, 0 };

    ensure_cls_keys();
    ctx.key = g_cls_keys[2];

    g_cls_main_val = 10;
    g_cls_a_val    = 20;
    g_cls_b_val    = 30;

    p0_expect(__LINE__, "nested main set",
              co_cls_set(ctx.key, &g_cls_main_val), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "nested main get", co_cls_get(ctx.key), &g_cls_main_val);

    coroutine *co_a = co_create(CO_MIN_STACK_SIZE, fn_cls_fiber_a, &ctx);
    if (!co_a) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "nested resume A",
              co_resume(co_a, &g_cls_main_val, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "nested main after A", co_cls_get(ctx.key), &g_cls_main_val);

    p0_expect(__LINE__, "nested resume A finish",
              co_resume(co_a, NULL, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "nested main done", co_cls_get(ctx.key), &g_cls_main_val);
    p0_expect(__LINE__, "nested destroy A", co_destroy(co_a), CO_RESULT_OK);
}

static void fn_cls_current(coroutine *self, void *userdata, void *initial_input)
{
    co_cls_key key = *(co_cls_key *)userdata;
    int        x   = 77;

    (void)initial_input;
    p0_expect_ptr(__LINE__, "self is current", self, co_current());
    p0_expect(__LINE__, "cls set in callback",
              co_cls_set(key, &x), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "cls get in callback", co_cls_get(key), &x);
}

void test_cls_current_consistency(void)
{
    co_cls_key key;

    ensure_cls_keys();
    key = g_cls_keys[3];

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_cls_current, &key);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume current", co_resume(co, NULL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy current", co_destroy(co), CO_RESULT_OK);
}

void test_cls_invalid_key(void)
{
    int dummy = 1;

    ensure_cls_keys();

    p0_expect_ptr(__LINE__, "get -1", co_cls_get(-1), NULL);
    p0_expect_ptr(__LINE__, "get slots", co_cls_get(CO_CLS_SLOTS), NULL);
    p0_expect(__LINE__, "set -1",
              co_cls_set(-1, &dummy), CO_RESULT_INVALID_ARGUMENT);
    p0_expect(__LINE__, "set slots",
              co_cls_set(CO_CLS_SLOTS, &dummy), CO_RESULT_INVALID_ARGUMENT);
}

void test_cls_explicit_null(void)
{
    int        value = 99;
    co_cls_key key;

    ensure_cls_keys();
    key = g_cls_keys[4];

    p0_expect(__LINE__, "set value",
              co_cls_set(key, &value), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "get value", co_cls_get(key), &value);
    p0_expect(__LINE__, "set null",
              co_cls_set(key, NULL), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "get null", co_cls_get(key), NULL);
}

typedef struct {
    co_cls_key key;
    int        tid;
    int        value;
    int        ok;
} cls_thread_arg_t;

static void *cls_thread_main(void *arg)
{
    cls_thread_arg_t *a = arg;

    a->value = (a->tid == 0) ? 111 : 222;
    if (co_cls_set(a->key, &a->value) != CO_RESULT_OK)
        a->ok = 0;
  else if (co_cls_get(a->key) != &a->value)
        a->ok = 0;
    else
        a->ok = 1;
    return NULL;
}

void test_cls_thread_isolation(void)
{
    pthread_t        t0, t1;
    cls_thread_arg_t a0 = { 0, 0, 0, 0 };
    cls_thread_arg_t a1 = { 0, 1, 0, 0 };

    ensure_cls_keys();
    a0.key = g_cls_keys[5];
    a1.key = g_cls_keys[5];

    if (pthread_create(&t0, NULL, cls_thread_main, &a0) != 0 ||
        pthread_create(&t1, NULL, cls_thread_main, &a1) != 0) {
        fprintf(stderr, "FAIL: pthread_create for cls thread isolation\n");
        g_p0_failures++;
        return;
    }
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);

    p0_expect(__LINE__, "thread A ok", a0.ok, 1);
    p0_expect(__LINE__, "thread B ok", a1.ok, 1);
}

/* ------------------------------------------------------------------ *
 * Concurrent alloc — run in a fresh process via --cls-alloc-race
 * ------------------------------------------------------------------ */
#define CLS_RACE_THREADS 8
#define CLS_RACE_ITERS   32

typedef struct {
    co_cls_key keys[CLS_RACE_ITERS];
    int        count;
} cls_race_result_t;

static void *cls_alloc_race_thread(void *arg)
{
    cls_race_result_t *r = arg;
    int                i;

    for (i = 0; i < CLS_RACE_ITERS; ++i) {
        co_cls_key k = co_cls_alloc();
        if (k == CO_CLS_KEY_INVALID)
            break;
        r->keys[r->count++] = k;
    }
    return NULL;
}

void test_cls_alloc_race(void)
{
    pthread_t           threads[CLS_RACE_THREADS];
    cls_race_result_t   results[CLS_RACE_THREADS];
    int                 seen[CO_CLS_SLOTS];
    int                 total = 0;
    int                 t, i, k;

    memset(seen, 0, sizeof seen);
    memset(results, 0, sizeof results);

    for (t = 0; t < CLS_RACE_THREADS; ++t) {
        if (pthread_create(&threads[t], NULL, cls_alloc_race_thread,
                           &results[t]) != 0) {
            fprintf(stderr, "FAIL: pthread_create for cls alloc race\n");
            g_p0_failures++;
            return;
        }
    }
    for (t = 0; t < CLS_RACE_THREADS; ++t)
        pthread_join(threads[t], NULL);

    for (t = 0; t < CLS_RACE_THREADS; ++t) {
        for (i = 0; i < results[t].count; ++i) {
            k = results[t].keys[i];
            if (k < 0 || k >= CO_CLS_SLOTS) {
                fprintf(stderr, "FAIL: race key out of range: %d\n", k);
                g_p0_failures++;
                return;
            }
            if (seen[k]) {
                fprintf(stderr, "FAIL: duplicate cls key %d from race\n", k);
                g_p0_failures++;
                return;
            }
            seen[k] = 1;
            total++;
        }
    }

    p0_expect(__LINE__, "race total keys", total, CO_CLS_SLOTS);
    p0_expect(__LINE__, "race exhausted",
              co_cls_alloc(), CO_CLS_KEY_INVALID);
}
