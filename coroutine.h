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
 * 資料模型（三通道分離）
 * ----------------------------------------------------------------
 *   userdata — 建立時綁定的固定上下文（state），生命週期同協程
 *   mailbox  — resume/yield 的一次性訊息（message），讀取後清空
 *   storage  — 呼叫端提供的長期記憶體區（memory）
 *
 * ownership：庫只搬運 void*，不負責指向物件的配置／釋放。
 */

/*
 * co_function — 協程本體回呼。
 *
 * 契約：
 *   - callback 不可拋出 C++ 例外（本實作為 C，無法攔截）
 *   - callback 不可 longjmp 到協程外部的 jmp_buf
 *   - callback 內配置的資源必須在返回前自行釋放；C 沒有解構子，
 *     而掛起中的協程不允許銷毀（co_destroy）
 *
 * 參數：
 *   - self           — 本協程（等同協程內 co_current()）
 *   - userdata       — co_create 綁定的固定上下文（亦可用 co_userdata）
 *   - initial_input  — 首次 co_resume 經 mailbox 傳入的訊息；之後傳值
 *                      只走 co_yield_now 的 next_input
 */
typedef void (*co_function)(coroutine *self, void *userdata, void *initial_input);

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
 * userdata：寫入協程固定上下文，不經 mailbox；可用 co_userdata 讀回。
 * 首次訊息由 co_resume(co, input, ...) 的 input 作為 initial_input。
 */
coroutine *co_create(size_t stack_size, co_function function, void *userdata);

/*
 * co_create_ex — 建立協程並回傳 co_result。
 * 成功：CO_RESULT_OK，*out 指向新協程（*out 不可為 NULL）。
 * 失敗：*out 設為 NULL，並回傳
 *   CO_RESULT_INVALID_ARGUMENT — out 為 NULL、function 為 NULL、stack_size 小於 CO_MIN_STACK_SIZE、
 *     或自訂 allocator 回傳未達 _Alignof(struct coroutine) 對齊的指標
 *   CO_RESULT_OUT_OF_MEMORY    — 物件 allocator 或平台 stack 配置失敗
 *
 * userdata 語意同 co_create。
 */
co_result  co_create_ex(size_t stack_size, co_function function, void *userdata,
                        coroutine **out);

/*
 * Mailbox 契約（resume / yield 一次性訊息）
 * ----------------------------------------------------------------
 * 每條協程有一個 mailbox。切換時：
 *   - 切出方寫入收件人：to->mailbox = outgoing
 *   - 切回後讀自己信箱並清空：incoming = from->mailbox; from->mailbox = NULL
 *
 * co_resume(co, input, output) — 從當前協程 resume 目標 co：
 *   - input：傳給目標的訊息（首次 resume 時作為 co_function 的 initial_input）
 *   - output：可為 NULL；非 NULL 時，resume 返回後 *output 為目標 yield 傳回的值。
 *     若目標已 CO_DONE（正常跑完），*output 強制為 NULL。
 *
 * co_yield_now(output, next_input) — 從當前協程 yield 回 caller：
 *   - output：傳給 caller 的訊息（出現在 caller 的 *output）
 *   - next_input：可為 NULL；非 NULL 時，yield 返回後（下次被 resume 醒來）
 *     *next_input 為 caller 此次 resume 的 input
 *
 * 公開 API 使用 co_yield_now 而非 co_yield，以相容 C++。
 */
co_result  co_resume(coroutine *co, void *input, void **output);
co_result  co_yield_now(void *output, void **next_input);
co_result  co_destroy(coroutine *co);
int        co_finished(const coroutine *co);

/* 以 -DCO_DEBUG_STACK_USAGE 編譯，否則失效*/
size_t     co_stack_peak(const coroutine *co);

/*
 * 目前執行中的協程（含主協程 TLS 槽）。首次呼叫會確保已初始化。
 */
coroutine *co_current(void);

/*
 * 讀取 create 時綁定的 userdata；co == NULL 時回 NULL。
 * userdata 生命週期同協程；庫不擁有其指向的物件。
 */
void      *co_userdata(const coroutine *co);

/*
 * Storage 契約（可選 per-coroutine buffer，呼叫端自有記憶體）
 * ----------------------------------------------------------------
 * 與 mailbox（每次切換一個 void*）、userdata（固定上下文）分離：
 *   - Storage 是綁在協程上的 byte 區，供協程內長期讀寫（跨 yield 保留內容）。
 *   - 庫只記錄 buf 指標與 cap，不 malloc、不 free、不拷貝、不解析 layout。
 *
 * 典型流程：co_create → co_set_storage(co, buf, cap) → co_resume(...)；
 * 協程內透過 co_storage(self) 或 userdata 取得上下文。
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
 * 配置對象：struct coroutine 控制塊（context、狀態、mailbox 等）。
 * 不經 allocator 的資源：
 *   - 執行 stack — 預設 co_stack_create（mmap/VirtualAlloc + guard）
 *   - storage buffer — co_set_storage 由呼叫端提供
 *   - mailbox / userdata — 僅存 void*，指向呼叫端資料
 *   - TLS main_coroutine — 靜態，不經 heap
 *
 * 與 mailbox、storage、userdata 分離：
 *   - userdata / storage 管「業務資料」；allocator 管「庫的執行控制塊」。
 *   - 高頻 co_create/co_destroy 時，自訂 allocator 可讓控制塊走 arena/pool，
 *     而不必讓每次 libc calloc/free。
 *
 * 流程：co_set_allocator(&a) → co_create / co_destroy（建議在首次 co_create 前設定）。
 *
 *
 * co_allocator：
 *   - alloc(size, ud) — 配置 size 位元組；成功回傳指標或 NULL。
 *     庫在 co_create_ex 傳入 sizeof(struct coroutine)；自訂 alloc 不保證清零，
 *     庫會在自訂路徑 memset。回傳指標須滿足 _Alignof(struct coroutine) 對齊
 *     （Linux x86 通常 8、Win64 為 16）；未達標回 CO_RESULT_INVALID_ARGUMENT。
 *     CO_ALLOC_ALIGN 為跨平台最壞情況上限承諾。
 *   - free(ptr, size, ud) — 釋放先前 alloc 回傳的 ptr；size 與 alloc 時相同。
 *     可為 NULL：arena / bump 等不逐筆回收時，co_destroy 對控制塊為 no-op。
 *   - userdata — 每次 alloc/free 傳入，可綁 arena / pool 上下文。
 *
 * co_set_allocator(a)：
 *   - 進程級：拷貝 *a 到庫內 g_allocator（非持有指標）；之後 create/destroy 皆用此副本。
 *   - a == NULL，或 alloc 與 free 皆 NULL：還原預設 libc（calloc / free）。
 *   - 設自訂時 alloc 必須非 NULL；free 可為 NULL（僅 alloc 的 arena 語意）。
 *   - alloc 為 NULL 但 free 非 NULL：視為無效，還原預設 libc。
 *   - 已有活協程時更換 allocator 為 Undefined Behavior；建議程式啟動、尚未 co_create 時設定。
 */
#define CO_ALLOC_ALIGN 16u /* 跨平台最壞情況；執行期校驗用 _Alignof(struct coroutine) */

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
