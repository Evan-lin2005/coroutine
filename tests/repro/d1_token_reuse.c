/*
 * D-1 regression（standalone）：owner 仍存活時，他執行緒 resume 必須
 * CO_RESULT_WRONG_THREAD。另以 join→新執行緒建立確認序號身分不因 TLS
 * 位址重用而混淆（D-1b reclaim 後舊指標不可再用）。
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
static volatile int g_ready;
static volatile int g_probe_done;

static void fn(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    g_step = 1;
    (void)co_yield_now(NULL, NULL);
    g_step = 2;
}

static void *thread_owner(void *arg)
{
    (void)arg;
    g_co = co_create(CO_MIN_STACK_SIZE, fn, NULL);
    if (!g_co) {
        fprintf(stderr, "create fail\n");
        g_ready = 1;
        return NULL;
    }
    if (co_resume(g_co, NULL, NULL) != CO_RESULT_OK)
        fprintf(stderr, "owner resume fail\n");
    g_ready = 1;
    while (!g_probe_done) {
    }
    (void)co_resume(g_co, NULL, NULL);
    (void)co_destroy(g_co);
    g_co = NULL;
    return NULL;
}

static void *thread_probe(void *arg)
{
    co_result r;
    (void)arg;
    while (!g_ready) {
    }
    if (!g_co) {
        g_probe_done = 1;
        return (void *)(intptr_t)1;
    }
    r = co_resume(g_co, NULL, NULL);
    printf("probe resume -> %d step=%d\n", (int)r, g_step);
    g_probe_done = 1;
    if (r == CO_RESULT_WRONG_THREAD && g_step == 1) {
        printf("D-1 PASS: got WRONG_THREAD from non-owner\n");
        return (void *)(intptr_t)0;
    }
    printf("D-1 FAIL: r=%d step=%d\n", (int)r, g_step);
    return (void *)(intptr_t)2;
}

int main(void)
{
    pthread_t a, b;
    void *ret = (void *)(intptr_t)3;

    g_step = 0;
    g_co = NULL;
    g_ready = 0;
    g_probe_done = 0;
    pthread_create(&a, NULL, thread_owner, NULL);
    pthread_create(&b, NULL, thread_probe, NULL);
    pthread_join(b, &ret);
    pthread_join(a, NULL);
    return (int)(intptr_t)ret;
}
