/*
 * P2 hop benchmark: quantify co_transfer vs the original asymmetric scheduler.
 *
 * Metrics
 *   - worker activation: one trip into a fiber that then leaves (yield/transfer)
 *   - peer dispatch:     handing control to the next worker
 *       sched_main:  A yield → main resume B          = 2 context switches
 *       nested:      ping resume pong / pong yield    = 1 context switch
 *       transfer:    A co_transfer(B)                 = 1 context switch
 *   - main_yield: original project stress (main ↔ one fiber); no sibling hop
 */

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "coroutine.h"

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

static double timespec_sec(struct timespec t0, struct timespec t1)
{
    double s = (double)(t1.tv_sec - t0.tv_sec)
             + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    return s > 0.0 ? s : 1e-9;
}

static volatile int g_run;
static long long g_remain;
static coroutine *g_main;
static coroutine *g_a;
static coroutine *g_b;

static void fn_yield_until_stop(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    while (g_run) {
        if (co_yield_now(NULL, NULL) != CO_RESULT_OK)
            break;
    }
}

static void fn_ping_nested(coroutine *self, void *ud, void *in)
{
    long long i;

    (void)self;
    (void)ud;
    (void)in;
    for (i = 0; i < g_remain; i++) {
        if (co_resume(g_b, NULL, NULL) != CO_RESULT_OK)
            break;
    }
}

static void fn_xfer_a(coroutine *self, void *ud, void *in)
{
    long long i;

    (void)self;
    (void)ud;
    (void)in;
    for (i = 0; i < g_remain; i++) {
        if (co_transfer(g_b, NULL, NULL) != CO_RESULT_OK)
            break;
    }
    (void)co_transfer(g_main, NULL, NULL);
}

static void fn_xfer_b(coroutine *self, void *ud, void *in)
{
    (void)self;
    (void)ud;
    (void)in;
    for (;;) {
        if (co_transfer(g_a, NULL, NULL) != CO_RESULT_OK)
            break;
    }
}

typedef struct {
    const char *name;
    long long activations;
    long long peer_hops;
    long long switches;
    double    sec;
    double    cycles_raw;
} bench_row;

static void print_row(const bench_row *r)
{
    double ns_act  = r->sec * 1e9 / (double)r->activations;
    double ns_hop  = r->peer_hops ? r->sec * 1e9 / (double)r->peer_hops : 0.0;
    double act_s   = (double)r->activations / r->sec;
    double hop_s   = r->peer_hops ? (double)r->peer_hops / r->sec : 0.0;
    double cyc_sw  = (double)r->cycles_raw / (double)r->switches;

    printf("%-14s  act=%lld  hops=%lld  sw=%lld  time=%.4fs\n",
           r->name, r->activations, r->peer_hops, r->switches, r->sec);
    printf("               act/s=%.0f  hop/s=%.0f  ns/act=%.1f  ns/hop=%.1f"
           "  cycles/sw=%.1f\n",
           act_s, hop_s, ns_act, ns_hop, cyc_sw);
}

static bench_row run_main_yield(long long iters)
{
    struct timespec t0, t1;
    unsigned long long c0, c1;
    long long i;
    bench_row row;
    coroutine *co;

    g_run = 1;
    co = co_create(CO_DEFAULT_STACK_SIZE, fn_yield_until_stop, NULL);
    if (!co) {
        fprintf(stderr, "bench: create failed\n");
        exit(1);
    }
    (void)co_resume(co, NULL, NULL); /* park at first yield */

    c0 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < iters; i++)
        (void)co_resume(co, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    c1 = bench_rdtsc();

    g_run = 0;
    (void)co_resume(co, NULL, NULL);
    (void)co_destroy(co);

    memset(&row, 0, sizeof row);
    row.name         = "main_yield";
    row.activations  = iters;
    row.peer_hops    = 0;
    row.switches     = iters * 2; /* resume + yield */
    row.sec          = timespec_sec(t0, t1);
    row.cycles_raw   = (double)(c1 - c0);
    return row;
}

static bench_row run_sched_main(long long iters)
{
    struct timespec t0, t1;
    unsigned long long c0, c1;
    long long i;
    bench_row row;
    coroutine *a, *b;

    g_run = 1;
    a = co_create(CO_DEFAULT_STACK_SIZE, fn_yield_until_stop, NULL);
    b = co_create(CO_DEFAULT_STACK_SIZE, fn_yield_until_stop, NULL);
    if (!a || !b) {
        fprintf(stderr, "bench: create failed\n");
        exit(1);
    }
    (void)co_resume(a, NULL, NULL);
    (void)co_resume(b, NULL, NULL);

    c0 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < iters; i++) {
        (void)co_resume(a, NULL, NULL);
        (void)co_resume(b, NULL, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    c1 = bench_rdtsc();

    g_run = 0;
    (void)co_resume(a, NULL, NULL);
    (void)co_resume(b, NULL, NULL);
    (void)co_destroy(a);
    (void)co_destroy(b);

    memset(&row, 0, sizeof row);
    row.name         = "sched_main";
    row.activations  = iters * 2; /* A and B each run once per iter */
    row.peer_hops    = iters * 2; /* A→B and B→A via main, 2 sw each */
    row.switches     = iters * 4;
    row.sec          = timespec_sec(t0, t1);
    row.cycles_raw   = (double)(c1 - c0);
    return row;
}

static bench_row run_nested(long long iters)
{
    struct timespec t0, t1;
    unsigned long long c0, c1;
    bench_row row;
    coroutine *ping;

    g_run = 1;
    g_remain = iters;
    g_b = co_create(CO_DEFAULT_STACK_SIZE, fn_yield_until_stop, NULL);
    ping = co_create(CO_DEFAULT_STACK_SIZE, fn_ping_nested, NULL);
    if (!g_b || !ping) {
        fprintf(stderr, "bench: create failed\n");
        exit(1);
    }
    (void)co_resume(g_b, NULL, NULL); /* park pong at yield */

    c0 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    (void)co_resume(ping, NULL, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    c1 = bench_rdtsc();

    g_run = 0;
    (void)co_resume(g_b, NULL, NULL);
    (void)co_destroy(ping);
    (void)co_destroy(g_b);

    memset(&row, 0, sizeof row);
    row.name         = "nested_resume";
    row.activations  = iters * 2; /* resume + yield per iter */
    row.peer_hops    = iters * 2;
    row.switches     = iters * 2;
    row.sec          = timespec_sec(t0, t1);
    row.cycles_raw   = (double)(c1 - c0);
    return row;
}

static bench_row run_transfer(long long iters)
{
    struct timespec t0, t1;
    unsigned long long c0, c1;
    bench_row row;
    co_transfer_t t;

    g_main = co_current();
    g_remain = iters;
    g_a = co_create(CO_DEFAULT_STACK_SIZE, fn_xfer_a, NULL);
    g_b = co_create(CO_DEFAULT_STACK_SIZE, fn_xfer_b, NULL);
    if (!g_a || !g_b) {
        fprintf(stderr, "bench: create failed\n");
        exit(1);
    }

    c0 = bench_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    (void)co_transfer(g_a, NULL, &t);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    c1 = bench_rdtsc();

    (void)co_abandon(g_a);
    (void)co_abandon(g_b);

    memset(&row, 0, sizeof row);
    row.name         = "transfer";
    /* A→B then B→A, iters of each, plus one transfer home (not counted as hop) */
    row.activations  = iters * 2;
    row.peer_hops    = iters * 2;
    row.switches     = iters * 2;
    row.sec          = timespec_sec(t0, t1);
    row.cycles_raw   = (double)(c1 - c0);
    return row;
}

int main(int argc, char **argv)
{
    long long iters = 300000;
    int repeats = 5;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--iter=", 7) == 0)
            iters = atoll(argv[i] + 7);
        if (strncmp(argv[i], "--repeat=", 9) == 0)
            repeats = atoi(argv[i] + 9);
    }

    printf("bench_hop: iter=%lld repeat=%d (median of hop/s)\n", iters, repeats);
    printf("  main_yield     original: main resume / fiber yield\n");
    printf("  sched_main     two fibers, main dispatcher (2 sw / hop)\n");
    printf("  nested_resume  ping resume pong (1 sw / hop, already possible)\n");
    printf("  transfer       co_transfer sibling (1 sw / hop, P2)\n\n");

    /* warmup */
    (void)run_main_yield(20000);
    (void)run_sched_main(20000);
    (void)run_nested(20000);
    (void)run_transfer(20000);

    for (i = 0; i < repeats; i++) {
        bench_row rows[4];
        rows[0] = run_main_yield(iters);
        rows[1] = run_sched_main(iters);
        rows[2] = run_nested(iters);
        rows[3] = run_transfer(iters);
        printf("--- sample %d ---\n", i + 1);
        print_row(&rows[0]);
        print_row(&rows[1]);
        print_row(&rows[2]);
        print_row(&rows[3]);
        if (rows[1].peer_hops && rows[3].peer_hops) {
            double hop_sched = (double)rows[1].peer_hops / rows[1].sec;
            double hop_xfer  = (double)rows[3].peer_hops / rows[3].sec;
            printf("               transfer vs sched_main hop/s: %.2fx\n",
                   hop_xfer / hop_sched);
        }
        if (rows[2].peer_hops && rows[3].peer_hops) {
            double hop_nest = (double)rows[2].peer_hops / rows[2].sec;
            double hop_xfer = (double)rows[3].peer_hops / rows[3].sec;
            printf("               transfer vs nested_resume hop/s: %.2fx\n",
                   hop_xfer / hop_nest);
        }
        putchar('\n');
    }

    return 0;
}
