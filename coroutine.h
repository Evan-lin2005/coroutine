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
 *   CO_RESULT_OUT_OF_MEMORY    — 物件 allocator 或平台 stack 配置失敗
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

/*
 * Storage 契約（可選 per-coroutine buffer，呼叫端自有記憶體）
 * ----------------------------------------------------------------
 * 與 transfer（每次切換傳一個 void*）、co_create 的 argument（userdata）分離：
 *   - Storage 是綁在協程上的 byte 區，供協程內長期讀寫（跨 yield 保留內容）。
 *   - 庫只記錄 buf 指標與 cap，不 malloc、不 free、不拷貝、不解析 layout。
 *
 * 典型流程：co_create → co_set_storage(co, buf, cap) → co_resume(...)；
 * 協程內透過 co_storage(co) 取得指標（或 co_create 的 userdata 持有 co*）。
 *
 * co_set_storage(co, buf, cap) — 綁定或清除 storage：
 *   - 僅允許 CO_READY（建立後、首次 co_resume 前）；跑過或掛起後回 CO_RESULT_INVALID_STATE。
 *   - buf != NULL 且 cap > 0：綁定；可再次呼叫以替換綁定（舊 buffer 由呼叫端管理）。
 *   - buf == NULL 且 cap == 0：清除綁定。
 *   - buf 與 cap 僅一方為零（buf 非 NULL 但 cap==0，或 buf==NULL 但 cap>0）：
 *     CO_RESULT_INVALID_ARGUMENT。
 *   - 非建立執行緒：CO_RESULT_WRONG_THREAD。
 *
 * co_storage(co) / co_storage_size(co) — 唯讀查詢：
 *   - 未綁定或 co==NULL：co_storage 回 NULL，co_storage_size 回 0。
 *   - 不檢查執行緒；跨 thread 讀寫 buffer 內容為 UB。
 *
 * 生命週期：
 *   - buf 須在協程仍可能存取 storage 期間有效（至少至 co_destroy 完成後才可釋放）。
 *   - co_destroy 不觸碰 buf；與外部 stack 策略一致，buffer 由呼叫端負責保護與釋放。
 *   - 協程掛起期間若呼叫端已釋放 buf 而協程仍持有指標 → UB。
 *   - 對齊、struct layout、是否含指標由呼叫端負責；預設不配置，避免庫內隱式 heap。
 */
co_result  co_set_storage(coroutine *co, void *buf, size_t cap);
void      *co_storage(coroutine *co);
size_t     co_storage_size(coroutine *co);

/*
 * Allocator 契約（custom allocator）
 * ----------------------------------------------------------------
 * 配置對象：struct coroutine 控制塊（context、狀態、transfer 等）。
 * 不經 allocator 的資源：
 *   - 執行 stack — 預設 co_stack_create（mmap/VirtualAlloc + guard）
 *   - storage buffer — co_set_storage 由呼叫端提供
 *   - transfer / argument — 僅存 void*，指向呼叫端資料
 *   - TLS main_coroutine — 靜態，不經 heap
 *
 * 與 transfer、storage、argument 分離：
 *   - argument / storage 管「業務資料」；allocator 管「庫的執行控制塊」。
 *   - 高頻 co_create/co_destroy 時，自訂 allocator 可讓控制塊走 arena/pool，
 *     而不必讓每次 libc calloc/free。
 *
 * 流程：co_set_allocator(&a) → co_create / co_destroy（建議在首次 co_create 前設定）。
 *
 *
 * co_allocator：
 *   - alloc(size, ud) — 配置 size 位元組；成功回傳指標或 NULL。
 *     庫在 co_create_ex 傳入 sizeof(struct coroutine)；自訂 alloc 不保證清零，
 *     庫會在自訂路徑 memset。
 *   - free(ptr, size, ud) — 釋放先前 alloc 回傳的 ptr；size 與 alloc 時相同。
 *   - userdata — 每次 alloc/free 傳入，可綁 arena / pool 上下文。
 *
 * co_set_allocator(a)：
 *   - 進程級：拷貝 *a 到庫內 g_allocator（非持有指標）；之後 create/destroy 皆用此副本。
 *   - a == NULL：還原預設 libc（calloc / free）。
 *   - 設自訂時 alloc 與 free 皆非 NULL。
 *   - 已有活協程時更換 allocator 為 Undefined Behavior；建議程式啟動、尚未 co_create 時設定。
 */
#define CO_ALLOC_ALIGN 16u

typedef struct co_allocator {
    void *(*alloc)(size_t size, void *ud);
    void  (*free)(void *ptr, size_t size, void *ud);
    void  *userdata;
} co_allocator;

/* NULL = 還原預設 libc；非 NULL = 拷貝 *a 為進程級物件 allocator */
void co_set_allocator(const co_allocator *a);

#ifdef __cplusplus
}
#endif

#endif /* COROUTINE_H */