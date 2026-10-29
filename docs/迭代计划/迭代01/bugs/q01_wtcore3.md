# lane Q01 缺陷记录 —— WtCore 选股/高频策略上下文（SelStraBaseCtx / HftStraBaseCtx）

> 以下为源码与测试前提复核结果；本迭代不修改 `src/**`。未运行的路径不作为运行证据。

## BUG-Q01-001

- 状态: 真实缺陷（源码核实；完整平仓日志路径未运行）
- 严重级别: Major
- 模块/文件: `src/WtCore/SelStraBaseCtx.cpp` `SelStraBaseCtx::log_close`（:159-168）
- 相关测试: `src/TestUnits/coverage/q01_wtcore3/q01_sel_position_test.cpp`（当前启用用例未覆盖平仓日志；`DISABLED_SelCrashClass_null_history_mgr` 使用未挂数据管理器的引擎，不能作为日志路径运行证据）
- 输入/前置条件: 选股策略发生一笔平仓，`do_set_position` 走到平仓分支调用 `log_close(...)`。
- 期望行为: 平仓明细写入 `_close_logs`（`closes.csv`），与 CTA 侧 `CtaStraBaseCtx::log_close` 及本函数自身 `if (_close_logs)` 守卫一致。
- 实际行为: 条件判断的是 `if (_close_logs)`，函数体内却调用 `_trade_logs->write_file(...)`（:164），因此 `log_close` 不会把平仓记录写入 `closes.csv`，而会把同一行写入 `trades.csv`。
- 复现步骤: 静态核对 `SelStraBaseCtx.cpp:159-168` 中守卫和写入句柄，并与同文件 `log_trade`、CTA/HFT 的 `log_close` 对照。本轮未构造完整引擎并运行平仓落盘路径。
- 影响范围: 依赖 `closes.csv` 做平仓/盈亏复盘的工具会拿到空文件；交易流水被重复污染。
- 备注: 修复方向（不实施）：把 `log_close` 体内的 `_trade_logs->write_file` 改为 `_close_logs->write_file`。当前 Q01 位置测试 fixture 未挂 `WtDtMgr`，且直接调用上下文 `on_tick` 不填充引擎侧 `_price_map`；后续 `handle_pos_change -> WtEngine::append_signal -> get_cur_price` 在价格缓存未命中时会访问 `_data_mgr`。完整运行需提供价格缓存或数据管理器。此测试限制与 BUG-P01-002 的空数据管理器问题相关，但不是同一日志句柄缺陷；本轮未运行平仓断言。

## BUG-Q01-002

- 状态: 已合并（同类缺失 positions 守卫已由 BUG-P01-004 统一跟踪）
- 严重级别: Minor
- 模块/文件: `src/WtCore/SelStraBaseCtx.cpp` `SelStraBaseCtx::load_data`（:268 `root["positions"]`；:348 `root["signals"]` 有守卫但 positions 无）
- 相关测试: 本 lane 未对其做变绿断言；`q01_sel_savedata_test.cpp` 使用 `save_data` 产出的完整 JSON（必含 `positions`）或手写完整夹具，没有缺失键用例。
- 输入/前置条件: 外部/手工编辑的 `stradata/<name>.json` 缺少顶层 `positions` 键。
- 期望行为: 与 `fund`/`signals` 段一致，`positions` 段访问前先 `HasMember` 判空，缺失则跳过。
- 实际行为: 直接 `const rj::Value& jPos = root["positions"];`，键不存在时触发 rapidjson 断言/未定义行为。
- 复现步骤: 见 `SelStraBaseCtx.cpp:268`，对比 `:251` `if (root.HasMember("fund"))` 与 `:348` `if (root.HasMember("signals"))`。
- 影响范围: 只有自产 JSON（`save_data` 一定写 `positions`）时安全；一旦夹具残缺即崩，妨碍“部分文件恢复”类场景。
- 备注: 与 BUG-P01-004 是不同上下文中同一恢复格式问题：CTA 与 Sel 的 `load_data` 都直接索引根级 `positions` 且不先检查成员存在。已合并到 BUG-P01-004 统一跟踪；此条保留 Sel 的源码位置供定位，不重复计数。源码推断可见 RapidJSON 的缺失成员索引断言/无效访问，本轮没有运行缺字段输入。
