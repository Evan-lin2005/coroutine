/*
 * P0: co_cancel + CO_CANCEL sentinel
 */

#include "p0_common.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ *
 * co_is_cancel
 * ------------------------------------------------------------------ */
void test_cancel_sentinel(void)
{
    p0_expect(__LINE__, "is_cancel NULL", co_is_cancel(NULL), 0);
    p0_expect(__LINE__, "is_cancel CO_CANCEL", co_is_cancel(CO_CANCEL), 1);
}

/* ------------------------------------------------------------------ *
 * READY / DONE
 * ------------------------------------------------------------------ */
static void fn_never_run(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_p0_failures++;
}

void test_cancel_ready(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "cancel ready", co_cancel(co), CO_RESULT_OK);
}

static void fn_done_quick(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

void test_cancel_done(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_done_quick, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to done", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel done", co_cancel(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * SUSPENDED — cooperative cancel
 * ------------------------------------------------------------------ */
typedef struct {
    int cleaned;
    int cancel_pass;
} cancel_ctx_t;

static void fn_cooperative_cancel(coroutine *self, void *ud, void *in)
{
    cancel_ctx_t *ctx = ud;
    void         *cmd = in;

    (void)self;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd)) {
        ctx->cleaned = 1;
        return;
    }
    g_p0_failures++;
}

void test_cancel_suspended_ok(void)
{
    cancel_ctx_t ctx = {0};
    coroutine   *co  = co_create(CO_MIN_STACK_SIZE, fn_cooperative_cancel, &ctx);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel suspended", co_cancel(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cleanup ran", ctx.cleaned, 1);
}

/* ------------------------------------------------------------------ *
 * SUSPENDED — ignored cancel (yield after sentinel)
 * ------------------------------------------------------------------ */
static void fn_ignore_cancel(coroutine *self, void *ud, void *in)
{
    void *cmd = in;

    (void)self;
    (void)ud;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd))
        (void)co_yield_now(NULL, &cmd); /* 違約：看到 cancel 後再 yield */
    else
        g_p0_failures++;
}

void test_cancel_ignored(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_ignore_cancel, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "destroy still blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "resume finish", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after finish", co_destroy(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * Second cancel after ignore — escalate (no second sentinel)
 * debug：abort + 印出協程；release：CANCEL_IGNORED、不可回收、計入 leaked
 * ------------------------------------------------------------------ */
#ifdef NDEBUG
static void fn_ignore_cancel_count(coroutine *self, void *ud, void *in)
{
    cancel_ctx_t *ctx = ud;
    void         *cmd = in;

    (void)self;
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (!co_is_cancel(cmd)) {
        g_p0_failures++;
        return;
    }
    ctx->cancel_pass++;
    p0_expect(__LINE__, "ignore yield", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd))
        ctx->cancel_pass++;
}

void test_cancel_retry_after_ignore(void)
{
    cancel_ctx_t ctx = {0};
    size_t       leaked = 99;
    coroutine   *co  = co_create(CO_MIN_STACK_SIZE, fn_ignore_cancel_count, &ctx);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume to suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "first cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "one sentinel", ctx.cancel_pass, 1);
    p0_expect(__LINE__, "second cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "no second sentinel", ctx.cancel_pass, 1);
    p0_expect(__LINE__, "destroy still blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "shutdown leaked",
              co_thread_shutdown(&leaked), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "leaked count 1", (int)leaked, 1);
    p0_expect(__LINE__, "resume finish", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after finish", co_destroy(co), CO_RESULT_OK);
    leaked = 99;
    p0_expect(__LINE__, "shutdown clean", co_thread_shutdown(&leaked),
              CO_RESULT_OK);
    p0_expect(__LINE__, "leaked cleared", (int)leaked, 0);
}
#elif defined(__linux__) || defined(__APPLE__)
static const char k_cancel_ignored_msg[] = "coroutine: co_cancel ignored:";

void test_cancel_retry_after_ignore(void)
{
    int   pipefd[2];
    pid_t pid;
    char  buf[2048];
    size_t n = 0;
    ssize_t r;
    int   status = 0;
    int   aborted;
    int   found_msg;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        g_p0_failures++;
        return;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        g_p0_failures++;
        return;
    }

    if (pid == 0) {
        coroutine *co;

        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);

        co = co_create(CO_MIN_STACK_SIZE, fn_ignore_cancel, NULL);
        if (!co)
            _exit(2);
        if (P0_RESUME(co) != CO_RESULT_OK)
            _exit(3);
        if (co_cancel(co) != CO_RESULT_CANCEL_IGNORED)
            _exit(4);
        (void)co_cancel(co); /* debug：應 abort */
        _exit(5);
    }

    close(pipefd[1]);
    while (n + 1 < sizeof buf &&
           (r = read(pipefd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)r;
    close(pipefd[0]);
    buf[n] = '\0';

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        g_p0_failures++;
        return;
    }

    aborted = (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) ||
              (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGABRT);
    found_msg = strstr(buf, k_cancel_ignored_msg) != NULL;

    if (!aborted) {
        fprintf(stderr,
                "FAIL cancel-retry: child did not abort (status=%d)\n",
                status);
        g_p0_failures++;
    }
    if (!found_msg) {
        fprintf(stderr,
                "FAIL cancel-retry: expected coroutine dump in stderr:\n%s\n",
                buf);
        g_p0_failures++;
    }
}
#else
void test_cancel_retry_after_ignore(void)
{
    p0_log("SKIP", "test_cancel.c:test_cancel_retry_after_ignore",
           "debug abort test skipped (no fork)", "{}");
}
#endif

/* ------------------------------------------------------------------ *
 * RUNNING — cancel self inside callback
 * ------------------------------------------------------------------ */
static void fn_cancel_self(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "cancel self",
              co_cancel(self), CO_RESULT_ALREADY_RUNNING);
}

void test_cancel_running(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_cancel_self, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume running", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy after run", co_destroy(co), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * WAITING — inner tries to cancel outer
 * ------------------------------------------------------------------ */
static coroutine *g_cancel_wait_outer;

static void fn_cancel_waiting_inner(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "cancel waiting outer",
              co_cancel(g_cancel_wait_outer), CO_RESULT_INVALID_STATE);
}

static void fn_cancel_waiting_outer(coroutine *self, void *ud, void *in)
{
    coroutine *inner;

    (void)self;
    (void)ud;
    (void)in;
    inner = co_create(CO_MIN_STACK_SIZE, fn_cancel_waiting_inner, NULL);
    if (!inner) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume inner", P0_RESUME(inner), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy inner", co_destroy(inner), CO_RESULT_OK);
}

void test_cancel_waiting(void)
{
    g_cancel_wait_outer = co_create(CO_MIN_STACK_SIZE, fn_cancel_waiting_outer, NULL);
    if (!g_cancel_wait_outer) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume outer", P0_RESUME(g_cancel_wait_outer), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy outer", co_destroy(g_cancel_wait_outer), CO_RESULT_OK);
}

/* ------------------------------------------------------------------ *
 * NULL / wrong thread
 * ------------------------------------------------------------------ */
void test_cancel_null(void)
{
    p0_expect(__LINE__, "cancel null", co_cancel(NULL), CO_RESULT_INVALID_ARGUMENT);
}

#if defined(__linux__) || defined(__APPLE__)
static coroutine *g_cancel_wrong_co;
static pthread_barrier_t g_cancel_wrong_barrier;

static void *cancel_wrong_worker(void *arg)
{
    co_result r;

    (void)arg;
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    r = co_cancel(g_cancel_wrong_co);
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    return (void *)(intptr_t)r;
}

void test_cancel_wrong_thread(void)
{
    pthread_t t;
    void     *ret;
    co_result r;

    g_cancel_wrong_co = co_create(CO_MIN_STACK_SIZE, fn_never_run, NULL);
    if (!g_cancel_wrong_co) {
        g_p0_failures++;
        return;
    }
    if (pthread_barrier_init(&g_cancel_wrong_barrier, NULL, 2) != 0) {
        g_p0_failures++;
        (void)co_destroy(g_cancel_wrong_co);
        return;
    }
    if (pthread_create(&t, NULL, cancel_wrong_worker, NULL) != 0) {
        g_p0_failures++;
        pthread_barrier_destroy(&g_cancel_wrong_barrier);
        (void)co_destroy(g_cancel_wrong_co);
        return;
    }
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    pthread_barrier_wait(&g_cancel_wrong_barrier);
    pthread_join(t, &ret);
    pthread_barrier_destroy(&g_cancel_wrong_barrier);

    r = (co_result)(intptr_t)ret;
    p0_expect(__LINE__, "wrong thread", r, CO_RESULT_WRONG_THREAD);
    p0_expect(__LINE__, "destroy after wrong thread",
              co_destroy(g_cancel_wrong_co), CO_RESULT_OK);
}
#else
void test_cancel_wrong_thread(void)
{
    p0_log("SKIP", "test_cancel.c:test_cancel_wrong_thread",
           "pthread barrier test skipped", "{}");
}
#endif
