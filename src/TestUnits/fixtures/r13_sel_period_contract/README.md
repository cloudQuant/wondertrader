# R13-004 周期参数失败子项

当前 `WtRtRunner::createSelContext` 对空 period 和未识别 token 在构造 context 前返回零句柄，避免返回不可能被调度的有效句柄；已识别的 `d/w/m/y/min`（包括大小写）仍按原分支转换。此为局部修复，**不是 R13-004 完整验收**。目前文档示例 `d1/w1/m1` 会被明确拒绝，尚未支持，不能称为兼容或满足原始验收。

## 范围与复现

`extract.py` 从当前工作树的 `WtRtRunner.cpp` 和 `WtPorter.cpp` 精确抽取 `createSelContext` 与公开导出 `create_sel_context` 的定义，在临时目录编译，链接 `test_support.hpp` 中的受控替身 runner、context 与 engine；这是将生产函数体经导出包装器运行，**不是真实 Porter 二进制或真实调度器的端到端测试**。替身仅记录 context 构造数、句柄和 period 类型。baseline 在隔离副本中只撤销此次空指针/未知周期守卫，并非完整旧工作树快照。每种模式在独立进程中运行。

```sh
bash src/TestUnits/fixtures/r13_sel_period_contract/run.sh /tmp/wt-r13-period-contract-review-new
```

输出目录须不存在。普通与局部 ASan/UBSan/LSan 各验证：合法五个旧 token 大小写正常；无效字符串和空指针返回零、不构造或注册替身 context。旧版无效字符串反例 exit 1、空指针普通进程 SIGSEGV 139 / sanitizer exit 134；当前代码对应 exit 0。`d1/w1/m1` 支持检查旧版和当前均 exit 1，已知红控不能计通过。编译器与系统库并未全插桩，替身验证不能证明真实上下文注册、策略行为或生命周期。第一次夹具因替身未 retain context 失败，第二次因错误地把旧版本返回了 TPT_None 的非零句柄视作文档支持而失败；第三次修正记录在 `evidence-20261102/results.txt`。脚本检查输入前后哈希一致。

此外，使用 LLVM 22 `clang++ -std=c++17 -fsyntax-only` 和本机实体 Boost/fmt/spdlog/zstd 头对当前完整 `WtRtRunner.cpp` 语法检查 exit 0；未链接产品目标、未运行真实交易、未做整体构建或其他平台检查。

## 未解决

- `d1/w1/m1` 如何映射仍须明确；`m1` 是每月还是每分钟，旧 `m`（月）和 `min`（分钟）不能由本探针消歧。公开文档时间为 HHMMSS，而当前 `on_minute_end` 比较 HHMM。
- `WtSelEngine::addContext` 以 context id 做 map key，但 `_id` 设置为独立 task id；`on_minute_end` 按 task id 查 context。即使触发，当前临时 joinable 线程在离开作用域时析构；与并发/回调/关闭协议一起需要完整设计和可运行验证。
- 因上述问题未闭合，本次没有运行真实 `on_schedule` 事件，也不声称该条已完成。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。本套件用 `extract.py` 按固定文本从 `WtRtRunner.cpp`/`WtPorter.cpp` 抽取 `createSelContext`/`create_sel_context` 函数体，链接替身编译；K27=A 实施后（`Includes/SelScheduleRules.hpp`）源码片段已改写，抽取锚点 `assert body.count(before)==1` 失败，未编译任何探针。这是“按签名抽取产品函数”的探针，维护它等于维护一份影子实现。
- 由以下 coverage 用例取代（F3 `final3/run/results.jsonl` 均为 pass）：`CoverageTests_porter::R13PorterSelExport.create_sel_context_registers_contexts_for_supported_period_tokens`、`CoverageTests_porter::R13PorterSelExport.create_sel_context_rejects_schedules_that_can_never_fire`（含别名 `d1` 拒绝）、`CoverageTests_porter::S3R13SelPublicSchedule.DocumentedPeriodsCreatedThroughTheCApiFireOnTheirSchedule`、`CoverageTests_porter::S3R13SelPublicSchedule.AliasesAndUnschedulableInputsAreRejectedAndNeverFire`（`d1/w1/m1`）、`CoverageTests_porter::I03S5RtRunnerSel.InvalidSelPeriodIsRejectedByPreflight`。这些用例走真实 Porter 导出与真实 runner，而非替身。
- **缺口**：本套件的“`period` 为 NULL/空串时返回零句柄、不构造 context”子项在 coverage 中没有直接用例（未找到以 NULL/空 period 调用 `create_sel_context` 的用例）。需主代理决定是否补 coverage 用例；本处置不改产品与 coverage 源码。
