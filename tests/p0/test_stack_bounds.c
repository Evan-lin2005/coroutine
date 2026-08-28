/*
 * D-2 / D-3：stack_size 上下界 — create 不得因參數 SIGSEGV；超大值拒絕。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdint.h>
#include <stdio.h>

static void fn_nop(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

#if defined(__linux__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>

#if !CO_TEST_TSAN
/* fork 子行程：確認極端 size 回錯誤碼而非 crash */
static int create_ex_in_child(size_t sz, co_result *out_r)
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    co_result rbuf = (co_result)-1;

    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        coroutine *co = NULL;
        co_result r = co_create_ex(sz, fn_nop, NULL, &co);
        close(pipefd[0]);
        (void)!write(pipefd[1], &r, sizeof r);
        close(pipefd[1]);
        if (co)
            (void)co_destroy(co);
        _exit(0);
    }
    close(pipefd[1]);
    if (read(pipefd[0], &rbuf, sizeof rbuf) != (ssize_t)sizeof rbuf)
        rbuf = (co_result)-1;
    close(pipefd[0]);
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status))
        return WTERMSIG(status); /* >0 signal */
    if (out_r)
        *out_r = rbuf;
    return 0;
}
#endif /* !CO_TEST_TSAN */
#endif /* linux/apple */

void test_stack_bounds(void)
{
    coroutine *co = NULL;
    co_result r;

    /* D-3：1<<46 必須 INVALID_ARGUMENT */
    r = co_create_ex((size_t)1 << 46, fn_nop, NULL, &co);
    p0_expect(__LINE__, "1<<46 invalid", r, CO_RESULT_INVALID_ARGUMENT);
    p0_expect_ptr(__LINE__, "1<<46 out null", co, NULL);

    /* CO_MAX + 1 */
    r = co_create_ex(CO_MAX_STACK_SIZE + 1, fn_nop, NULL, &co);
    p0_expect(__LINE__, "MAX+1 invalid", r, CO_RESULT_INVALID_ARGUMENT);

    /* 合法下界仍可用 */
    r = co_create_ex(CO_MIN_STACK_SIZE, fn_nop, NULL, &co);
    p0_expect(__LINE__, "MIN create ok", r, CO_RESULT_OK);
    if (co) {
        p0_expect(__LINE__, "MIN resume", P0_RESUME(co), CO_RESULT_OK);
        p0_expect(__LINE__, "MIN destroy", co_destroy(co), CO_RESULT_OK);
    }

#if defined(__linux__) || defined(__APPLE__)
#if CO_TEST_TSAN
    r = co_create_ex(SIZE_MAX, fn_nop, NULL, &co);
    p0_expect(__LINE__, "SIZE_MAX invalid", r, CO_RESULT_INVALID_ARGUMENT);
    r = co_create_ex(SIZE_MAX - 4094, fn_nop, NULL, &co);
    p0_expect(__LINE__, "SIZE_MAX-4094 invalid", r, CO_RESULT_INVALID_ARGUMENT);
#else
    {
        co_result child_r = (co_result)-1;
        int sig;

        /* D-2：SIZE_MAX / SIZE_MAX-4094 不得 crash（exit 139） */
        sig = create_ex_in_child(SIZE_MAX, &child_r);
        if (sig > 0) {
            fprintf(stderr, "FAIL SIZE_MAX crashed with signal %d\n", sig);
            g_p0_failures++;
        } else {
            p0_expect(__LINE__, "SIZE_MAX invalid", child_r,
                      CO_RESULT_INVALID_ARGUMENT);
        }

        child_r = (co_result)-1;
        sig = create_ex_in_child(SIZE_MAX - 4094, &child_r);
        if (sig > 0) {
            fprintf(stderr, "FAIL SIZE_MAX-4094 crashed with signal %d\n", sig);
            g_p0_failures++;
        } else {
            p0_expect(__LINE__, "SIZE_MAX-4094 invalid", child_r,
                      CO_RESULT_INVALID_ARGUMENT);
        }
    }
#endif
#else
    r = co_create_ex(SIZE_MAX, fn_nop, NULL, &co);
    p0_expect(__LINE__, "SIZE_MAX invalid", r, CO_RESULT_INVALID_ARGUMENT);
#endif
}
