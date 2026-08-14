/*
 * Boundary / crash probe: each scenario runs in a forked child.
 * Parent classifies: OK_ERROR_CODE / OK_SUCCESS / CRASH_SIGNAL /
 * CRASH_ABORT / HANG_TIMEOUT.
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "coroutine.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_TIMEOUT_SEC 3
#define EXIT_OK_SUCCESS   0
#define EXIT_OK_ERROR     40
#define EXIT_OK_NULL      50
#define EXIT_UNEXPECTED   99

static const char *result_name(co_result r)
{
    switch (r) {
    case CO_RESULT_OK: return "CO_RESULT_OK";
    case CO_RESULT_INVALID_ARGUMENT: return "CO_RESULT_INVALID_ARGUMENT";
    case CO_RESULT_ALREADY_RUNNING: return "CO_RESULT_ALREADY_RUNNING";
    case CO_RESULT_FINISHED: return "CO_RESULT_FINISHED";
    case CO_RESULT_NO_CALLER: return "CO_RESULT_NO_CALLER";
    case CO_RESULT_WRONG_THREAD: return "CO_RESULT_WRONG_THREAD";
    case CO_RESULT_INVALID_STATE: return "CO_RESULT_INVALID_STATE";
    case CO_RESULT_OUT_OF_MEMORY: return "CO_RESULT_OUT_OF_MEMORY";
    case CO_RESULT_CANCEL_IGNORED: return "CO_RESULT_CANCEL_IGNORED";
    default: return "CO_RESULT_UNKNOWN";
    }
}

static void child_error(co_result r)
{
    fprintf(stderr, "err=%s(%d)\n", result_name(r), (int)r);
    fflush(stderr);
    _exit(EXIT_OK_ERROR + (int)r);
}

static void child_success(const char *msg)
{
    fprintf(stderr, "ok=%s\n", msg ? msg : "ok");
    fflush(stderr);
    _exit(EXIT_OK_SUCCESS);
}

static void child_null_ok(const char *msg)
{
    fprintf(stderr, "null_ok=%s\n", msg ? msg : "null");
    fflush(stderr);
    _exit(EXIT_OK_NULL);
}

static void fn_nop(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
}

static void fn_yield_once(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
    (void)co_yield_now(NULL, NULL);
}

static void fn_resume_self(coroutine *self, void *ud, void *in)
{
    (void)ud; (void)in;
    co_result r = co_resume(self, NULL, NULL);
    fprintf(stderr, "resume_self=%s\n", result_name(r));
    fflush(stderr);
    _exit(EXIT_OK_ERROR + (int)r);
}

static coroutine *g_outer;
static coroutine *g_peer;

static void fn_waiting_inner(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
    co_result r1 = co_resume(g_outer, NULL, NULL);
    co_result r2 = co_destroy(g_outer);
    fprintf(stderr, "resume_waiting=%s destroy_waiting=%s\n",
            result_name(r1), result_name(r2));
    fflush(stderr);
    if (r1 != CO_RESULT_OK)
        _exit(EXIT_OK_ERROR + (int)r1);
    if (r2 != CO_RESULT_OK)
        _exit(EXIT_OK_ERROR + (int)r2);
    _exit(EXIT_OK_SUCCESS);
}

static void fn_waiting_outer(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
    coroutine *inner = co_create(CO_MIN_STACK_SIZE, fn_waiting_inner, NULL);
    if (!inner) {
        fprintf(stderr, "inner create failed\n");
        _exit(EXIT_UNEXPECTED);
    }
    (void)co_resume(inner, NULL, NULL);
    (void)co_destroy(inner);
}

static __attribute__((noinline)) void fn_stack_overflow(coroutine *self,
                                                         void *ud, void *in)
{
    (void)ud; (void)in;
    volatile char frame[4096];
    frame[0] = (char)(uintptr_t)frame;
    fn_stack_overflow(self, ud, in);
}

static void fn_deep_nest(coroutine *self, void *ud, void *in)
{
    (void)self; (void)in;
    int *depth = (int *)ud;
    if (*depth <= 0)
        return;
    (*depth)--;
    coroutine *child = co_create(CO_MIN_STACK_SIZE, fn_deep_nest, depth);
    if (!child) {
        fprintf(stderr, "nest_oom at remaining=%d\n", *depth);
        fflush(stderr);
        _exit(EXIT_OK_ERROR + (int)CO_RESULT_OUT_OF_MEMORY);
    }
    (void)co_resume(child, NULL, NULL);
    (void)co_destroy(child);
}

static int g_ping_left;

static void fn_pong(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
    for (;;) {
        co_result r = co_yield_now(NULL, NULL);
        if (r != CO_RESULT_OK) {
            fprintf(stderr, "pong_yield=%s\n", result_name(r));
            fflush(stderr);
            _exit(EXIT_OK_ERROR + (int)r);
        }
    }
}

static void fn_ping(coroutine *self, void *ud, void *in)
{
    (void)self; (void)ud; (void)in;
    while (g_ping_left-- > 0) {
        co_result r = co_resume(g_peer, NULL, NULL);
        if (r != CO_RESULT_OK) {
            fprintf(stderr, "ping_resume=%s left=%d\n", result_name(r), g_ping_left);
            fflush(stderr);
            _exit(EXIT_OK_ERROR + (int)r);
        }
    }
}

struct race_args {
    coroutine *co;
    co_result result;
};

static void *race_resume_thread(void *arg)
{
    struct race_args *a = arg;
    a->result = co_resume(a->co, NULL, NULL);
    return NULL;
}

static void *misaligned_alloc(size_t size, void *ud)
{
    (void)ud; (void)size;
    void *p = malloc(size + 64);
    if (!p)
        return NULL;
    uintptr_t u = (uintptr_t)p;
    return (void *)((u + 1) | 1u);
}

static void misaligned_free(void *ptr, size_t size, void *ud)
{
    (void)ptr; (void)size; (void)ud;
}

static void *null_alloc(size_t size, void *ud)
{
    (void)size; (void)ud;
    return NULL;
}

typedef void (*scenario_fn)(void);

static void sc_create_null_fn(void)
{
    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, NULL, NULL);
    if (co == NULL)
        child_null_ok("co_create NULL fn -> NULL");
    fprintf(stderr, "unexpected non-NULL\n");
    _exit(EXIT_UNEXPECTED);
}

static void sc_create_stack_too_small(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE - 1, fn_nop, NULL);
    if (co == NULL)
        child_null_ok("co_create stack too small -> NULL");
    fprintf(stderr, "unexpected non-NULL\n");
    _exit(EXIT_UNEXPECTED);
}

static void sc_create_ex_null_fn(void)
{
    coroutine *out = (coroutine *)0x1;
    co_result r = co_create_ex(CO_DEFAULT_STACK_SIZE, NULL, NULL, &out);
    if (r != CO_RESULT_OK && out == NULL)
        child_error(r);
    fprintf(stderr, "unexpected r=%s out=%p\n", result_name(r), (void *)out);
    _exit(EXIT_UNEXPECTED);
}

static void sc_resume_null(void) { child_error(co_resume(NULL, NULL, NULL)); }
static void sc_destroy_null(void) { child_error(co_destroy(NULL)); }
static void sc_yield_on_main(void) { child_error(co_yield_now(NULL, NULL)); }

static void sc_finished_null(void)
{
    int f = co_finished(NULL);
    fprintf(stderr, "co_finished(NULL)=%d\n", f);
    fflush(stderr);
    if (f == 1)
        child_success("finished NULL => 1");
    _exit(EXIT_UNEXPECTED);
}

static void sc_userdata_null(void)
{
    void *p = co_userdata(NULL);
    fprintf(stderr, "co_userdata(NULL)=%p\n", p);
    fflush(stderr);
    if (p == NULL)
        child_null_ok("userdata NULL");
    _exit(EXIT_UNEXPECTED);
}

static void sc_storage_null(void)
{
    void *p = co_storage(NULL);
    size_t n = co_storage_size(NULL);
    fprintf(stderr, "co_storage(NULL)=%p size=%zu\n", p, n);
    fflush(stderr);
    if (p == NULL && n == 0)
        child_null_ok("storage NULL");
    _exit(EXIT_UNEXPECTED);
}

static void sc_set_storage_invalid(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    char buf[16];
    co_result r1 = co_set_storage(co, buf, 0);
    co_result r2 = co_set_storage(co, NULL, 16);
    fprintf(stderr, "set_storage buf/0=%s NULL/16=%s\n",
            result_name(r1), result_name(r2));
    fflush(stderr);
    (void)co_destroy(co);
    if (r1 == CO_RESULT_INVALID_ARGUMENT)
        child_error(r1);
    if (r2 == CO_RESULT_INVALID_ARGUMENT)
        child_error(r2);
    _exit(EXIT_UNEXPECTED);
}

static void sc_cls_invalid_keys(void)
{
    co_result r1 = co_cls_set(-1, NULL);
    co_result r2 = co_cls_set(CO_CLS_SLOTS, NULL);
    void *g1 = co_cls_get(-1);
    void *g2 = co_cls_get(CO_CLS_SLOTS);
    fprintf(stderr, "cls_set(-1)=%s cls_set(SLOTS)=%s get=%p/%p\n",
            result_name(r1), result_name(r2), g1, g2);
    fflush(stderr);
    if (r1 == CO_RESULT_INVALID_ARGUMENT)
        child_error(r1);
    _exit(EXIT_UNEXPECTED);
}

static void sc_resume_finished(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    co_result r = co_resume(co, NULL, NULL);
    fprintf(stderr, "resume_finished=%s finished=%d\n", result_name(r),
            co_finished(co));
    fflush(stderr);
    (void)co_destroy(co);
    child_error(r);
}

static void sc_destroy_suspended(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_yield_once, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    co_result r = co_destroy(co);
    fprintf(stderr, "destroy_suspended=%s\n", result_name(r));
    fflush(stderr);
    (void)co_resume(co, NULL, NULL);
    (void)co_destroy(co);
    child_error(r);
}

static void sc_resume_self(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_resume_self, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    _exit(EXIT_UNEXPECTED);
}

static void sc_waiting_outer_ops(void)
{
    g_outer = co_create(CO_DEFAULT_STACK_SIZE, fn_waiting_outer, NULL);
    if (!g_outer)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(g_outer, NULL, NULL);
    (void)co_destroy(g_outer);
    _exit(EXIT_UNEXPECTED);
}

static void sc_double_destroy(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    co_result r1 = co_destroy(co);
    fprintf(stderr, "first_destroy=%s; second_destroy UAF...\n", result_name(r1));
    fflush(stderr);
    co_result r2 = co_destroy(co);
    fprintf(stderr, "second_destroy=%s\n", result_name(r2));
    fflush(stderr);
    child_error(r2);
}

static void sc_guard_overflow(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_stack_overflow, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    fprintf(stderr, "overflow resumed without crash\n");
    _exit(EXIT_UNEXPECTED);
}

static void sc_deep_nest_main_overflow(void)
{
    int depth = 200000;
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_deep_nest, &depth);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    fprintf(stderr, "deep nest completed remaining=%d\n", depth);
    fflush(stderr);
    (void)co_destroy(co);
    child_success("deep nest finished");
}

static void sc_min_stack_ok(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co)
        child_null_ok("min stack create failed unexpectedly");
    co_result r = co_resume(co, NULL, NULL);
    int fin = co_finished(co);
    co_result d = co_destroy(co);
    fprintf(stderr, "resume=%s finished=%d destroy=%s\n", result_name(r), fin,
            result_name(d));
    fflush(stderr);
    if (r == CO_RESULT_OK && fin && d == CO_RESULT_OK)
        child_success("min stack ok");
    child_error(r != CO_RESULT_OK ? r : d);
}

static void sc_min_stack_minus_one(void)
{
    coroutine *out = (coroutine *)0x1;
    co_result r = co_create_ex(CO_MIN_STACK_SIZE - 1, fn_nop, NULL, &out);
    fprintf(stderr, "create_ex min-1=%s out=%p\n", result_name(r), (void *)out);
    fflush(stderr);
    child_error(r);
}

static void sc_create_huge(void)
{
    coroutine *out = NULL;
    co_result r = co_create_ex(SIZE_MAX / 2, fn_nop, NULL, &out);
    fprintf(stderr, "create_ex huge=%s out=%p\n", result_name(r), (void *)out);
    fflush(stderr);
    if (out) {
        (void)co_resume(out, NULL, NULL);
        (void)co_destroy(out);
        child_success("huge create succeeded");
    }
    child_error(r);
}

static void sc_create_zero(void)
{
    coroutine *co = co_create(0, fn_nop, NULL);
    if (co == NULL)
        child_null_ok("co_create(0) -> NULL");
    fprintf(stderr, "unexpected co=%p\n", (void *)co);
    _exit(EXIT_UNEXPECTED);
}

static void sc_resume_after_destroy(void)
{
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    (void)co_resume(co, NULL, NULL);
    (void)co_destroy(co);
    fprintf(stderr, "resume after destroy UAF...\n");
    fflush(stderr);
    co_result r = co_resume(co, NULL, NULL);
    fprintf(stderr, "resume_uaf=%s\n", result_name(r));
    fflush(stderr);
    child_error(r);
}

static void sc_yield_main_repeated(void)
{
    co_result r1 = co_yield_now(NULL, NULL);
    co_result r2 = co_yield_now(NULL, NULL);
    co_result r3 = co_yield_now(NULL, NULL);
    fprintf(stderr, "yield_main x3: %s %s %s\n", result_name(r1), result_name(r2),
            result_name(r3));
    fflush(stderr);
    child_error(r1);
}

static void sc_concurrent_resume(void)
{
    /* Owner is this (main) thread; race owner resume vs non-owner thread. */
    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, fn_yield_once, NULL);
    if (!co)
        _exit(EXIT_UNEXPECTED);
    struct race_args other = { .co = co, .result = CO_RESULT_OK };
    pthread_t t;
    if (pthread_create(&t, NULL, race_resume_thread, &other) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        _exit(EXIT_UNEXPECTED);
    }
    co_result owner_r = co_resume(co, NULL, NULL);
    pthread_join(t, NULL);
    fprintf(stderr, "race_results owner=%s other=%s\n", result_name(owner_r),
            result_name(other.result));
    fflush(stderr);
    if (!co_finished(co))
        (void)co_resume(co, NULL, NULL);
    if (co_finished(co))
        (void)co_destroy(co);
    child_success("concurrent resume completed without signal");
}

static void sc_garbage_pointer(void)
{
    coroutine *bogus = (coroutine *)(uintptr_t)0xdeadbeefu;
    fprintf(stderr, "resume garbage...\n");
    fflush(stderr);
    co_result r = co_resume(bogus, NULL, NULL);
    fprintf(stderr, "resume_garbage=%s; destroy garbage...\n", result_name(r));
    fflush(stderr);
    co_result d = co_destroy(bogus);
    fprintf(stderr, "destroy_garbage=%s\n", result_name(d));
    fflush(stderr);
    child_error(r);
}

static void sc_allocator_misaligned(void)
{
    co_allocator a = { .alloc = misaligned_alloc, .free = misaligned_free,
                       .userdata = NULL };
    co_set_allocator(&a);
    coroutine *out = (coroutine *)0x1;
    co_result r = co_create_ex(CO_MIN_STACK_SIZE, fn_nop, NULL, &out);
    fprintf(stderr, "misaligned_alloc create=%s out=%p\n", result_name(r),
            (void *)out);
    fflush(stderr);
    co_set_allocator(NULL);
    if (out)
        (void)co_destroy(out);
    child_error(r);
}

static void sc_allocator_null(void)
{
    co_allocator a = { .alloc = null_alloc, .free = NULL, .userdata = NULL };
    co_set_allocator(&a);
    coroutine *out = (coroutine *)0x1;
    co_result r = co_create_ex(CO_MIN_STACK_SIZE, fn_nop, NULL, &out);
    fprintf(stderr, "null_alloc create=%s out=%p\n", result_name(r), (void *)out);
    fflush(stderr);
    co_set_allocator(NULL);
    child_error(r);
}

static void sc_ping_pong_deep(void)
{
    g_ping_left = 50000;
    coroutine *ping = co_create(CO_DEFAULT_STACK_SIZE, fn_ping, NULL);
    g_peer = co_create(CO_DEFAULT_STACK_SIZE, fn_pong, NULL);
    if (!ping || !g_peer)
        _exit(EXIT_UNEXPECTED);
    co_result r = co_resume(ping, NULL, NULL);
    fprintf(stderr, "ping_done resume=%s left=%d\n", result_name(r), g_ping_left);
    fflush(stderr);
    if (co_finished(ping))
        (void)co_destroy(ping);
    child_success("ping-pong deep completed");
}

struct case_entry {
    const char *name;
    scenario_fn fn;
};

static const struct case_entry g_cases[] = {
    { "A_create_null_fn", sc_create_null_fn },
    { "A_create_stack_too_small", sc_create_stack_too_small },
    { "A_create_ex_null_fn", sc_create_ex_null_fn },
    { "A_resume_null", sc_resume_null },
    { "A_destroy_null", sc_destroy_null },
    { "A_yield_on_main", sc_yield_on_main },
    { "A_finished_null", sc_finished_null },
    { "A_userdata_null", sc_userdata_null },
    { "A_storage_null", sc_storage_null },
    { "A_set_storage_invalid", sc_set_storage_invalid },
    { "A_cls_invalid_keys", sc_cls_invalid_keys },
    { "B_resume_finished", sc_resume_finished },
    { "B_destroy_suspended", sc_destroy_suspended },
    { "B_resume_self", sc_resume_self },
    { "B_resume_running_inside", sc_resume_self },
    { "B_waiting_outer_ops", sc_waiting_outer_ops },
    { "B_double_destroy_uaf", sc_double_destroy },
    { "C_guard_stack_overflow", sc_guard_overflow },
    { "C_deep_nest_main_overflow", sc_deep_nest_main_overflow },
    { "D_min_stack_ok", sc_min_stack_ok },
    { "D_min_stack_minus_one", sc_min_stack_minus_one },
    { "D_create_huge", sc_create_huge },
    { "D_create_zero", sc_create_zero },
    { "E_resume_after_destroy_uaf", sc_resume_after_destroy },
    { "E_destroy_then_resume_uaf", sc_resume_after_destroy },
    { "E_yield_main_repeated", sc_yield_main_repeated },
    { "E_concurrent_resume_race", sc_concurrent_resume },
    { "E_garbage_pointer", sc_garbage_pointer },
    { "E_allocator_misaligned", sc_allocator_misaligned },
    { "E_allocator_null", sc_allocator_null },
    { "E_ping_pong_deep", sc_ping_pong_deep },
};

static volatile pid_t g_child_pid;
static volatile int g_timed_out;

static void on_alarm(int sig)
{
    (void)sig;
    g_timed_out = 1;
    if (g_child_pid > 0)
        kill(g_child_pid, SIGKILL);
}

static void run_case(const struct case_entry *c)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        printf("CASE %s: HANG_TIMEOUT detail=pipe_failed\n", c->name);
        return;
    }

    g_timed_out = 0;
    g_child_pid = 0;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        printf("CASE %s: HANG_TIMEOUT detail=fork_failed errno=%d\n", c->name, errno);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        alarm(CHILD_TIMEOUT_SEC);
        c->fn();
        _exit(EXIT_UNEXPECTED);
    }

    close(pipefd[1]);
    g_child_pid = pid;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    alarm(CHILD_TIMEOUT_SEC + 1);

    char buf[4096];
    size_t n = 0;
    ssize_t rd;
    while (n + 1 < sizeof buf &&
           (rd = read(pipefd[0], buf + n, sizeof buf - 1 - n)) > 0)
        n += (size_t)rd;
    close(pipefd[0]);
    buf[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r')
            buf[i] = ' ';
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        alarm(0);
        printf("CASE %s: HANG_TIMEOUT detail=waitpid_failed\n", c->name);
        return;
    }
    alarm(0);
    g_child_pid = 0;

    const char *result;
    char detail[4600];

    if (g_timed_out || (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
        result = "HANG_TIMEOUT";
        snprintf(detail, sizeof detail, "killed_after_%ds child_msg=[%s]",
                 CHILD_TIMEOUT_SEC, buf);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        result = (sig == SIGABRT) ? "CRASH_ABORT" : "CRASH_SIGNAL";
        snprintf(detail, sizeof detail, "signal=%d(%s) child_msg=[%s]", sig,
                 strsignal(sig), buf);
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == EXIT_OK_SUCCESS) {
            result = "OK_SUCCESS";
            snprintf(detail, sizeof detail, "exit=0 child_msg=[%s]", buf);
        } else if (code == EXIT_OK_NULL) {
            result = "OK_ERROR_CODE";
            snprintf(detail, sizeof detail, "clean_null_or_defined child_msg=[%s]", buf);
        } else if (code >= EXIT_OK_ERROR && code < EXIT_OK_ERROR + 32) {
            result = "OK_ERROR_CODE";
            int cr = code - EXIT_OK_ERROR;
            snprintf(detail, sizeof detail, "co_result=%s(%d) child_msg=[%s]",
                     result_name((co_result)cr), cr, buf);
        } else if (code == 128 + SIGABRT) {
            result = "CRASH_ABORT";
            snprintf(detail, sizeof detail, "exit=%d(128+SIGABRT) child_msg=[%s]",
                     code, buf);
        } else if (code == 128 + SIGSEGV || code == 128 + SIGBUS) {
            result = "CRASH_SIGNAL";
            snprintf(detail, sizeof detail, "exit=%d(128+signal) child_msg=[%s]",
                     code, buf);
        } else {
            result = "OK_ERROR_CODE";
            snprintf(detail, sizeof detail, "unexpected_exit=%d child_msg=[%s]",
                     code, buf);
        }
    } else {
        result = "HANG_TIMEOUT";
        snprintf(detail, sizeof detail, "unknown_status=%d child_msg=[%s]", status, buf);
    }

    printf("CASE %s: %s detail=%s\n", c->name, result, detail);
    fflush(stdout);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t n = sizeof g_cases / sizeof g_cases[0];
    printf("=== boundary / crash probe (%zu cases) ===\n", n);

    int cnt_ok_err = 0, cnt_ok_suc = 0, cnt_sig = 0, cnt_abt = 0, cnt_hang = 0;
    for (size_t i = 0; i < n; i++)
        run_case(&g_cases[i]);

    printf("\n=== SUMMARY NOTES ===\n");
    printf("EXPECTED crashes:\n");
    printf("  - C_guard_stack_overflow: SIGSEGV + guard page message\n");
    printf("  - C_deep_nest_main_overflow: may CRASH (main stack) or OOM cleanly\n");
    printf("BUG / UB exposures (crash or silent corruption acceptable):\n");
    printf("  - B_double_destroy_uaf, E_resume_after_destroy_uaf,\n");
    printf("    E_destroy_then_resume_uaf, E_garbage_pointer,\n");
    printf("    E_concurrent_resume_race\n");
    printf("Clean error-code expectations:\n");
    printf("  - Section A NULL/invalid args, B state machine (non-UAF),\n");
    printf("    D size rejects, E_yield_main_repeated, allocator probes\n");
    (void)cnt_ok_err; (void)cnt_ok_suc; (void)cnt_sig; (void)cnt_abt; (void)cnt_hang;
    return 0;
}