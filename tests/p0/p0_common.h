#ifndef P0_COMMON_H
#define P0_COMMON_H

#include "coroutine.h"
#include "p0_debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_p0_failures;

#ifdef __SANITIZE_THREAD__
#  define CO_TEST_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CO_TEST_TSAN 1
#  endif
#endif
#ifndef CO_TEST_TSAN
#  define CO_TEST_TSAN 0
#endif

#ifdef __SANITIZE_ADDRESS__
#  define CO_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CO_TEST_ASAN 1
#  endif
#endif
#ifndef CO_TEST_ASAN
#  define CO_TEST_ASAN 0
#endif

/* resume/yield：無傳值時傳 NULL */
#define P0_RESUME(co)             co_resume((co), NULL, NULL)
#define P0_RESUME_IN(co, input)   co_resume((co), (input), NULL)
#define P0_YIELD()                co_yield_now(NULL, NULL)

#define p0_expect(line, name, got, want)                                      \
    do {                                                                        \
        if ((got) != (want)) {                                                  \
            fprintf(stderr, "FAIL line %d: %s got=%d want=%d\n",              \
                    (line), (name), (int)(got), (int)(want));                   \
            g_p0_failures++;                                                    \
            char _p0b[128];                                                     \
            snprintf(_p0b, sizeof _p0b,                                        \
                     "{\"line\":%d,\"name\":\"%s\",\"got\":%d,\"want\":%d}",   \
                     (line), (name), (int)(got), (int)(want));                  \
            p0_log("FAIL", "p0_common.h:p0_expect", "assertion failed", _p0b);  \
        }                                                                       \
    } while (0)

#define p0_expect_ptr(line, name, got, want)                                   \
    do {                                                                        \
        const void *_g = (const void *)(got);                                   \
        const void *_w = (const void *)(want);                                  \
        if (_g != _w) {                                                         \
            fprintf(stderr, "FAIL line %d: %s got=%p want=%p\n",                \
                    (line), (name), _g, _w);                                    \
            g_p0_failures++;                                                    \
            char _p0b[160];                                                     \
            snprintf(_p0b, sizeof _p0b,                                         \
                     "{\"line\":%d,\"name\":\"%s\",\"got\":\"%p\","            \
                     "\"want\":\"%p\"}",                                        \
                     (line), (name), _g, _w);                                   \
            p0_log("FAIL", "p0_common.h:p0_expect_ptr", "ptr mismatch", _p0b);  \
        }                                                                       \
    } while (0)

#define p0_expect_u64(line, name, got, want)                                   \
    do {                                                                        \
        unsigned long long _g = (unsigned long long)(got);                      \
        unsigned long long _w = (unsigned long long)(want);                     \
        if (_g != _w) {                                                         \
            fprintf(stderr, "FAIL line %d: %s got=0x%llx want=0x%llx\n",        \
                    (line), (name), _g, _w);                                    \
            g_p0_failures++;                                                    \
            char _p0b[160];                                                     \
            snprintf(_p0b, sizeof _p0b,                                         \
                     "{\"line\":%d,\"name\":\"%s\",\"got\":\"0x%llx\","        \
                     "\"want\":\"0x%llx\"}",                                    \
                     (line), (name), _g, _w);                                   \
            p0_log("FAIL", "p0_common.h:p0_expect_u64", "u64 mismatch", _p0b);  \
        }                                                                       \
    } while (0)

#endif /* P0_COMMON_H */
