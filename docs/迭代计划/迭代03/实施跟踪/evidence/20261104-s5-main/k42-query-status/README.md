# 决策 K42（BUG-I02-NEW-154）：订单查询回报的版本化完整性状态

裁定（2026-11-04）：查询回报加版本化状态，逐 provider 迁移；过渡期维持保守处理。

## ABI（`src/Includes/ITraderApi.h`）
- 状态常量：`WT_QUERY_STATUS_UNKNOWN`（旧回调）、`COMPLETE`、`PARTIAL`、`FAILED`。
- `ITraderSpi` 末尾追加虚函数 `onRspOrdersWithStatus(ayOrders, status)`。已有槽位不变，旧 provider 不受影响。缺省实现把 COMPLETE 转给旧 `onRspOrders`，忽略其他状态。
- 宿主能力协商：provider 可选导出 `setTraderSpiFeatures(ITraderApi*, uint64_t)`（`WT_TRADER_SPI_FEATURES_SYMBOL`），宿主在 `createTrader` 之后、`init` 之前传入 `WT_TRADER_SPI_FEATURE_QUERY_STATUS`。只有收到该位的 provider 才可以调用新回调，因为在旧宿主上调用新虚函数是未定义行为。

## 宿主（WtCore `TraderAdapter`，CRLF 保持）
- 原 `onRspOrders` 主体改为 `reconcileOrders(ayOrders, queryStatus)`，按状态决定是否信任“缺席”：
  - 旧回调（UNKNOWN）：维持现行保守规则，空快照不可信；
  - COMPLETE：信任，包括空快照，从而区分“确无订单”与“查询失败”；
  - PARTIAL：只合并行，不据缺席下结论。
- `onRspOrdersWithStatus` 对 FAILED、NULL 或未知状态记告警，不改变任何状态。
- 模块宿主在创建 provider 后调用其 `setTraderSpiFeatures`（存在时）。UFT 宿主未声明该能力，继续走旧回调。

## 用例（`p11_wtcore_traderadapter/p11_sync_report_test.cpp` 末尾，3 例）
- COMPLETE 空快照使缺席活单退场，对照旧回调空快照保留活单；
- PARTIAL 快照不使缺席订单退场，同样的行作为 COMPLETE 则退场；
- FAILED 或 NULL 不改变状态。
- 红控：保留新头文件（缺省转发），只换回改前 `TraderAdapter.{h,cpp}` 重编，COMPLETE 空快照用例失败，其余两例通过（`k42-red.log`）；恢复后 3/3 通过（`k42-green.log`）。
- 逐例回归：wtcore、porter、m_WtRunner、uftcore 共 1682 例，pass 1672、skipped 9、SIGSEGV 1（`regression-summary.json`）。崩溃用例 `R41EngineFixture.R41OnScheduleWithPoolCommitsLeftoverPositionToZero` 单跑 6/6、12 路×15 次 180/180 均通过，int1–int13 也从未失败，登记为 I03-NEW-100 待查。

## 未完成（按裁定“逐 provider 迁移”）
- 各交易插件（CTP 系、XTP、YD 等）尚未导出 `setTraderSpiFeatures`，也未改用带状态回调，现仍走旧回调与保守处理。
- 成交、持仓查询未扩展。
