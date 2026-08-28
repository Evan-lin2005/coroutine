/*
 * P0: co_defer — registration and three execution paths (D-10)
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static int g_defer_order[8];
static int g_defer_order_n;
static int g_defer_runs;
static int g_ctx_cleaned;
static int g_yield_rejected;

static void defer_record(void *p)
{
    if (g_defer_order_n < (int)(sizeof g_defer_order / sizeof g_defer_order[0]))
        g_defer_order[g_defer_order_n++] = (int)(intptr_t)p;
    g_defer_runs++;
}

static void defer_mark(void *p)
{
    (void)p;
    g_defer_runs++;
}

static void defer_try_yield(void *p)
{
    (void)p;
    if (co_yield_now(NULL, NULL) == CO_RESULT_INVALID_STATE)
        g_yield_rejected = 1;
}

static void destroy_ctx(void *p)
{
    (void)p;
    g_ctx_cleaned = 1;
}

static void fn_defer_lifo(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "defer 1", co_defer(self, defer_record, (void *)(intptr_t)1),
              CO_RESULT_OK);
    p0_expect(__LINE__, "defer 2", co_defer(self, defer_record, (void *)(intptr_t)2),
              CO_RESULT_OK);
    p0_expect(__LINE__, "defer 3", co_defer(self, defer_record, (void *)(intptr_t)3),
              CO_RESULT_OK);
}

void test_defer_lifo(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_defer_lifo, NULL);

    g_defer_order_n = 0;
    g_defer_runs    = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume lifo", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "count after trampoline", (int)co_defer_count(co), 0);
    p0_expect(__LINE__, "runs once path", g_defer_runs, 3);
    p0_expect(__LINE__, "lifo 0", g_defer_order[0], 3);
    p0_expect(__LINE__, "lifo 1", g_defer_order[1], 2);
    p0_expect(__LINE__, "lifo 2", g_defer_order[2], 1);
    p0_expect(__LINE__, "destroy after trampoline",
              co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "runs still 3", g_defer_runs, 3);
}

static void fn_defer_once(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "register once",
              co_defer(self, defer_mark, NULL), CO_RESULT_OK);
}

void test_defer_exactly_once(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_defer_once, NULL);

    g_defer_runs = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume once", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "count zero", (int)co_defer_count(co), 0);
    p0_expect(__LINE__, "ran in trampoline", g_defer_runs, 1);
    p0_expect(__LINE__, "destroy noop defer",
              co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "still one run", g_defer_runs, 1);
}

static void fn_never_body(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_p0_failures++;
}

void test_defer_ready_cancel(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_body, NULL);
    int dummy = 0;

    g_ctx_cleaned = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "ready pre-register",
              co_defer(co, destroy_ctx, &dummy), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel not started",
              co_cancel(co), CO_RESULT_CANCEL_NOT_STARTED);
    p0_expect(__LINE__, "defer pending", (int)co_defer_count(co), 1);
    p0_expect(__LINE__, "destroy runs defer",
              co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "ctx cleaned", g_ctx_cleaned, 1);
}

static void fn_ignore_then_finish(coroutine *self, void *ud, void *in)
{
    void *cmd = in;

    (void)self;
    (void)ud;
    p0_expect(__LINE__, "defer before yield",
              co_defer(self, defer_mark, (void *)(intptr_t)42), CO_RESULT_OK);
    p0_expect(__LINE__, "yield once", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (!co_is_cancel(cmd)) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "ignore yield", co_yield_now(NULL, &cmd), CO_RESULT_OK);
    if (co_is_cancel(cmd))
        return;
    g_p0_failures++;
}

void test_defer_cancel_ignored(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_ignore_then_finish, NULL);

    g_defer_runs = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "count before cancel", (int)co_defer_count(co), 1);
    p0_expect(__LINE__, "cancel ignored",
              co_cancel(co), CO_RESULT_CANCEL_IGNORED);
    p0_expect(__LINE__, "count unchanged", (int)co_defer_count(co), 1);
    p0_expect(__LINE__, "defer not run", g_defer_runs, 0);
    p0_expect(__LINE__, "manual resume cancel",
              co_resume(co, (void *)CO_CANCEL, NULL), CO_RESULT_OK);
    p0_expect(__LINE__, "count after finish", (int)co_defer_count(co), 0);
    p0_expect(__LINE__, "defer ran", g_defer_runs, 1);
    p0_expect(__LINE__, "destroy done", co_destroy(co), CO_RESULT_OK);
}

static void fn_fill_slots(coroutine *self, void *ud, void *in)
{
    unsigned i;
    co_result r;
    void *extra;

    (void)ud;
    (void)in;
    for (i = 0; i < CO_DEFER_SLOTS; i++) {
        r = co_defer(self, defer_record, (void *)(intptr_t)(100 + (int)i));
        if (r != CO_RESULT_OK) {
            g_p0_failures++;
            return;
        }
    }
    extra = malloc(32);
    if (!extra) {
        g_p0_failures++;
        return;
    }
    r = co_defer(self, free, extra);
    p0_expect(__LINE__, "slot full", r, CO_RESULT_OUT_OF_MEMORY);
    p0_expect(__LINE__, "count full", (int)co_defer_count(self), CO_DEFER_SLOTS);
    if (r != CO_RESULT_OK) {
        free(extra);
        return;
    }
}

void test_defer_slots_full(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_fill_slots, NULL);

    g_defer_order_n = 0;
    g_defer_runs    = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume fill", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "all ran", g_defer_runs, CO_DEFER_SLOTS);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_cancel_middle(coroutine *self, void *ud, void *in)
{
    void *a = (void *)(intptr_t)1;
    void *b = (void *)(intptr_t)2;
    void *c = (void *)(intptr_t)3;

    (void)ud;
    (void)in;
    p0_expect(__LINE__, "d1", co_defer(self, defer_record, a), CO_RESULT_OK);
    p0_expect(__LINE__, "d2", co_defer(self, defer_record, b), CO_RESULT_OK);
    p0_expect(__LINE__, "d3", co_defer(self, defer_record, c), CO_RESULT_OK);
    p0_expect(__LINE__, "cancel b",
              co_defer_cancel(self, defer_record, b), CO_RESULT_OK);
    p0_expect(__LINE__, "count 2", (int)co_defer_count(self), 2);
}

void test_defer_cancel_middle(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_cancel_middle, NULL);

    g_defer_order_n = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume cancel mid", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "order 0", g_defer_order[0], 3);
    p0_expect(__LINE__, "order 1", g_defer_order[1], 1);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

static void fn_suspend_only(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "yield", P0_YIELD(), CO_RESULT_OK);
}

void test_defer_reject_register(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_suspend_only, NULL);

    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "cancel missing on ready",
              co_defer_cancel(co, defer_mark, NULL),
              CO_RESULT_INVALID_ARGUMENT);
    p0_expect(__LINE__, "main defer",
              co_defer(co_current(), defer_mark, NULL),
              CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "resume suspend", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "owner on suspended",
              co_defer(co, defer_mark, NULL), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "cancel on suspended",
              co_defer_cancel(co, defer_mark, NULL),
              CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy blocked",
              co_destroy(co), CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "resume finish", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy ok", co_destroy(co), CO_RESULT_OK);
}

static void fn_defer_yield_block(coroutine *self, void *ud, void *in)
{
    (void)ud;
    (void)in;
    p0_expect(__LINE__, "defer yield test",
              co_defer(self, defer_try_yield, NULL), CO_RESULT_OK);
}

void test_defer_running_blocks_yield(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_defer_yield_block, NULL);

    g_yield_rejected = 0;
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "yield rejected in defer", g_yield_rejected, 1);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
}

/* Destroy/orphan 路徑：callback 在銷毀者 context 上跑，TLS 仍須拒絕 mutating API。 */
static int g_ctx_yield_rc;
static int g_ctx_resume_rc;
static int g_ctx_cls_rc;
static int g_ctx_shutdown_rc;
static int g_ctx_destroy_finished;
static co_cls_key g_ctx_cls_key = CO_CLS_KEY_INVALID;

static void fn_instant_done(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

static void defer_try_resume_then_yield(void *p)
{
    coroutine *peer = p;
    size_t leaked = 99;

    g_ctx_resume_rc = (int)co_resume(peer, NULL, NULL);
    g_ctx_yield_rc = (int)co_yield_now(NULL, NULL);
    g_ctx_cls_rc = (int)co_cls_set(g_ctx_cls_key, &g_ctx_cls_rc);
    g_ctx_shutdown_rc = (int)co_thread_shutdown(&leaked);
}

static void fn_nested_destroyer(coroutine *self, void *ud, void *in)
{
    coroutine *victim;
    coroutine *peer = ud;

    (void)self;
    (void)in;
    victim = co_create(CO_MIN_STACK_SIZE, fn_never_body, NULL);
    if (!victim) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "victim pre-register",
              co_defer(victim, defer_try_resume_then_yield, peer),
              CO_RESULT_OK);
    p0_expect(__LINE__, "destroy victim", co_destroy(victim), CO_RESULT_OK);
    g_ctx_destroy_finished = 1;
}

void test_defer_destroy_ctx_mismatch(void)
{
    coroutine *peer;
    coroutine *destroyer;

    g_ctx_yield_rc = -999;
    g_ctx_resume_rc = -999;
    g_ctx_cls_rc = -999;
    g_ctx_shutdown_rc = -999;
    g_ctx_destroy_finished = 0;

    if (g_ctx_cls_key == CO_CLS_KEY_INVALID) {
        g_ctx_cls_key = co_cls_alloc();
        if (g_ctx_cls_key == CO_CLS_KEY_INVALID) {
            g_p0_failures++;
            return;
        }
    }

    peer = co_create(CO_MIN_STACK_SIZE, fn_instant_done, NULL);
    if (!peer) {
        g_p0_failures++;
        return;
    }
    destroyer = co_create(CO_MIN_STACK_SIZE, fn_nested_destroyer, peer);
    if (!destroyer) {
        g_p0_failures++;
        (void)co_destroy(peer);
        return;
    }
    p0_expect(__LINE__, "resume destroyer", P0_RESUME(destroyer), CO_RESULT_OK);
    if (!g_ctx_destroy_finished)
        p0_expect(__LINE__, "resume destroyer again",
                  P0_RESUME(destroyer), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy path resume blocked",
              g_ctx_resume_rc, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy path yield blocked",
              g_ctx_yield_rc, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy path cls_set blocked",
              g_ctx_cls_rc, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy path shutdown blocked",
              g_ctx_shutdown_rc, CO_RESULT_INVALID_STATE);
    p0_expect(__LINE__, "destroy finished first resume",
              g_ctx_destroy_finished, 1);
    p0_expect(__LINE__, "destroy destroyer", co_destroy(destroyer), CO_RESULT_OK);
    if (!co_finished(peer))
        p0_expect(__LINE__, "finish peer", P0_RESUME(peer), CO_RESULT_OK);
    p0_expect(__LINE__, "destroy peer", co_destroy(peer), CO_RESULT_OK);
}

static void *orphan_defer_worker(void *arg)
{
    coroutine *co;
    int *ran = arg;

    (void)arg;
    co = co_create(CO_MIN_STACK_SIZE, fn_suspend_only, ran);
    if (!co) {
        g_p0_failures++;
        return NULL;
    }
    p0_expect(__LINE__, "worker defer",
              co_defer(co, defer_mark, ran), CO_RESULT_OK);
    p0_expect(__LINE__, "worker resume", P0_RESUME(co), CO_RESULT_OK);
    /* deliberate leak: thread exit orphan reclaim */
    return NULL;
}

void test_defer_orphan_reclaim(void)
{
    pthread_t t;

    g_defer_runs = 0;
    if (pthread_create(&t, NULL, orphan_defer_worker, &g_defer_runs) != 0) {
        g_p0_failures++;
        return;
    }
    pthread_join(t, NULL);
    p0_expect(__LINE__, "orphan ran defer", g_defer_runs, 1);
}

#if !CO_TEST_TSAN
static int run_main_atexit_capture(char *buf, size_t buf_sz)
{
    int pipefd[2];
    pid_t pid;
    size_t n = 0;
    ssize_t r;
    int status = 0;

    if (pipe(pipefd) != 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        coroutine *co;

        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);

        co = co_create(CO_MIN_STACK_SIZE, fn_suspend_only, NULL);
        if (!co)
            _exit(126);
        p0_expect(__LINE__, "atexit defer",
                  co_defer(co, defer_mark, NULL), CO_RESULT_OK);
        if (P0_RESUME(co) != CO_RESULT_OK)
            _exit(125);
        exit(0);
    }

    close(pipefd[1]);
    while (n + 1 < buf_sz &&
           (r = read(pipefd[0], buf + n, buf_sz - 1 - n)) > 0)
        n += (size_t)r;
    close(pipefd[0]);
    buf[n] = '\0';

    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}
#endif

void test_defer_main_atexit(void)
{
#if CO_TEST_TSAN
    fprintf(stderr, "SKIP defer-atexit: fork-based capture is unsupported under TSan\n");
    return;
#else
    char buf[2048];

    if (run_main_atexit_capture(buf, sizeof buf) != 0) {
        fprintf(stderr, "FAIL defer-atexit: child failed\n");
        g_p0_failures++;
        return;
    }
    if (!strstr(buf, "coroutine orphan:")) {
        fprintf(stderr,
                "FAIL defer-atexit: expected orphan warning, stderr:\n%s\n",
                buf);
        g_p0_failures++;
    }
#endif
}

/*
 * D-10 ASan 負向測：READY 預登已結束的 caller local，co_destroy 路徑應報
 * stack-use-after-return。非 ASan 建置 SKIP（不會 abort）。
 */
#if defined(__SANITIZE_ADDRESS__)

static void defer_touch_uar(void *p)
{
    volatile unsigned char v = *(const volatile unsigned char *)p;

    (void)v;
}

static __attribute__((noinline)) void plant_dead_caller_local(coroutine *co)
{
    char local[64];

    memset(local, 0x41, sizeof local);
    if (co_defer(co, defer_touch_uar, local) != CO_RESULT_OK)
        _exit(124);
}

static int run_defer_uar_destroy_child(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_never_body, NULL);

    if (!co)
        return 126;
    plant_dead_caller_local(co);
    if (co_destroy(co) != CO_RESULT_OK)
        return 125;
    return 3;
}

void test_defer_asan_uar_destroy(void)
{
#ifndef __linux__
    fprintf(stderr, "SKIP defer-asan-uar: fork-based test needs Linux\n");
#else
    int pipefd[2];
    pid_t pid;
    char buf[16384];
    size_t n = 0;
    ssize_t r;
    int status = 0;
    int found;

    if (pipe(pipefd) != 0) {
        g_p0_failures++;
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        g_p0_failures++;
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        _exit(run_defer_uar_destroy_child());
    }

    close(pipefd[1]);
    while (n + 1 < sizeof buf &&
           (r = read(pipefd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)r;
    close(pipefd[0]);
    buf[n] = '\0';

    if (waitpid(pid, &status, 0) < 0) {
        g_p0_failures++;
        return;
    }

    found = strstr(buf, "stack-use-after-return") != NULL ||
            strstr(buf, "use-after-return") != NULL;
    if (!found) {
        fprintf(stderr,
                "FAIL defer-asan-uar: expected ASan use-after-return "
                "(set ASAN_OPTIONS=detect_stack_use_after_return=1); "
                "status signaled=%d term=%d exited=%d code=%d\nstderr:\n%s\n",
                WIFSIGNALED(status),
                WIFSIGNALED(status) ? WTERMSIG(status) : -1,
                WIFEXITED(status),
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                buf);
        g_p0_failures++;
    }
#endif
}

#else /* !__SANITIZE_ADDRESS__ */

void test_defer_asan_uar_destroy(void)
{
    fprintf(stderr, "SKIP defer-asan-uar: needs -fsanitize=address\n");
}

#endif
