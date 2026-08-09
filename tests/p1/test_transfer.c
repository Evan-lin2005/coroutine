/*
 * P1: mailbox (resume/yield) + userdata + initial_input.
 */

#include "p1_common.h"

#include <stdint.h>

static int g_mailbox_step;

static void fn_mailbox_roundtrip(coroutine *self, void *userdata, void *initial_input)
{
    void *next1;
    void *next2;

    (void)self;
    (void)userdata;

    p0_expect(__LINE__, "initial_input", (intptr_t)initial_input, 42);

    g_mailbox_step = 1;
    p0_expect(__LINE__, "yield rc",
              co_yield_now((void *)(intptr_t)100, &next1), CO_RESULT_OK);
    p0_expect(__LINE__, "next_input after first yield", (intptr_t)next1, 300);

    p0_expect(__LINE__, "yield out rc",
              co_yield_now((void *)(intptr_t)200, &next2), CO_RESULT_OK);
    p0_expect(__LINE__, "next_input after second yield", (intptr_t)next2, 0);

    g_mailbox_step = 2;
}

void test_transfer_roundtrip(void)
{
    void *output = NULL;

    g_mailbox_step = 0;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_mailbox_roundtrip, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume entry",
              co_resume(co, (void *)(intptr_t)42, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "yield output", (intptr_t)output, 100);
    p0_expect(__LINE__, "step after yield", g_mailbox_step, 1);

    p0_expect(__LINE__, "resume wake",
              co_resume(co, (void *)(intptr_t)300, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "yield output 2", (intptr_t)output, 200);
    p0_expect(__LINE__, "step mid", g_mailbox_step, 1);

    p0_expect(__LINE__, "resume finish",
              co_resume(co, NULL, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "step done", g_mailbox_step, 2);
    p0_expect(__LINE__, "finished", co_finished(co), 1);
    p0_expect(__LINE__, "output on finish", (intptr_t)output, 0);

    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_userdata_and_initial(coroutine *self, void *userdata, void *initial_input)
{
    p0_expect(__LINE__, "self is current",
              (intptr_t)self, (intptr_t)co_current());
    p0_expect(__LINE__, "co_userdata matches",
              (intptr_t)co_userdata(self), (intptr_t)userdata);
    p0_expect(__LINE__, "userdata value", *(int *)userdata, 99);
    p0_expect(__LINE__, "initial_input separate", (intptr_t)initial_input, 7);
}

void test_transfer_vs_create_argument(void)
{
    int userdata = 99;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_userdata_and_initial, &userdata);
    void *output = NULL;

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "userdata readable before resume",
              (intptr_t)co_userdata(co), (intptr_t)&userdata);
    p0_expect(__LINE__, "resume with initial_input",
              co_resume(co, (void *)(intptr_t)7, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "userdata still", userdata, 99);
    p0_expect(__LINE__, "finish output null", (intptr_t)output, 0);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

/* 連續 resume 的 mailbox 讀後清空：第二次 resume 不殘留第一次訊息 */
static void *g_seen_initial;
static void *g_seen_next;

static void fn_mailbox_cleared(coroutine *self, void *userdata, void *initial_input)
{
    void *next;

    (void)self;
    (void)userdata;
    g_seen_initial = initial_input;

    p0_expect(__LINE__, "yield",
              co_yield_now((void *)(intptr_t)1, &next), CO_RESULT_OK);
    g_seen_next = next;
}

void test_mailbox_cleared_after_read(void)
{
    void *output = NULL;

    g_seen_initial = (void *)(intptr_t)-1;
    g_seen_next    = (void *)(intptr_t)-1;

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_mailbox_cleared, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume1",
              co_resume(co, (void *)(intptr_t)11, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "initial was 11", (intptr_t)g_seen_initial, 11);
    p0_expect(__LINE__, "output was 1", (intptr_t)output, 1);

    p0_expect(__LINE__, "resume2",
              co_resume(co, (void *)(intptr_t)22, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "next was 22 not stale 11", (intptr_t)g_seen_next, 22);
    p0_expect(__LINE__, "finished output null", (intptr_t)output, 0);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}
