# lane Q01 缺陷记录 —— WtCore 选股/高频策略上下文（SelStraBaseCtx / HftStraBaseCtx）

> 状态一律「待审核」；本迭代不修改 `src/**`。

## BUG-Q01-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/SelStraBaseCtx.cpp` `SelStraBaseCtx::log_close`（:159-168）
- 相关测试: `src/TestUnits/coverage/q01_wtcore3/q01_sel_position_test.cpp`（`DISABLED_SelCrashClass_null_history_mgr` 之外的 do_set_position 平仓链；因该链需完整引擎，本 lane 未启用）
- 输入/前置条件: 选股策略发生一笔平仓，`do_set_position` 走到平仓分支调用 `log_close(...)`。
- 期望行为: 平仓明细写入 `_close_logs`（`closes.csv`），与 CTA 侧 `CtaStraBaseCtx::log_close` 及本函数自身 `if (_close_logs)` 守卫一致。
- 实际行为: 条件判断的是 `if (_close_logs)`，但函数体内实际调用的是 `_trade_logs->write_file(...)`（:164），把平仓记录写进了 `trades.csv`，`closes.csv` 永远不落数据。
- 复现步骤: 阅读 `SelStraBaseCtx.cpp:159-168`——守卫对象（`_close_logs`）与写入对象（`_trade_logs`）不一致；对比同文件 `log_trade`（:150-157）写 `_trade_logs`。
- 影响范围: 依赖 `closes.csv` 做平仓/盈亏复盘的工具会拿到空文件；交易流水被重复污染。
- 备注: 修复方向（不实施）：把 `log_close` 体内的 `_trade_logs->write_file` 改为 `_close_logs->write_file`。因该分支只在 `do_set_position` 平仓路径触发，而该路径在裸引擎下会经 `handle_pos_change -> WtEngine::append_signal -> get_cur_price` 解引用空 `_data_mgr` 段错误（见 BUG-P01-002 同源），故本 lane 只登记缺陷、不启用断言。

## BUG-Q01-002

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtCore/SelStraBaseCtx.cpp` `SelStraBaseCtx::load_data`（:268 `root["positions"]`；:348 `root["signals"]` 有守卫但 positions 无）
- 相关测试: 本 lane 未对其做变绿断言；`q01_sel_savedata_test.cpp` 全部使用 `save_data` 产出的完整 JSON（必含 `positions`）或手写完整夹具。
- 输入/前置条件: 外部/手工编辑的 `stradata/<name>.json` 缺少顶层 `positions` 键。
- 期望行为: 与 `fund`/`signals` 段一致，`positions` 段访问前先 `HasMember` 判空，缺失则跳过。
- 实际行为: 直接 `const rj::Value& jPos = root["positions"];`，键不存在时触发 rapidjson 断言/未定义行为。
- 复现步骤: 见 `SelStraBaseCtx.cpp:268`，对比 `:251` `if (root.HasMember("fund"))` 与 `:348` `if (root.HasMember("signals"))`。
- 影响范围: 只有自产 JSON（`save_data` 一定写 `positions`）时安全；一旦夹具残缺即崩，妨碍“部分文件恢复”类场景。
- 备注: **同源缺陷，引用已有编号 BUG-P01-004**（`CtaStraBaseCtx::load_data` 同类无守卫访问）；此处仅记录 Sel 侧同一模式的复现位置，建议随 P01-004 一并裁定。
