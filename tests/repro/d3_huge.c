/*
 * D-3 regression：1<<46 必須被 CO_MAX_STACK_SIZE 拒絕 → INVALID_ARGUMENT。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "coroutine.h"
#include <stdio.h>

static void nop(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

int main(void)
{
    coroutine *co = NULL;
    size_t sz = (size_t)1 << 46;
    co_result r = co_create_ex(sz, nop, NULL, &co);
    printf("co_create_ex(1<<46) -> %d out=%p\n", (int)r, (void *)co);
    if (r == CO_RESULT_INVALID_ARGUMENT && co == NULL) {
        printf("D-3 PASS: rejected as INVALID_ARGUMENT\n");
        return 0;
    }
    if (r == CO_RESULT_OK && co) {
        printf("D-3 FAIL: huge mapping accepted\n");
        (void)co_destroy(co);
        return 1;
    }
    printf("D-3 FAIL: unexpected r=%d\n", (int)r);
    return 1;
}
