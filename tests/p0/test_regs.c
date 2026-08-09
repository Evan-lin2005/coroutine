/*
 * P0: per-arch callee-saved register / FP control preservation across yield/resume.
 * External test — does not modify the coroutine library.
 *
 * Hypotheses:
 *   H1 — GPR callee-saved registers survive context switch
 *   H2 — FP control state (MXCSR on x86, d8-d15 on AArch64) survives switch
 */

#include "p0_common.h"

#if defined(__x86_64__) || defined(__amd64__)

typedef struct {
    unsigned long long rbx, rbp, r12, r13, r14, r15;
    unsigned int       mxcsr;
    unsigned short     x87cw;
    int                phase;
} reg_state_t;

/* 在單一函式內完成 load/store，避免跨函式呼叫後 compiler 覆寫 callee-saved */
static inline __attribute__((always_inline)) void x86_set_regs(const reg_state_t *st)
{
    unsigned long long rbx = st->rbx;
    unsigned long long rbp = st->rbp;
    unsigned long long r12 = st->r12;
    unsigned long long r13 = st->r13;
    unsigned long long r14 = st->r14;
    unsigned long long r15 = st->r15;
    unsigned int       mxcsr = st->mxcsr;
    unsigned short     x87cw = st->x87cw;

    __asm__ volatile(
        "mov %0, %%rbx\n\t"
        "mov %1, %%rbp\n\t"
        "mov %2, %%r12\n\t"
        "mov %3, %%r13\n\t"
        "mov %4, %%r14\n\t"
        "mov %5, %%r15\n\t"
        "ldmxcsr %6\n\t"
        "fldcw %7\n\t"
        :
        : "r"(rbx), "r"(rbp), "r"(r12), "r"(r13), "r"(r14), "r"(r15),
          "m"(mxcsr), "m"(x87cw)
        : "rbx", "rbp", "r12", "r13", "r14", "r15", "memory");
}

static inline __attribute__((always_inline)) void x86_get_regs(reg_state_t *out)
{
    __asm__ volatile(
        "mov %%rbx, %0\n\t"
        "mov %%rbp, %1\n\t"
        "mov %%r12, %2\n\t"
        "mov %%r13, %3\n\t"
        "mov %%r14, %4\n\t"
        "mov %%r15, %5\n\t"
        "stmxcsr %6\n\t"
        "fnstcw %7\n\t"
        : "=m"(out->rbx), "=m"(out->rbp), "=m"(out->r12), "=m"(out->r13),
          "=m"(out->r14), "=m"(out->r15), "=m"(out->mxcsr), "=m"(out->x87cw)
        :
        : "memory");
}

static int reg_state_match(const reg_state_t *a, const reg_state_t *b)
{
    return a->rbx == b->rbx && a->rbp == b->rbp &&
           a->r12 == b->r12 && a->r13 == b->r13 &&
           a->r14 == b->r14 && a->r15 == b->r15 &&
           a->mxcsr == b->mxcsr && a->x87cw == b->x87cw;
}

/* Plan P0：caller 在 resume 前覆寫同組 callee-saved，驗證 fiber 內值仍復原 */
static void poison_callee_saved_x86(void)
{
    reg_state_t poison = {
        .rbx  = 0xababababababababULL,
        .rbp  = 0xbcbcbcbcbcbcbcbcULL,
        .r12  = 0xcdcdcdcdcdcdcdcdULL,
        .r13  = 0xdedededededededeULL,
        .r14  = 0xefefefefefefefefULL,
        .r15  = 0xfafafafafafafafaULL,
        .mxcsr = 0x00001f80u,
        .x87cw = 0x027fu,
    };
    poison.mxcsr = (poison.mxcsr & ~0x6000u) | 0x4000u;
    x86_set_regs(&poison);
}

static void fn_regs_x86(coroutine *self, void *userdata, void *initial_input)
{
    reg_state_t *expect = userdata;
    reg_state_t  cur;

    (void)self;
    (void)initial_input;

    x86_set_regs(expect);
    x86_get_regs(&cur);

    p0_log("H1", "test_regs.c:fn_regs_x86", "before yield",
           reg_state_match(&cur, expect) ? "{\"match\":true}" : "{\"match\":false}");

    if (!reg_state_match(&cur, expect)) {
        p0_expect_u64(__LINE__, "pre-yield rbx", cur.rbx, expect->rbx);
        p0_expect_u64(__LINE__, "pre-yield r12", cur.r12, expect->r12);
    }

    p0_expect(__LINE__, "yield", P0_YIELD(), CO_RESULT_OK);

    x86_get_regs(&cur);
    p0_log("H1", "test_regs.c:fn_regs_x86", "after resume",
           reg_state_match(&cur, expect) ? "{\"match\":true}" : "{\"match\":false}");

    p0_expect_u64(__LINE__, "post-yield rbx", cur.rbx, expect->rbx);
    p0_expect_u64(__LINE__, "post-yield rbp", cur.rbp, expect->rbp);
    p0_expect_u64(__LINE__, "post-yield r12", cur.r12, expect->r12);
    p0_expect_u64(__LINE__, "post-yield r13", cur.r13, expect->r13);
    p0_expect_u64(__LINE__, "post-yield r14", cur.r14, expect->r14);
    p0_expect_u64(__LINE__, "post-yield r15", cur.r15, expect->r15);
    p0_expect_u64(__LINE__, "post-yield mxcsr",
                  (unsigned long long)cur.mxcsr,
                  (unsigned long long)expect->mxcsr);
    p0_expect_u64(__LINE__, "post-yield x87cw",
                  (unsigned long long)cur.x87cw,
                  (unsigned long long)expect->x87cw);

    expect->phase = 2;
}

void test_regs(void)
{
#if defined(__SANITIZE_ADDRESS__)
    fprintf(stderr, "SKIP test_regs: inline asm callee-saved probe conflicts with ASan instrumentation\n");
    p0_log("H1", "test_regs.c:test_regs", "skipped under ASan", "{}");
    return;
#endif
    reg_state_t st = {
        .rbx  = 0x0123456789abcdefULL,
        .rbp  = 0xfedcba9876543210ULL,
        .r12  = 0x1111222233334444ULL,
        .r13  = 0x5555666677778888ULL,
        .r14  = 0x9999aaaabbbbccccULL,
        .r15  = 0xddddeeeeffff0000ULL,
        .mxcsr = 0x00001f80u,
        .x87cw = 0x037fu,
        .phase = 0,
    };

    st.mxcsr = (st.mxcsr & ~0x6000u) | 0x2000u;

    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, fn_regs_x86, &st);
    if (!co) {
        p0_log("H1", "test_regs.c:test_regs", "co_create failed", "{}");
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "phase after yield", st.phase, 0);

    poison_callee_saved_x86();
    p0_log("H1", "test_regs.c:test_regs", "caller poisoned callee-saved before resume2",
           "{}");

    p0_expect(__LINE__, "resume2", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "phase done", st.phase, 2);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);

    p0_log("H1", "test_regs.c:test_regs", "x86 gpr test finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
    p0_log("H2", "test_regs.c:test_regs", "x86 fp control test finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
}

#elif defined(__aarch64__)

typedef struct {
    unsigned long long x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    unsigned long long fp;
    unsigned long long d8, d9, d10, d11, d12, d13, d14, d15;
    int                phase;
} reg_state_a64_t;

static void load_a64(const reg_state_a64_t *st)
{
    __asm__ volatile(
        "mov x9, %0\n\t"
        "mov x19, x9\n\t"
        "mov x9, %1\n\t"
        "mov x20, x9\n\t"
        "mov x9, %2\n\t"
        "mov x21, x9\n\t"
        "mov x9, %3\n\t"
        "mov x22, x9\n\t"
        "mov x9, %4\n\t"
        "mov x23, x9\n\t"
        "mov x9, %5\n\t"
        "mov x24, x9\n\t"
        "mov x9, %6\n\t"
        "mov x25, x9\n\t"
        "mov x9, %7\n\t"
        "mov x26, x9\n\t"
        "mov x9, %8\n\t"
        "mov x27, x9\n\t"
        "mov x9, %9\n\t"
        "mov x28, x9\n\t"
        "mov x9, %10\n\t"
        "mov x29, x9\n\t"
        "fmov d8, %11\n\t"
        "fmov d9, %12\n\t"
        "fmov d10, %13\n\t"
        "fmov d11, %14\n\t"
        "fmov d12, %15\n\t"
        "fmov d13, %16\n\t"
        "fmov d14, %17\n\t"
        "fmov d15, %18\n\t"
        :
        : "m"(st->x19), "m"(st->x20), "m"(st->x21), "m"(st->x22),
          "m"(st->x23), "m"(st->x24), "m"(st->x25), "m"(st->x26),
          "m"(st->x27), "m"(st->x28), "m"(st->fp),
          "m"(st->d8), "m"(st->d9), "m"(st->d10), "m"(st->d11),
          "m"(st->d12), "m"(st->d13), "m"(st->d14), "m"(st->d15)
        : "x9", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26",
          "x27", "x28", "memory");
}

static void store_a64(reg_state_a64_t *out)
{
    __asm__ volatile(
        "mov %0, x19\n\t"
        "mov %1, x20\n\t"
        "mov %2, x21\n\t"
        "mov %3, x22\n\t"
        "mov %4, x23\n\t"
        "mov %5, x24\n\t"
        "mov %6, x25\n\t"
        "mov %7, x26\n\t"
        "mov %8, x27\n\t"
        "mov %9, x28\n\t"
        "mov %10, x29\n\t"
        "fmov %11, d8\n\t"
        "fmov %12, d9\n\t"
        "fmov %13, d10\n\t"
        "fmov %14, d11\n\t"
        "fmov %15, d12\n\t"
        "fmov %16, d13\n\t"
        "fmov %17, d14\n\t"
        "fmov %18, d15\n\t"
        : "=m"(out->x19), "=m"(out->x20), "=m"(out->x21), "=m"(out->x22),
          "=m"(out->x23), "=m"(out->x24), "=m"(out->x25), "=m"(out->x26),
          "=m"(out->x27), "=m"(out->x28), "=m"(out->fp),
          "=m"(out->d8), "=m"(out->d9), "=m"(out->d10), "=m"(out->d11),
          "=m"(out->d12), "=m"(out->d13), "=m"(out->d14), "=m"(out->d15)
        :
        : "memory");
}

static void poison_a64(void)
{
    reg_state_a64_t poison = {
        .x19 = 0xababababababababULL,
        .x20 = 0xbcbcbcbcbcbcbcbcULL,
        .x21 = 0xcdcdcdcdcdcdcdcdULL,
        .x22 = 0xdedededededededeULL,
        .x23 = 0xefefefefefefefefULL,
        .x24 = 0xfafafafafafafafaULL,
        .x25 = 0x1212121212121212ULL,
        .x26 = 0x2323232323232323ULL,
        .x27 = 0x3434343434343434ULL,
        .x28 = 0x4545454545454545ULL,
        .fp  = 0x5656565656565656ULL,
        .d8  = 0x3ff0000000000000ULL, .d9  = 0x3ff8000000000000ULL,
        .d10 = 0x4000000000000000ULL, .d11 = 0x4004000000000000ULL,
        .d12 = 0x4008000000000000ULL, .d13 = 0x400a000000000000ULL,
        .d14 = 0x400c000000000000ULL, .d15 = 0x400e000000000000ULL,
        .phase = 0,
    };
    load_a64(&poison);
}

static void fn_regs_a64(coroutine *self, void *userdata, void *initial_input)
{
    reg_state_a64_t *st = userdata;
    reg_state_a64_t  cur;

    (void)self;
    (void)initial_input;

    load_a64(st);
    store_a64(&cur);

    p0_expect_u64(__LINE__, "pre-yield x19", cur.x19, st->x19);
    p0_expect(__LINE__, "yield", P0_YIELD(), CO_RESULT_OK);

    store_a64(&cur);
    p0_log("H1", "test_regs.c:fn_regs_a64", "after resume",
           "{\"arch\":\"aarch64\"}");

    p0_expect_u64(__LINE__, "post-yield x19", cur.x19, st->x19);
    p0_expect_u64(__LINE__, "post-yield x28", cur.x28, st->x28);
    p0_expect_u64(__LINE__, "post-yield fp", cur.fp, st->fp);
    p0_expect_u64(__LINE__, "post-yield d8", cur.d8, st->d8);
    p0_expect_u64(__LINE__, "post-yield d15", cur.d15, st->d15);

    st->phase = 2;
}

void test_regs(void)
{
#if defined(__SANITIZE_ADDRESS__)
    fprintf(stderr, "SKIP test_regs: inline asm callee-saved probe conflicts with ASan instrumentation\n");
    p0_log("H1", "test_regs.c:test_regs", "skipped under ASan", "{}");
    return;
#endif
    reg_state_a64_t st = {
        .x19 = 0x0123456789abcdefULL,
        .x20 = 0xfedcba9876543210ULL,
        .x21 = 0x1111222233334444ULL,
        .x22 = 0x5555666677778888ULL,
        .x23 = 0x9999aaaabbbbccccULL,
        .x24 = 0xddddeeeeffff0000ULL,
        .x25 = 0x1357246813572468ULL,
        .x26 = 0x2468135724681357ULL,
        .x27 = 0xacefacefacefacefULL,
        .x28 = 0xdeadbeefcafebabeULL,
        .fp  = 0x0badc0de0badc0deULL,
        .d8  = 0x3ff2000000000000ULL, .d9  = 0x4002000000000000ULL,
        .d10 = 0x400b000000000000ULL, .d11 = 0x4012000000000000ULL,
        .d12 = 0x4016800000000000ULL, .d13 = 0x401b000000000000ULL,
        .d14 = 0x401f800000000000ULL, .d15 = 0x4022000000000000ULL,
        .phase = 0,
    };

    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, fn_regs_a64, &st);
    if (!co) {
        g_p0_failures++;
        return;
    }

    p0_expect(__LINE__, "resume", P0_RESUME(co), CO_RESULT_OK);

    poison_a64();
    p0_log("H1", "test_regs.c:test_regs", "caller poisoned a64 regs before resume2",
           "{}");

    p0_expect(__LINE__, "resume2", P0_RESUME(co), CO_RESULT_OK);
    p0_expect(__LINE__, "phase done", st.phase, 2);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);

    p0_log("H1", "test_regs.c:test_regs", "aarch64 reg test finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
    p0_log("H2", "test_regs.c:test_regs", "aarch64 d8-d15 test finished",
           g_p0_failures ? "{\"ok\":false}" : "{\"ok\":true}");
}

#else

void test_regs(void)
{
#if defined(__SANITIZE_ADDRESS__)
    fprintf(stderr, "SKIP test_regs: inline asm callee-saved probe conflicts with ASan instrumentation\n");
    p0_log("H1", "test_regs.c:test_regs", "skipped under ASan", "{}");
    return;
#endif
    fprintf(stderr, "SKIP test_regs: unsupported arch\n");
    p0_log("H1", "test_regs.c:test_regs", "skipped unsupported arch", "{}");
}

#endif
