/*
 * D-1 regression（standalone）：TLS 位址世代重用後，新執行緒 resume 舊協程
 * 必須回 CO_RESULT_WRONG_THREAD（修後期望 exit 0）。
 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "coroutine.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static coroutine *g_co;
static volatile int g_step;

static void fn(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_step = 1;
    (void)co_yield_now(NULL, NULL);
    g_step = 2;
}

static void *thread_a(void *arg)
{
    (void)arg;
    g_co = co_create(CO_MIN_STACK_SIZE, fn, NULL);
    if (!g_co) {
        fprintf(stderr, "create fail\n");
        return NULL;
    }
    if (co_resume(g_co, NULL, NULL) != CO_RESULT_OK)
        fprintf(stderr, "A resume fail\n");
    return NULL;
}

static void *thread_b(void *arg)
{
    co_result r;
    (void)arg;
    r = co_resume(g_co, NULL, NULL);
    printf("B resume -> %d step=%d\n", (int)r, g_step);
    if (r == CO_RESULT_WRONG_THREAD && g_step == 1) {
        printf("D-1 PASS: got WRONG_THREAD after generation reuse\n");
        return (void *)(intptr_t)0;
    }
    if (r == CO_RESULT_OK && g_step == 2) {
        printf("D-1 FAIL: cross-generation resume succeeded\n");
        return (void *)(intptr_t)1;
    }
    printf("D-1 inconclusive: r=%d step=%d\n", (int)r, g_step);
    return (void *)(intptr_t)2;
}

int main(void)
{
    pthread_t a, b;
    void *ret = (void *)(intptr_t)3;

    g_step = 0;
    g_co = NULL;
    pthread_create(&a, NULL, thread_a, NULL);
    pthread_join(a, NULL);
    if (!g_co)
        return 1;
    pthread_create(&b, NULL, thread_b, NULL);
    pthread_join(b, &ret);
    return (int)(intptr_t)ret;
}
