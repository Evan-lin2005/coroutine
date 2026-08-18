#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
/* D-6 regression: concurrent co_create must not race on page_size cache.
 * Run under TSan: data race on page_size indicates a regression. */
#include "coroutine.h"
#include <pthread.h>
#include <stdio.h>

static void nop(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
}

static void *worker(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 32; i++) {
        coroutine *co = co_create(CO_MIN_STACK_SIZE, nop, NULL);
        if (co)
            (void)co_destroy(co);
    }
    return NULL;
}

int main(void)
{
    pthread_t a, b;
    if (pthread_create(&a, NULL, worker, NULL) != 0)
        return 1;
    if (pthread_create(&b, NULL, worker, NULL) != 0)
        return 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("d6 ok\n");
    return 0;
}
