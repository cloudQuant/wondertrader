# 决策 K44：owner 路由开启时 HFT 与人工裸 API 订单参与合约归属（BUG-I02-NEW-008，含 I03-NEW-088）

主线程，2026-11-05。HEAD 见 `HEAD.txt`，改前副本在 `before/`，改后副本在 `after/`，哈希见 `sources.sha256`。

## 改动
- `WtCore/TraderAdapter.cpp` `doEntrust`：owner 路由开启且当前线程没有 owner 作用域（人工/裸 API 下单）时，以账户级 owner `manual` 独占认领合约。认领失败时拒单，日志为 “manual entrust rejected”。这类订单的回报仍按原口径投递给非 owner 观察者。路由关闭时不认领，行为不变。
- `WtCore/HftStraBaseCtx.{h,cpp}`：
  - 新增 `ownerId()`，取值 `hft:<名称>`，跨重启稳定，可与 journal 恢复的 owner 对应。
  - `setTrader` 与执行器一致，调用 `declareOwnerSink(this, ownerId())`。路由开启时上下文成为自身 owner 的 sink，只收到自己订单的回报，这同时修复 I03-NEW-088（他人成交计入本策略持仓）。路由关闭时声明不生效。
  - `stra_buy/stra_sell`（经 `submitTagged`）以及 `stra_enter/exit_long/short`：路由开启时在 `OwnerScope(trader, ownerId(), false)` 内下单。HFT 上下文没有宿主可核验的身份协议，因此独占认领，属非共享模式。

## 影响（裁定所述“HFT 与执行器混用同一账户”）
- 路由开启时，同一账户同一合约上，HFT 上下文、执行器与人工下单三者互斥：先认领者持有该合约，后来者拒单并记录诊断。两个同名 HFT 上下文的第二个无法注册 owner sink。
- 认领目前终身保留（I03-NEW-089：执行器与 HFT 都不调用 `releaseOwner`）。人工下单一旦认领某合约，本进程内执行器和 HFT 都不能再交易该合约，包括人工平仓之后。
- 路由关闭（缺省）时三者都不认领，HFT 上下文仍收到同通道全部回报，沿用单通道单消费者的旧口径。I03-NEW-088 只在路由开启时解决。

## 用例（`n01_wtcore_tradepaths/s5_main_k44_owner_participation_test.cpp`）
1. `ManualEntryUnderRoutingClaimsTheContractExclusively`
2. `ExecuterOwnedContractRejectsManualEntryUnderRouting`
3. `WithoutRoutingManualAndHftEntriesDoNotClaim`（对照：改前改后均应通过）
4. `HftContextUnderRoutingOwnsItsContractAndSeesOnlyItsOwnFills`

**红控**（改前三文件重建）：
- 1、2、4 失败：执行器可交易人工/HFT 持有的合约；人工单可交易执行器持有的合约；HFT 上下文把执行器 2 手成交计入自身（booked=2）；HFT 直接下单绕过执行器认领。
- 3 通过。
- 日志 `logs/k44-red-*.log`。

**绿控**：恢复改后文件（cmp 一致）并全量重建后 4/4 通过（`logs/k44-green-*.log`）。

**逐例回归**：
- wtcore/porter/m_WtRunner/exefact/riskmon 共 1804 例，1795 通过、9 跳过（`regression-summary.json`）；
- m_WtLatencyHFT 21/21（`regression-summary-latencyhft.json`）。

## 未完成与外部项
- 两个真实 Diff 执行器与 HFT 同账户同合约的端到端运行未做；真实 provider 的 owner 路由能力只由配置声明（X）。
- HFT 的 `stra_get_undone` 仍取账户级未完成量，未改为按 owner。
- 认领释放（I03-NEW-089）未处理。
