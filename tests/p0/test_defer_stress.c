/*
 * Stress: co_thread_shutdown vs non-DONE, trampoline+destroy run_defers,
 * and co_defer slot-full residual.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#define ST_SUSP 32
#define ST_DONE 8
#define ST_READY 8
#define ST_NEST 64
#define ST_OOM 32

static void st_defer_inc(void *p)
{
    *(int *)p += 1;
}

static void fn_st_yield(coroutine *self, void *ud, void *in)
{
    (void)in;
    p0_expect(__LINE__, "st yield defer",
              co_defer(self, st_defer_inc, ud), CO_RESULT_OK);
    p0_expect(__LINE__, "st yield", P0_YIELD(), CO_RESULT_OK);
}

static void fn_st_done(coroutine *self, void *ud, void *in)
{
    (void)in;
    p0_expect(__LINE__, "st done defer",
              co_defer(self, st_defer_inc, ud), CO_RESULT_OK);
}

static void fn_st_never(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_p0_failures++;
}

/*
 * H1: shutdown 只 destroy DONE/READY；SUSPENDED 計入 leaked，defer 不跑。
 */
void test_shutdown_stress_non_done(void)
{
    coroutine *susp[ST_SUSP];
    coroutine *done[ST_DONE];
    coroutine *ready[ST_READY];
    int susp_ran[ST_SUSP];
    int done_ran[ST_DONE];
    int ready_ran[ST_READY];
    size_t leaked = 99;
    co_result r;
    int i;

    memset(susp_ran, 0, sizeof susp_ran);
    memset(done_ran, 0, sizeof done_ran);
    memset(ready_ran, 0, sizeof ready_ran);

    for (i = 0; i < ST_SUSP; i++) {
        susp[i] = co_create(CO_MIN_STACK_SIZE, fn_st_yield, &susp_ran[i]);
        if (!susp[i]) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "st susp resume", P0_RESUME(susp[i]), CO_RESULT_OK);
        p0_expect(__LINE__, "st susp not finished", co_finished(susp[i]), 0);
    }
    for (i = 0; i < ST_DONE; i++) {
        done[i] = co_create(CO_MIN_STACK_SIZE, fn_st_done, &done_ran[i]);
        if (!done[i]) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "st done resume", P0_RESUME(done[i]), CO_RESULT_OK);
        p0_expect(__LINE__, "st done finished", co_finished(done[i]), 1);
    }
    for (i = 0; i < ST_READY; i++) {
        ready[i] = co_create(CO_MIN_STACK_SIZE, fn_st_never, NULL);
        if (!ready[i]) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "st ready defer",
                  co_defer(ready[i], st_defer_inc, &ready_ran[i]),
                  CO_RESULT_OK);
    }

    r = co_thread_shutdown(&leaked);

    p0_expect(__LINE__, "shutdown non-done", r, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "leaked suspended", (int)leaked, ST_SUSP);

    for (i = 0; i < ST_SUSP; i++) {
        p0_expect(__LINE__, "susp defer not run", susp_ran[i], 0);
        p0_expect(__LINE__, "susp still live", co_finished(susp[i]), 0);
        p0_expect(__LINE__, "susp destroy blocked",
                  co_destroy(susp[i]), CO_RESULT_INVALID_STATE);
        p0_expect(__LINE__, "susp resume finish", P0_RESUME(susp[i]),
                  CO_RESULT_OK);
        p0_expect(__LINE__, "susp defer after trampoline", susp_ran[i], 1);
        p0_expect(__LINE__, "susp destroy after done",
                  co_destroy(susp[i]), CO_RESULT_OK);
        p0_expect(__LINE__, "susp defer once", susp_ran[i], 1);
    }
    for (i = 0; i < ST_DONE; i++)
        p0_expect(__LINE__, "done trampoline ran", done_ran[i], 1);
    for (i = 0; i < ST_READY; i++)
        p0_expect(__LINE__, "ready shutdown ran", ready_ran[i], 1);

    leaked = 99;
    p0_expect(__LINE__, "shutdown clean", co_thread_shutdown(&leaked),
              CO_RESULT_OK);
    p0_expect(__LINE__, "leaked cleared", (int)leaked, 0);
}

typedef struct {
    size_t leaked;
    co_result rc;
    int outer_ran;
    int inner_ran;
    int ready_ran;
    int done_ran;
} st_fiber_sd_t;

static void fn_st_inner_shutdown(coroutine *self, void *ud, void *in)
{
    st_fiber_sd_t *ctx = ud;

    (void)in;
    p0_expect(__LINE__, "inner defer",
              co_defer(self, st_defer_inc, &ctx->inner_ran), CO_RESULT_OK);
    ctx->rc = co_thread_shutdown(&ctx->leaked);
}

static void fn_st_outer_wait(coroutine *self, void *ud, void *in)
{
    st_fiber_sd_t *ctx = ud;
    coroutine *inner;

    (void)in;
    p0_expect(__LINE__, "outer defer",
              co_defer(self, st_defer_inc, &ctx->outer_ran), CO_RESULT_OK);
    inner = co_create(CO_MIN_STACK_SIZE, fn_st_inner_shutdown, ctx);
    if (!inner) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume inner shutdown", P0_RESUME(inner),
              CO_RESULT_OK);
    p0_expect(__LINE__, "inner finished", co_finished(inner), 1);
    p0_expect(__LINE__, "destroy inner", co_destroy(inner), CO_RESULT_OK);
}

/*
 * H1/H5: fiber 內呼叫 shutdown 時，RUNNING 自身與 WAITING 外層無法清理。
 */
void test_shutdown_from_fiber_non_done(void)
{
    st_fiber_sd_t ctx;
    coroutine *ready;
    coroutine *done;
    coroutine *outer;

    memset(&ctx, 0, sizeof ctx);
    ctx.leaked = 99;

    ready = co_create(CO_MIN_STACK_SIZE, fn_st_never, NULL);
    done = co_create(CO_MIN_STACK_SIZE, fn_st_done, &ctx.done_ran);
    if (!ready || !done) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "ready pre defer",
              co_defer(ready, st_defer_inc, &ctx.ready_ran), CO_RESULT_OK);
    p0_expect(__LINE__, "done resume", P0_RESUME(done), CO_RESULT_OK);

    outer = co_create(CO_MIN_STACK_SIZE, fn_st_outer_wait, &ctx);
    if (!outer) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume outer", P0_RESUME(outer), CO_RESULT_OK);

    p0_expect(__LINE__, "fiber shutdown rc", ctx.rc,
              CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "fiber leaked waiting+running", (int)ctx.leaked, 2);
    p0_expect(__LINE__, "ready cleaned by shutdown", ctx.ready_ran, 1);
    p0_expect(__LINE__, "done trampoline once", ctx.done_ran, 1);
    p0_expect(__LINE__, "inner trampoline after return", ctx.inner_ran, 1);
    p0_expect(__LINE__, "outer trampoline after return", ctx.outer_ran, 1);
    p0_expect(__LINE__, "destroy outer", co_destroy(outer), CO_RESULT_OK);
    p0_expect(__LINE__, "inner still once", ctx.inner_ran, 1);
    p0_expect(__LINE__, "outer still once", ctx.outer_ran, 1);
}

static int g_st_hits;

static void st_hit(void *p)
{
    (void)p;
    g_st_hits++;
}

static void fn_st_child_hits(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "child d1", co_defer(self, st_hit, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "child d2", co_defer(self, st_hit, NULL), CO_RESULT_OK);
}

static void fn_st_parent_hits(coroutine *self, void *ud, void *in)
{
    coroutine *child;

    (void)ud;
    (void)in;
    p0_expect(__LINE__, "parent d1", co_defer(self, st_hit, NULL),
              CO_RESULT_OK);
    child = co_create(CO_MIN_STACK_SIZE, fn_st_child_hits, NULL);
    if (!child) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume child", P0_RESUME(child), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy child", co_destroy(child), CO_RESULT_OK);
}

/*
 * H2/H3: child callback 返回後 trampoline run_defers；接著 destroy 第二次
 * run_defers 應為空。巢狀 fiber 不造成同一條 co 的 defer 跑兩次。
 */
void test_defer_second_run_stress(void)
{
    int i;

    g_st_hits = 0;
    for (i = 0; i < ST_NEST; i++) {
        coroutine *parent = co_create(CO_MIN_STACK_SIZE, fn_st_parent_hits,
                                      NULL);
        if (!parent) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "resume parent", P0_RESUME(parent), CO_RESULT_OK);
        p0_expect(__LINE__, "destroy parent", co_destroy(parent),
                  CO_RESULT_OK);
    }

    p0_expect(__LINE__, "hits exactly once", g_st_hits, ST_NEST * 3);
}

typedef struct {
    int slot_ran[CO_DEFER_SLOTS];
    int caller_freed;
    void *leftover;
    int oom_rc;
} st_oom_t;

static void fn_st_oom(coroutine *self, void *ud, void *in)
{
    st_oom_t *ctx = ud;
    unsigned i;

    (void)in;
    for (i = 0; i < CO_DEFER_SLOTS; i++) {
        p0_expect(__LINE__, "oom fill",
                  co_defer(self, st_defer_inc, &ctx->slot_ran[i]),
                  CO_RESULT_OK);
    }
    ctx->leftover = malloc(32);
    if (!ctx->leftover) {
        g_p0_failures++;
        return;
    }
    memset(ctx->leftover, 0x5A, 32);
    ctx->oom_rc = (int)co_defer(self, free, ctx->leftover);
    if (ctx->oom_rc != CO_RESULT_OK) {
        free(ctx->leftover);
        ctx->leftover = NULL;
        ctx->caller_freed = 1;
        return;
    }
}

/*
 * 槽滿：呼叫端在 co_defer 失敗後自行 free 再 return；已登記的 N 筆仍由
 * trampoline 執行。
 */
void test_defer_oom_residual_stress(void)
{
    int i;
    int freed_total = 0;
    int slot_total = 0;

    for (i = 0; i < ST_OOM; i++) {
        st_oom_t ctx;
        coroutine *co;
        unsigned s;

        memset(&ctx, 0, sizeof ctx);
        ctx.oom_rc = -999;
        co = co_create(CO_MIN_STACK_SIZE, fn_st_oom, &ctx);
        if (!co) {
            g_p0_failures++;
            return;
        }
        p0_expect(__LINE__, "resume oom", P0_RESUME(co), CO_RESULT_OK);
        p0_expect(__LINE__, "oom rc", ctx.oom_rc, CO_RESULT_OUT_OF_MEMORY);
        p0_expect(__LINE__, "caller freed leftover", ctx.caller_freed, 1);
        p0_expect_ptr(__LINE__, "leftover consumed", ctx.leftover, NULL);
        for (s = 0; s < CO_DEFER_SLOTS; s++) {
            p0_expect(__LINE__, "slot ran trampoline", ctx.slot_ran[s], 1);
            slot_total += ctx.slot_ran[s];
        }
        p0_expect(__LINE__, "destroy after oom", co_destroy(co), CO_RESULT_OK);
        freed_total += ctx.caller_freed;
    }

    p0_expect(__LINE__, "all slots ran", slot_total, ST_OOM * CO_DEFER_SLOTS);
    p0_expect(__LINE__, "all leftovers caller-freed", freed_total, ST_OOM);
}
