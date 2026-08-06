# Coroutine — 可嵌入的 C Fiber Primitive
精簡的 **非對稱（asymmetric）** 協程庫，目標演進為可嵌入的 C fiber primitive確保切換正確性與嵌入性，再以對稱 `transfer` 為底層原語，最後才做堆疊密度優化。
## 現況一覽
| 階段 | 內容 | 狀態 |
|------|------|------|
|  正確性 | 暫存器/FP 測、ASan fiber 註解、guard/巢狀/生命週期測、`cycles/switch` bench | **完成** |
|  嵌入鉤子 | `void*` 傳值、custom allocator、CLS / `co_current`、可選 storage | 待做 |
|  對稱原語 | `co_transfer`；resume/yield 建其上 | 待做 |
|  密度 | stack pool → shared copy-stack | 待做 |
公開 API 目前仍為非對稱：`co_resume` / `co_yield_now`（無傳值）。行為不變的前提下，已補齊回歸測試與效能基準。
## 功能摘要
- **非對稱 API**：主執行緒 `co_resume`，協程內 `co_yield_now` 回到 caller
- **獨立堆疊**：每協程 `mmap` / `VirtualAlloc`，雙端 guard page（溢位可偵測）
- **巢狀 resume**：外層進入 `CO_WAITING`，禁止對同一協程重入或銷毀
- **執行緒親和**：協程綁定建立所在執行緒（`owner_token`），不可跨執行緒切換
- **ASan fiber 註解**：`-fsanitize=address` 下於切換前後呼叫 sanitizer fiber API
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

## 公開 API（目前）
```c
#include "coroutine.h"
typedef void (*co_function)(void *argument);
coroutine *co_create(size_t stack_size, co_function function, void *argument);
co_result  co_create_ex(size_t stack_size, co_function fn, void *arg, coroutine **out);
co_result  co_resume(coroutine *co);
co_result  co_yield_now(void);
co_result  co_destroy(coroutine *co);
int        co_finished(const coroutine *co);
size_t     co_stack_peak(const coroutine *co);  /* 需 -DCO_DEBUG_STACK_USAGE */
```
最小範例：
```c
#include "coroutine.h"
#include <stdio.h>
static void worker(void *arg)
{
    (void)arg;
    puts("in fiber");
    co_yield_now();
    puts("resumed");
}
int main(void)
{
    coroutine *co = co_create(CO_DEFAULT_STACK_SIZE, worker, NULL);
    co_resume(co);   /* → "in fiber" */
    co_resume(co);   /* → "resumed" */
    co_destroy(co);
    return 0;
}
```
堆疊下限 `CO_MIN_STACK_SIZE`（16 KiB）、預設 `CO_DEFAULT_STACK_SIZE`（64 KiB）。
### 使用限制
- callback **不可**拋出 C++ 例外，也不可 `longjmp` 出協程
- 掛起中不可 `co_destroy`；資源須在返回前自行釋放
- 不可跨執行緒 `co_resume` / `co_yield_now`
## 目錄結構
```
coroutine.h / coroutine.c     公開 API 與狀態機
coroutine_internal.h          內部結構、stack / context
co_context.h                  平台 context 聚合
platform/                     各架構 asm + stack 實作
tests/p0/                     P0 正確性測與 bench_switch
test_coroutine.c              基本功能 / 壓力測
Plan.md                       路線圖（P0–P3）
```
## 測試
| 目標 | 說明 |
|------|------|
| `make test` | 基本 create/resume/yield、巢狀、多執行緒親和等 |
| `make -f Makefile.p0 test` | callee-saved 暫存器、guard 溢位、巢狀深度、大量生命週期、`CO_WAITING` 重入 |
| `make -f Makefile.p0 bench` | 空切換 throughput / cycles/switch |
| `make p0-asan` | ASan build 跑 P0（`test_regs` 在 ASan 下會 SKIP，其餘應通過） |
```bash
./test_p0 --regs|--guard|--nested|--mass|--waiting
```
## 路線圖摘要
完整說明與 API 鎖定表見 [Plan.md](Plan.md)。
1. **0（已完成）** — 切換與堆疊契約有可回歸證據
2. **1** — `co_resume`/`co_yield` 傳 `void*`、allocator、`co_current` + CLS、可選 storage
3. **2** — `co_transfer` 為底層；非對稱 API 改為其上包裝
4. **3** — stack pool，再可選 shared copy-stack（禁巢狀、高風險）
