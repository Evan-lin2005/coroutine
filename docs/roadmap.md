---
title: 路線圖
---

{% include nav.html %}

這是介紹用摘要。完整契約與 checklist 見倉庫內 [`Plan.md`](https://github.com/Evan-lin2005/coroutine/blob/master/Plan.md)。

## 階段

| 階段 | 狀態 | 說明 |
|------|------|------|
| P0 | 完成 | 暫存器／FP 測、ASan fiber 註解、guard／巢狀／生命週期、cycles／switch |
| P1 | 完成 | mailbox `void*`、allocator、storage、`co_current`、CLS |
| D-1..D-7 | 完成 | `owner_id`、shutdown、stack MAX、opt-in handler、ASan bounds／fake_stack |
| D-9 | 完成 | `co_cancel` + `CO_CANCEL`；回收走 `co_destroy` |
| P2 | 完成 | C 層 `co_transfer`；resume／yield 建其上；防 steal 與 in-flight abandon |
| P2 後續 | 未做 | 公開 `co_create_with_stack`（建議） |
| P3a | 完成 | TLS size class LIFO pool；公開 API 不變 |
| P3b | 放棄 | 不實作 shared copy-stack |

密度只做到 private stack 回收 mmap。不引入 `CO_STACK_SHARE`。

## 刻意不做

- 再加平台（i386、Windows ARM 等）
- 跨執行緒 migrate 協程
- C++ 例外穿越／自動 unwind destroy
- AArch64 保存 FPCR／FPSR
- 公開 API 使用 C++20 關鍵字（`co_yield`／`co_await`／`co_return`）
- shared copy-stack

## 回歸

`make -f Makefile.p{0,1,2,3} test`。fiber／共享狀態加 `test-tsan`，stack／guard 加 `test-asan`。

{% include footer.html %}
