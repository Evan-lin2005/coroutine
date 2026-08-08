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
 * co_function — 協程本體回呼。
 *
 * 契約：
 *   - callback 不可拋出 C++ 例外（本實作為 C，無法攔截）
 *   - callback 不可 longjmp 到協程外部的 jmp_buf
 *   - callback 內配置的資源必須在返回前自行釋放；C 沒有解構子，
 *     而掛起中的協程不允許銷毀（co_destroy）
 *
 * 入口參數（與 co_create 的 argument 分離）：
 *   - 首次 co_resume 傳入的 arg 會經 transfer 信箱交給本回呼，作為 argument 指標。
 *   - co_create / co_create_ex 的 argument 僅存入 coroutine，不參與切換傳值；
 *     可作 userdata（例如固定上下文），勿與 resume/yield 的 arg/out 混用。
 */
typedef void (*co_function)(void *argument);

/*
 * 平台暫存器契約：
 * - x86_64 SystemV / Win64：切換保存 callee-saved GPR、MXCSR/x87 CW；Win64 另保存 xmm6–xmm15。
 * - AArch64：保存 x19–x28、fp（x29）、d8–d15。
 * - AArch64 不保存 FPCR/FPSR（非 ABI callee-saved）；跨切換後 FP 環境暫存器可能改變，屬預期行為。
 *
 * 巢狀 resume：外層協程在子協程執行期間為 CO_WAITING，不可 co_resume / co_destroy。
 */

/*
 * co_create — 簡便建立協程。
 * 成功回傳 coroutine *；任一失敗（參數不合法或 OOM）皆回 NULL，無法區分原因。
 * 若需明確錯誤碼（例如 CO_RESULT_OUT_OF_MEMORY），請改用 co_create_ex。
 *
 * argument：僅寫入 coroutine 內部欄位，不經 transfer 傳遞；協程入口參數由首次
 * co_resume(co, arg, ...) 的 arg 提供（見下方 transfer 契約）。
 */
coroutine *co_create(size_t stack_size, co_function function, void *argument);

/*
 * co_create_ex — 建立協程並回傳 co_result。
 * 成功：CO_RESULT_OK，*out 指向新協程（*out 不可為 NULL）。
 * 失敗：*out 設為 NULL，並回傳
 *   CO_RESULT_INVALID_ARGUMENT — out 為 NULL、function 為 NULL、或 stack_size 小於 CO_MIN_STACK_SIZE
 *   CO_RESULT_OUT_OF_MEMORY    — calloc 或平台 stack 配置失敗
 *
 * argument 語意同 co_create（userdata，與 resume/yield 傳值分離）。
 */
co_result  co_create_ex(size_t stack_size, co_function function, void *argument,
                        coroutine **out);

/*
 * Transfer 契約（resume / yield 傳值，Lua 風格 void*）
 * ----------------------------------------------------------------
 * 每條協程有一個 transfer 收件匣。切換時：
 *   - 切出方寫入「收件人」信箱：to->transfer = data（data 即 arg）
 *   - 切回後讀「自己」信箱：got = from->transfer（對方剛寫入的值）
 *
 * co_resume(co, arg, out) — 從當前協程 resume 目標 co：
 *   - arg：傳給目標協程的值（首次 resume 時作為 co_function 的入口參數）。
 *   - out：可為 NULL；非 NULL 時，resume 返回後 *out 為目標 yield 或結束時傳回的值。
 *     若目標已 CO_DONE（正常跑完），*out 強制為 NULL（與 co_finished 一致）。
 *
 * co_yield_now(arg, out) — 從當前協程 yield 回 caller：
 *   - arg：傳給 caller 的值。
 *   - out：可為 NULL；非 NULL 時，yield 返回後（下次被 resume 醒來）*out 為
 *     caller 此次 resume 傳入的 arg。
 *
 * 協程內讀「對方上次傳入的值」：在 co_yield_now 返回後讀 *out，勿用 co_create 的 argument。
 * 公開 API 使用 co_yield_now 而非 co_yield，以相容 C++。
 */
co_result  co_resume(coroutine *co, void *arg, void **out);
co_result  co_yield_now(void *arg, void **out);
co_result  co_destroy(coroutine *co);
int        co_finished(const coroutine *co);

/* 以 -DCO_DEBUG_STACK_USAGE 編譯，否則失效*/
size_t     co_stack_peak(const coroutine *co);

#ifdef __cplusplus
}
#endif

#endif /* COROUTINE_H */