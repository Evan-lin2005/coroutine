/*
 * P0 test runner — external suite; library sources linked unchanged.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdio.h>
#include <string.h>

void test_regs(void);
void test_guard_overflow(void);
void test_altstack_preserve(void);
void test_nested_depth(void);
void test_mass_lifecycle(void);
void test_waiting_reentry(void);
void test_owner_cross_generation(void);
void test_orphan_shutdown_ok(void);
void test_orphan_shutdown_warns(void);
void test_orphan_thread_exit_reclaim(void);
void test_orphan_warn_count(void);
void test_stack_bounds(void);
void test_cancel_sentinel(void);
void test_cancel_ready(void);
void test_cancel_done(void);
void test_cancel_suspended_ok(void);
void test_cancel_ignored(void);
void test_cancel_retry_after_ignore(void);
void test_cancel_manual_resume_after_ignore(void);
void test_cancel_requested(void);
void test_cancel_running(void);
void test_cancel_waiting(void);
void test_cancel_null(void);
void test_cancel_wrong_thread(void);

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
        if (strcmp(argv[i], "--regs") == 0)       run_all = 0, run_one("regs", test_regs);
        if (strcmp(argv[i], "--guard") == 0)        run_all = 0, run_one("guard", test_guard_overflow);
        if (strcmp(argv[i], "--altstack") == 0)     run_all = 0, run_one("altstack", test_altstack_preserve);
        if (strcmp(argv[i], "--nested") == 0)       run_all = 0, run_one("nested", test_nested_depth);
        if (strcmp(argv[i], "--mass") == 0)         run_all = 0, run_one("mass", test_mass_lifecycle);
        if (strcmp(argv[i], "--waiting") == 0)      run_all = 0, run_one("waiting", test_waiting_reentry);
        if (strcmp(argv[i], "--owner-gen") == 0)    run_all = 0, run_one("owner-gen", test_owner_cross_generation);
        if (strcmp(argv[i], "--orphan-ok") == 0)    run_all = 0, run_one("orphan-ok", test_orphan_shutdown_ok);
        if (strcmp(argv[i], "--orphan-warn") == 0)  run_all = 0, run_one("orphan-warn", test_orphan_shutdown_warns);
        if (strcmp(argv[i], "--orphan-exit") == 0)  run_all = 0, run_one("orphan-exit", test_orphan_thread_exit_reclaim);
        if (strcmp(argv[i], "--orphan-count") == 0) run_all = 0, run_one("orphan-count", test_orphan_warn_count);
        if (strcmp(argv[i], "--stack-bounds") == 0) run_all = 0, run_one("stack-bounds", test_stack_bounds);
        if (strcmp(argv[i], "--cancel") == 0)       run_all = 0, run_one("cancel", test_cancel_sentinel);
        if (strcmp(argv[i], "--cancel-ready") == 0) run_all = 0, run_one("cancel-ready", test_cancel_ready);
        if (strcmp(argv[i], "--cancel-done") == 0)  run_all = 0, run_one("cancel-done", test_cancel_done);
        if (strcmp(argv[i], "--cancel-ok") == 0)    run_all = 0, run_one("cancel-ok", test_cancel_suspended_ok);
        if (strcmp(argv[i], "--cancel-ignored") == 0) run_all = 0, run_one("cancel-ignored", test_cancel_ignored);
        if (strcmp(argv[i], "--cancel-retry") == 0) run_all = 0, run_one("cancel-retry", test_cancel_retry_after_ignore);
        if (strcmp(argv[i], "--cancel-manual") == 0) run_all = 0, run_one("cancel-manual", test_cancel_manual_resume_after_ignore);
        if (strcmp(argv[i], "--cancel-requested") == 0) run_all = 0, run_one("cancel-requested", test_cancel_requested);
        if (strcmp(argv[i], "--cancel-running") == 0) run_all = 0, run_one("cancel-running", test_cancel_running);
        if (strcmp(argv[i], "--cancel-waiting") == 0) run_all = 0, run_one("cancel-waiting", test_cancel_waiting);
        if (strcmp(argv[i], "--cancel-null") == 0)  run_all = 0, run_one("cancel-null", test_cancel_null);
        if (strcmp(argv[i], "--cancel-wrong") == 0) run_all = 0, run_one("cancel-wrong", test_cancel_wrong_thread);
    }

    if (run_all) {
        run_one("regs", test_regs);
        run_one("guard", test_guard_overflow);
        run_one("altstack", test_altstack_preserve);
        run_one("nested", test_nested_depth);
        run_one("waiting", test_waiting_reentry);
        run_one("owner-gen", test_owner_cross_generation);
        run_one("orphan-ok", test_orphan_shutdown_ok);
        run_one("orphan-warn", test_orphan_shutdown_warns);
        run_one("orphan-exit", test_orphan_thread_exit_reclaim);
        run_one("orphan-count", test_orphan_warn_count);
        run_one("stack-bounds", test_stack_bounds);
        run_one("cancel-sentinel", test_cancel_sentinel);
        run_one("cancel-ready", test_cancel_ready);
        run_one("cancel-done", test_cancel_done);
        run_one("cancel-suspended-ok", test_cancel_suspended_ok);
        run_one("cancel-ignored", test_cancel_ignored);
        run_one("cancel-retry", test_cancel_retry_after_ignore);
        run_one("cancel-manual", test_cancel_manual_resume_after_ignore);
        run_one("cancel-requested", test_cancel_requested);
        run_one("cancel-running", test_cancel_running);
        run_one("cancel-waiting", test_cancel_waiting);
        run_one("cancel-null", test_cancel_null);
        run_one("cancel-wrong-thread", test_cancel_wrong_thread);
        run_one("mass", test_mass_lifecycle);
    }

    p0_log("SUMMARY", "test_p0_main.c:main", "p0 suite finished",
           g_p0_failures ? "{\"failures\":true}" : "{\"failures\":false}");

    if (g_p0_failures) {
        fprintf(stderr, "\n%d P0 test failure(s)\n", g_p0_failures);
        return 1;
    }
    puts("\nAll P0 tests passed.");
    return 0;
}
