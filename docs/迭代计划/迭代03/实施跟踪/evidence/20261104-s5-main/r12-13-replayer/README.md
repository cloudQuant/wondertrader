# BUG-R12-13：HisDataReplayer 日期映射改经 checked 接口（卡1 路径 A）

- 产品：`src/WtBtCore/HisDataReplayer.{h,cpp}`
  - 新增 `checked_trading_date`、`checked_boundary_time`、`checked_next_tdate`、`checked_prev_tdate`，经 `WTSBaseDataMgr::checked*` 映射，失败时记录具体状态并返回 0。约 12 处旧 `calcTradingDate`/`getBoundaryTime`/`getNextTDate`/`getPrevTDate` 调用改用这些函数；调用方原本已对 0 中止回放。
  - `get_daily_bar_close_time` 在映射失败且不是 `MissingCommodity` 时，以明确错误终止回放（例如品种引用的节假日日历未加载）。改前会退回按会话推算的收盘时刻。`MissingCommodity` 仍按会话查找，保持原行为。
- 用例：`p04_btcore_replayer/s5_main_r1213_replayer_calendar_test.cpp`
  - 品种日历 `NOPE` 未加载（默认日历 `CHINA` 已加载）：prepare 通过，日线收盘映射报 `MissingCalendar`，回放终止。
  - 对照组：日历已加载，5 根日线全部回放。
- 红控：用改前回放器源码重编，缺日历用例失败（旧接口按周末规则照常回放 5 根，`r1213rep-red.log/xml`）；恢复改后源码（哈希见 `sources.sha256`）后 2/2 通过（`r1213rep-green*.log`）。
- int9（迁移后、全部覆盖率二进制逐例）：回测相关用例无新增失败；现有回测夹具均已加载日历。
