---
title: 可嵌入的 C Fiber Primitive
---

{% include nav.html %}

**Coroutine** 是用 C17 寫的纖維（fiber）原語：公開面是非對稱 `co_resume`／`co_yield_now`，底層是對稱 `co_transfer`。每條協程有獨立堆疊與雙端 guard；銷毀後依 size class 進執行緒本地 pool，下次建立時重用 mmap。不採用 shared copy-stack。

適合嵌進主程式，而不是當獨立執行期。授權為 [MIT](https://github.com/Evan-lin2005/coroutine/blob/master/LICENSE)。

[原始碼](https://github.com/Evan-lin2005/coroutine) · [coroutine.h](https://github.com/Evan-lin2005/coroutine/blob/master/coroutine.h)

## 現況

| 階段 | 內容 | 狀態 |
|------|------|------|
| P0 正確性 | 暫存器測、ASan、guard／生命週期、切換 bench | 完成 |
| P1 嵌入鉤子 | mailbox、allocator、storage、`co_current`、CLS | 完成 |
| D-1..D-7／D-9 | owner、shutdown、stack bounds、cancel | 完成 |
| P2 對稱原語 | `co_transfer`；拒絕巢狀 WAITING steal 與 in-flight abandon | 完成 |
| P3a stack pool | TLS size class LIFO | 完成 |
| P3b copy-stack | shared stack + suspend copy-out | 放棄 |

## 平台

| 平台 | 目錄 | 建置 |
|------|------|------|
| Linux x86_64 (System V) | `platform/Linux_x86/` | `Makefile` |
| Windows x64 | `platform/Windows_x64/` | `Makefile.win` |
| Linux AArch64 | `platform/linux_aarch64/` | `Makefile.aarch64` |
| macOS arm64 | `platform/macos_arm64/` | `Makefile.macos_proxy` |

暫存器：x86_64 保存 callee-saved GPR 與 MXCSR／x87 CW（Win64 另含 xmm6–xmm15）。AArch64 保存 x19–x28、fp、d8–d15，**不**保存 FPCR／FPSR。

## 最小範例

```c
#include "coroutine.h"
#include <stdio.h>

static void worker(coroutine *self, void *userdata, void *initial_input)
{
    (void)self; (void)userdata; (void)initial_input;
    puts("in fiber");
    co_yield_now(NULL, NULL);
    puts("resumed");
}

int main(void)
{
    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, worker, NULL);
    co_resume(co, NULL, NULL);
    co_resume(co, NULL, NULL);
    co_destroy(co);
    return 0;
}
```

Linux x86_64：`make && make test`。P0–P3 與 sanitizer 見 [README](https://github.com/Evan-lin2005/coroutine/blob/master/README.md)。

## 契約要點

- 不可跨執行緒呼叫 mutating API（`owner_id` 單調、不重用）
- `co_transfer` 不清 `from->caller`；WAITING + `resume_target` 防 steal
- in-flight resume 禁止 `co_abandon`
- `co_cancel` 只推到 `DONE`，回收走 `co_destroy`（或掛起時 `co_abandon`）
- 執行緒結束前應 `co_thread_shutdown`；未呼叫時 thread-exit 仍會回收庫資源與 TLS pool

完整公開面見 [API]({{ '/api/' | relative_url }})，階段與刻意不做見 [路線圖]({{ '/roadmap/' | relative_url }})。

{% include footer.html %}
