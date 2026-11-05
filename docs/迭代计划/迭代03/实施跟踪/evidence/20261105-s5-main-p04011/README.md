# BUG-P04-011 产品缺陷部分：默认历史 tick 查询按品种自身交易日取桶，不改写派发共用缓存

主线程，2026-11-05。HEAD 见 `HEAD.txt`，改前在 `before/`，改前用例在 `before-tests/`，改后在 `after/`，哈希见 `sources.sha256`。

## 缺陷
混合会话回放中，`HisDataReplayer::get_tick_slice` 固定按全局交易日 `_cur_tdate` 取桶。例如 24 小时品种与夜盘品种同时回放，当全局交易日已是夜盘品种的下一交易日时，24 小时品种在其自然日末查询，看不到同日更早的记录。这是 btcore 证据中已知的红控 `…NoDataDayAndFinalMillisecond`。

## 改动（`src/WtBtCore/HisDataReplayer.{h,cpp}`）
- 查询先按回放时钟 `(_cur_date, _cur_time)` 经 `checked_trading_date` 得到该品种自身的交易日：
  - 与 `_cur_tdate` 相同时，走原路径（`checkTicks` 与 `_ticks_cache`）；
  - 不同时，用独立的 `_tick_query_cache` 查询桶，经 `loadTickQueryBucket` 按与 `checkTicks` 相同的数据源顺序装载（回放桶 → 外部加载器 → CSV → 二进制），派发共用的 `_ticks_cache` 不被改写。
- 交易日只取自回放时钟，不取自 `etime`。首版按 `etime` 映射，导致 `R43…MixedNaturalDate` 用 21:01 的 etime 读到下一交易日的桶，即前视（`regression-first-attempt.json`），已改正。`etime` 仍只作桶内截止。
- 映射失败时记错误并返回 NULL，不回退到其他交易日。
- `clear_cache()`、`reset()` 同步清空查询桶（查询桶可能复制自回放桶）。

## 用例与夹具
- 新增 `p07_tick_loader_test.cpp` 的 `p04011TickQueryUsesTheInstrumentsOwnTradingDayInMixedSessions`，取自 btcore 证据中适配卡1/卡2 的临时副本，去掉全局日志初始化：
  - 第 3 个事件处全局交易日为 20200109，BTC 只能看到 1 条且为同日 00:05 的记录（价 1）；
  - 派发序列不变，可见记录都严格早于时钟。
- 夹具调整（按卡1“日期映射须有交易日历”，同 I03-NEW-053 先例）：btporter 的 r134/r143/r173 三个夹具中，品种 `SHFE.rb` 缺 `holiday`，品种级映射得到 MissingCalendar。已补 `"holiday":"CHINA"`，断言不变；生产 `dist/common/commodities.json` 70 个品种均有 holiday。改前副本在 `before-tests/`。

## 结果
- 红控：改前回放器重建后，新增用例在 `visible[2]` 失败（`logs/p04011-red.log`）。
- 绿控：恢复改后文件（cmp 一致）并全量重建后通过（`logs/p04011-green.log`）。
- 逐例回归：btcore/btporter/m_WtBtRunner 共 857 例，852 通过、5 跳过、失败 0（`regression-summary.json`，含按回放时钟映射的修正与夹具补日历）。

## 第二步：同类查询（同日）
- `get_order_detail_slice`、`get_order_queue_slice`、`get_transaction_slice` 改用同一规则：新增 `query_trading_date` 按回放时钟取品种交易日；与全局交易日不同时，使用各自独立的查询桶（`_orddtl_query_cache`/`_ordque_query_cache`/`_trans_query_cache`，从二进制装载，CSV 模式仍拒绝），不改写派发缓存。tick 查询也改为调用该函数。
- 品种没有 commodity 元数据时（如两段式 `SSE.600000`）不存在品种日历，按原口径使用全局交易日；其余映射失败（缺日历、缺 session、时钟非法）仍记错误并拒绝。首次修改把 MissingCommodity 也当作失败，R205/R214 的 L2 用例因此失败，已按此规则改正。中途给 r205/r214 夹具补的 holiday 并非原因，已恢复为原文件。
- 用例扩展：同一混合会话用例在第 3 个事件处，逐笔成交、委托明细、委托队列查询都只能看到 BTC 自身交易日 20200108 的 1 条记录。L2 红控为改前（仅 tick 修复）得 {0,0,0}，期望 {1,1,1}（`logs/p04011-l2-red.log`）；绿控通过（`logs/p04011-l2-green.log`）。
- 最终逐例：btcore/btporter/m_WtBtRunner 共 857 例，852 通过、5 跳过、失败 0（`regression-summary.json`，覆盖首份结果）。

## 仍未闭合（条目继续阻塞）
- 真实 provider 同步回调合同与同刻并发顺序（X）。
- 负 offset 混合例 fail-closed 的合同未裁定。
- 旧依赖未全插桩，整体回归未运行。
- 真实 provider 部分按 2026-11-05 用户指示登记为平台跳过（`regression-tools/external-platform-items.json`），skip 不计通过。
- 负 offset 混合回放合同：2026-11-05 用户裁定“维持 fail-closed”。新增 `p04011NegativeOffsetMixedTickReplayFailsClosed` 把现行为写成合同：纯 tick 混合会话中含 offset=-300、0:00–2:00 品种时，prepare 成功，前 3 笔按序派发，到该品种跨日边界以 `WT_REPLAY_ERROR_INTERNAL` 终止，第 4、5 笔不派发（`logs/neg-probe.log`、`logs/p04011-final.log`）。这是固化既有行为的用例，红控不适用。
