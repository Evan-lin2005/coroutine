/*
 * P0: nested resume depth + mass create/destroy lifecycle.
 * Hypotheses:
 *   H4 — deep nested resume chains preserve CO_WAITING / resume semantics
 *   H5 — 10k+ short-lived coroutines create/resume/destroy without failure
 */

#include "p0_common.h"

#include <stdio.h>
#include <string.h>

#define NEST_DEPTH 32
#define MASS_COUNT 10000

static long read_vmrss_kb(void)
{
#if defined(__linux__)
    FILE *f = fopen("/proc/self/status", "r");
    char  line[256];
    long  kb = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
#else
    return -1;
#endif
}

typedef struct {
    int   depth;
    int   target;
    int  *done;
} nest_ctx_t;

static void fn_nest(coroutine *self, void *userdata, void *initial_input)
{
    nest_ctx_t *ctx = userdata;

    (void)self;
    (void)initial_input;

    if (ctx->depth < ctx->target) {
        nest_ctx_t child = {
            .depth  = ctx->depth + 1,
            .target = ctx->target,
            .done   = ctx->done,
        };
        coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nest, &child);
        if (!co) {
            (*ctx->done) = -1;
            return;
        }
        p0_expect(__LINE__, "nested resume", P0_RESUME(co), CO_RESULT_OK);
        p0_expect(__LINE__, "nested finished", co_finished(co), 1);
        p0_expect(__LINE__, "nested destroy", co_destroy(co), CO_RESULT_OK);
    } else {
        (*ctx->done)++;
        /* Deepest leaf returns normally — no yield (yield would leave child SUSPENDED). */
    }
}

void test_nested_depth(void)
{
    int done = 0;
    nest_ctx_t root = { .depth = 0, .target = NEST_DEPTH, .done = &done };

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nest, &root);
    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_log("H4", "test_lifecycle.c:test_nested_depth", "start nested chain",
           "{\"depth\":32}");

    p0_expect(__LINE__, "root resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "done count", done, 1);
    p0_expect(__LINE__, "root finished", co_finished(co), 1);
    p0_expect(__LINE__, "root destroy", co_destroy(co), CO_RESULT_OK);

    p0_log("H4", "test_lifecycle.c:test_nested_depth", "nested chain finished",
           done == 1 && !g_p0_failures ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void fn_mass(coroutine *self, void *userdata, void *initial_input)
{
    int *v = userdata;

    (void)self;
    (void)initial_input;
    (*v)++;
}

void test_mass_lifecycle(void)
{
    int i;
    int failures_before = g_p0_failures;
    long rss_before = read_vmrss_kb();
    long rss_after  = -1;

    char rss_buf[96];
    snprintf(rss_buf, sizeof rss_buf, "{\"rss_before_kb\":%ld}", rss_before);
    p0_log("H5", "test_lifecycle.c:test_mass_lifecycle", "mass lifecycle start",
           rss_buf);

    for (i = 0; i < MASS_COUNT; i++) {
        int v = 0;
        coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_mass, &v);
        if (!co) {
            fprintf(stderr, "FAIL mass: co_create at i=%d\n", i);
            g_p0_failures++;
            break;
        }
        if (P0_RESUME(co) != CO_RESULT_OK || v != 1 || !co_finished(co)) {
            fprintf(stderr, "FAIL mass: resume/finish at i=%d v=%d\n", i, v);
            g_p0_failures++;
            co_destroy(co);
            break;
        }
        if (co_destroy(co) != CO_RESULT_OK) {
            fprintf(stderr, "FAIL mass: destroy at i=%d\n", i);
            g_p0_failures++;
            break;
        }
    }

    rss_after = read_vmrss_kb();
    char buf[128];
    snprintf(buf, sizeof buf,
             "{\"completed\":%d,\"new_failures\":%d,\"rss_before_kb\":%ld,"
             "\"rss_after_kb\":%ld}",
             i, g_p0_failures - failures_before, rss_before, rss_after);
    p0_log("H5", "test_lifecycle.c:test_mass_lifecycle", "mass lifecycle done",
           buf);

    p0_expect(__LINE__, "mass count", i, MASS_COUNT);
}

/* ------------------------------------------------------------------ *
 * CO_WAITING 重入：巢狀 resume 時外層為 WAITING，不可再 resume / destroy
 * ------------------------------------------------------------------ */
static coroutine *g_wait_outer;

static void fn_waiting_inner(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;

    p0_log("H4", "test_lifecycle.c:fn_waiting_inner", "try resume waiting outer",
           "{}");
    p0_expect(__LINE__, "resume waiting outer",
              P0_RESUME(g_wait_outer), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy waiting outer",
              co_destroy(g_wait_outer), CO_RESULT_INVALID_STATE);
}

static void fn_waiting_outer(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
    coroutine *inner = co_create(CO_MIN_STACK_SIZE, fn_waiting_inner, NULL);
    if (!inner) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume inner", P0_RESUME(inner), CO_RESULT_OK);
    p0_expect(__LINE__, "inner finished", co_finished(inner), 1);
    p0_expect(__LINE__, "destroy inner", co_destroy(inner), CO_RESULT_OK);
}

void test_waiting_reentry(void)
{
    g_wait_outer = co_create(CO_MIN_STACK_SIZE, fn_waiting_outer, NULL);
    if (!g_wait_outer) {
        g_p0_failures++;
        return;
    }

    p0_log("H4", "test_lifecycle.c:test_waiting_reentry", "start waiting test", "{}");

    p0_expect(__LINE__, "resume outer", P0_RESUME(g_wait_outer), CO_RESULT_OK);
    p0_expect(__LINE__, "outer finished", co_finished(g_wait_outer), 1);
    p0_expect(__LINE__, "destroy outer", co_destroy(g_wait_outer), CO_RESULT_OK);

    p0_log("H4", "test_lifecycle.c:test_waiting_reentry", "waiting reentry finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
}

/* ------------------------------------------------------------------ *
 * D-1：owner 跨世代 — join 後新執行緒不得 resume 舊協程（TLS 位址可重用）
 * D-1b：co_thread_shutdown + thread-exit orphan reclaim
 * ------------------------------------------------------------------ */
static void fn_orphan_yield(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    (void)P0_YIELD();
}

void test_orphan_shutdown_ok(void)
{
    size_t     leaked = 99;
    co_result  r;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_orphan_yield, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to yield", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "resume to done", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "finished", co_finished(co), 1);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);

    r = co_thread_shutdown(&leaked);
    p0_expect(__LINE__, "shutdown ok", r, CO_RESULT_OK);
    p0_expect(__LINE__, "leaked count 0", (int)leaked, 0);
}

void test_orphan_shutdown_warns(void)
{
    size_t     leaked = 0;
    co_result  r;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_orphan_yield, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy suspended", co_destroy(co),
              CO_RESULT_INVALID_STATE);

    r = co_thread_shutdown(&leaked);
    p0_expect(__LINE__, "shutdown warns", r, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "leaked count 1", (int)leaked, 1);

    /* 清理：resume 完成後再 destroy，避免污染後續測／主執行緒 exit */
    p0_expect(__LINE__, "resume finish", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after done", co_destroy(co), CO_RESULT_OK);
    leaked = 99;
    p0_expect(__LINE__, "shutdown clean", co_thread_shutdown(&leaked),
              CO_RESULT_OK);
    p0_expect(__LINE__, "leaked cleared", (int)leaked, 0);
}

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <stdint.h>

static coroutine *g_d1_co;
static volatile int g_d1_step;
static volatile int g_d1_ready;
static volatile int g_d1_done;

static void fn_d1_owner(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_d1_step = 1;
    (void)P0_YIELD();
    g_d1_step = 2;
}

static void *d1_owner_thread(void *arg)
{
    (void)arg;
    g_d1_co = co_create(CO_MIN_STACK_SIZE, fn_d1_owner, NULL);
    if (!g_d1_co) {
        g_p0_failures++;
        g_d1_ready = 1;
        return NULL;
    }
    if (P0_RESUME(g_d1_co) != CO_RESULT_OK)
        g_p0_failures++;
    /* owner 仍存活：讓另一執行緒探測 WRONG_THREAD（避免 exit reclaim 造成 UAF） */
    g_d1_ready = 1;
    while (!g_d1_done) {
        /* spin */
    }
    if (g_d1_co) {
        (void)P0_RESUME(g_d1_co);
        (void)co_destroy(g_d1_co);
        g_d1_co = NULL;
    }
    return NULL;
}

static void *d1_reuse_thread(void *arg)
{
    co_result r;
    (void)arg;
    while (!g_d1_ready) {
    }
    if (!g_d1_co) {
        g_d1_done = 1;
        return NULL;
    }
    r = co_resume(g_d1_co, NULL, NULL);
    p0_expect(__LINE__, "cross-thread resume", r, CO_RESULT_WRONG_THREAD);
    p0_expect(__LINE__, "step still after first yield", g_d1_step, 1);
    r = co_destroy(g_d1_co);
    p0_expect(__LINE__, "cross-thread destroy", r, CO_RESULT_WRONG_THREAD);
    g_d1_done = 1;
    return NULL;
}

void test_owner_cross_generation(void)
{
    pthread_t a, b;

    g_d1_step = 0;
    g_d1_co = NULL;
    g_d1_ready = 0;
    g_d1_done = 0;

    p0_log("D1", "test_lifecycle.c:test_owner_cross_generation",
           "start cross-thread owner test", "{}");

    if (pthread_create(&a, NULL, d1_owner_thread, NULL) != 0) {
        g_p0_failures++;
        return;
    }
    if (pthread_create(&b, NULL, d1_reuse_thread, NULL) != 0) {
        g_d1_done = 1;
        pthread_join(a, NULL);
        g_p0_failures++;
        return;
    }
    pthread_join(b, NULL);
    pthread_join(a, NULL);

    p0_log("D1", "test_lifecycle.c:test_owner_cross_generation",
           "cross-thread owner finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
}

static void *orphan_exit_worker(void *arg)
{
    coroutine *co;
    (void)arg;
    co = co_create(CO_MIN_STACK_SIZE, fn_orphan_yield, NULL);
    if (!co)
        return (void *)(intptr_t)1;
    if (P0_RESUME(co) != CO_RESULT_OK)
        return (void *)(intptr_t)2;
    /* 故意不 shutdown、不 destroy：依賴 thread-exit reclaim */
    return NULL;
}

void test_orphan_thread_exit_reclaim(void)
{
    enum { N = 40 };
    long rss0, rss1, growth;
    int i;
    int v = 0;
    coroutine *co;

    rss0 = read_vmrss_kb();
    for (i = 0; i < N; i++) {
        pthread_t t;
        void *ret = (void *)(intptr_t)-1;
        if (pthread_create(&t, NULL, orphan_exit_worker, NULL) != 0) {
            g_p0_failures++;
            return;
        }
        pthread_join(t, &ret);
        if (ret != NULL) {
            fprintf(stderr, "FAIL orphan worker i=%d ret=%ld\n",
                    i, (long)(intptr_t)ret);
            g_p0_failures++;
            return;
        }
    }
    rss1 = read_vmrss_kb();
    growth = (rss0 >= 0 && rss1 >= 0) ? (rss1 - rss0) : 0;

    {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "{\"n\":%d,\"rss0_kb\":%ld,\"rss1_kb\":%ld,\"growth_kb\":%ld}",
                 N, rss0, rss1, growth);
        p0_log("D1b", "test_lifecycle.c:test_orphan_thread_exit_reclaim",
               "rss after orphan exits", buf);
    }
    if (rss0 >= 0 && rss1 >= 0 && growth > (long)(N * 8)) {
        fprintf(stderr,
                "FAIL orphan reclaim: rss growth %ld KiB after %d exits "
                "(suspect mmap leak)\n",
                growth, N);
        g_p0_failures++;
    }

    co = co_create(CO_MIN_STACK_SIZE, fn_mass, &v);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "post-reclaim resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "post-reclaim destroy", co_destroy(co), CO_RESULT_OK);
}
#else
void test_owner_cross_generation(void)
{
    p0_log("D1", "test_lifecycle.c:test_owner_cross_generation",
           "skipped on non-POSIX", "{}");
}

void test_orphan_thread_exit_reclaim(void)
{
    p0_log("D1b", "test_lifecycle.c:test_orphan_thread_exit_reclaim",
           "skipped on non-POSIX", "{}");
}
#endif
