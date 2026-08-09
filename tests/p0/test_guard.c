/*
 * P0: guard page stack overflow detection.
 * Hypothesis H3 — writing past stack hits guard and aborts with library message.
 *
 * Runs overflow in a forked child so the parent test suite survives.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char k_guard_msg[] = "coroutine stack overflow (guard page hit)";

static __attribute__((noinline)) void fn_stack_overflow(coroutine *self,
                                                         void *userdata,
                                                         void *initial_input);
static __attribute__((noinline)) void fn_stack_overflow(coroutine *self,
                                                         void *userdata,
                                                         void *initial_input)
{
    (void)userdata;
    (void)initial_input;
    volatile char frame[4096];
    frame[0] = (char)(uintptr_t)frame;
    fn_stack_overflow(self, userdata, initial_input);
}

static int run_overflow_child(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_stack_overflow, NULL);
    if (!co)
        return 2;

    p0_log("H3", "test_guard.c:run_overflow_child", "resume into overflow co",
           "{}");
    co_resume(co, NULL, NULL); /* should not return */
    return 3;
}

void test_guard_overflow(void)
{
#ifndef __linux__
    fprintf(stderr, "SKIP test_guard_overflow: fork-based test needs POSIX\n");
    p0_log("H3", "test_guard.c:test_guard_overflow", "skipped non-linux", "{}");
    return;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        g_p0_failures++;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        g_p0_failures++;
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        _exit(run_overflow_child());
    }

    close(pipefd[1]);
    char buf[4096];
    size_t n = 0;
    ssize_t r;
    while (n + 1 < sizeof buf &&
           (r = read(pipefd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)r;
    close(pipefd[0]);
    buf[n] = '\0';

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        g_p0_failures++;
        return;
    }

    int crashed = (WIFSIGNALED(status) &&
                   (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) ||
                  (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGSEGV);
    int found_msg = strstr(buf, k_guard_msg) != NULL;

    p0_log("H3", "test_guard.c:test_guard_overflow", "child stderr captured",
           found_msg ? "{\"guard_msg\":true}" : "{\"guard_msg\":false}");

    char detail[256];
    snprintf(detail, sizeof detail,
             "{\"signaled\":%d,\"exited\":%d,\"exit_code\":%d,\"guard_msg\":%s}",
             WIFSIGNALED(status), WIFEXITED(status),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             found_msg ? "true" : "false");
    p0_log("H3", "test_guard.c:test_guard_overflow", "child exit status", detail);

    if (!crashed) {
        fprintf(stderr,
                "FAIL guard: child did not terminate on overflow (status=%d)\n",
                status);
        g_p0_failures++;
    }
    if (!found_msg) {
        fprintf(stderr, "FAIL guard: expected message in stderr:\n%s\n", buf);
        g_p0_failures++;
    }
#endif
}
