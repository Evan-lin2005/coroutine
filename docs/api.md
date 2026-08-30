---
title: 公開 API
---

{% include nav.html %}

契約以 [`coroutine.h`](https://github.com/Evan-lin2005/coroutine/blob/master/coroutine.h) 為準。庫只搬運 `void*`，不擁有訊息或 userdata 指向的物件。

## 資料通道

| 通道 | 用途 |
|------|------|
| userdata | 建立時綁定，生命週期同協程 |
| mailbox | resume／yield／transfer 的一次性訊息，讀後清空 |
| storage | 呼叫端提供的 buffer |
| CLS | process-global key、per-coroutine 的 `void*` 槽 |

## 生命週期

```c
typedef void (*co_function)(coroutine *self, void *userdata, void *initial_input);

coroutine *co_create(size_t stack_size, co_function function, void *userdata);
co_result  co_create_ex(size_t stack_size, co_function fn, void *userdata, coroutine **out);
co_result  co_destroy(coroutine *co);   /* 僅 READY／DONE */
co_result  co_abandon(coroutine *co);  /* 掛起時公開回收：跑 defer，不 resume */
```

堆疊：`CO_MIN_STACK_SIZE`（16 KiB）‥`CO_MAX_STACK_SIZE`（1 GiB），預設 64 KiB。非法大小回錯誤碼，不因參數 SIGSEGV。

## 切換

```c
co_result co_resume(coroutine *co, void *input, void **output);
co_result co_yield_now(void *output, void **next_input);

typedef struct co_transfer_t {
    coroutine *prev;
    void      *data;
} co_transfer_t;

co_result co_transfer(coroutine *to, void *data, co_transfer_t *out);
```

- `co_resume`／`co_yield_now` 建在 `co_transfer` 上
- `co_transfer` 不清 `from->caller`，也不把 from 標成 `WAITING`
- 目標為 `WAITING` 且其 `resume_target` 仍為 `WAITING` → `INVALID_STATE`（防巢狀 steal）
- 外層仍停在 `co_resume(co)` 時禁止 `co_abandon(co)`

公開名稱用 `co_yield_now`，避免撞 C++ 的 `co_yield`。

## 取消與清理

```c
extern const void *const CO_CANCEL;
int       co_is_cancel(const void *msg);
int       co_cancel_requested(const coroutine *co);
co_result co_cancel(coroutine *co);   /* 推到 DONE，不釋放 */

co_result co_defer(coroutine *co, void (*fn)(void *), void *arg);
co_result co_defer_cancel(coroutine *co, void (*fn)(void *), void *arg);
size_t    co_defer_count(const coroutine *co);
```

`co_cancel` 成功則協程已 `CO_DONE`；READY 回 `CANCEL_NOT_STARTED`。回收一律 `co_destroy`。defer 的 `arg` 不得指向該協程堆疊。

## 執行緒與嵌入

```c
co_result co_thread_shutdown(size_t *leaked_count);
coroutine *co_current(void);
void       co_install_crash_handler(int enable);
void       co_set_allocator(const co_allocator *a);

co_result  co_set_storage(coroutine *co, void *buf, size_t cap);
void      *co_storage(coroutine *co);

co_cls_key co_cls_alloc(void);
co_result  co_cls_set(co_cls_key key, void *value);
void      *co_cls_get(co_cls_key key);
```

Mutating API（`resume`／`transfer`／`destroy`／`abandon`／`cancel`／`set_storage`／`defer`）僅限 owner 執行緒，否則 `WRONG_THREAD`。`owner_id` 是行程生命週期單調序號，不因執行緒結束而重用。不可 migrate 協程。

Query API（`co_finished`、`co_stack_peak` 等）不檢查 owner；與 mutating API 並行同一 `coroutine *` 是 data race。

## 錯誤碼

`CO_RESULT_OK`、`INVALID_ARGUMENT`、`ALREADY_RUNNING`、`FINISHED`、`NO_CALLER`、`WRONG_THREAD`、`INVALID_STATE`、`OUT_OF_MEMORY`、`CANCEL_IGNORED`、`CANCEL_NOT_STARTED`。

{% include footer.html %}
