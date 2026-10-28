# lane N01 manifest — WtCore 前半（交易路径）

工作流: 测试覆盖率迭代01 / lane N01。范围限 `src/WtCore` 交易路径 16 个文件
（ActionPolicyMgr、Cta/Hft/Sel 三套 StraBaseCtx+StraContext+StrategyMgr、EventNotifier、IExecCommand、
ITrdNotifySink、ParserAdapter、TraderAdapter、WtArbiExecuter，含对应 .h）。`src/**` 生产代码未做任何修改。

## 新增文件（行数与用例数均从磁盘统计）

| 文件 | 行数 | TEST 总数 | 其中 DISABLED_ |
|---|---|---|---|
| `src/TestUnits/coverage/n01_wtcore_tradepaths/n01_action_policy_test.cpp` | 326 | 10 | 1 |
| `src/TestUnits/coverage/n01_wtcore_tradepaths/n01_parser_adapter_test.cpp` | 473 | 12 | 1 |
| `src/TestUnits/coverage/n01_wtcore_tradepaths/n01_stra_registry_iface_test.cpp` | 198 | 10 | 0 |
| `src/TestUnits/coverage/n01_wtcore_tradepaths/n01_trader_adapter_test.cpp` | 237 | 13 | 1 |
| 合计 | 1234 | 45 | 3（启用用例 42 条）|

说明: `n01_parser_adapter_test.cpp` 因内嵌一整套 `IBaseDataMgr` 替身（11 个纯虚）而略超 400 行上限，其余 3 个文件都在 150–400 区间。

## 覆盖的类 / 函数（符号级）

### n01_action_policy_test.cpp
- `ActionPolicyMgr::init`（ActionPolicyMgr.cpp:47-125）：规则组遍历、`order` 数组解析、`wt_stricmp` 大小写不敏感动作名映射（open/close/closetoday/closeyestoday）、未识别动作 `continue` 分支、`order` 缺失/非数组分支、`filters` 品种映射分支、`load_from_file` 返回 NULL（文件不存在 + JSON 非法）两个分支。
- `ActionPolicyMgr::getActionRules`（:133-161）：命中品种组 / 未命中回落 `default` / 映射到不存在的组时二次回落并打错误日志。
- `ActionRule` 默认构造（memset 归零，实测 `_atype` 为 0 而非 AT_Unknown）与 `ActionType` 枚举连续性。

### n01_trader_adapter_test.cpp
- 构造函数初值：`state()`(=AS_NOTLOGIN)、`isReady()`、`id()`、`getBaseDataMgr()`。
- `init(NULL params)`(:99)、`initExt(NULL api)`(:209) 入参校验早退。
- `run()`(:404) 空 api 返回 false、`release()`(:419)、`queryFund()`(:2356) 状态非 AS_ALLREADY 早退。
- `getPosition()`(:428) 未登记合约分支（flag 1/2/3 × bValidOnly）、`getUndoneQty`、`getOrders()`(:467) `_orders==NULL` 分支、`isSelfMatched`。
- `checkOrderLimits()`(:609) / `checkCancelLimits()`(:544) 在 `_risk_mon_enabled==false` 时的短路分支。
- `addSink()`、`onRspPosition(NULL)`(:1545)、`onRspOrders(NULL)`、`onPushOrder(NULL)`。
- 自由函数 `makeLocalOrderID()`(:44)：本地委托号单调性与互异性（不断言墙钟绝对值）。
- `TraderAdapterMgr::addAdapter/getAdapter/getAdapters/run/release/refresh_funds`(:2369-2422)。

### n01_parser_adapter_test.cpp
- `ParserAdapter` 构造/析构、`id()`、`getBaseDataMgr()`、`handleSymbolList` 空实现、`run()`/`release()` 空 api 分支。
- `init(NULL cfg)` / `initExt(NULL api)` 校验；`init(cfg 无 module)` 返回 false 但已写入 `_stub/_bd_mgr/_id/_check_time` 的路径（:85-107）。
- `handleQuote()`(:239-294)：`quote==NULL`、`_stopped`、`actiondate==0`、`tradingdate==0` 早退；`check_time` 为 true 时未来时间戳（>1h）丢弃；未登记合约丢弃；`CC_Future` 分月→`rawMonthCodeToStdCode`、`CC_SpotOption`→`rawFutOptCodeToStdCode`、`CC_Stock`→`rawFlatCodeToStdCode` 三条标准化分支；tick 自带 ContractInfo 时跳过 `_bd_mgr` 查询。
- `handleParserLog()`(:367) release 前后。
- `ParserAdapterMgr::addAdapter/getAdapter/release/run`(:388-424)（重名/空名/NULL 三分支）。
- 测试替身：`FakeBaseDataMgr`（实现 `IBaseDataMgr` 全部 11 个纯虚）、`RecordingStub`（`IParserStub`）。

### n01_stra_registry_iface_test.cpp
- `CtaStrategyMgr::createStrategy(name,id)` / `createStrategy(fact,unit,id)` / `getStrategy(id)`（CtaStrategyMgr.cpp:95-133）及同构的 `HftStrategyMgr`、`SelStrategyMgr`：单段名、`StrUtil::split` 后工厂未加载、id 未命中三类查找分支；`loadFactories(空目录 / 不存在目录)`。
- `CtaStraWrapper` / `HftStraWrapper` / `SelStraWrapper`（三处头文件内联）：构造、`self()`、析构 `if(_stra)` 保护。
- `IExecCommand`（纯头文件）：`IExecCommand(name)` 构造、`name()/setName()`、`setStub()`、`set_position/on_position_changed/on_tick` 三个默认空实现。
- `ITrdNotifySink`（纯头文件）：`on_position/on_entrust/on_account` 三个默认空实现 + 经基类引用调用带默认实参的 `on_order`。

## 跳过目标及原因（供排除清单评审）

A. 需要真实交易通道 / 服务端回报
- `TraderAdapter`：`handleEvent`、`onLoginResult`、`onLogout`、`onRspEntrust`、`onRspAccount`、`onRspOrders`(实体)、`onRspTrades`、`onPushOrder`(实体)、`onPushTrade`、`onTraderError`、`doEntrust`、`doCancel`、`buy`、`sell`、`cancel`(两个重载)、`openLong/openShort/closeLong/closeShort`、`initSaveData`、`saveData`、`logTrade`、`logOrder`、`updateUndone`、`getRiskParams`、`checkSelfMatch`、`init`(带 module)。原因：都要 `ITraderApi*` 会话与真实 `WTSEntrust/WTSOrderInfo/WTSTradeInfo/WTSPositionItem`；且 `onPushOrder/onPushTrade` 在 `_stat_map==NULL` 时会先崩（BUG-N01-005），无通道时不可隔离。
- `checkOrderLimits/checkCancelLimits` 的风控启用分支（频率窗口、总量封禁、`_exclude_codes`）：`_risk_mon_enabled` 只能在 `init()` 解析 riskmon 时置位，而 `init()` 必然 dlopen trader 插件；该分支的 `getRiskParams` 在无 `default` 规则时越界（BUG-N01-003）。

B. 需要 MQ server / python 回调
- `EventNotifier.cpp`(399 行) + `EventNotifier.h`(70 行) 整文件：`init()` 要加载 `libwtpy` 并 `FuncCreateMQServer` 建服务端；`notify*`/`notify_log`/`notify_event`/`notify_chart_marker`/`notify_chart_index`/`notify_trade` 全部 `boost::asio::post(_asyncio, ...)` 后走 python 端；`tradeToJson/orderToJson` 亦依赖 `_asyncio` 已启动。整类跳过。

C. 需要 WtEngine 实体（引擎 getter 是非虚 inline，无法替身）
- `CtaStraBaseCtx`(2063+1178) / `HftStraBaseCtx`(987+802) / `SelStraBaseCtx`(1274+928) 的下单前置校验（手数/价格/涨跌停/是否可平空/合约有效性）与 `stra_enter_*/stra_exit_*/do_set_position/save_data/load_data/load_userdata/save_userdata/dump_chart_info`：三者构造需要 `WtCtaEngine/WtHftEngine/WtSelEngine*`，而 `WtEngine::get_basedata_mgr()/get_hot_mgr()/get_cur_price()/get_trading_date()` 都是非虚 inline，替身无法顶掉，只能拉起完整引擎（IBaseDataMgr + WtDtMgr + IHotMgr + 配置文件）。已在 BUG-N01-007 记录 `HftStraBaseCtx::stra_buy/stra_sell` 的「判了空又照用」缺陷。
- `CtaStraContext`/`HftStraContext`/`SelStraContext` 的 `on_tick/on_bar_close/on_calculate/on_trade/on_order/on_entrust/on_strategy_schedule`：同上，且需要真实行情与回报对象。

D. 需要 SHARED 插件（hidden 可见性）
- `*StrategyMgr::loadFactories` 成功路径与 `createStrategy` 命中路径：策略工厂是 `SHARED + CXX_VISIBILITY_PRESET hidden` 插件（同 BUG-L00-002），只能目录扫描 + dlopen 得到。
- `WtArbiExecuter.cpp`(891) + `.h`(439) 拆单/代码组算法：`set_position`(:404-547)、`on_position_changed`、`on_tick`、`buy/sell/cancel`、`on_trade/on_order/on_entrust` 直接依赖 `_trader->buy()/sell()` 与 `_data_mgr`；`ExecuteUnit` 与代码组又依赖 `WtExecuterFactory` 插件。整类跳过。
- `ParserAdapter::init` 的模块加载与订阅段(:108-211)、`initExt` 的 `getContracts()` 订阅段(:45-83)：需要真实 `IParserApi` 与全量合约表。

## 挂死/崩溃类缺陷的处置（纪律 1）

所有判定为「空指针/越界解引用 → 段错误」的输入统一收进同一个 DISABLED 用例，启用用例一条不留：
- `N01ActionPolicy.DISABLED_empty_rules_map_crashes_get_action_rules` — 缺陷类：规则表里没有目标组且没有 `default` 组（入口条件与 pid 取值无关，故该类只需一个例子）。
- `N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map` — 缺陷类：「回报在 `run()` 之前到达 → `_stat_map==NULL` 解引用」；同类第二入口 `getRiskParams` 返回 `&end()->second` 已在注释与 BUG-N01-003/005 点名。
- `N01ParserAdapter.DISABLED_uninitialized_check_time_and_null_bd_mgr_crash` — 缺陷类：「未 init 就喂数据」，含 `_check_time` 未初始化、level2 三通道 `_bd_mgr==NULL`、`commInfo` 未判空三个同类入口（BUG-N01-002/006）。

## 时间与状态纪律（纪律 2、3）

- 未断言任何墙钟绝对值或本地时区。`makeLocalOrderID()` 只断言 +1 递增与 64 次互异；`check_time` 用例用 2099 年时间戳，只断言「必然大于本地时间+1h」这一侧。
- 用例之间零共享状态：每个 TEST 自建对象，`TraderAdapter`/`ParserAdapter`/三个 StrategyMgr 都是栈或局部堆对象。
- 落盘只写 `std::filesystem::temp_directory_path()` 下带 `n01_` 前缀 + pid 的唯一目录（`n01_action_policy_*`、`n01_parser_cfg_*`、`n01_stra_reg_*`），RAII 析构里 `remove_all` 清理。

## 门禁结果

规定的 `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror ...` 逐文件执行：
4/4 文件全部 0 error 0 warning。未跑 cmake/ninja/全量构建/测试二进制。
首轮门禁暴露 3 处并已修掉：`boost::noncopyable` 需先显式包含；`TraderAdapter::isTradeEnabled` 触发 `-Wundefined-inline`（记为 BUG-N01-008 并从用例移除调用）；`WTSCfgLoader::load_from_json` 是 private（改用 `load_from_file` + 临时 json 文件）。

## 待主控处理

1. 未改任何 `src/**` 生产代码与 `CMakeLists.txt`。`src/TestUnits/coverage/CMakeLists.txt` 用 `GLOB_RECURSE COV_SRCS *.cpp`，新目录 `n01_wtcore_tradepaths/` 会被自动纳入，无需改构建脚本。
2. WtCore 是 STATIC 库，本轮被调符号（`ActionPolicyMgr`、`TraderAdapter`、`TraderAdapterMgr`、`ParserAdapter`、`ParserAdapterMgr`、`makeLocalOrderID`、三个 `*StrategyMgr`）都能直接链接，不涉及 hidden 可见性 TU 入表问题。
3. 疑似缺陷 8 条（Blocker 3 条），状态一律「待审核」：`docs/迭代计划/迭代01/bugs/n01_wtcore_tradepaths.md`（BUG-N01-001 ~ BUG-N01-008），未动源码。
