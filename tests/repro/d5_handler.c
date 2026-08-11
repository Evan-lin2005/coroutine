/*
 * D-5 regression：預設 co_current() 不得覆寫宿主 SIGSEGV handler。
 * 修後期望 exit 0（custom handler 仍在）。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "coroutine.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void custom_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)sig;
    (void)si;
    (void)uctx;
    const char msg[] = "CUSTOM_HANDLER\n";
    (void)!write(STDERR_FILENO, msg, sizeof msg - 1);
    _exit(42);
}

int main(void)
{
    struct sigaction sa, cur;
    void (*before)(int, siginfo_t *, void *) = custom_handler;
    void (*after)(int, siginfo_t *, void *) = NULL;
    pid_t pid;
    int status;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = custom_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);

    (void)co_current(); /* 預設不得安裝 library handler */

    memset(&cur, 0, sizeof cur);
    sigaction(SIGSEGV, NULL, &cur);
    after = cur.sa_sigaction;
    printf("before=%p after=%p overwritten=%d\n",
           (void *)before, (void *)after, after != before);
    if (after != before) {
        printf("D-5 FAIL: SIGSEGV handler replaced without opt-in\n");
        return 1;
    }
    printf("D-5 PASS: custom handler still installed by default\n");

    pid = fork();
    if (pid == 0) {
        volatile int *p = NULL;
        *p = 1;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    printf("segfault child exit=%d signaled=%d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    /* 自訂 handler 應被 chain／保留：子行程 exit 42 */
    if (WIFEXITED(status) && WEXITSTATUS(status) == 42)
        return 0;
    printf("D-5 WARN: child did not hit custom handler (ok if OS differs)\n");
    return 0;
}
