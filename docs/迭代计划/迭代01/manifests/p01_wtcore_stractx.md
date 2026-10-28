# p01_wtcore_stractx 交付清单（第三波 · WtCore 策略上下文）

- 工作流/lane: `p01`，目标二进制 `CoverageTests_wtcore`（只编译 `src/WtCore/*.cpp`）
- 简报: `docs/迭代计划/迭代01/manifests/api-brief-L07.md`
- 日期: 2026-10-30

## 新增文件（`src/TestUnits/coverage/p01_wtcore_stractx/`）

| 文件 | 行数级别 | 说明 |
| --- | --- | --- |
| `p01_cta_env.h` | 头文件（不参与编译单元枚举，若 CMake glob 只收 `*.cpp` 则自动忽略） | 共用替身：`P01TempEnv`（`WtHelper::setGenerateDir` 重定向到 `temp_directory_path()/p01_<tag>_<pid>/`，析构清理）、`FakeBaseDataMgr`（`IBaseDataMgr` 替身）、`FakeHotMgr`（`IHotMgr` 14 个纯虚全实现）、`P01CtaEngine`（`WtCtaEngine` 派生类，仅给 protected 的 `_base_data_mgr/_hot_mgr` 接线，不 `init`/不 `run`/不起线程）、`P01CtaCtx`（`CtaStraBaseCtx` 具体子类，只补 `on_tick_updated/on_bar_close/on_calculate/on_condition_triggered`，其余全走被测基类实现）、`p01MakeComm/p01MakeCt/p01MakeTick` |
| `p01_cta_ctx_query_test.cpp` | ~200 行 / 9 个启用用例 | 查询、装配、订阅、日志、参数存取 |
| `p01_cta_order_cond_test.cpp` | ~250 行 / 12 个启用用例 + 1 个 `DISABLED_` | 下单前置校验与条件单装配 |

用例合计：23 个启用 `TEST_F`（query 9 + order 14）+ 1 个 `DISABLED_P01CtaCrashClass`。

## 覆盖符号（CtaStraBaseCtx.cpp 为主）

- 生命周期/装配：`on_init`(L668-677) → `init_outputs`(L77-189 首轮建档)、`load_data`(L277-286 文件缺失早退分支)、`load_userdata`(L246 起 文件缺失早退分支)；载体是 `p01_cta_ctx_query_test.cpp` 的 `sub_ticks_is_idempotent`（先调用 `ctx->on_init()`）
- 数据装配：`on_tick`(L795-935) 的 `_price_map` 写入、信号分支（时段外不触发）、`update_dyn_profit`、条件单分支 5 个 `COND_ACTION_*` × `WCT_SmallerOrEqual`/`WCT_LargerOrEqual` 判定、触发后 `_condtions.erase`、`bEmitStrategy` 开关、`on_bar`(L647-666) 的 `newBar==NULL` 早退 / `tag._notify` 通知 / 未订阅周期不通知 / `_main_key` 日志分支
- 下单前置校验：`stra_enter_long`(L1119)、`stra_enter_short`(L1171)、`stra_exit_long`(L1229)、`stra_exit_short`(L1280)、`stra_set_position`(L1352)、`append_signal`(L1395)、`get_cond_entrusts`(L1111) —— 覆盖「品种缺失拒绝」「`canShort()` 拒绝」「平多/平空早退（可平持仓 ≤0、多空方向不符）」「立即模式按当前仓位累加/反手」「`min(curQty, qty)` 截断」「limit/stop 双分支只登记条件单不产生信号」
- 查询透传：`id`、`stra_get_tdate/date/time`（透传 `WtCtaEngine::get_trading_day/get_date/get_min_time`）、`stra_get_comminfo`、`stra_get_rawcode`、`stra_get_price`（本地表命中 + 回落引擎）、`stra_get_fund_data`（flag 0/1/2/3/default）
- 空簿零值：`stra_get_position`（含 `bOnlyValid`、`userTag`、信号存在时的告警分支）、`stra_get_position_avgpx`、`stra_get_position_profit`、`stra_get_first_entertime`、`stra_get_last_entertime`、`stra_get_last_exittime`、`stra_get_last_enterprice`、`stra_get_last_entertag`、`stra_get_detail_entertime`、`stra_get_detail_cost`、`stra_get_detail_profit`（flag 0/1/-1/2/-2/其它）
- 订阅：`stra_sub_ticks`（重复订阅走 `_tick_subs.insert` + `WtEngine::sub_tick`）、`stra_sub_bar_events`
- 参数存取与日志：`stra_save_user_data`/`stra_load_user_data`（含缺省值）、`stra_log_info/debug/warn/error`（L1726-1744）
- 替身自身顺带覆盖的 `WtEngine` 内联/透传（不是本 lane 指标，但为真实链路）：`set_date_time`、`set_trading_date`、`get_commodity_info`、`get_contract_info`、`get_session_info`、`get_rawcode`

`CtaStraBaseCtx.h` 的 `log_signal/log_trade/log_close` 内联只在被登记过仓位的链路上才会执行，本 wave 的 `append_signal` 已触发 `log_signal`。

## 跳过目标及原因

| 目标 | 原因 |
| --- | --- |
| `HftStraBaseCtx.*` / `HftStraContext.*` | 预算收口（30 次工具调用上限已超出），未开工。其 `stra_buy/stra_sell/do_set_position` 需要 `TraderAdapter` + 真实柜台回报；`on_channel_ready/lost`、`setTrader` 后的查询族可用「裸 TraderAdapter（未 init）」+ `WtHftEngine` 派生替身覆盖，建议下一波沿用同一替身头文件模式。 |
| `SelStraBaseCtx.*` / `SelStraContext.*` | 同上，预算收口未开工。`do_set_position/append_signal/save_data/load_data` 与 CTA 侧同构，可直接套用 `P01CtaEnv` 的写法（引擎换成 `WtSelEngine`，策略基类换成 `ISelStraCtx` 的 3 个回调）。 |
| `save_data` / `load_data` 的**往返**与 `save_userdata/load_userdata` 写读 | 需要先确认 `save_data`(L495-646) 的完整 key 集才能构造合法 `datas.json`，`load_data` 对 `positions`/`conditions` 无 `HasMember` 守卫（BUG-P01-004），凭猜写夹具会在启用用例里触发 rapidjson 断言。已列为下一波首选目标（单文件可覆盖 ~370 行）。 |
| `stra_get_bars` / `stra_get_ticks` / `stra_get_last_tick` 的正常返回路径 | 需要真实 `WtDtMgr`（store DLL/LMDB/HDF5），且 `_data_mgr == NULL` 时会段错误（BUG-P01-002），已收进 `DISABLED_` 用例。 |
| `do_set_position`(L1411-1576) 的实体下单/T+1 冻结/费用与平仓明细分支 | 只有当 `WTSSessionInfo::isInTradingTime()` 为真时才会进入，需要有时段明细（`m_tradingTimes`）的 `WTSSessionInfo` 夹具 + 引擎侧 `WtPortContext` 持仓簿；`on_schedule`(L937)、`on_session_begin/end`(L1018/L1073) 会写 funds.csv 并驱动组合资金，同样依赖未 init 引擎以外的装配。 |
| `enum_position`、`set_chart_kline`/`add_chart_mark`/`register_index*`/`dump_chart_info` | `add_chart_mark`/`set_index_value` 在非 schedule 阶段直接 `WTSLogger::error` 早退（L1976/L2032），其余分支需要 `_chart_*` 装配 + 落地目录夹具，本波未安排。 |
| 需要真实柜台/网络/DLL 的分支 | 一律跳过：`CtaStraContext`/`HftStraContext` 的策略 DLL 工厂路径、`EventNotifier` 的 MQ 推送。 |

## 门禁结果

仓库根执行（每个文件写完即跑，逐文件通过）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p01_wtcore_stractx/<file>.cpp
```

- `p01_cta_ctx_query_test.cpp`：**0 error 0 warning**（首轮唯一错误来自替身头文件 `regis()` 保留了 `return ct;`，已修）
- `p01_cta_order_cond_test.cpp`：**0 error 0 warning**
- `p01_cta_env.h` 作为上述两个 TU 的公共头被 `-fsyntax-only` 完整检查（无独立编译单元）
- 未运行 cmake / ninja / 测试二进制（交由主控）

## 约束遵守说明

- 只引用 `src/WtCore`、`src/Includes`、`src/Share`、`WTSTools`/`WTSUtils`；没有引用 `WtUftCore`/`WtBtCore`/`WtDtCore` 的任何同名类（BUG-L00-005 归因风险）。
- `boost/core/noncopyable.hpp`、`IBaseDataMgr.h`、`IHotMgr.h`、`WTSContractInfo.hpp`、`WTSSessionInfo.hpp` 全部显式 include（BUG-L00-004）。
- 启用用例不含任何 bind/connect/join/起线程动作：`P01CtaEngine` 只构造、不 `init`/不 `run`；`WtEngine::release()` 只在析构时 `join` 一个从未创建的 `_thrd_task`。
- 判定会段错误的输入（未挂时段的 `getSessionInfo()`、NULL `_data_mgr` 的三件套）全部收在唯一的 `DISABLED_P01CtaCrashClass` 里，启用用例零残留。
- 落盘全部在 `std::filesystem::temp_directory_path()/p01_<tag>_<pid>/`，`P01TempEnv` 析构 `remove_all`；未断言墙钟时间或本地时区（引擎时间一律 `set_date_time` 固定为 20250110/0931）。
- 未修改任何生产代码与 `CMakeLists.txt`。
