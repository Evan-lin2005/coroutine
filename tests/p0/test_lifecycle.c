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
