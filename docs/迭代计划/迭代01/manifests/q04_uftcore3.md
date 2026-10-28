# Lane q04 交付清单 — WtUftCore 交易回报/风控推进 + UftStraContext 本地块（迭代01 第五波）

二进制归属：`CoverageTests_uftcore`（只编译 `src/WtUftCore/*.cpp`，目录 `src/TestUnits/coverage/q04_uftcore3/` 已登记）。
未引用任何 `WtCore`/`WtBtCore`/`WtDtCore` 的类；未使用 `#define private public`；未改任何 `CMakeLists.txt`；未改 `src/**`（除 `src/TestUnits/coverage/`）生产代码。
只用了 `WtUftCore/TraderAdapter.h`（非 WtCore 同名头），并按 BUG-L00-004 在替身头里显式 include `boost/noncopyable`。

## 1. 新增文件（6 个用例 + 1 个共用替身头）

| 文件 | 行数 | 用例数 | 说明 |
|---|---|---|---|
| `q04_trader_fixture.h` | 438 | — | 假 `ITraderApi`（记录委托/撤销快照、可注入返回值）、假 `IBaseDataMgr`（自建 `WTSCommodityInfo/WTSContractInfo`）、假 `ITrdNotifySink`、临时目录根、策略文件文本、回报构造助手 |
| `q04_trader_reply_machine_test.cpp` | 501 | 8 启用 + 1 DISABLED | `initExt/run/release`、`handleEvent`、`onLoginResult/onLogout`、`onRspPosition/onRspOrders/onRspTrades/onRspAccount` 推进链、`onRspEntrust` 错单去重、`onTraderError/handleTraderLog`、`init()` 风控段解析 |
| `q04_trader_entrust_paths_test.cpp` | 388 | 6 启用 + 1 DISABLED | 四个下单入口、`doEntrust` 代码改写与 usertag 组装、市价/限价与订单标志、委托失败支、`updateUndone/getUndoneQty`、`doCancel/cancel/cancelAll`、跨通道 usertag 隔离 |
| `q04_trader_buy_sell_rules_test.cpp` | 369 | 9 启用 + 1 DISABLED | `buy/sell` 动作规则全分支：`AT_Open` 的 `_limit_l/_limit_s/_limit`、`AT_CloseToday/AT_CloseYestoday` 的 `_pure` 与 `CoverMode`、`AT_Close` 区分/不区分昨今、单笔最大委托量拆分、剩余量未触发告警支 |
| `q04_trader_push_report_test.cpp` | 367 | 7 启用 + 1 DISABLED | `onPushOrder`（NULL/未知合约/撤销统计/首次扣可平/重复推送回补并夹回昨仓/本通道认领与 `_orders` 增删/sink 通知）、`onPushTrade`（T+1 与免 T1、平今、先扣昨仓）、外部单不认领 |
| `q04_trader_risk_limits_test.cpp` | 273 | 5 启用 + 1 DISABLED | `init()` 的 `riskmon` 解析（激活/未激活/无配置/无 default 告警）、`getRiskParams` 品种键、`checkCancelLimits/checkOrderLimits` 总量限额与排除队列 |
| `q04_uftctx_local_report_test.cpp` | 293 | 5 启用 + 1 DISABLED | `UftStraContext`：`on_channel_ready → load_local_data` 四个 membin 块建立、`mannual.yaml` 手工持仓导入与改名、`on_trade` 净头寸推进与回合盈亏、`on_order/on_entrust/on_position/on_channel_lost`、`stra_buy/sell/enter_*/exit_*/cancel/cancel_all/get_position/get_undone/get_infos/get_comminfo/enum_position`、交易日切换 |

合计 **40 条启用用例 + 6 条 `DISABLED_`**（`grep -c '^TEST'` 实测 9/7/10/8/6/6 = 46）。
`q04_trader_reply_machine_test.cpp` 超出简报建议的 400 行上限 101 行（夹具与回报构造助手已尽量抽进 `q04_trader_fixture.h`，剩余是同一条推进链的连续断言，拆分会破坏「一个夹具一份状态、先写后读」约束）。

## 2. 覆盖的符号 / 分支（此前 `TraderAdapter.cpp` 1335 行未覆盖 / 11.71%，`UftStraContext.cpp` 712 行未覆盖 / 11.99%）

### `src/WtUftCore/TraderAdapter.cpp`（n03 覆盖 init/initExt/run/release/查询去重之外的剩余部分）
- `init`：`riskmon.active=true` 的 `policy` 逐组解析（六个限额字段）、`active=false` 支、无 `riskmon` 支、无 `default` 组的告警支、`_cfg != NULL` 的二次拒绝支、`module` 缺失早退支（返回 false 但风控表已建好，是本波唯一能把适配器推进到「可用而不加载动态库」的入口）。
- `initExt/run/release` 与 `handleEvent(WTE_Connect, ec==0)` 的 `login(user,pass,product)` 支（靠 `init` 留下的 `_cfg`）。
- `onLoginResult`（成功/失败）、`onLogout`、`onRspEntrust`（错误码过滤、动作文本三分支、错单重复通知去重、合约缺失早退、usertag 解析 localid）、`onTraderError`、`handleTraderLog`、`getBaseDataMgr`。
- `onRspPosition`（多头/空头填充、未知合约 continue、`printPosition`、逐 sink 发 `on_position`、状态推进 + `queryOrders`）、`onRspOrders`（buy/sell 判定、错单/自动撤单/普通撤单、`_infos` 累计、非本通道 usertag 跳过登记、NULL 数组元素、`_undone_qty` 清空重算、状态推进 + `queryTrades`）、`onRspTrades`（六个开平统计组合、无 ContractInfo continue、统计日志循环、状态推进 + `queryAccount`）、`onRspAccount`（`AS_TRADES_QRYED → AS_ALLREADY` + `on_channel_ready`）。
- `doEntrust`（`makeEntrustID`、标准代码 → 市场/裸代码改写、usertag `wtp.<id>.<localid>` 组装、`orderInsert<0` 返 `UINT_MAX`）、`openLong/openShort/closeLong/closeShort`（市价/限价、`WOF_NOR+flag`、开平与平今标志、`updateUndone`）、`buy/sell` 全部规则分支、`doCancel/cancel/cancelAll`（活单判定、未知 localid、`_actionRet<0`、按 fullcode 过滤）、`getOrders/getPosition/enumPosition/getUndoneQty/getInfos` 的有数据支。
- `onPushOrder`（撤销统计四分支、撤单退回未完成量 + `formatAction`、首次推送扣可平（平今/平仓先昨后今）、重复推送回补可平并夹回昨仓上限、本通道认领后 `_orders` 增删、sink `on_order` 的 offset 三分支）、`onPushTrade`（本通道 `updateUndone`、多空开平四种持仓推进、T+1 与免 T1 的可用仓差异、`on_trade` 通知、`queryAccount`）、`checkCancelLimits/checkOrderLimits`（排除队列、总量限额、`getRiskParams` 的 default 回落与品种键错切）、`isTradeEnabled`（定义 TU 内可达，外部 TU 因 BUG-Q04-006 无法引用）。
- `TraderAdapterMgr::run/release/getAdapter/addAdapter` 已在 n03 覆盖，本波未重复。

### `src/WtUftCore/UftStraContext.cpp`（p06 覆盖 stra_get_*/stra_sub_*/stra_log_* 之外的剩余部分）
- `on_channel_ready`（交易日变化才 `load_local_data`、同交易日幂等、`_strategy==NULL` 支）。
- `load_local_data`：`_tradingday==0` 早退、`outputs/<name>/` 自建、`mannual.yaml` 检测/解析/逐条导入（合约不存在 continue）、导入后改名避免二次生效、`position/order/trade/round` 四个 membin 的新建与 `_blk_flag/_date/_capacity` 写入、明细重载进 `_positions`（`_opencost/_volume/_details`）、跨交易日 `memset` 回收与 `_size` 收缩。
- `on_trade`：`is_my_order` 过滤、买入侧（先平空→生成回合与成交明细→剩余开多）、卖出侧（先平多→回合盈亏→剩余开空）、`_valid_idx` 递进与 `_direct` 不匹配 continue、`_strategy==NULL` 时不转发。
- `on_order`（非本单 continue、`curOrd==NULL` 建块与 `_state` 三分支、二次更新）、`on_entrust`（本单/非本单）、`on_position`（空实现）、`on_channel_lost`。
- `stra_buy/stra_sell/stra_enter_long/stra_enter_short/stra_exit_long/stra_exit_short`（含 localid 登记进 `_order_ids`）、`stra_cancel/stra_cancel_all`、`stra_get_position/stra_get_undone/stra_get_infos/stra_get_comminfo/stra_enum_position` 与 `stra_get_local_*` 的有数据支。

### 跳过目标及原因（供排除清单评审）
- `UftStraContext` 的 `watch_param/read_param/sync_param/commit_param_watcher`（`ShareManager::self()` 无共享内存段即空函数指针解引用）与 `stra_get_bars/ticks/order_detail/order_queue/transaction/last_tick`（需历史行情库）：沿用 n03/p06 的排除结论。
- `TraderAdapter::init` 的动态库装载段（`DLLHelper::load_library/get_symbol/createTrader`）与 `check*Limits` 的时间窗分支（BUG-Q04-004 死代码）：单测不可达。
- `TraderAdapter` 的 `UftStraContext::load_local_data` 里 `WTSCfgLoader` 解析失败支（写坏 `mannual.yaml` 会走 `_pos_blk._block->_size` 的空指针解引用，已并入 BUG-Q04-007 家族，未写启用断言）。
- `EventNotifier`、`ShareManager`、`ParserAdapter::init` 的 DLL 装载与 L2 链、`WtUftEngine::run`、`WtUftRtTicker::run/stop`：前几波已排除，本波未触碰。

## 3. 待审核缺陷

`docs/迭代计划/迭代01/bugs/q04_uftcore3.md`：**9 条（BUG-Q04-001 … BUG-Q04-009），状态一律待审核**，其中 Blocker 4 条：
- BUG-Q04-003 `getRiskParams` 品种键多切 2 个字符（`ag24` 而非 `ag`）→ 品种级风控静默失效
- BUG-Q04-005 `getRiskParams` 无 `default` 时对 `end()` 取址（引用 BUG-N03-002 同类）
- BUG-Q04-007 `load_local_data` 交易日切换不清 `_positions` → 净持仓翻倍 + 旧 mmap 悬垂指针（待裁定）
- BUG-Q04-008 `cancelAll` 不回落 `_bd_mgr` 却直接用 `getContractInfo()->getFullCode()` → 普通回报形态下必崩
Major 4 条：BUG-Q04-001（`getOrders` 按标准代码恒空，待裁定）、Q04-002（委托失败不回滚未完成量，待裁定）、Q04-004（时间窗频率风控是死代码）、Q04-006（`isTradeEnabled` 声明 `inline` 却在 `.cpp` 定义，外部 TU 编译失败）；Minor 1 条：Q04-009（构造未初始化 `_trading_day/_bd_mgr/_policy_mgr` + `_orders` 泄漏，引用 BUG-N03-005/BUG-P06-001 同类）。

## 4. 风险处置与硬约束遵守情况

- 崩溃/越界/UB 类输入整类收进 6 个 `DISABLED_` 用例，启用用例零残留：未在未 `run()`（`_stat_map==NULL`）的适配器上调用任何回报/限额接口；未在未 `on_channel_ready` 的 ctx 上推成交；未读未初始化的 `_trading_day`（`onLoginResult` 先写后读）；裸代码下单/`cancelAll`/无 default 组/`initExt(NULL api)` 全部留在 `DISABLED_`。
- 关于 `TraderAdapter::run()`：它是 `_stat_map` 的唯一创建点（回报链入口），本波用替身 `ITraderApi` 驱动——替身的 `registerSpi/connect/login/orderInsert/orderAction/query*` 全部是纯计数/记录，无 socket、无线程、无共享内存、无 `sleep`，因此不构成简报禁止的阻塞路径；`TraderAdapterMgr::run`、`WtUftEngine::run`、`WtUftRtTicker::run`、`ParserAdapter::run` 一次都没调用。
- 不断言墙钟/时区：`makeLocalOrderID()` 产生的 localid 只做非 `UINT_MAX`、互不相等与 usertag 拼接校验；`_oder_time/_open_time/_trading_time` 等时间戳一律不断言。
- 落盘只在 `std::filesystem::temp_directory_path()/q04_ctx*`、`q04_*` 唯一目录（含 pid + 序号）内，RAII 析构 `remove_all`；`WtHelper::getOutputDir()` 的路径是函数内静态，所以 q04 用例统一把根目录定死为 `temp_directory_path()/q04_uftctx_root`，再用唯一策略名划分子目录，保证同进程多用例互不干扰。
- 期望值全部由实现/注释/约定推导；三处「实现与约定不一致」的条目（Q04-001、Q04-002、Q04-007）按实现值断言并在用例注释与 bug 文件里标注「待裁定」，未为了让断言变绿而迁就。

## 5. 门禁结果

按简报指定的唯一构建动作，对 6 个 `.cpp` 逐个执行（并在最后全目录复跑一次确认落盘状态）：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/q04_uftcore3/<file>.cpp
```
结果：**6/6 通过，0 error 0 warning**（`GATE_FAIL=0`）。
未运行 cmake/ninja/全量构建/测试二进制，未自行编译运行用例。

## 6. 预算与剩余

- 本轮工具调用 51 次（预算 60，第 6 次前产出第一个 Write）。
- 剩余可接手的同类目标（已定位，未做）：
  1. `UftStraContext::on_tick` 的盯市 `_dynprofit` 推进与 `stra_get_bars/ticks`（需行情缓存注入）。
  2. `TraderAdapter` 的 `buy/sell` 在 `_limit` 与 `unitQty` 组合下的多级拆分（本波只覆盖了单层拆分）。
  3. `EventNotifier::tradeToJson/orderToJson`（仍需主控先决定是否提为可测自由函数，沿用 n03 结论）。
