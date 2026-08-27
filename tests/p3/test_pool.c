/*
 * P3a: stack pool — sequential reuse, size class, cap, thread-exit drain.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#  include <pthread.h>
#endif

static void fn_nop(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
}

static void fn_yield_once(coroutine *self, void *userdata, void *initial_input)
{
    int *n = userdata;

    (void)self;
    (void)initial_input;
    if (n)
        (*n)++;
    P0_YIELD();
    if (n)
        (*n)++;
}

static int create_run_destroy(size_t stack)
{
    coroutine *co = co_create(stack, fn_nop, NULL);

    if (!co)
        return -1;
    if (P0_RESUME(co) != CO_RESULT_OK || !co_finished(co)) {
        co_destroy(co);
        return -1;
    }
    if (co_destroy(co) != CO_RESULT_OK)
        return -1;
    return 0;
}

#if defined(__linux__)
static long read_vma_count(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char  line[512];
    long  n = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        n++;
    fclose(f);
    return n;
}
#endif

void test_pool_sequential_reuse(void)
{
    int i;

    for (i = 0; i < 64; i++) {
        if (create_run_destroy(CO_MIN_STACK_SIZE) != 0) {
            fprintf(stderr, "FAIL pool sequential 16K i=%d\n", i);
            g_p0_failures++;
            return;
        }
        if (create_run_destroy(CO_DEFAULT_STACK_SIZE) != 0) {
            fprintf(stderr, "FAIL pool sequential 64K i=%d\n", i);
            g_p0_failures++;
            return;
        }
    }
}

void test_pool_resume_after_reuse(void)
{
    int n = 0;
    coroutine *co;

    co = co_create(CO_MIN_STACK_SIZE, fn_yield_once, &n);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "first resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "first n", n, 1);
    p0_expect(__LINE__, "first destroy blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "second resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "second n", n, 2);
    p0_expect(__LINE__, "destroy done", co_destroy(co), CO_RESULT_OK);

    n = 0;
    co = co_create(CO_MIN_STACK_SIZE, fn_yield_once, &n);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "reused resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "reused n", n, 1);
    p0_expect(__LINE__, "reused second", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "reused n2", n, 2);
    p0_expect(__LINE__, "reused destroy", co_destroy(co), CO_RESULT_OK);
}

void test_pool_over_cap(void)
{
    enum { N = 12 };
    coroutine *co[N];
    int i;
    int v = 0;

    memset(co, 0, sizeof co);
    for (i = 0; i < N; i++) {
        co[i] = co_create(CO_MIN_STACK_SIZE, fn_nop, &v);
        if (!co[i]) {
            fprintf(stderr, "FAIL pool over-cap create i=%d\n", i);
            g_p0_failures++;
            break;
        }
        p0_expect(__LINE__, "over-cap resume", P0_RESUME(co[i]), CO_RESULT_OK);
    }
    for (i = 0; i < N; i++) {
        if (co[i])
            p0_expect(__LINE__, "over-cap destroy",
                      co_destroy(co[i]), CO_RESULT_OK);
    }
    for (i = 0; i < 8; i++) {
        if (create_run_destroy(CO_MIN_STACK_SIZE) != 0) {
            fprintf(stderr, "FAIL pool after cap i=%d\n", i);
            g_p0_failures++;
            return;
        }
    }
}

void test_pool_oversized_not_cached(void)
{
#if defined(__linux__)
    const size_t big = 600u * 1024u;
    long vma0, vma1, growth;

    (void)co_thread_shutdown(NULL);
    (void)co_current();
    if (create_run_destroy(big) != 0) {
        g_p0_failures++;
        return;
    }
    vma0 = read_vma_count();
    if (create_run_destroy(big) != 0) {
        g_p0_failures++;
        return;
    }
    vma1 = read_vma_count();
    growth = (vma0 >= 0 && vma1 >= 0) ? (vma1 - vma0) : 0;
    if (vma0 >= 0 && vma1 >= 0 && (growth > 2 || growth < -2)) {
        fprintf(stderr,
                "FAIL oversized pool: VMA growth %ld (vma0=%ld vma1=%ld)\n",
                growth, vma0, vma1);
        g_p0_failures++;
    }
#else
    if (create_run_destroy(600u * 1024u) != 0)
        g_p0_failures++;
#endif
}

void test_pool_vma_one_cached(void)
{
#if defined(__linux__)
    enum { N = 8, VMA_LO = 1, VMA_HI = 12 };
    long vma0, vma1, growth;
    int i;

    (void)co_thread_shutdown(NULL);
    (void)co_current();
    vma0 = read_vma_count();
    for (i = 0; i < N; i++) {
        if (create_run_destroy(CO_MIN_STACK_SIZE) != 0) {
            g_p0_failures++;
            return;
        }
    }
    vma1 = read_vma_count();
    growth = (vma0 >= 0 && vma1 >= 0) ? (vma1 - vma0) : 0;
    {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "{\"n\":%d,\"vma0\":%ld,\"vma1\":%ld,\"growth\":%ld}",
                 N, vma0, vma1, growth);
        p0_log("P3", "test_pool.c:test_pool_vma_one_cached",
               "sequential reuse VMA", buf);
    }
    if (vma0 >= 0 && vma1 >= 0 && (growth < VMA_LO || growth > VMA_HI)) {
        fprintf(stderr,
                "FAIL pool cache VMA growth %ld after %d sequential "
                "(vma0=%ld vma1=%ld; want %d..%d)\n",
                growth, N, vma0, vma1, VMA_LO, VMA_HI);
        g_p0_failures++;
    }
#else
    p0_log("P3", "test_pool.c:test_pool_vma_one_cached",
           "skipped on non-Linux", "{}");
#endif
}

#if !defined(_WIN32)
static void *pool_thread_worker(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 32; i++) {
        if (create_run_destroy(CO_MIN_STACK_SIZE) != 0)
            return (void *)(intptr_t)1;
    }
    return NULL;
}
#endif

void test_pool_thread_local(void)
{
#if defined(_WIN32)
    p0_log("P3", "test_pool.c:test_pool_thread_local",
           "skipped on Windows", "{}");
#else
    pthread_t t;
    void *ret = (void *)(intptr_t)-1;

    if (pthread_create(&t, NULL, pool_thread_worker, NULL) != 0) {
        g_p0_failures++;
        return;
    }
    if (create_run_destroy(CO_MIN_STACK_SIZE) != 0)
        g_p0_failures++;
    pthread_join(t, &ret);
    if (ret != NULL) {
        fprintf(stderr, "FAIL pool worker ret=%ld\n", (long)(intptr_t)ret);
        g_p0_failures++;
    }
#endif
}
