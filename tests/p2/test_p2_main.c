/*
 * P2 test runner — co_transfer sibling hop / first entry / rejects.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "p0_common.h"

#include <stdio.h>
#include <string.h>

void test_sibling_hop(void);
void test_abandon_in_flight_resume(void);
void test_first_entry_transfer(void);
void test_transfer_no_caller_yield(void);
void test_transfer_to_waiting(void);
void test_nested_steal_rejected(void);
void test_indirect_steal_rejected(void);
void test_transfer_rejects(void);
void test_transfer_wrong_thread(void);

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
        if (strcmp(argv[i], "--sibling") == 0)
            run_all = 0, run_one("sibling-hop", test_sibling_hop);
        if (strcmp(argv[i], "--abandon-inflight") == 0)
            run_all = 0, run_one("abandon-inflight", test_abandon_in_flight_resume);
        if (strcmp(argv[i], "--first") == 0)
            run_all = 0, run_one("first-entry", test_first_entry_transfer);
        if (strcmp(argv[i], "--no-caller") == 0)
            run_all = 0, run_one("no-caller-yield", test_transfer_no_caller_yield);
        if (strcmp(argv[i], "--waiting") == 0)
            run_all = 0, run_one("transfer-waiting", test_transfer_to_waiting);
        if (strcmp(argv[i], "--nested-steal") == 0)
            run_all = 0, run_one("nested-steal", test_nested_steal_rejected);
        if (strcmp(argv[i], "--indirect-steal") == 0)
            run_all = 0, run_one("indirect-steal", test_indirect_steal_rejected);
        if (strcmp(argv[i], "--rejects") == 0)
            run_all = 0, run_one("rejects", test_transfer_rejects);
        if (strcmp(argv[i], "--wrong-thread") == 0)
            run_all = 0, run_one("wrong-thread", test_transfer_wrong_thread);
    }

    if (run_all) {
        run_one("sibling-hop", test_sibling_hop);
        run_one("abandon-inflight", test_abandon_in_flight_resume);
        run_one("first-entry", test_first_entry_transfer);
        run_one("no-caller-yield", test_transfer_no_caller_yield);
        run_one("transfer-waiting", test_transfer_to_waiting);
        run_one("nested-steal", test_nested_steal_rejected);
        run_one("indirect-steal", test_indirect_steal_rejected);
        run_one("rejects", test_transfer_rejects);
        run_one("wrong-thread", test_transfer_wrong_thread);
    }

    if (g_p0_failures) {
        fprintf(stderr, "\n%d P2 test failure(s)\n", g_p0_failures);
        return 1;
    }
    puts("\nAll P2 tests passed.");
    return 0;
}
