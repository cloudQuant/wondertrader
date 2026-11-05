## 决策卡草稿（需用户裁定；未据此改写任何断言）

### 草稿 S5-A：无规则代码的 rawcode 合同（影响 BUG-Q02-003 集成两项、BUG-I02-NEW-102 rawcode 半项）

- 要决定什么：`stra_get_rawcode` / `HisDataReplayer::get_rawcode` / C 导出 `get_raw_stdcode` 对**不带 hot/second 规则**的标准代码（含未知合约）返回什么。
- 现状证据：实盘 `WtEngine::get_rawcode`（WtEngine.cpp:141-150）、回测 `HisDataReplayer::get_rawcode`、HEAD `ccaad010` 三处实现一致，非规则码返回 `""`；规则码返回 `rawMonthCodeToStdCode(...)`，格式为**带交易所的标准分月代码**（如 `SHFE.rb.2001`）。仓内消费者 `WtStraDualThrust`/`WtStraTwoMa::on_session_begin` 把返回值当标准代码传给 `stra_set_position`，对非规则码以 `""` 表示“无换月”。公开头 `ICtaStraCtx.h:444-452` 只写“将标准化合约代码转换为符合交易所格式的原始分月合约代码”，未说明非规则码。
- 测试现断言：`Q02CtaIntegrationTest.queriesDelegateToReplayer` 期望 `SSE.STK.000001 → "000001"`（裸代码），`unknownCodeAndFlatQueriesAreSafe` 期望 `DCE.FUTU.999999 → "DCE.FUTU.999999"`（原样），`R2PublicTest.metadataAndTimelineQueries` 期望 `"000001"`。两种期望彼此格式不一致，且“裸代码”与规则分支的标准代码格式不一致。
- 选项：
  - A 维持 `""`（实盘/回测/HEAD 一致，可区分“无规则映射”）；改写三处旧断言为 `""`。
  - B 非规则码原样返回标准代码（实盘、回测、C 导出同时改；DualThrust 不受影响）。
  - C 返回交易所裸代码（与规则分支格式不一致，DualThrust 会把裸码当标准码下单）——不建议。
- 建议：A。
- 影响：BUG-Q02-003（集成两项）、BUG-I02-NEW-102（rawcode）；若选 B 还需改 WtCore（非本包）。

### 草稿 S5-B：提前停止是否送达 handle_replay_done（BUG-I02-NEW-101）

- 现状：HEAD 与上游 `run_by_bars` 终止后仍调用 `handle_replay_done`；迭代03 回放状态 V1（`i03_replay_status_20261103`，16/16）与 Porter `EVENT_BACKTEST_END` 合同为“每代恰好一次，回调内状态 STOPPED/FAILED/SUCCEEDED 可区分”。`R2PublicTest.stopBeforeRunQuitsReplayEarly` 旧断言期望停止后 `_doneCnt == 0`。
- 选项：A 停止也恰好一次 done（状态 STOPPED），改写旧断言为 1 并断言状态；B 停止不发 done（需改回放状态合同、Porter 事件与 Mocker 落盘：停止后不再输出结果文件）。
- 建议：A（不改产品，现状即 A）。在未裁定前按卡15记“用户指示忽略”，状态阻塞。

### 草稿 S5-C：未知合约的商品查询层级（BUG-I02-NEW-102 commodity 半项）

- 公开接口 `ICtaStraCtx::stra_get_comminfo`（及 Sel/Hft 同名）注释为“获取**品种**信息”；实盘 `WtEngine::get_commodity_info` 与回测实现相同，按 `交易所+品种` 查找。`SSE.STK.999999` 的品种 `SSE.STK` 存在，返回该品种对象；品种不存在（`DCE.FUTU`）返回空（Q02 集成用例已按此断言并通过）。
- 测试 `R2PublicTest.metadataAndTimelineQueries:415` 期望 `nullptr`，与公开接口的“品种信息”语义冲突。
- 建议：认定合同为“品种级查询”，批准把该断言改为 `EXPECT_EQ(get_commodity_info("SSE.STK.000001"), get_commodity_info("SSE.STK.999999"))`。因不属已裁定卡，本包未改写。

### S5-B 扩展（I03-NEW-053 归因补充）

同一合同问题还覆盖以下旧断言（均未改）：`P04ReplayTermTest.invertedTimeRangeIsRejectedBeforePrepare`（prepare 被拒仍送达一次 done）、`P07MinuteReplayTest.throwingBarCloseCallbackAbortsAndClearsRunningState` / `throwingInitCallbackLeavesReplayerRetryable`、`P07TickLoaderTest.hftTickCallbackExceptionAllowsReplayByDayRetry`（回调抛异常的一代也送达 done，状态 FAILED）、`P07TickLoaderTest.emptyNullTickLoaderCallbackIsAValidEmptyBucket` / `emptyReplayBucketsStayBoundedAcrossLongNoDataRange`（区间内无任何 HFT 事件判 `WT_REPLAY_ERROR_NO_SOURCE` 并终止）。需一并裁定：A 维持回放状态 V1（每代恰好一次 done、状态区分；无事件即 NO_SOURCE 失败）并改写上述旧断言；B 恢复“失败/拒绝不发 done、空数据视为成功”（需改回放状态合同、Porter 事件与 Mocker 落盘）。建议 A；“空桶是否算失败”可单列为 B' 子项（建议仍按 NO_SOURCE 失败，可观察）。
