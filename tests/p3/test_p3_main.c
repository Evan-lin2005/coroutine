/*
 * P3 test runner — stack pool reuse / cap / VMA（ASan／TSan 下改看 hit/miss/drop）。
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdio.h>
#include <string.h>

void test_pool_sequential_reuse(void);
void test_pool_resume_after_reuse(void);
void test_pool_over_cap(void);
void test_pool_oversized_not_cached(void);
void test_pool_vma_one_cached(void);
void test_pool_thread_local(void);
void test_pool_thread_exit_drain(void);

static void run_one(const char *name, void (*fn)(void))
{
    int before = g_p0_failures;
    printf("== %s ==\n", name);
    fn();
    if (g_p0_failures == before)
        printf("PASS %s\n", name);
    else
        printf("FAIL %s (%d new failures)\n", name, g_p0_failures - before);
}

int main(int argc, char **argv)
{
    int run_all = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sequential") == 0)
            run_all = 0, run_one("sequential", test_pool_sequential_reuse);
        if (strcmp(argv[i], "--resume") == 0)
            run_all = 0, run_one("resume-reuse", test_pool_resume_after_reuse);
        if (strcmp(argv[i], "--cap") == 0)
            run_all = 0, run_one("over-cap", test_pool_over_cap);
        if (strcmp(argv[i], "--oversized") == 0)
            run_all = 0, run_one("oversized", test_pool_oversized_not_cached);
        if (strcmp(argv[i], "--vma") == 0)
            run_all = 0, run_one("vma-cached", test_pool_vma_one_cached);
        if (strcmp(argv[i], "--thread") == 0)
            run_all = 0, run_one("thread-local", test_pool_thread_local);
        if (strcmp(argv[i], "--thread-exit") == 0)
            run_all = 0, run_one("thread-exit-drain", test_pool_thread_exit_drain);
    }

    if (run_all) {
        run_one("sequential", test_pool_sequential_reuse);
        run_one("resume-reuse", test_pool_resume_after_reuse);
        run_one("over-cap", test_pool_over_cap);
        run_one("oversized", test_pool_oversized_not_cached);
        run_one("vma-cached", test_pool_vma_one_cached);
        run_one("thread-local", test_pool_thread_local);
        run_one("thread-exit-drain", test_pool_thread_exit_drain);
    }

    if (g_p0_failures) {
        fprintf(stderr, "\n%d P3 test failure(s)\n", g_p0_failures);
        return 1;
    }
    puts("\nAll P3 tests passed.");
    return 0;
}
