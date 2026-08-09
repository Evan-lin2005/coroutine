/*
 * P1: optional per-coroutine storage buffer.
 */

#include "p1_common.h"

#include <string.h>

typedef struct {
    coroutine *co;
    int        phase;
} storage_ctx_t;

static void fn_storage_persist(coroutine *self, void *userdata, void *initial_input)
{
    storage_ctx_t *ctx = userdata;
    unsigned char *buf = co_storage(self);

    (void)initial_input;
    p0_expect_ptr(__LINE__, "userdata is ctx", userdata, ctx);
    p0_expect_ptr(__LINE__, "self matches ctx.co", self, ctx->co);
    p0_expect(__LINE__, "storage ptr nonnull", buf != NULL, 1);
    p0_expect(__LINE__, "storage size", co_storage_size(self), 16);

    buf[0]  = 0xAB;
    buf[15] = 0xCD;
    ctx->phase = 1;

    p0_expect(__LINE__, "yield", P1_YIELD(), CO_RESULT_OK);

    p0_expect(__LINE__, "after yield byte0", buf[0], 0xAB);
    p0_expect(__LINE__, "after yield byte15", buf[15], 0xCD);
    ctx->phase = 2;
}

void test_storage_persist_across_yield(void)
{
    unsigned char backing[16];
    storage_ctx_t ctx = { NULL, 0 };
    coroutine    *co;

    memset(backing, 0, sizeof backing);
    co = co_create(CO_MIN_STACK_SIZE, fn_storage_persist, &ctx);
    if (!co) {
        g_p0_failures++;
        return;
    }
    ctx.co = co;

    p0_expect(__LINE__, "set storage",
              co_set_storage(co, backing, sizeof backing), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "query ptr", co_storage(co), backing);
    p0_expect(__LINE__, "query size", co_storage_size(co), 16);

    p0_expect(__LINE__, "resume", P1_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "phase after yield", ctx.phase, 1);

    p0_expect(__LINE__, "resume finish", P1_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "phase done", ctx.phase, 2);
    p0_expect(__LINE__, "backing intact", backing[0], 0xAB);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_nop(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
}

void test_storage_set_state_and_meta(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    unsigned char a[8];
    unsigned char b[8];

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect_ptr(__LINE__, "no storage ptr", co_storage(co), NULL);
    p0_expect(__LINE__, "no storage size", co_storage_size(co), 0);

    p0_expect(__LINE__, "buf cap0",
              co_set_storage(co, a, 0), CO_RESULT_INVALID_ARGUMENT);
    p0_expect(__LINE__, "null cap",
              co_set_storage(co, NULL, 8), CO_RESULT_INVALID_ARGUMENT);

    p0_expect(__LINE__, "bind a",
              co_set_storage(co, a, sizeof a), CO_RESULT_OK);
    p0_expect(__LINE__, "replace b",
              co_set_storage(co, b, sizeof b), CO_RESULT_OK);

    p0_expect(__LINE__, "clear",
              co_set_storage(co, NULL, 0), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "cleared ptr", co_storage(co), NULL);
    p0_expect(__LINE__, "cleared size", co_storage_size(co), 0);

    p0_expect(__LINE__, "resume blocks set",
              P1_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "set after run",
              co_set_storage(co, a, sizeof a), CO_RESULT_INVALID_STATE);

    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}
