/*
 * P1 test runner — mailbox + userdata + storage + allocator.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p1_common.h"

#include <stdio.h>
#include <string.h>

void test_transfer_roundtrip(void);
void test_userdata_vs_initial_input(void);
void test_mailbox_cleared_after_read(void);
void test_storage_persist_across_yield(void);
void test_storage_set_state_and_meta(void);

void test_allocator_counting(void);
void test_allocator_snapshot(void);
void test_allocator_oom(void);
void test_allocator_restore_default(void);
void test_allocator_misaligned_reject(void);
void test_external_stack_destroy(void);

void test_cls_allocation(void);
void test_cls_main(void);
void test_cls_fiber_isolation(void);
void test_cls_nested(void);
void test_cls_current_consistency(void);
void test_cls_invalid_key(void);
void test_cls_explicit_null(void);
void test_cls_thread_isolation(void);
void test_cls_alloc_race(void);

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
        if (strcmp(argv[i], "--transfer") == 0)
            run_all = 0, run_one("mailbox", test_transfer_roundtrip);
        if (strcmp(argv[i], "--userdata") == 0)
            run_all = 0, run_one("userdata", test_userdata_vs_initial_input);
        if (strcmp(argv[i], "--mailbox-clear") == 0)
            run_all = 0, run_one("mailbox-clear", test_mailbox_cleared_after_read);
        if (strcmp(argv[i], "--storage") == 0)
            run_all = 0, run_one("storage", test_storage_persist_across_yield);
        if (strcmp(argv[i], "--storage-meta") == 0)
            run_all = 0, run_one("storage-meta", test_storage_set_state_and_meta);
        if (strcmp(argv[i], "--allocator") == 0)
            run_all = 0, run_one("allocator", test_allocator_counting);
        if (strcmp(argv[i], "--allocator-snapshot") == 0)
            run_all = 0, run_one("allocator-snapshot", test_allocator_snapshot);
        if (strcmp(argv[i], "--allocator-oom") == 0)
            run_all = 0, run_one("allocator-oom", test_allocator_oom);
        if (strcmp(argv[i], "--allocator-default") == 0)
            run_all = 0, run_one("allocator-default", test_allocator_restore_default);
        if (strcmp(argv[i], "--allocator-align") == 0)
            run_all = 0, run_one("allocator-align", test_allocator_misaligned_reject);
        if (strcmp(argv[i], "--ext-stack") == 0)
            run_all = 0, run_one("ext-stack", test_external_stack_destroy);
        if (strcmp(argv[i], "--cls-alloc") == 0)
            run_all = 0, run_one("cls-alloc", test_cls_allocation);
        if (strcmp(argv[i], "--cls-main") == 0)
            run_all = 0, run_one("cls-main", test_cls_main);
        if (strcmp(argv[i], "--cls-isolation") == 0)
            run_all = 0, run_one("cls-isolation", test_cls_fiber_isolation);
        if (strcmp(argv[i], "--cls-nested") == 0)
            run_all = 0, run_one("cls-nested", test_cls_nested);
        if (strcmp(argv[i], "--cls-current") == 0)
            run_all = 0, run_one("cls-current", test_cls_current_consistency);
        if (strcmp(argv[i], "--cls-invalid") == 0)
            run_all = 0, run_one("cls-invalid", test_cls_invalid_key);
        if (strcmp(argv[i], "--cls-null") == 0)
            run_all = 0, run_one("cls-null", test_cls_explicit_null);
        if (strcmp(argv[i], "--cls-thread") == 0)
            run_all = 0, run_one("cls-thread", test_cls_thread_isolation);
        if (strcmp(argv[i], "--cls-alloc-race") == 0) {
            run_all = 0;
            run_one("cls-alloc-race", test_cls_alloc_race);
            return g_p0_failures ? 1 : 0;
        }
    }

    if (run_all) {
        run_one("mailbox", test_transfer_roundtrip);
        run_one("userdata", test_userdata_vs_initial_input);
        run_one("mailbox-clear", test_mailbox_cleared_after_read);
        run_one("storage", test_storage_persist_across_yield);
        run_one("storage-meta", test_storage_set_state_and_meta);
        run_one("allocator", test_allocator_counting);
        run_one("allocator-snapshot", test_allocator_snapshot);
        run_one("allocator-oom", test_allocator_oom);
        run_one("allocator-default", test_allocator_restore_default);
        run_one("allocator-align", test_allocator_misaligned_reject);
        run_one("ext-stack", test_external_stack_destroy);
        run_one("cls-alloc", test_cls_allocation);
        run_one("cls-main", test_cls_main);
        run_one("cls-isolation", test_cls_fiber_isolation);
        run_one("cls-nested", test_cls_nested);
        run_one("cls-current", test_cls_current_consistency);
        run_one("cls-invalid", test_cls_invalid_key);
        run_one("cls-null", test_cls_explicit_null);
        run_one("cls-thread", test_cls_thread_isolation);
    }

    if (g_p0_failures) {
        fprintf(stderr, "\n%d P1 test failure(s)\n", g_p0_failures);
        return 1;
    }
    puts("\nAll P1 tests passed.");
    return 0;
}
