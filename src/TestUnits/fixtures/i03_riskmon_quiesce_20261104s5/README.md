# I03 风控监控器停止/排空与资金对象借用（卡9a=A+C，BUG-I02-NEW-034）

只用本地 fake 风控插件，不连接柜台、不下单。

- `fake_riskmon.cpp`：在 `init()` 中启动工作线程，持续调用 `WtPortContext`（`getFundInfo`/`isInTrading`/`getTradingDate`）。`-DFAKE_SYNC_STOP` 时导出 `getRiskMonPluginCapsV2`（`WT_RISKMON_PLUGIN_V2_CAP_SYNC_STOP`），`stop()` join 工作线程；否则 `stop()` 只请求停止，线程在 stop 后继续使用上下文 300 ms（模拟无法证明排空的第三方插件）。
- `riskmon_test.cpp`：真实 `WtSelEngine`/`WtEngine` 经 `init_riskmon` 真实 dlopen 加载 fake。
- `run.sh <workdir> <red|green> [改前 WtEngine 目录]`：rsync 实体快照 `src/`，Homebrew LLVM 22 ASan/UBSan 构建 `WtCore WTSTools WTSUtils`（-j3），红相位只替换 `WtEngine.{cpp,h}` 为改前副本。

用例：

| 用例 | 变体 | LSan | 期望 |
|---|---|---|---|
| UnprovenMonitorIsQuarantinedAndOutlivesEngineSafely | nocap | 关（隔离按设计常驻到退出） | 引擎销毁后工作线程继续调用上下文：无 UAF，资金指针仍有效（非空），stop 恰 1 次，监控器未被删除 |
| ProvenMonitorIsStoppedThenFundAndMonitorReleased | sync | 开（无抑制） | stop 恰 1 次且已 join，监控器经工厂删除 1 次，工厂经 `IRiskMonPluginV2` 的按值 `destroy_factory` 释放 1 次（I03-NEW-062），引擎释放自己对资金对象的引用（观测引用计数回到 1） |
| RepeatedRiskMonitorInitIsRejected | sync | 开 | 卡9c=A：第二次 init_riskmon 返回 false，监控器只创建 1 次 |

I03-NEW-062：`RiskMonDefs.h` 的旧 `FuncDeleteRiskMonFact` 按引用、而插件按值导出，宿主从不调用旧删除函数；`IRiskMonPluginV2` 表追加按值 `destroy_factory`，引擎只在已证明停止并删除全部监控器后（或创建监控器失败时）调用它；无该字段的模块工厂随模块常驻。故本夹具不再需要 LSan 抑制（`lsan.supp` 为空文件）。
