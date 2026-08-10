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
 * 資料模型（四通道分離）
 * ----------------------------------------------------------------
 *   userdata — 建立時綁定的固定上下文（state），生命週期同協程
 *   mailbox  — resume/yield 的一次性訊息（message），讀取後清空
 *   storage  — 呼叫端提供的長期記憶體區（memory），由呼叫端管理
 *   CLS      — process-global key、per-coroutine 的 void* 槽（runtime metadata）
 *
 * ownership：庫只搬運 void*，不負責指向物件的配置／釋放。
 */

/*
 * 執行緒契約（owner affinity）
 * ----------------------------------------------------------------
 * 每條協程在 co_create / co_create_ex 時綁定建立執行緒為 owner。
 * 主協程（TLS main）亦綁定該執行緒。
 *
 * Mutating API — 僅允許 owner thread，否則回 CO_RESULT_WRONG_THREAD：
 *   co_resume, co_destroy, co_set_storage
 *
 * co_yield_now — 必須在目標協程執行中呼叫（因此已在 owner thread）；
 *   在主協程或無 caller 時回 CO_RESULT_NO_CALLER / INVALID_STATE。
 *
 * co_cls_set — 寫入目前協程的 CLS 槽；co_cls_get — 讀取（見下方 CLS 契約）。
 *   兩者皆操作 co_current()，不檢查 owner_token；不同 OS thread 的
 *   current_coroutine 互不影響。
 *
 * Query API — 不檢查 owner、不回 WRONG_THREAD：
 *   co_finished, co_userdata, co_storage, co_storage_size, co_stack_peak,
 *   co_cls_get
 *   - 設計意圖：在 owner thread 讀取。
 *   - 與任一 mutating API（或另一執行緒上的查詢／寫入）並行存取同一
 *     coroutine * 為 data race → Undefined Behavior。
 *   - co == NULL 時各有定義回傳值（見各函式註解）。
 *
 * co_current — 回傳呼叫執行緒的 TLS「目前協程」（含 main）；
 *   首次呼叫會初始化該執行緒狀態。只反映本執行緒，不能觀察其他執行緒。
 *
 * co_set_allocator — 進程級；create 時快照進協程（見 allocator 契約）。
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
 * Out 參數（output / next_input）：
 *   - 可為 NULL（表示呼叫端不取回傳值）。
 *   - 非 NULL 時，函式入口即寫入 *ptr = NULL；之後成功路徑再覆寫為實際訊息。
 *   - 因此任一錯誤返回後，*output / *next_input 皆為 NULL（不會殘留呼叫前舊值）。
 *
 * Message pointer lifetime（庫只搬運 void*，不擁有指向物件）：
 *   - ownership：傳入的 input / output 指標本體由呼叫端擁有；庫不 malloc、不 free、不深拷貝。
 *   - 有效期：指向的物件須在收件方「讀取並用完」前保持有效。典型上：
 *       · resume 的 input：至少活到目標協程消費後（首次為 callback 的 initial_input；
 *         其後為對方 co_yield_now 返回的 *next_input）。
 *       · yield 的 output：至少活到 caller 的 co_resume 返回並讀完 *output。
 *   - 掛起期間若呼叫端已釋放訊息指向的記憶體而對方仍持有該 void* → Undefined Behavior。
 *   - 讀後清空：mailbox 消費後即為 NULL，不可當成跨多次切換的永久狀態。
 *
 * Normal-return semantics（callback 正常返回）：
 *   - co_function 返回後協程進入 CO_DONE；trampoline 將 caller->mailbox 設為 NULL。
 *   - 因此成功的最後一次 co_resume 在目標結束時：*output == NULL（若 output 非 NULL）。
 *   - 沒有「函式回傳值」通道；若需傳回結果，必須在返回前以 co_yield_now(output, ...) 送出，
 *     或寫入 userdata / storage。正常結束本身不攜帶訊息。
 *
 * co_resume(co, input, output) — 從當前協程 resume 目標 co（須為 owner thread）：
 *   - input：傳給目標的訊息（首次 resume 時作為 co_function 的 initial_input）
 *   - output：成功且目標 yield 時為其 output；目標 CO_DONE（正常返回）時為 NULL。
 *
 * co_yield_now(output, next_input) — 從當前協程 yield 回 caller：
 *   - output：傳給 caller 的訊息（出現在 caller 的 *output）
 *   - next_input：yield 返回後（下次被 resume 醒來）為 caller 此次 resume 的 input
 *
 * 公開 API 使用 co_yield_now 而非 co_yield，以相容 C++。
 */
co_result  co_resume(coroutine *co, void *input, void **output);
co_result  co_yield_now(void *output, void **next_input);
co_result  co_destroy(coroutine *co);

/*
 * co_finished — 是否已正常跑完（CO_DONE）。
 * co == NULL 視為 finished（回 1）。執行緒契約見上方 Query API。
 */
int        co_finished(const coroutine *co);

/*
 * co_stack_peak — 以 -DCO_DEBUG_STACK_USAGE 編譯時回傳估計峰值用量，否則回 0。
 * co == NULL 或無 stack 時回 0。執行緒契約見上方 Query API。
 */
size_t     co_stack_peak(const coroutine *co);

/*
 * co_current — 本執行緒目前協程（含 TLS main 槽）。
 *
 * 首次呼叫會初始化該執行緒的 main 協程狀態（state=CO_RUNNING）。
 * 只反映呼叫執行緒，無法觀察其他執行緒上的協程。
 *
 * CLS（co_cls_get / co_cls_set）與 callback 的 self 參數皆應與
 * co_current() 一致（在協程本體執行期間）。
 */
coroutine *co_current(void);

/*
 * co_userdata — create 時綁定的固定上下文；co == NULL 時回 NULL。
 * 庫不擁有其指向的物件。執行緒契約見上方 Query API。
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
 * co_storage(co) / co_storage_size(co) — 唯讀查詢（Query API，見上方執行緒契約）：
 *   - 未綁定或 co==NULL：co_storage 回 NULL，co_storage_size 回 0。
 *   - 跨 thread 並行讀寫 buffer 內容（或與 set_storage / resume 並行）為 UB。
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
 * Coroutine Local Storage (CLS)
 * ----------------------------------------------------------------
 * 類似 TLS，但 value 綁定「目前正在執行的協程」，而非 OS thread。
 * 適合 runtime / middleware（logger、tracing、scheduler metadata），
 * 無需修改 co_create 的 userdata layout。
 *
 * 與 userdata（固定上下文）、mailbox（單次訊息）、storage（byte buffer）分離：
 *   - Key  — process-global；co_cls_alloc() 配置；thread-safe
 *   - Value — per-coroutine；每條協程（含 TLS main）擁有 CO_CLS_SLOTS 個 void*
 *
 * 典型流程（模組初始化期間）：
 *   static co_cls_key k = CO_CLS_KEY_INVALID;
 *   k = co_cls_alloc();
 *   co_cls_set(k, ctx);          // 在 main 或 fiber 內
 *   void *p = co_cls_get(k);
 *
 * CO_CLS_SLOTS — 每條協程固定槽數（目前 16）；控制塊約 +128 bytes（64-bit）。
 *
 * co_cls_key — 由 co_cls_alloc() 回傳的 key 型別；可與 int 互轉比較。
 *
 * CO_CLS_KEY_INVALID (-1) — co_cls_alloc() 用盡槽位時的回傳值。
 *
 * co_cls_alloc() — 配置一個 process-global CLS key。
 *   - thread-safe；多執行緒同時呼叫不會拿到相同 key
 *   - 成功：0 .. CO_CLS_SLOTS-1（依配置順序遞增，不保證跨 process 相同）
 *   - 失敗：CO_CLS_KEY_INVALID（已配置 CO_CLS_SLOTS 個 key，且不可回收）
 *   - 建議在 application / runtime / module 初始化期間配置；不提供 co_cls_free
 *   - Key 一旦配置，process lifetime 內不重用（避免 stale pointer）
 *
 * co_cls_set(key, value) — 寫入 co_current()->cls[key]。
 *   - 成功：CO_RESULT_OK
 *   - key < 0 或 key >= CO_CLS_SLOTS：CO_RESULT_INVALID_ARGUMENT
 *   - value 可為 NULL（表示清除槽位；與「從未設定」在 co_cls_get 上同為 NULL）
 *   - 不檢查 owner_token；操作呼叫執行緒的 current_coroutine（含 main）
 *   - 庫只存 void*；指向物件由呼叫端擁有；不 malloc / free / deep copy
 *
 * co_cls_get(key) — 讀取 co_current()->cls[key]（Query API，見上方執行緒契約）。
 *   - key 合法：回傳槽內指標（可能為 NULL）
 *   - key 無效（< 0 或 >= CO_CLS_SLOTS）：回 NULL
 *   - NULL 可能表示：槽未設定、曾 co_cls_set(key, NULL)、或 key 無效
 *     （無法僅憑回傳值區分後兩者與「未設定」— 請保存合法 key）
 *
 * 生命週期與切換：
 *   - resume 進 worker 後 co_cls_get(k) 讀到 worker.cls[k]
 *   - yield 回 main 後讀到 main.cls[k]；各協程槽位彼此獨立
 *   - co_destroy 不釋放 cls[i] 指向的物件、不 invoke destructor
 *   - 指向物件須在協程仍可能讀取前有效；destroy 後不可再透過該協程存取
 */
#define CO_CLS_SLOTS 16

typedef int co_cls_key;

#define CO_CLS_KEY_INVALID (-1)

co_cls_key co_cls_alloc(void);
co_result  co_cls_set(co_cls_key key, void *value);
void      *co_cls_get(co_cls_key key);

/*
 * Allocator 契約（custom allocator）
 * ----------------------------------------------------------------
 * 配置對象：struct coroutine 控制塊（context、狀態、mailbox 等）。
 * 不經 allocator 的資源：
 *   - 執行 stack — 預設 co_stack_create（mmap/VirtualAlloc + guard）
 *   - storage buffer — co_set_storage 由呼叫端提供
 *   - mailbox / userdata / CLS value — 僅存 void*，指向呼叫端資料
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
 *   - 進程級：拷貝 *a 到庫內 g_allocator（非持有指標）；之後新 create 用此副本。
 *   - a == NULL，或 alloc 與 free 皆 NULL：還原預設 libc（calloc / free）。
 *   - 設自訂時 alloc 必須非 NULL；free 可為 NULL（僅 alloc 的 arena 語意）。
 *   - alloc 為 NULL 但 free 非 NULL：視為無效，還原預設 libc。
 *
 * Allocator snapshot（避免 alloc/free mismatch）：
 *   - 每條協程在 co_create_ex 成功配置控制塊時，將當時的 g_allocator 快照進物件。
 *   - co_destroy（以及 create 失敗回滾）一律用該快照釋放，不讀當前 g_allocator。
 *   - 因此「先 create、再 co_set_allocator 換成另一套、再 destroy」仍會用原 allocator 的 free，
 *     不會把自訂區塊誤丟給 libc free（或反之）。
 *   - 仍建議在尚未有活協程時設定策略；中途更換只影響之後新建立的協程。
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
