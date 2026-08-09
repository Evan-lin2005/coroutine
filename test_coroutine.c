#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "coroutine.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static int g_failures;
static int g_iterations = 100000;

static void expect_eq(int line, const char *name, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL line %d: %s got=%d want=%d\n", line, name, got, want);
        g_failures++;
    }
}

static void expect_ptr_null(int line, const char *name, void *p)
{
    if (p) {
        fprintf(stderr, "FAIL line %d: %s expected NULL\n", line, name);
        g_failures++;
    }
}

static void expect_ptr_nonnull(int line, const char *name, void *p)
{
    if (!p) {
        fprintf(stderr, "FAIL line %d: %s expected non-NULL\n", line, name);
        g_failures++;
    }
}

/* --- basic tests --- */

static void fn_finish(coroutine *self, void *userdata, void *initial_input)
{
    int *v = userdata;

    (void)self;
    (void)initial_input;
    (*v)++;
}

static void fn_yield_once(coroutine *self, void *userdata, void *initial_input)
{
    int *v = userdata;

    (void)self;
    (void)initial_input;
    (*v)++;
    expect_eq(__LINE__, "yield_once", co_yield_now(NULL, NULL), CO_RESULT_OK);
    (*v)++;
}

static void fn_nested(coroutine *self, void *userdata, void *initial_input)
{
    int *depth = userdata;

    (void)self;
    (void)initial_input;
    (*depth)++;

    coroutine *child = co_create(CO_MIN_STACK_SIZE, fn_finish, depth);
    expect_ptr_nonnull(__LINE__, "nested child", child);
    expect_eq(__LINE__, "nested resume", co_resume(child, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "nested finished", co_finished(child), 1);
    expect_eq(__LINE__, "nested destroy", co_destroy(child), CO_RESULT_OK);
}

static void test_basic(void)
{
    int v = 0;

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_finish, &v);
    expect_ptr_nonnull(__LINE__, "create", co);
    expect_eq(__LINE__, "resume finish", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "value", v, 1);
    expect_eq(__LINE__, "finished", co_finished(co), 1);
    expect_eq(__LINE__, "resume done", co_resume(co, NULL, NULL), CO_RESULT_FINISHED);
    expect_eq(__LINE__, "destroy done", co_destroy(co), CO_RESULT_OK);
}

static void test_yield_cycle(void)
{
    int v = 0;

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_yield_once, &v);
    expect_eq(__LINE__, "yield resume", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "yield value mid", v, 1);
    expect_eq(__LINE__, "yield resume2", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "yield value end", v, 2);
    expect_eq(__LINE__, "yield destroy", co_destroy(co), CO_RESULT_OK);
}

static void test_nested(void)
{
    int depth = 0;

    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, fn_nested, &depth);
    expect_eq(__LINE__, "nested outer", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "nested depth", depth, 2);
    expect_eq(__LINE__, "nested outer destroy", co_destroy(co), CO_RESULT_OK);
}

/* --- edge / error tests --- */

static void test_edge_errors(void)
{
    coroutine *co_ex = (coroutine *)(uintptr_t)1;

    expect_ptr_null(__LINE__, "create null fn", co_create(CO_MIN_STACK_SIZE, NULL, NULL));
    expect_ptr_null(__LINE__, "create small stack", co_create(CO_MIN_STACK_SIZE - 1, fn_finish, NULL));

    expect_eq(__LINE__, "create_ex null out",
              co_create_ex(CO_MIN_STACK_SIZE, fn_finish, NULL, NULL),
              CO_RESULT_INVALID_ARGUMENT);
    expect_eq(__LINE__, "create_ex null fn",
              co_create_ex(CO_MIN_STACK_SIZE, NULL, NULL, &co_ex),
              CO_RESULT_INVALID_ARGUMENT);
    expect_ptr_null(__LINE__, "create_ex null fn out", co_ex);
    co_ex = (coroutine *)(uintptr_t)1;
    expect_eq(__LINE__, "create_ex small stack",
              co_create_ex(CO_MIN_STACK_SIZE - 1, fn_finish, NULL, &co_ex),
              CO_RESULT_INVALID_ARGUMENT);
    expect_ptr_null(__LINE__, "create_ex small stack out", co_ex);
    co_ex = NULL;
    expect_eq(__LINE__, "create_ex ok",
              co_create_ex(CO_MIN_STACK_SIZE, fn_finish, &(int){0}, &co_ex),
              CO_RESULT_OK);
    expect_ptr_nonnull(__LINE__, "create_ex ok out", co_ex);
    expect_eq(__LINE__, "create_ex destroy", co_destroy(co_ex), CO_RESULT_OK);

    expect_eq(__LINE__, "resume null",
              co_resume(NULL, NULL, NULL), CO_RESULT_INVALID_ARGUMENT);
    expect_eq(__LINE__, "destroy null", co_destroy(NULL), CO_RESULT_INVALID_ARGUMENT);
    expect_eq(__LINE__, "yield no caller",
              co_yield_now(NULL, NULL), CO_RESULT_NO_CALLER);
    expect_eq(__LINE__, "finished null", co_finished(NULL), 1);
    expect_ptr_nonnull(__LINE__, "co_current main", co_current());
    expect_ptr_null(__LINE__, "userdata null co", co_userdata(NULL));

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_yield_once, &(int){0});
    expect_ptr_nonnull(__LINE__, "edge co", co);
    expect_eq(__LINE__, "partial resume", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "destroy suspended", co_destroy(co), CO_RESULT_INVALID_STATE);
    expect_eq(__LINE__, "resume suspended", co_resume(co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "destroy after finish", co_destroy(co), CO_RESULT_OK);
}

/* --- thread tests --- */

typedef struct {
    coroutine *co;
    co_result  resume_rc;
    co_result  destroy_rc;
    int        owner_resume_ok;
} thread_xfer_t;

static void *worker_thread(void *arg)
{
    thread_xfer_t *x = arg;

    x->resume_rc  = co_resume(x->co, NULL, NULL);
    x->destroy_rc = co_destroy(x->co);

    return NULL;
}

static void test_wrong_thread(void)
{
    int v = 0;
    thread_xfer_t x = {0};
    pthread_t tid;

    x.co = co_create(CO_MIN_STACK_SIZE, fn_finish, &v);
    expect_ptr_nonnull(__LINE__, "thread co", x.co);

    expect_eq(__LINE__, "pthread_create", pthread_create(&tid, NULL, worker_thread, &x), 0);
    expect_eq(__LINE__, "pthread_join", pthread_join(tid, NULL), 0);

    expect_eq(__LINE__, "wrong thread resume", x.resume_rc, CO_RESULT_WRONG_THREAD);
    expect_eq(__LINE__, "wrong thread destroy", x.destroy_rc, CO_RESULT_WRONG_THREAD);

    expect_eq(__LINE__, "owner resume", co_resume(x.co, NULL, NULL), CO_RESULT_OK);
    expect_eq(__LINE__, "owner destroy", co_destroy(x.co), CO_RESULT_OK);
}

/* --- stress / speed test --- */

typedef struct {
    unsigned long *counter;
} stress_arg_t;

static void fn_stress(coroutine *self, void *userdata, void *initial_input)
{
    stress_arg_t *a = userdata;
    unsigned long i;

    (void)self;
    (void)initial_input;

    for (i = 0; i < (unsigned long)g_iterations; i++) {
        (*a->counter)++;
        if (co_yield_now(NULL, NULL) != CO_RESULT_OK) {
            fprintf(stderr, "FAIL: yield failed at iter %lu\n", i);
            g_failures++;
            return;
        }
    }
}

static void test_stress(const char *speed_label)
{
    unsigned long counter = 0;
    stress_arg_t arg = { &counter };
    struct timespec t0, t1;
    double elapsed_ms;
    coroutine *co;
    co_result rc;

    co = co_create(CO_DEFAULT_STACK_SIZE, fn_stress, &arg);
    expect_ptr_nonnull(__LINE__, "stress co", co);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!co_finished(co)) {
        rc = co_resume(co, NULL, NULL);
        if (rc != CO_RESULT_OK) {
            fprintf(stderr, "FAIL: stress resume got=%d at counter=%lu\n",
                    (int)rc, counter);
            g_failures++;
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
               + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    expect_eq(__LINE__, "stress counter", (int)(counter == (unsigned long)g_iterations),
              1);
    printf("  speed=%s iterations=%d elapsed=%.2f ms (%.0f switches/sec)\n",
           speed_label, g_iterations, elapsed_ms,
           g_iterations * 2 * 1000.0 / (elapsed_ms > 0 ? elapsed_ms : 1));

    expect_eq(__LINE__, "stress destroy", co_destroy(co), CO_RESULT_OK);
}

static void parse_speed(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (strcmp(s, "slow") == 0)       g_iterations = 1000;
            else if (strcmp(s, "normal") == 0) g_iterations = 100000;
            else if (strcmp(s, "fast") == 0)   g_iterations = 1000000;
            else if (strcmp(s, "turbo") == 0)  g_iterations = 5000000;
            else fprintf(stderr, "unknown speed '%s', use slow|normal|fast|turbo\n", s);
        } else if (strcmp(argv[i], "--iter") == 0 && i + 1 < argc) {
            g_iterations = atoi(argv[++i]);
            if (g_iterations <= 0) g_iterations = 100000;
        }
    }
}

int main(int argc, char **argv)
{
    parse_speed(argc, argv);

    printf("=== coroutine tests (iterations=%d) ===\n", g_iterations);

    test_basic();
    test_yield_cycle();
    test_nested();
    test_edge_errors();
    test_wrong_thread();
    test_stress(getenv("CO_SPEED") ? getenv("CO_SPEED") : "custom");

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }

    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
