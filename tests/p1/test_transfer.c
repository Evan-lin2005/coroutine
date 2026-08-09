/*
 * P1: mailbox (resume/yield) + userdata + initial_input.
 * 訊息一律用真實物件指標，不做 integer → pointer。
 */

#include "p1_common.h"

static int g_mailbox_step;

/* 靜態訊息物件：指標本身即為傳遞值 */
static int msg_initial;
static int msg_out_100;
static int msg_wake_300;
static int msg_out_200;
static int msg_out_1;
static int msg_in_11;
static int msg_in_22;
static int msg_initial_7;

static void fn_mailbox_roundtrip(coroutine *self, void *userdata, void *initial_input)
{
    void *next1;
    void *next2;

    (void)self;
    (void)userdata;

    p0_expect_ptr(__LINE__, "initial_input", initial_input, &msg_initial);

    g_mailbox_step = 1;
    p0_expect(__LINE__, "yield rc",
              co_yield_now(&msg_out_100, &next1), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "next_input after first yield", next1, &msg_wake_300);

    p0_expect(__LINE__, "yield out rc",
              co_yield_now(&msg_out_200, &next2), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "next_input after second yield", next2, NULL);

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
              co_resume(co, &msg_initial, &output), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "yield output", output, &msg_out_100);
    p0_expect(__LINE__, "step after yield", g_mailbox_step, 1);

    p0_expect(__LINE__, "resume wake",
              co_resume(co, &msg_wake_300, &output), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "yield output 2", output, &msg_out_200);
    p0_expect(__LINE__, "step mid", g_mailbox_step, 1);

    p0_expect(__LINE__, "resume finish",
              co_resume(co, NULL, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "step done", g_mailbox_step, 2);
    p0_expect(__LINE__, "finished", co_finished(co), 1);
    p0_expect_ptr(__LINE__, "output on finish", output, NULL);

    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_userdata_and_initial(coroutine *self, void *userdata, void *initial_input)
{
    p0_expect_ptr(__LINE__, "self is current", self, co_current());
    p0_expect_ptr(__LINE__, "co_userdata matches",
                  co_userdata(self), userdata);
    p0_expect(__LINE__, "userdata value", *(int *)userdata, 99);
    p0_expect_ptr(__LINE__, "initial_input separate",
                  initial_input, &msg_initial_7);
}

void test_userdata_vs_initial_input(void)
{
    int userdata = 99;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_userdata_and_initial, &userdata);
    void *output = NULL;

    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect_ptr(__LINE__, "userdata readable before resume",
                  co_userdata(co), &userdata);
    p0_expect(__LINE__, "resume with initial_input",
              co_resume(co, &msg_initial_7, &output), CO_RESULT_OK);
    p0_expect(__LINE__, "userdata still", userdata, 99);
    p0_expect_ptr(__LINE__, "finish output null", output, NULL);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

/* 連續 resume 的 mailbox 讀後清空：第二次 resume 不殘留第一次訊息 */
static void *g_seen_initial;
static void *g_seen_next;
static char g_poison_initial;
static char g_poison_next;

static void fn_mailbox_cleared(coroutine *self, void *userdata, void *initial_input)
{
    void *next;

    (void)self;
    (void)userdata;
    g_seen_initial = initial_input;

    p0_expect(__LINE__, "yield",
              co_yield_now(&msg_out_1, &next), CO_RESULT_OK);
    g_seen_next = next;
}

void test_mailbox_cleared_after_read(void)
{
    void *output = NULL;

    g_seen_initial = &g_poison_initial;
    g_seen_next    = &g_poison_next;

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_mailbox_cleared, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume1",
              co_resume(co, &msg_in_11, &output), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "initial was 11", g_seen_initial, &msg_in_11);
    p0_expect_ptr(__LINE__, "output was 1", output, &msg_out_1);

    p0_expect(__LINE__, "resume2",
              co_resume(co, &msg_in_22, &output), CO_RESULT_OK);
    p0_expect_ptr(__LINE__, "next was 22 not stale 11", g_seen_next, &msg_in_22);
    p0_expect_ptr(__LINE__, "finished output null", output, NULL);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}
