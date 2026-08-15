# Coroutine — 可嵌入的 C Fiber Primitive

精簡的 **非對稱（asymmetric）** 協程庫，目標演進為可嵌入的 C fiber primitive：先確保切換正確性與嵌入鉤子，再以對稱 `transfer` 為底層原語，最後才做堆疊密度優化。

詳細路線圖見 [Plan.md](Plan.md)。

## 現況一覽

| 階段 | 內容 | 狀態 |
|------|------|------|
| **P0** 正確性 | 暫存器/FP 測、ASan fiber 註解、guard/巢狀/生命週期測、`cycles/switch` bench | **完成** |
| **P1** 嵌入鉤子 | mailbox 傳值、custom allocator、storage、`co_current`、CLS | **完成** |
| **D-1..D-7** | `owner_id`、orphan shutdown、stack bounds、opt-in crash handler、ASan bounds/fake_stack、atomic `page_size`、非淘汰 guard 表 | **完成** |
| **D-9** 合作式取消 | `co_cancel` + `CO_CANCEL` sentinel；`CANCEL_IGNORED` | **完成** |
| **P2** 對稱原語 | `co_transfer`；resume/yield 建其上；TSan fiber；外部 stack 公開決策 | 待做 |
| **P3** 密度 | stack pool → shared copy-stack | 待做 |

公開 API 為非對稱 `co_resume` / `co_yield_now`（mailbox 傳值）、可選 storage、CLS、`co_current`、custom allocator、`co_thread_shutdown`、opt-in crash handler。P0/P1 與 D-1..D-7 契約已補齊回歸測試。

## 功能摘要

- **合作式取消**：`co_cancel` / `CO_CANCEL` sentinel（對齊 Python `GeneratorExit`）；違約 yield → `CANCEL_IGNORED`（`cancelling` 保留）。再次 `co_cancel`：debug 印出協程後 abort；release 標為不可回收，計入 `co_thread_shutdown` leaked_count
- **Mailbox 傳值**：`co_resume(co, input, output)` / `co_yield_now(output, next_input)`
- **CLS**：`co_cls_alloc` / `co_cls_set` / `co_cls_get`（process-global key、per-coroutine value）
- **可選 storage**：`co_set_storage` / `co_storage`（呼叫端自有 buffer）
- **Custom allocator**：`co_set_allocator`（控制塊配置；create 時快照）
- **獨立堆疊**：每協程 `mmap` / `VirtualAlloc`，雙端 guard page；大小 `CO_MIN_STACK_SIZE`‥`CO_MAX_STACK_SIZE`（1 GiB）
- **巢狀 resume**：外層進入 `CO_WAITING`，禁止對同一協程重入或銷毀
- **執行緒親和**：`owner_id` 為 process 生命週期單調序號（非 TLS 位址）；不可跨執行緒 mutating API
- **執行緒結束**：`co_thread_shutdown` 為 caller 義務入口；未清理時 thread-exit 會 orphan reclaim 庫資源
- **Guard 溢位診斷**：預設嵌入友善（不安裝 handler）；`co_install_crash_handler(1)` 才啟用；表滿（4096）時不淘汰舊條目，新堆疊可能缺診斷
- **ASan fiber 註解**：主協程邊界來自 `co_platform_query_thread_stack`；`asan_fake_stack` 支援 `-fsanitize=address` 下 use-after-return
- **可選堆疊用量偵測**：`-DCO_DEBUG_STACK_USAGE` 啟用 `co_stack_peak`

## 支援平台

| 平台 | 目錄 | 建置 |
|------|------|------|
| Linux x86_64 (System V) | `platform/Linux_x86/` | `Makefile`（預設） |
| Windows x64 | `platform/Windows_x64/` | `Makefile.win` |
| Linux AArch64 | `platform/linux_aarch64/` | `Makefile.aarch64` |
| macOS arm64 | `platform/macos_arm64/` | `Makefile.macos_proxy`（qemu 代理測） |

暫存器契約：

- **x86_64 SystemV / Win64**：callee-saved GPR、MXCSR / x87 CW；Win64 另含 xmm6–xmm15
- **AArch64**：x19–x28、fp、d8–d15；**不**保存 FPCR/FPSR（非 ABI callee-saved，屬預期）

## 快速開始

### Linux x86_64

```bash
make          # 建置 test_coroutine
make test     # 基本 suite（--speed normal）
make p0       # P0 正確性測 + bench（Makefile.p0）
make p0-asan  # ASan 下跑 P0 suite
make bench    # 空 yield/resume，報 cycles/switch
```

### 其他平台

```bash
# Windows（MinGW-w64）
make -f Makefile.win
./run_win_test.sh

# Linux AArch64（cross + qemu）
make -f Makefile.aarch64
./run_aarch64_test.sh

# macOS arm64 切換邏輯（qemu 代理）
./run_macos_proxy_test.sh
```

壓力／速度檔位（基本 suite）：

```bash
make slow|normal|fast|turbo
# 或
./test_coroutine --speed normal
```

## 公開 API（目前）

```c
#include "coroutine.h"

typedef void (*co_function)(coroutine *self, void *userdata, void *initial_input);

coroutine *co_create(size_t stack_size, co_function function, void *userdata);
co_result  co_create_ex(size_t stack_size, co_function fn, void *userdata, coroutine **out);
co_result  co_resume(coroutine *co, void *input, void **output);
co_result  co_yield_now(void *output, void **next_input);
co_result  co_destroy(coroutine *co);
co_result  co_cancel(coroutine *co);              /* 成功則等同 destroy */
int        co_is_cancel(const void *msg);
extern const void *const CO_CANCEL;
co_result  co_thread_shutdown(size_t *leaked_count);
void       co_install_crash_handler(int enable);
int        co_finished(const coroutine *co);
size_t     co_stack_peak(const coroutine *co);  /* 需 -DCO_DEBUG_STACK_USAGE */
```

最小範例：

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
    co_resume(co, NULL, NULL);   /* → "in fiber" */
    co_resume(co, NULL, NULL);   /* → "resumed" */
    co_destroy(co);
    return 0;
}
```

堆疊：`CO_MIN_STACK_SIZE`（16 KiB）‥`CO_MAX_STACK_SIZE`（1 GiB），預設 `CO_DEFAULT_STACK_SIZE`（64 KiB）。非法大小回錯誤碼，不因參數 SIGSEGV。

### 使用限制

- callback **不可**拋出 C++ 例外，也不可 `longjmp` 出協程
- 掛起中不可 `co_destroy`（`SUSPENDED`/`WAITING`/`RUNNING` → `INVALID_STATE`）；提前放棄請用 `co_cancel`（回呼須在 yield 點檢查 `co_is_cancel`）
- 不可跨執行緒 `co_resume` / `co_destroy` / `co_cancel` / `co_set_storage`（`owner_id` 親和）
- Owner 結束前應呼叫 `co_thread_shutdown`；仍有掛起協程時建議先 `co_cancel`；否則 thread-exit 會 reclaim 庫資源，但 callback 內未釋放的物件視同 abort
- Guard 溢位診斷預設關閉；需要時 `co_install_crash_handler(1)`（不覆寫宿主 altstack；ASan 下跳過）

## 目錄結構

```
coroutine.h / coroutine.c     公開 API 與狀態機
coroutine_internal.h          內部結構、stack / context
co_context.h                  平台 context 聚合
platform/                     各架構 asm + stack 實作
tests/p0/                     P0 正確性測與 bench_switch
tests/p1/                     P1 mailbox / storage / allocator / CLS
test_coroutine.c              基本功能 / 壓力測
Plan.md                       路線圖（P0–P3 + D-1..D-7）
```

## 測試

| 目標 | 說明 |
|------|------|
| `make test` | 基本 create/resume/yield、巢狀、多執行緒親和等 |
| `make -f Makefile.p0 test` | callee-saved 暫存器、guard 溢位、巢狀深度、大量生命週期、`CO_WAITING` 重入 |
| `make -f Makefile.p0 bench` | 空切換 throughput / cycles/switch |
| `make p0-asan` | ASan build 跑 P0（`test_regs` 在 ASan 下會 SKIP，其餘應通過） |
| `make -f Makefile.p1 test` | P1 mailbox、storage、allocator、CLS（含 `--cls-alloc-race` 獨立 process） |

P0 可單獨跑：

```bash
./test_p0 --regs|--guard|--nested|--mass|--waiting
```

## 路線圖摘要

完整說明與 API 鎖定表見 [Plan.md](Plan.md)。

1. **P0（已完成）** — 切換與堆疊契約有可回歸證據
2. **P1（已完成）** — mailbox 傳值、allocator、storage、`co_current`、CLS
3. **D-1..D-7（已完成）** — owner／shutdown、stack bounds、opt-in handler、ASan、page_size、guard 表
4. **P2** — `co_transfer` 為底層；TSan fiber；鎖定外部 stack 公開 API（建議 `co_create_with_stack`）
5. **P3** — stack pool，再可選 shared copy-stack（禁巢狀、高風險）

### 刻意不做

- 再擴平台（i386、Windows ARM 等）
- 跨執行緒 migrate 協程
- C++ 例外穿越 / 自動 unwind destroy
- AArch64 保存 FPCR/FPSR
- 公開 API 使用 C++20 coroutine 關鍵字（`co_yield` / `co_await` / `co_return`）

## 授權

見專案內授權檔（若尚未加入，以倉庫後續補充為準）。
