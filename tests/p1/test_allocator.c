/*
 * P1: custom allocator + external stack injection.
 * 測試 allocator 不用 aligned_alloc（C11 / size 倍數限制 / 部分平台缺失）。
 */

#include "p1_common.h"
#include "coroutine_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int g_alloc_n;
static int g_free_n;
static size_t g_last_size;

static void fn_nop(coroutine *self, void *userdata, void *initial_input)
{
    (void)self;
    (void)userdata;
    (void)initial_input;
}

/*
 * 可攜對齊配置：malloc + 手動對齊，原始指標存在對齊位址前方。
 * 避開 aligned_alloc（需 C11、size 為 align 倍數、MSVC/舊 libc 常缺）。
 */
static void *portable_aligned_alloc(size_t align, size_t size)
{
    void *raw;
    uintptr_t base;
    uintptr_t aligned;
    void **slot;

    if (align < sizeof(void *))
        align = sizeof(void *);

    raw = malloc(size + align + sizeof(void *));
    if (!raw)
        return NULL;

    base    = (uintptr_t)raw + sizeof(void *);
    aligned = (base + (align - 1u)) & ~(uintptr_t)(align - 1u);
    slot    = (void **)aligned;
    slot[-1] = raw;
    return (void *)aligned;
}

static void portable_aligned_free(void *ptr)
{
    void *raw;

    if (!ptr)
        return;
    raw = ((void **)ptr)[-1];
    free(raw);
}

static void *count_alloc(size_t size, void *ud)
{
    (void)ud;
    void *p = portable_aligned_alloc(CO_ALLOC_ALIGN, size);
    if (!p)
        return NULL;
    g_alloc_n++;
    g_last_size = size;
    return p;
}

static void count_free(void *ptr, size_t size, void *ud)
{
    (void)ud;
    g_free_n++;
    g_last_size = size;
    portable_aligned_free(ptr);
}

void test_allocator_counting(void)
{
    co_allocator a = {
        .alloc    = count_alloc,
        .free     = count_free,
        .userdata = NULL,
    };

    g_alloc_n = g_free_n = 0;
    co_set_allocator(&a);

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co) {
        g_p0_failures++;
        co_set_allocator(NULL);
        return;
    }

    p0_expect(__LINE__, "alloc once", g_alloc_n, 1);
    p0_expect(__LINE__, "destroy", co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "free once", g_free_n, 1);
    if (g_last_size < sizeof(struct coroutine)) {
        fprintf(stderr, "FAIL line %d: alloc size too small %zu\n",
                __LINE__, g_last_size);
        g_p0_failures++;
    }

    co_set_allocator(NULL);
}

/* create 後換回預設 allocator，destroy 仍須走快照的 count_free */
void test_allocator_snapshot(void)
{
    co_allocator a = {
        .alloc    = count_alloc,
        .free     = count_free,
        .userdata = NULL,
    };

    g_alloc_n = g_free_n = 0;
    co_set_allocator(&a);

    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co) {
        g_p0_failures++;
        co_set_allocator(NULL);
        return;
    }

    p0_expect(__LINE__, "alloc once", g_alloc_n, 1);

    /* 換回 libc；若無 snapshot，destroy 會對自訂區塊呼叫 free → mismatch */
    co_set_allocator(NULL);
    p0_expect(__LINE__, "destroy after switch", co_destroy(co), CO_RESULT_OK);
    p0_expect(__LINE__, "custom free still used", g_free_n, 1);
}

static void *oom_alloc(size_t size, void *ud)
{
    (void)size;
    (void)ud;
    return NULL;
}

static void oom_free(void *ptr, size_t size, void *ud)
{
    (void)ptr;
    (void)size;
    (void)ud;
}

void test_allocator_oom(void)
{
    co_allocator a = {
        .alloc    = oom_alloc,
        .free     = oom_free,
        .userdata = NULL,
    };
    coroutine   *co = NULL;

    co_set_allocator(&a);
    p0_expect(__LINE__, "create oom",
              co_create_ex(CO_MIN_STACK_SIZE, fn_nop, NULL, &co),
              CO_RESULT_OUT_OF_MEMORY);
    p0_expect_ptr(__LINE__, "out null", co, NULL);
    co_set_allocator(NULL);
}

void test_allocator_restore_default(void)
{
    co_set_allocator(NULL);
    coroutine *co = co_create(CO_MIN_STACK_SIZE, fn_nop, NULL);
    if (!co) {
        g_p0_failures++;
        return;
    }
    p0_expect(__LINE__, "destroy default", co_destroy(co), CO_RESULT_OK);
}

static unsigned char g_misalign_buf[sizeof(struct coroutine) + 16];
static int           g_misalign_free_n;

static void *misalign_alloc(size_t size, void *ud)
{
    (void)ud;
    (void)size;
    return (void *)((uintptr_t)g_misalign_buf + 1);
}

static void misalign_free(void *ptr, size_t size, void *ud)
{
    (void)ptr;
    (void)size;
    (void)ud;
    g_misalign_free_n++;
}

void test_allocator_misaligned_reject(void)
{
    co_allocator a = {
        .alloc    = misalign_alloc,
        .free     = misalign_free,
        .userdata = NULL,
    };
    coroutine   *co = NULL;

    g_misalign_free_n = 0;
    co_set_allocator(&a);
    p0_expect(__LINE__, "misalign create",
              co_create_ex(CO_MIN_STACK_SIZE, fn_nop, NULL, &co),
              CO_RESULT_INVALID_ARGUMENT);
    p0_expect(__LINE__, "misalign freed", g_misalign_free_n, 1);
    co_set_allocator(NULL);
}

void test_external_stack_destroy(void)
{
    void             *base = malloc(CO_MIN_STACK_SIZE);
    struct co_stack   st;

    if (!base) {
        g_p0_failures++;
        return;
    }

    memset(&st, 0, sizeof st);
    p0_expect(__LINE__, "create_from ok",
              co_stack_create_from(&st, base, CO_MIN_STACK_SIZE), 0);
    p0_expect(__LINE__, "external", st.external, 1);
    p0_expect_ptr(__LINE__, "lo is base", st.lo, base);

    co_stack_destroy(&st);
    p0_expect_ptr(__LINE__, "cleared base", st.base, NULL);
    free(base);
}
