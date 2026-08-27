/*
 * P3 pool cap 對照：每 class 8 塊 vs 全池合計 8 塊。
 * 編譯期 -DCO_POOL_PER_CLASS=1|0；數字看 hit/miss 與 ns/op。
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "coroutine_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CO_POOL_PER_CLASS
#  define CO_POOL_PER_CLASS 1
#endif

static void fn_nop(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
}

static int create_run_destroy(size_t stack)
{
    coroutine *co = co_create(stack, fn_nop, NULL);

    if (!co)
        return -1;
    if (co_resume(co, NULL, NULL) != CO_RESULT_OK || !co_finished(co)) {
        co_destroy(co);
        return -1;
    }
    return co_destroy(co) == CO_RESULT_OK ? 0 : -1;
}

static double elapsed_ns(const struct timespec *t0, const struct timespec *t1)
{
    return (double)(t1->tv_sec - t0->tv_sec) * 1e9 +
           (double)(t1->tv_nsec - t0->tv_nsec);
}

static void report(const char *name, int ops, const struct timespec *t0,
                   const struct timespec *t1)
{
    unsigned long long hit = 0, miss = 0, drop = 0;
    double ns = elapsed_ns(t0, t1);

    co_pool_debug_stats(&hit, &miss, &drop);
    printf("  %-20s  ops=%d  %.1f ns/op  hit=%llu miss=%llu drop=%llu\n",
           name, ops, ops > 0 ? ns / (double)ops : 0.0,
           hit, miss, drop);
}

static int bench_sequential(size_t stack, int iters)
{
    struct timespec t0, t1;
    int i;

    for (i = 0; i < 8; i++) {
        if (create_run_destroy(stack) != 0)
            return -1;
    }
    co_pool_debug_stats_reset();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < iters; i++) {
        if (create_run_destroy(stack) != 0)
            return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    report("sequential-same", iters, &t0, &t1);
    return 0;
}

static int bench_sequential_mixed(int iters)
{
    static const size_t sizes[] = {
        CO_MIN_STACK_SIZE, CO_DEFAULT_STACK_SIZE, 128u * 1024u
    };
    struct timespec t0, t1;
    int i;

    for (i = 0; i < 8; i++) {
        unsigned k;
        for (k = 0; k < 3; k++) {
            if (create_run_destroy(sizes[k]) != 0)
                return -1;
        }
    }
    co_pool_debug_stats_reset();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < iters; i++) {
        unsigned k;
        for (k = 0; k < 3; k++) {
            if (create_run_destroy(sizes[k]) != 0)
                return -1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    report("sequential-mixed", iters * 3, &t0, &t1);
    return 0;
}

/* 每 round：三個 class 各同時活 WAVE 條，再一次銷毀。工作集 = 3*WAVE。 */
static int bench_wave_mixed(int wave, int rounds)
{
    static const size_t sizes[] = {
        CO_MIN_STACK_SIZE, CO_DEFAULT_STACK_SIZE, 128u * 1024u
    };
    coroutine **all;
    struct timespec t0, t1;
    int r, c, i, nclass = 3, ops;

    all = calloc((size_t)wave * (size_t)nclass, sizeof *all);
    if (!all)
        return -1;

    for (c = 0; c < nclass; c++) {
        for (i = 0; i < wave; i++) {
            if (create_run_destroy(sizes[c]) != 0) {
                free(all);
                return -1;
            }
        }
    }

    co_pool_debug_stats_reset();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (r = 0; r < rounds; r++) {
        for (c = 0; c < nclass; c++) {
            for (i = 0; i < wave; i++) {
                coroutine *co = co_create(sizes[c], fn_nop, NULL);
                if (!co)
                    goto fail;
                if (co_resume(co, NULL, NULL) != CO_RESULT_OK) {
                    co_destroy(co);
                    goto fail;
                }
                all[c * wave + i] = co;
            }
        }
        for (c = 0; c < nclass; c++) {
            for (i = 0; i < wave; i++) {
                if (co_destroy(all[c * wave + i]) != CO_RESULT_OK)
                    goto fail;
                all[c * wave + i] = NULL;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ops = rounds * wave * nclass;
    report("wave-mixed-8x3", ops, &t0, &t1);
    free(all);
    return 0;

fail:
    for (i = 0; i < wave * nclass; i++) {
        if (all[i])
            co_destroy(all[i]);
    }
    free(all);
    return -1;
}

int main(int argc, char **argv)
{
    int seq_iters = 4000;
    int wave = 8;
    int rounds = 200;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--iter=", 7) == 0)
            seq_iters = atoi(argv[i] + 7);
        if (strncmp(argv[i], "--rounds=", 9) == 0)
            rounds = atoi(argv[i] + 9);
        if (strncmp(argv[i], "--wave=", 7) == 0)
            wave = atoi(argv[i] + 7);
    }

    printf("pool cap: %s (CO_POOL_SLOTS=%d)\n",
#if CO_POOL_PER_CLASS
           "8 per class",
#else
           "8 total",
#endif
           8);

    (void)co_current();

    if (bench_sequential(CO_MIN_STACK_SIZE, seq_iters) != 0 ||
        bench_sequential_mixed(seq_iters) != 0 ||
        bench_wave_mixed(wave, rounds) != 0) {
        fprintf(stderr, "bench_pool: failed\n");
        return 1;
    }
    return 0;
}
