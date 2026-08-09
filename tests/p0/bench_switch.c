/*
 * P0 benchmark: empty yield/resume cycles per second + approx cycles/switch.
 * Standalone; does not modify library.
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "coroutine.h"
#include "p0_debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long bench_rdtsc(void)
{
#if defined(__x86_64__) || defined(__amd64__)
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
#elif defined(__aarch64__)
    unsigned long long v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static volatile int g_bench_phase;

static void fn_bench(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
    while (g_bench_phase) {
        if (co_yield_now(NULL, NULL) != CO_RESULT_OK)
            break;
    }
}

int main(int argc, char **argv)
{
    long long iterations = 200000;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--iter=", 7) == 0)
            iterations = atoll(argv[i] + 7);
    }

    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, fn_bench, NULL);
    if (!co) {
        fprintf(stderr, "bench: co_create failed\n");
        return 1;
    }

    g_bench_phase = 1;
    co_resume(co, NULL, NULL);

    struct timespec t0, t1;
    unsigned long long c0 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (i = 0; i < iterations; i++) {
        if (co_resume(co, NULL, NULL) != CO_RESULT_OK)
            break;
    }

    unsigned long long c1 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    g_bench_phase = 0;
    co_resume(co, NULL, NULL);
    co_destroy(co);

    double sec = (double)(t1.tv_sec - t0.tv_sec) +
                 (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (sec <= 0.0)
        sec = 1e-9;

    double switches = (double)iterations * 2.0; /* resume + yield each iter */
    double per_sec  = switches / sec;
    double cycles   = (double)(c1 - c0) / switches;

    printf("bench_switch: iter=%lld switches=%.0f time=%.3fs\n",
           iterations, switches, sec);
    printf("  switches/sec: %.0f\n", per_sec);
    printf("  cycles/switch (raw counter): %.1f\n", cycles);

    char buf[160];
    snprintf(buf, sizeof buf,
             "{\"iter\":%lld,\"switches_per_sec\":%.0f,\"cycles_per_switch\":%.1f}",
             iterations, per_sec, cycles);
    p0_log("BENCH", "bench_switch.c:main", "benchmark result", buf);

    return 0;
}
