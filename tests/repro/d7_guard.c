#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "coroutine.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CO_MAX_TRACKED_STACKS
#  define CO_MAX_TRACKED_STACKS 4096
#endif

/*
 * D-7 regression: when the guard table is full, early registrations must
 * remain intact (no hash-slot eviction). Build with a small table, e.g.:
 *   -DCO_MAX_TRACKED_STACKS=8
 */

static __attribute__((noinline)) void burn(coroutine *self, void *ud, void *in)
{
    volatile char frame[4096];
    (void)ud;
    (void)in;
    frame[0] = 1;
    burn(self, ud, in);
    /* keep frame live across call to defeat sibling-call / TCO */
    frame[0] = (char)(frame[0] + 1);
}

static void idle(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

int main(void)
{
    coroutine **keepers;
    coroutine *victim;
    int i;
    int overfill;
    int pipefd[2];
    pid_t pid;
    int status;
    char buf[4096];
    size_t n = 0;
    ssize_t r;
    int found_ovf, found_out;

    /* victim + (CO_MAX_TRACKED_STACKS) keepers => table full then overfill */
    overfill = CO_MAX_TRACKED_STACKS;

    if (pipe(pipefd) != 0)
        return 2;

    pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        co_install_crash_handler(1);

        victim = co_create(CO_MIN_STACK_SIZE, burn, NULL);
        if (!victim)
            _exit(3);

        keepers = calloc((size_t)overfill, sizeof *keepers);
        if (!keepers)
            _exit(4);
        for (i = 0; i < overfill; i++) {
            keepers[i] = co_create(CO_MIN_STACK_SIZE, idle, NULL);
            if (!keepers[i]) {
                fprintf(stderr, "create fail at %d\n", i);
                _exit(5);
            }
        }
        fprintf(stderr, "victim first; overfill=%d (max=%d); overflowing\n",
                overfill, CO_MAX_TRACKED_STACKS);
        fflush(stderr);
        (void)co_resume(victim, NULL, NULL);
        _exit(6);
    }

    close(pipefd[1]);
    while (n + 1 < sizeof buf &&
           (r = read(pipefd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)r;
    close(pipefd[0]);
    buf[n] = '\0';
    waitpid(pid, &status, 0);

    found_ovf = strstr(buf, "coroutine stack overflow") != NULL;
    found_out = strstr(buf, "outside any coroutine guard page") != NULL;
    printf("max=%d overfill=%d exit=%d ovf_msg=%d outside_msg=%d\n",
           CO_MAX_TRACKED_STACKS, overfill,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           found_ovf, found_out);
    printf("stderr_snip: %.300s\n", buf);
    if (found_ovf && !found_out) {
        printf("D-7 PASS: overflow still diagnosed as guard page hit\n");
        return 0;
    }
    if (found_out) {
        printf("D-7 FAIL: overflow misreported as outside guard\n");
        return 1;
    }
    printf("D-7 inconclusive\n");
    return 2;
}
