#ifndef COROUTINE_H
#define COROUTINE_H

#include <stddef.h>

#define CO_MIN_STACK_SIZE     ((size_t)(16u * 1024u))
#define CO_DEFAULT_STACK_SIZE ((size_t)(64u * 1024u))

#ifdef __cplusplus
extern "C" {
#endif

typedef enum co_result {
    CO_RESULT_OK = 0,
    CO_RESULT_INVALID_ARGUMENT,
    CO_RESULT_ALREADY_RUNNING,
    CO_RESULT_FINISHED,
    CO_RESULT_NO_CALLER,
    CO_RESULT_WRONG_THREAD,
    CO_RESULT_INVALID_STATE,
    CO_RESULT_OUT_OF_MEMORY
} co_result;

typedef struct coroutine coroutine;

/* 
 *   - callback 不可拋出 C++ 例外（本實作為 C，無法攔截）
 *   - callback 不可 longjmp 到協程外部的 jmp_buf
 *   - callback 內配置的資源必須在返回前自行釋放；C 沒有解構子，
 *     而掛起中的協程不允許銷毀（co_destroy）
 */
typedef void (*co_function)(void *argument);

coroutine *co_create(size_t stack_size, co_function function, void *argument);
co_result  co_resume(coroutine *co);
co_result  co_yield_now(void);
co_result  co_destroy(coroutine *co);
int        co_finished(const coroutine *co);

/* 以 -DCO_DEBUG_STACK_USAGE 編譯，否則失效*/
size_t     co_stack_peak(const coroutine *co);

#ifdef __cplusplus
}
#endif

#endif /* COROUTINE_H */