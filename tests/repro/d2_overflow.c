/*
 * D-2 regression：極端 stack_size 不得在 create 內 SIGSEGV。
 * 修後期望：SIZE_MAX / SIZE_MAX-4094 → INVALID_ARGUMENT（CO_MAX 主防線）。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "coroutine.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void nop(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

static int run_one(size_t sz, const char *label, co_result want)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        coroutine *co = NULL;
        co_result r = co_create_ex(sz, nop, NULL, &co);
        if (r == want && co == NULL)
            _exit(0);
        fprintf(stderr, "%s: unexpected r=%d co=%p want=%d\n",
                label, (int)r, (void *)co, (int)want);
        _exit(1);
    }
    waitpid(pid, &status, 0);
    printf("CASE %s size=%zu ", label, sz);
    if (WIFSIGNALED(status)) {
        printf("CRASH_SIGNAL sig=%d FAIL\n", WTERMSIG(status));
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("OK (no crash, got want=%d)\n", (int)want);
        return 0;
    }
    printf("FAIL exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 1;
}

int main(void)
{
    int failed = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    failed |= run_one(SIZE_MAX, "SIZE_MAX", CO_RESULT_INVALID_ARGUMENT);
    failed |= run_one(SIZE_MAX - 4094, "SIZE_MAX-4094", CO_RESULT_INVALID_ARGUMENT);
    failed |= run_one(SIZE_MAX - 4095, "SIZE_MAX-4095", CO_RESULT_INVALID_ARGUMENT);
    return failed ? 1 : 0;
}
