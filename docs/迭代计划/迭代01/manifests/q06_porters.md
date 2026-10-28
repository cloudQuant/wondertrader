# Q06 manifest — 实盘/回测 C 导出层（第五波）

- 波次: 迭代01 第五波
- 目标二进制: `CoverageTests_porter`（编 `src/WtPorter/*.cpp` + `libWtCore.a`）、
  `CoverageTests_btporter`（编 `src/WtBtPorter/*.cpp` + `libWtBtCore.a`）
- 用例目录: `src/TestUnits/coverage/q06_porter/`（实盘侧）、`src/TestUnits/coverage/q06_btporter/`（回测侧）
  两侧严格分开放置：实盘侧引用 `WtCore`，回测侧引用 `WtBtCore`（37 个同名 `wtp::` 类，BUG-L00-005），
  且两个 porter 目录各自定义同名 `extern "C"` 入口（`get_version/write_log/cta_get_position/...`），不得同处一个二进制。
- 未改动任何 `CMakeLists.txt` 与生产代码。
- 简报依据: `docs/迭代计划/迭代01/manifests/api-brief-L05.md`（WtDtPorter/WtDtServo 的
  「未初始化即返回错误码」形态示范）+ `api-brief-L03.md`（WTSStruct/WTSDataDef/ICtaStraCtx 等类型入口）；
  WtPorter/WtBtPorter 未在简报内，符号名/行号一律取本轮 Read/Grep 工具结果。

## 一、文件与覆盖符号

### 实盘侧 `src/TestUnits/coverage/q06_porter/`（3 个文件，30 个启用用例 + 3 个 DISABLED_）

| 文件 | 行数 | 用例 | 覆盖符号（本轮工具结果确认） |
| --- | --- | --- | --- |
| `q06_porter_capi_test.cpp` | ~300 | 11 启用 + 1 `DISABLED_UninitializedEngineGettersCrash` | `WtPorter.cpp`：`get_version`(L297)、`write_log`(L333)、`feed_raw_ticks`(L223)、`register_evt_callback`(L48)、`register_cta_callbacks`(L65)、`register_sel_callbacks`(L81)、`register_hft_callbacks`(L105)、`register_parser_callbacks`(L121)、`register_exec_callbacks`(L135)、`register_ext_data_loader`(L179)、`parser_push_quote`(L2265)；`cta_*`(L422-680/839-1000)、`sel_*`(L1150-1557)、`hft_*` 全族 `ctx == NULL` 分支；`WtRtRunner.cpp`：`on_ext_parser_quote`(L1243) 未命中分支 |
| `q06_porter_runner_test.cpp` | 695（超 150–400 约定，两个夹具合一文件，待主控决定是否拆分）| 14 启用 + 1 `DISABLED_BlockingAndNullEnginePaths` | `WtRtRunner.cpp`：`registerCtaCallbacks`(L154)/`registerSelCallbacks`(L167)/`registerHftCallbacks`(L179)/`registerEvtCallback`(L129)/`registerParserPorter`(L138)/`registerExecuterPorter`(L146)、`loadFinalHisBars`(L203)/`loadRawHisBars`(L228)/`loadAllAdjFactors`(L253)/`loadAdjFactors`(L265)（d1/m1/m5 + default）、`feedRawBars`(L283)、`feedAdjFactors`(L277)、`createCtaContext`(L314)/`createHftContext`(L321)/`createSelContext`(L338)、`getCtaContext`(L369)/`getHftContext`(L374)/`getSelContext`(L379)、`ctx_on_bar`(L384)/`ctx_on_calc`(L396)/`ctx_on_cond_triggered`(L407)/`ctx_on_init`(L417)/`ctx_on_session_event`(L429)/`ctx_on_tick`(L441)、`hft_on_channel_lost`(L453)/`ready`(L459)/`entrust`(L465)/`order`(L471)/`trade`(L477)/`position`(L483)/`order_queue`(L1188)/`order_detail`(L1194)/`transaction`(L1200)、`parser_init`(L1207)…`parser_unsubscribe`(L1237)、`executer_init`(L1261)/`executer_set_position`(L1267)；`WtCtaEngine::addContext/getContext`、`WtSelEngine::addContext/getContext`、`WtHftEngine::addContext` |
| `q06_porter_expwrapper_test.cpp` | ~290 | 5 启用 + 1 `DISABLED_ExtParserCreationNeedsEngine` | `ExpParser.cpp`：`init/release/connect/disconnect/subscribe/unsubscribe/registerSpi` 全部；`ExpParser.h`：构造、`isConnected`；`ExpExecuter.cpp`：`init/set_position/on_position_changed`；`ExpExecuter.h` 构造 |

### 回测侧 `src/TestUnits/coverage/q06_btporter/`（2 个文件，18 个启用用例 + 2 个 DISABLED_）

| 文件 | 行数 | 用例 | 覆盖符号 |
| --- | --- | --- | --- |
| `q06_btporter_capi_test.cpp` | ~300 | 9 启用 + 1 `DISABLED_BacktestLifecycleNeedsStorageAndThreads` | `WtBtPorter.cpp`：`get_version`(L253)、`write_log`(L285)、`feed_raw_bars`(L131)/`feed_raw_ticks`(L142)/`feed_adj_factors`、`set_time_range`(L195)/`enable_tick`/`clear_cache`、`cta_*`(L589/729/913 等)、`sel_*`(L1092 等)、`hft_*`(L1470/1759/1900 等) 的 `mocker == NULL` 分支、`cta_get_price`(L744)/`cta_get_tdate`(L766)/`cta_get_date`(L776)/`cta_get_time`(L786) 与 sel/hft 同名（经 `WtBtRunner.h:531 replayer()` → `HisDataReplayer.h:887/894/901/908/915` 内联与 `get_cur_price`） |
| `q06_btporter_runner_mocker_test.cpp` | 612（超 150–400 约定，与实盘侧 runner 用例同形，待主控决定是否拆分） | 9 启用 + 1 `DISABLED_BacktestRunPathsNeedStorage` | `WtBtRunner.cpp`：`registerCtaCallbacks`(L215)/`registerSelCallbacks`(L230)/`registerHftCallbacks`(L244)、`loadRawHisBars`(L96)/`loadFinalHisBars`(L121)/`loadAllAdjFactors`(L146)/`loadAdjFactors`(L158)/`loadRawHisTicks`(L170)、`feedRawBars`(L182)/`feedAdjFactors`(L193)/`feedRawTicks`(L204)、`ctx_on_bar`(L315)/`ctx_on_calc`(L327)/`ctx_on_calc_done`(L338)/`ctx_on_init`(L349)/`ctx_on_cond_triggered`(L361)/`ctx_on_session_event`(L371)/`ctx_on_tick`(L383)、`hft_on_order_queue`(L395)…`hft_on_trade`(L431)；`WtBtRunner.h`：`on_initialize_event`(L548)/`on_schedule_event`(L561)/`on_session_event`(L577)/`on_backtest_end`(L591)、`cta_mocker/sel_mocker/hft_mocker/replayer`(L510-531)、构造列表(L47-88)；`ExpCtaMocker.cpp`：构造/析构/`on_init`(L58)/`on_session_begin`(L77)/`on_session_end`(L95)；`ExpSelMocker.cpp`：构造/`on_init`(L52)/`on_session_begin`(L70)；`ExpHftMocker.cpp`：构造/`on_init`(L44) |

## 二、跳过目标及原因（建议进排除清单评审）

实盘侧（`src/WtPorter/`）：
1. `WtRtRunner::run()`(L1091)、`config()`(L489)、`init()`(L107)、`release()`(L1141)、`initEngine/initDataMgr/initParsers/initTraders/initCtaStrategies/initSelStrategies/initHftStrategies/initEvtNotifier/addXxxFactories`：起线程并 join / 等待条件变量 / 依赖 `config.json`、共享内存与 `.so` 工厂目录，属阻塞路径（硬约束 2）。
2. `createExtParser()`(L295)、`createExtExecuter()`(L305)：未初始化时对 `_engine` 及其 `get_basedata_mgr()/get_hot_mgr()` 解引用（空指针崩溃，见缺陷 Q06-002）。
3. `get_raw_stdcode()`(L361)：`_engine->get_rawcode` 同样无守卫。
4. `feedAdjFactors()`(L277)：`_feeder_fcts` 无守卫（缺陷 Q06-004），未注册即调用为崩溃类。
5. `cta_get_price/day_price/tdate/date/time`、`sel_get_price/...`、`hft_get_price/date/time/secs`（导出层 14 处 `getEngine()->`）：缺陷 Q06-002。
6. `createExtParser` 之后的 `parser_push_quote` 正常分支（`WTSTickData::create` + `adapter->handleQuote`）：需要已注册 adapter 与数据管理器。
7. `ExpCtaContext/ExpSelContext/ExpHftContext` 的 `on_init/on_session_*/on_tick/on_bar_close/on_calculate/on_trade/on_order/on_entrust/on_position` 覆写：基类 `CtaStraBaseCtx::on_init` 等要访问 `_engine` 的未初始化数据管理器；只覆盖了 `create*Context` 的建表与只读 getter。

回测侧（`src/WtBtPorter/`）：
1. `WtBtRunner::init/config/run/stop/release`(L437/449/509/550/585)：读配置、建 `WtDataStorage`、起回放线程并 join。
2. `initCtaMocker/initSelMocker/initHftMocker`(L266/285/299)：`_replayer.register_sink/register_task` 依赖已装载的代码表与交易时段表；改用直接构造 `Exp*Mocker` 覆盖包装层。
3. `ExpCtaMocker::on_tick_updated/on_bar_close/on_calculate/on_calculate_done/on_bactest_end/on_condition_triggered`、`ExpSelMocker/ExpHftMocker` 同名覆写：依赖回放器已装载数据与落盘目录（`persistData` 路径）。
4. `cta_get_day_price/sel_get_day_price`：`_tick_enabled` 为真时走 `get_last_tick()`（未装载存储）。
5. `cta_step/hft_step`：依赖异步回放线程与等待原语。
6. `get_raw_stdcode`（回测侧）：`_replayer.get_rawcode` 依赖代码表。

## 三、落盘与残留

- 本轮用例不写任何文件：回测侧 Mocker 一律 `persistData=false`，实盘侧不触发 `WtHelper::setGenerateDir`/输出目录，
  因此无需 `temp_directory_path()/q06_*` 目录（若后续波次新增落盘，按该约定建唯一目录并在 TearDown 清理）。
- 每个启用用例结束都把 `getRunner()` 的回调槽位复位为 NULL（见两个 runner 夹具的 `TearDown`），零残留。
- 崩溃/挂死类输入全部收进各自文件的单个 `DISABLED_` 用例（共 5 个），启用用例一次都不触碰。

## 四、门禁结果

逐文件执行规定的 `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror ...`（`-Wno-deprecated-copy-with-user-provided-copy`、`-Wno-#pragma-messages`、`-DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY`、`-include src/Share/fmt_enum_compat.h`、`-Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include`）：

| 文件 | 结果 |
| --- | --- |
| `q06_porter/q06_porter_capi_test.cpp` | 0 error / 0 warning |
| `q06_porter/q06_porter_runner_test.cpp` | 0 error / 0 warning |
| `q06_porter/q06_porter_expwrapper_test.cpp` | 0 error / 0 warning |
| `q06_btporter/q06_btporter_capi_test.cpp` | 0 error / 0 warning |
| `q06_btporter/q06_btporter_runner_mocker_test.cpp` | 0 error / 0 warning |

未跑 cmake / ninja / 测试二进制，未自行编译运行用例（按任务约束）。
首轮返工记录：`WTSBarStruct` 字段写作 `stime/open_interest`（实际为 `date/time/hold`）、
Recorder 内一个未使用的回调函数、回测侧 `CHNL_EVENT_READY` 在 `WtBtPorter/PorterDefs.h` 未定义
（`WtBtRunner.cpp:413` 传字面量 1000，见缺陷 Q06-007）——三处均已修正后复检通过。

## 五、疑似缺陷

`docs/迭代计划/迭代01/bugs/q06_porters.md`：7 条，全部 `- 状态: 待审核` + `- 严重级别:`。
其中「待裁定」项：Q06-001（`cta_get_last_entertag` 返回 NULL 还是 `""`）、Q06-005（回测侧忽略 `CtxHandler`），
相应用例按实现现状写成宽松断言并注明。

## 六、工具调用次数

56 次（含 5 次 Write + 1 次 manifest）。预算 60 次内，超 50 次后按要求立即收口。
