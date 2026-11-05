# BUG-R12-13：ticker 启动交易日映射迁移到 DateMappingV1（卡1 A+C）

- 产品：`WtCtaTicker.cpp`、`WtSelTicker.cpp`、`WtHftTicker.cpp`（WtCore）与 `WtUftTicker.cpp`（WtUftCore）启动时改用 `DateMappingClient`（C 路径：旧虚接口调用前后校验）计算交易日；映射失败记错误并拒绝启动（CTA/SEL/HFT 返回 false，HFT 先完成停止收尾；UFT 收尾后返回），不再以哨兵/兜底日期驱动引擎。改前/改后副本 `before/`、`after/`，哈希 `sources.sha256`。行尾保持 CRLF。
- 夹具：q04 `uft_engine_session_gate_test.cpp`（5 处）、`r163_uftcore_guards_test.cpp`、p06 `p06_uft_engine_test.cpp` 的 `addTradingSection(0, 2400)` 按卡2b 改 0-0（原会话实际无时段，迁移后启动被正确拒绝）。
- 用例：`q01_wtcore3/s5_main_r1213_ticker_date_test.cpp`（会话无交易时段 → run() 失败、引擎未初始化、未开盘）。
- 红控：改前 `WtCtaTicker.cpp` 重编，用例失败（run 返回 true、on_init/on_session_begin 各 1 次，`red.log`）；恢复后通过（`green.log`/`green2.log`）。uftcore 逐例 302/302；wtcore/porter/runner/延迟工具相关二进制逐例仅并行负载时序类与缺 `WT_COV_PLUGIN_BIN` 环境类失败（单跑通过）。
- 未完成：`DataManager::getTradingDate`、TraderCTPMini 夜盘调用点、插件侧取 V1 表（D4 待裁定）。
