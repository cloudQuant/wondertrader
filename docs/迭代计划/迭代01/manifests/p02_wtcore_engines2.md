# p02_wtcore_engines2 — WtCore 引擎与执行器（第三波 lane P02）

- 范围：`src/WtCore/` 的 `WtEngine`、`WtLocalExecuter`、`WtDiffExecuter`、`WtArbiExecuter`、
  `WtDtMgr`（仅作替身基类）、三个 `Wt*Ticker` 不在本文件集内（n02 已收口，见下）。
- 二进制：`CoverageTests_wtcore`（`src/TestUnits/coverage/CMakeLists.txt` 第 46 行的
  `wtcore|WtCore|dir:...,dir:p02_wtcore_engines2`）。
- 未改任何生产代码；未改 `CMakeLists.txt`；**不需要额外 TU**
  （`WTSPortFundInfo` 的实现在 `src/Includes/WTSRiskDef.hpp` 内联，全仓 .cpp 中无定义点，
  已用 grep 复核：只有 `WtEngine.cpp:475` 的调用，故 `WtEngine.o` 被抽取时不会引入新的未定义符号）。

## 交付文件（3 个，均在 `src/TestUnits/coverage/p02_wtcore_engines2/`）

| 文件 | 行数 | 启用用例 | DISABLED_ |
|---|---|---|---|
| `p02_local_arbi_executer_test.cpp` | ~430 | 11 | 1（`P02ExecuterCrashClass.DISABLED_ChannelReadyWithoutTraderAndBadPolicyConfigs`） |
| `p02_diff_executer_test.cpp` | ~380 | 10 | 0 |
| `p02_wt_engine_state_test.cpp` | ~400 | 9 | 1（`P02EngineCrashClass.DISABLED_NullHotMgrAndUnseededBaseDataPaths`） |

合计 30 个 TEST 定义 = 28 启用 + 2 个缺陷类收口用例。
落盘：全部在 `std::filesystem::temp_directory_path()/p02_wtcore_<pid>/` 下，
用例内 `TearDown` + 目录名带 pid 唯一化；`WtHelper::setGenerateDir()` 由三个文件的静态对象在
任何用例之前指向同一目录（路径字符串一致，静态初始化先后不影响结果）。

## 实际覆盖到的符号（按函数，符号名均来自源码/grep 结果）

`WtLocalExecuter`：ctor / `~WtLocalExecuter` / `setTrader` / `init`（scale、strict_sync、poolsize、
`clear.active|includes(array|string)|excludes`、`groups` 全解析路径）/ `getUnit`（缓存命中、
`bAutoCreate=false`、自动创建时工厂返回 NULL）/ `getTicks` `grabLastTick` `getPosition`
`getUndoneQty` `getOrders`（`_data_mgr`/`_trader` 空指针护栏）/ `buy` `sell` `cancel×2`
（`!_channel_ready` 护栏）/ `writeLog` / `getCommodityInfo` `getSessionInfo` `getCurTime`（stub 转发）/
`on_position_changed` / `set_position`（含 `_groups` 取模取份的组合计算、`_strict_sync` 的通道持仓清理循环）/
`on_tick` `on_trade` `on_order` `on_entrust` `on_account`（空 `_unit_map` 早退）/
`on_channel_ready` `on_channel_lost` / `on_position`（`_auto_clear=false` 早退 + 非分月合约早退）。

`WtArbiExecuter`：ctor / `init`（同上配置形状）/ `getUnit` / 同一族空指针与通道护栏 /
`on_*` 转发 / `on_channel_ready|lost` / `set_position` / `on_position_changed` / `writeLog`。

`WtDiffExecuter`：ctor / `init` / `load_data` 四条入口（文件不存在、内容为空、`HasParseError`、
合约无效逐条跳过）/ `save_data`（targets/diffs 两段 JSON 序列化，private，经
`set_position` 末尾 `WtDiffExecuter.cpp:641` 的调用间接驱动）/ `getUnit`（default policy 选择）/
`getTicks` `grabLastTick` `getPosition` `getUndoneQty` `getOrders` `buy` `sell` `cancel×2` `writeLog`
`getCommodityInfo` `getSessionInfo` `getCurTime` / `on_position_changed`（`decimal::eq(diff,0)` 两条）/
`set_position`（目标循环 + 空集合）/ `on_tick` `on_trade` `on_order` `on_entrust` /
`on_channel_ready` `on_channel_lost`。

`WtEngine`：`WtEngine()`（时钟/`WtHelper::setTime` 初始化）/ `set_date_time`（含 `rawTime==0` 兜底分支）/
`set_trading_date` / 内联 `get_date` `get_min_time` `get_raw_time` `get_secs` `get_trading_date` /
`getCurDate` `getCurTime` `getTradingDate` `isInTrading` / `init`（`filters`/`fees` 空串早退 +
无 `riskmon` 段时 `_fund_udt_span=5` 分支）/ `load_fees`（`strlen==0` 早退）/
`load_datas`（`fund` 全 12 字段 + `maxmd/minmd` + `positions[].details` + `riskmon` 反序列化；
空文件 / 非法 JSON / 无 riskmon 三种变体；`_port_fund = WTSPortFundInfo::create()`）/
`save_datas`（经 `setVolScale` 驱动，落盘后用文本断言做往返校验）/ `setVolScale` /
`get_commodity_info` / `get_exright_factor`（商品缺失 → 1.0）/ `get_adjusting_flag`（丢弃返回值那条）/
`get_kline_slice`（`cInfo==NULL` 早退，`m`/`d` 两种 period）/ `calc_fee`（无模板返回 0.0）/
`get_cur_price` / `get_day_price`（flag 0..3 与 default，无 tick → 0.0）/
`append_signal`（`bStandBy` 两条）/ `do_set_position`（仓位未变早退）/
测试子类 `P02TestEngine::run|on_bar|on_tick|transTimeToMin`（仅占位，不起线程）。

替身（本 lane 自带，不引第三方 mock）：`IDataManager` 空实现 ×2、`IExecuterStub` ×2、
`IBaseDataMgr`（11 个纯虚全部返回 NULL/默认）×2、`WtDtMgr` 子类（覆写 `grab_last_tick` /
`get_tick_slice` / `get_kline_slice` / `get_adjusting_factor` / `get_adjusting_flag`）、
`WtExecuterFactory` 实例（不调 `loadFactories`，故所有 `createXXXExeUnit` 必然返回 NULL）。

## 明确跳过并点名的分支（不进启用用例）

1. **拆单与撮合算法本体不在 WtCore**：超时撤单追单、反向先撤后补、子单手数/价格类型这些逻辑都在
   `ExecuteUnit` 实现里（`WtExecuterFactory` 通过 dlopen 反射创建），而 `WtLocalExecuter::getUnit`
   返回的 `_unit_map` 是私有成员且没有注入点。本 lane 的 `CoverageTests_wtcore` 不加载任何执行单元
   插件 dylib，因此 WtCore 侧可覆盖的是「工厂创建失败 → `unit==NULL` → 跳过」这一条通路。
   真正的拆单用例需要把 `src/ExecuteUnits/*` 的单元实现编进同一个测试目标（建议另开 lane 或在
   `n05_exec_factories` 目标里做），本 wave 无法在不改 `CMakeLists.txt` 的前提下完成。
2. `WtEngine::run()` / `WtSelEngine::run()` / `task_loop()` / `push_task()`：起线程或 `while(!_terminated)`
   阻塞，按本轮 HANG 教训一律不驱动（`P02TestEngine::run()` 覆写成空实现）。
3. `WtEngine::init_riskmon()`：`dlopen` 风控插件；`on_session_end()`：写 `portfolio/funds.csv` 并推进结算日，
   且依赖 `_port_fund->fundInfo()` 状态机；`getFundInfo()` → `update_fund_dynprofit()` 未逐行确认，
   三者留待后续（已在 `p02_wt_engine_state_test.cpp` 文件头点名）。
4. `WtEngine::handle_push_quote()` / `get_rawcode()` / `get_session_info()` / `sub_tick()` /
   `get_contract_info()`：需要真实 `WTSTickData`+`WTSContractInfo`+`IHotMgr`（`_hot_mgr` 未判空即崩，
   见 BUG-P02-004）。
5. `WtDtMgr` 的订阅 key 组合与重复订阅抑制、本地日期推进：`initStore()` 要 dlopen 存储插件、
   `get_*_slice` 要 `_reader`，且 `WtDtMgr::_map_*` 无外部可观测出口；本 lane 只把 `WtDtMgr` 当基类
   做了覆写替身（顺带覆盖其 ctor）。**建议**：`WtDtMgr` 的订阅去重需要一个能注入 `IDataReader` 的
   构建位，属主控决策。
6. `WtCtaTicker` / `WtHftTicker` / `WtSelTicker` 的 bar 切片推进：n02 已收口并记录了
   BUG-N02-002（三个 ticker 构造未初始化 `_s_info`/`_store`），本 lane 未重复触碰。
7. 需要真实交易通道/服务端回报的分支：`TraderAdapter` 的 `onRsp*`/`onPush*` 驱动链、
   `on_entrust` 成功后的补单、`on_position` 分月主力合约换月清仓（需 `IHotMgr::getPrevRawCode`）。

## 门禁

三个文件逐一通过本轮唯一允许的构建动作（`clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror
-Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" -DSPDLOG_FMT_EXTERNAL
-DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h -Isrc -Isrc/TestUnits/gtest
-I/opt/homebrew/include -I/usr/local/include <file>`）：**0 error 0 warning**（最后一次全量复核见
`GATE DONE`，未跑 cmake/ninja/测试二进制）。
门禁期间暴露并已就地修正的三处（都是「符号名/可见性只能来自简报或工具结果」的实例）：
`Wt*Executer` 的 ctor 首参是指针；`getUnit` 与 `save_data` 在头文件里是 private（改为经公开入口间接驱动）；
`WtEngine::load_datas/append_signal/do_set_position/save_datas` 是 protected（由测试子类公开）；
本仓 `src/TestUnits/gtest` 版本没有 `GTEST_SKIP()`，DISABLED_ 用例改为注释登记。

## 缺陷登记

`docs/迭代计划/迭代01/bugs/p02_wtcore_engines2.md`，6 条，状态一律「待审核」，未动源码：
BUG-P02-001（通道就绪后空 `_trader` 下单，崩溃类 ×3 执行器）、
BUG-P02-002（`getUnit` 不校验 policy 配置，`init` 却返回 true）、
BUG-P02-003（`WtEngine::get_adjusting_flag()` 丢弃返回值恒为 0）、
BUG-P02-004（`WtEngine` 空指针类：`_hot_mgr`/`_data_mgr`/`commInfo`/费率键无 `.`）、
BUG-P02-005（`set_position` 的 `real_targets` 算完即弃，`groups` 配置不生效）、
BUG-P02-006（`WtDiffExecuter` JSON 无字段校验 + 单元创建失败时仓位静默丢失）。

## 预算

约 30 次工具调用收口（读简报 1 次 + 定向精读 12 次 + 写文件 3 次 + 门禁 3 次 + 缺陷/清单 2 次 + 复核 grep 4 次），
未产生第 4 个文件；剩余可继续的方向按投产比排序：`WtEngine::on_session_end/update_fund_dynprofit`
（需可控 `_port_fund` 夹具）> `WtDtMgr` 订阅去重（需 `IDataReader` 注入位）>
Executer 拆单本体（需把执行单元源码纳入同一目标）。
