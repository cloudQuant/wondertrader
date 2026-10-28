# R13 manifest — WtPorter C 导出族 + Exp*Context 转发层（第七波 lane R13）

- 波次: 迭代01 第七波，路 13（范围与数字由全量报表 UNCOVERED 段生成，见任务简报）
- 本轮范围（`src/WtPorter/`，一行生产代码未改）:
  - `WtPorter.cpp` missed 435/848 —— **本轮主攻**
  - `ExpHftContext.cpp` missed 61/61、`ExpCtaContext.cpp` 28/33、`ExpSelContext.cpp` 24/29
  - `ExpParser.cpp` 1/28 + `ExpParser.h` 3/3、`ExpExecuter.h` 2/2
  - `WtRtRunner.cpp` 621/938、`WtRtRunner.h` 18/18 —— **凡 R6 点名为「需要接缝」的面一律留白**（第四节）
- 用例目录: `src/TestUnits/coverage/q06_porter/`（前缀 `r13_`，未动 q06/R6 的文件、未建目录、未改 CMakeLists）
- 归属目标: `CoverageTests_porter`
- 符号缝: 只引用 `WtPorter` 这一族 `extern "C"` 入口（`WtPorter/WtPorter.h`），一次都没引 `WtBtPorter`
- 门禁（真实构建口径，未自造 flags）:

```
cd build-cov && ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_porter.dir/q06_porter/q06_porter_runner_test.cpp.o | tail -1
# 截掉 " -MD ... -o ... -c ..." 尾巴后追加 -fsyntax-only <我的 cpp>
r13_porter_cta_export_test.cpp       -> exit=0
r13_porter_sel_hft_export_test.cpp   -> exit=0
r13_porter_expctx_forward_test.cpp   -> exit=0     （含 -Wall -Wextra -Werror）
```

- 未跑构建、未跑二进制、未跑 ctest、无 git 操作。

## 一、取样手段：用真实句柄打开 `if (ctx == NULL)` 的另一支

q06 lane 把三个族（cta/sel/hft）的导出入口都走了「句柄查不到上下文」的拒绝支；
`WtPorter.cpp` 里被计成 missed 的正是剩下的那一支。本轮的钥匙是
`create_cta_context`（WtPorter.cpp:404）/ `create_sel_context`（:1135）/ `create_hft_context`（:1652）
三个 C 入口——它们经 `WtRtRunner::createXxxContext`（WtRtRunner.cpp:314/:321/:338）
把上下文塞进各引擎的 `_ctx_map`，之后每个导出函数的 `getRunner().getXxxContext(h)` 都会命中。

之所以不需要 `config_porter` 也能建上下文（绕开 R6 卡在 BUG-R6-001/002/003 上的那批接缝）：
三个 `createXxxContext` 只引用 runner 自己的成员引擎 `_cta_engine/_hft_engine/_sel_engine`
（WtRtRunner.h:841/845/849），不碰 `_engine`；`CtaStraBaseCtx/SelStraBaseCtx/HftStraBaseCtx`
的构造函数（CtaStraBaseCtx.cpp:58-70、SelStraBaseCtx.cpp:40-52、HftStraBaseCtx.cpp:41-48）
也只存指针 + 分配 `_context_id`。

**只读安全面**（逐行核过实现，空表读数与 map 读写）:
`_pos_map/_sig_map` find-miss 即 return 0（CtaStraBaseCtx.cpp:1824-1836、:1867-1871、:1887-1895；
SelStraBaseCtx.cpp:1144-1148、:1176-1180、:1196-1200、:1225-1229、:1244-1248、:1081-1085；
HftStraBaseCtx.cpp:601-609、:611-629）；`_user_datas` 是纯 map（CtaStraBaseCtx.cpp:1746-1759、
SelStraBaseCtx.cpp:1066-1079、HftStraBaseCtx.cpp:711-722）；`StraFundInfo` 构造函数 memset 归零
（SelStraBaseCtx.h:909-913、HftStraBaseCtx.h:790-794）→ 资金族任意 flag 恒等于 0.0；
`stra_log_*` 只走 `WTSLogger::log_dyn_raw`，不建文件。

**取不到的面**一律不进启用用例，见第三节 DISABLED 与第四节留白清单。

## 二、用例 → 生产文件:行段（行号一律本轮 Read/Grep 实测）

### `src/TestUnits/coverage/q06_porter/r13_porter_cta_export_test.cpp`（7 启用 + 2 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| （夹具自检，随所有用例执行） | `Share/CodeHelper.hpp:601-616`、`:617-655` | 三段式 `SHFE.rb.2405` 的 product=rb/code=rb2405；两段式把点号后整段当 product（主控规则 1） |
| `R13PorterCtaExport.handle_resolves_to_the_same_context_on_repeated_lookups` | `WtPorter.cpp:404-407` + `WtRtRunner.cpp:314-319` + `WtCtaEngine.cpp:133-146` | 句柄即 `ctx->id()`；`getContext` 返回 `it->second` 拷贝，第三次取仍非空（对照 R4 的 std::move 掏空形态） |
| `R13PorterCtaExport.unknown_handle_still_resolves_to_null_context` | `WtRtRunner.cpp:369-372`、`WtPorter.cpp:701-708` | 反面对照：未知句柄必须查不到 |
| `R13PorterCtaExport.userdata_roundtrips_exact_value_and_falls_back_for_absent_key` | `WtPorter.cpp:951-958`、`:969-976` → `CtaStraBaseCtx.cpp:1746-1759` | save/load 精确回读、空串也是合法值、缺键回 defVal、同键后写胜出 |
| `R13PorterCtaExport.userdata_is_per_context_not_shared_between_two_ctas` | `WtPorter.cpp:404`、`:951`、`:969` + `CtaStraBaseCtx.cpp:58-70` | 两个同名不同句柄的上下文用户数据互不可见（若被提到静态即红） |
| `R13PorterCtaExport.virgin_context_position_getters_are_exactly_zero` | `WtPorter.cpp:701-708`、`:578-585`、`:655-662` → `CtaStraBaseCtx.cpp:1824-1836/1887-1895/1867-1871` | 空表读数精确 0.0，`bOnlyValid`/`openTag` 两种组合同样 0 |
| `R13PorterCtaExport.log_text_routes_four_levels_without_changing_context_state` | `WtPorter.cpp:918-941` → `CtaStraBaseCtx.cpp:1726-1734` | 四个级别 + default 支（PorterDefs.h:52-55 只有 0..3，越界值进 default）；日志不得改状态、不得触发策略槽位 |
| `R13PorterCtaExport.fund_data_reads_zeroed_fund_struct_for_every_flag` | `WtPorter.cpp:720-727` → `CtaStraBaseCtx.cpp:1709-1724` | flag 0/1/2/3 与 default 全部 0.0（`StraFundInfo` memset） |
| `DISABLED_position_mutation_entries_deref_uninitialized_engine` | `WtPorter.cpp:420/440/460/480/743` → `WtEngine.cpp:956+` | 触发条件：真句柄下进 `do_set_position`，解引用未 init 的 `_data_mgr/_hot_mgr`（config 主体被 BUG-R6-001/002 卡死） |
| `DISABLED_history_slice_entries_need_a_data_manager_seam` | `WtPorter.cpp:501-532`、`:544-568` → `WtEngine.cpp:592-644` | 触发条件：`get_kline_slice/get_tick_slice` 依赖 `_data_mgr`；导出层包了 `catch(...)` 但 SIGSEGV 抓不到 |

### `src/TestUnits/coverage/q06_porter/r13_porter_sel_hft_export_test.cpp`（8 启用 + 3 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `R13PorterSelExport.create_sel_context_accepts_every_documented_period_and_returns_distinct_handles` | `WtPorter.cpp:1135-1138` → `WtRtRunner.cpp:338-355` → `WtSelEngine.cpp:390-415` | 六种 period 写法各得一个可查句柄；未识别写法落 `TPT_None` 且**照样返回句柄**（BUG-R13-004） |
| `R13PorterSelExport.sel_userdata_roundtrips_exact_value_and_default` | `WtPorter.cpp:1148-1155`、`:1166-1173` → `SelStraBaseCtx.cpp:1066-1079` | SEL 族用户数据精确回读 + 缺键回 defVal |
| `R13PorterSelExport.sel_virgin_position_getters_are_exactly_zero` | `WtPorter.cpp:1275/1454/1534/1494/1515/1553/1436` → `SelStraBaseCtx.cpp:1144/1196/1176/1225/1244/1081` + `SelStraBaseCtx.h:909-913` | 六个持仓读数 + 三个资金 flag 精确 0.0 |
| `R13PorterSelExport.sel_all_position_emits_only_the_terminator_for_an_empty_context` | `WtPorter.cpp:1249-1263` → `SelStraBaseCtx.cpp:659-680` | 空上下文恰好 1 次回调、`("", 0, true)`；内层枚举 0 次 |
| `R13PorterSelExport.sel_log_text_passes_all_levels_without_touching_state` | `WtPorter.cpp:1183-1206` | SEL 的四级 + default 支 |
| `R13PorterHftExport.hft_map_only_position_getters_are_exactly_zero` | `WtPorter.cpp:1687-1694`、`:1706-1713` → `HftStraBaseCtx.cpp:601-609/611-629` | 只读 `_pos_map` 的两个 HFT 入口安全且精确 0.0（对照下一条 DISABLED 的野指针族） |
| `R13PorterHftExport.hft_userdata_roundtrips_exact_value_and_default` | `WtPorter.cpp:2224-2231`、`:2244-2251` → `HftStraBaseCtx.cpp:711-722` | HFT 族用户数据精确回读 |
| `R13PorterHftExport.hft_log_text_passes_all_levels_without_touching_state` | `WtPorter.cpp:1994-2017` | HFT 的四级 + default 支，且不牵连持仓槽 |
| `DISABLED_hft_trader_bound_entries_deref_unbound_pointer` | `WtPorter.cpp:1668/1725/2100/2121/2157/2193/2027/2044/2062/2080` → `HftStraBaseCtx.cpp:188-199/211/262/631-679`、`HftStraBaseCtx.h:673` | 触发条件：`create_hft_context` 因通道不存在而未绑 `_trader`，10 个入口全部野指针解引用（BUG-R13-001） |
| `DISABLED_sel_mutation_and_subscription_need_engine_seam` | `WtPorter.cpp:1333-1341`、`:1391-1398` → `WtEngine.cpp:779+`、`:956+` | 触发条件：`do_set_position`/`sub_tick` 依赖 `_data_mgr/_hot_mgr` |
| `R13PorterEngineSeam.DISABLED_engine_direct_getters_deref_null_engine` | `WtPorter.cpp:857/872/883/894/905`、`1215/1226/1237/1409/1421`、`1743/1755/1767/1779` | 触发条件：14 个 `getRunner().getEngine()->...` 直取入口，未 config 时 `_engine==NULL`（同 BUG-Q06-002/R6-003） |

### `src/TestUnits/coverage/q06_porter/r13_porter_expctx_forward_test.cpp`（19 启用 + 1 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `R13ExpContext.cta_on_bar_close_forwards_handle_code_period_verbatim` | `ExpCtaContext.cpp:115-119` → `WtRtRunner.cpp:384-394`（ET_CTA 支） | 句柄/合约/周期原样透传，不牵连 calc/cond/HFT 槽 |
| `R13ExpContext.cta_on_bar_close_passes_null_bar_through_without_deref` | `ExpCtaContext.cpp:118` | 转发层对 NULL bar 没有守卫（对比 `ExpHftContext.cpp:32-33` 有），指针只是递给导出方 |
| `R13ExpContext.cta_on_calculate_forwards_date_and_time_pair` | `ExpCtaContext.cpp:127-131` → `WtRtRunner.cpp:396-405` | (20240603, 145500) 精确落到 calc 槽 |
| `R13ExpContext.cta_on_condition_triggered_forwards_target_price_and_usertag` | `ExpCtaContext.cpp:141-145` → `WtRtRunner.cpp:407-415` | CTA 独有的 cond_trigger 槽；target/price 单次透传，精确比较 3520.5/3518.25 |
| `R13ExpContext.cta_on_tick_updated_is_dropped_when_the_code_was_never_subscribed` | `ExpCtaContext.cpp:97-106`（`:100-102` 的 find-miss 支） | 未订阅合约在转发前就被丢弃，NULL tick 不被解引用 |
| `R13ExpContext.cta_on_session_begin_forwards_with_isbegin_true` | `ExpCtaContext.cpp:66-73` → `CtaStraBaseCtx.cpp:1018-1038`（`_ud_modified` 假支，构造列表 :67 已初始化）+ `WtRtRunner.cpp:429-439` | isBegin=true、tdate 原样 |
| `R13ExpContext.sel_on_session_begin_forwards_to_the_sel_slot_only` | `ExpSelContext.cpp:62-69` → `SelStraBaseCtx.cpp:636-656` + `WtRtRunner.cpp:429-439` | SEL 支；不串到 CTA 的 bar/calc |
| `R13ExpContext.sel_on_strategy_schedule_forwards_as_a_calc_event` | `ExpSelContext.cpp:91-95` → `WtRtRunner.cpp:396-405` | 调度事件映射成 calc 事件 |
| `R13ExpContext.sel_on_bar_close_forwards_handle_and_period` | `ExpSelContext.cpp:104-108` | 句柄=_sel、周期 "d1" 原样 |
| `R13ExpContext.sel_on_tick_updated_is_dropped_when_the_code_was_never_subscribed` | `ExpSelContext.cpp:117-125`（`:120-122` find-miss 支） | 与 CTA 同形的丢弃支 |
| `R13ExpContext.hft_on_position_forwards_all_four_quantity_fields_exactly` | `ExpHftContext.cpp:161-165` → `WtRtRunner.cpp:483-487` 的 `hft_on_position` | 唯一「只转发、不调基类」的 HFT 数量入口：5.0/3.0/8.0/6.0 精确 |
| `R13ExpContext.hft_on_position_reports_short_side_with_islong_false` | 同上 | isLong=false 支 + 负数持仓 |
| `R13ExpContext.hft_on_bar_with_null_bar_returns_before_any_forward` | `ExpHftContext.cpp:29-33` | `newBar == NULL` 的早退守卫（非 NULL 路径要过 `_ud_modified`，BUG-R13-002） |
| `R13ExpContext.hft_micro_data_forwards_reach_the_runner_without_a_bound_callback` | `ExpHftContext.cpp:196-200`、`:208-212`、`:220-224` → `WtRtRunner.cpp:1188-1206` | 三个微数据转发点自身不解包（`_cb_*` 未注册时静默），NULL 载荷安全通过 |
| `R13ExpParser.lifecycle_forwards_its_own_id_to_every_parser_slot` | `ExpParser.cpp:26-31`、`:37-41`、`:48-53`、`:60-65` + `ExpParser.h:33`、`:73` → `WtRtRunner.cpp:1207-1228` | init/connect/disconnect/release 各 1 次事件、id 恒为自身；`isConnected()` 恒 true；**栈构造绕开了 `create_ext_parser` 的空 `_engine`（BUG-R6-003）** |
| `R13ExpParser.subscribe_and_unsubscribe_emit_one_event_per_code` | `ExpParser.cpp:72-78`、`:85-91` → `WtRtRunner.cpp:1231-1240` | 3 个合约 → 3 订 + 3 退；`isForSub` 分别为 true/false |
| `R13ExpParser.register_spi_with_null_listener_skips_the_basedatamgr_pull` | `ExpParser.cpp:98-106`（`:104` 的 `if (m_sink)` 假支） | NULL listener 不去取 BaseDataMgr，也不产生任何 parser 事件 |
| `R13ExpExecuter.init_forwards_the_executer_name_to_the_registered_slot` | `ExpExecuter.h:28`、`:30` + `ExpExecuter.cpp:24-28` → `WtRtRunner.cpp:1261-1264` | 栈构造（`IExecCommand` 无纯虚，见 `WtCore/IExecCommand.h:62-113`）；`name()` 即构造入参 |
| `R13ExpExecuter.set_position_emits_one_command_per_map_entry` | `ExpExecuter.cpp:35-43`、`:51-55` → `WtRtRunner.cpp:1267-1270` | map 有 2 项就发 2 次命令，且 code 与 value 成对透传 |
| `R13ExpContext（同上一条用例集）` | `WtRtRunner.cpp:1207-1242`、`1261-1270` | 转发层与注册族槽位的作用域按实例一致（对照 BUG-R6-007） |
| `DISABLED_init_and_session_end_and_hft_trader_paths_need_output_and_trader_seams` | `ExpCtaContext.cpp:48-58`、`ExpSelContext.cpp:48-55`、`ExpHftContext.cpp:50-70/80-88/93-100/141-148/173-188/235-242` | 触发条件：见第四节第 3 条（`init_outputs()` 无条件建目录、`save_data()` 落文件、`_trader`/`_ud_modified` 未初始化） |

## 三、启用/DISABLED 计数（grep 实测，非自估）

```
r13_porter_cta_export_test.cpp       :  7 启用 / 2 DISABLED_
r13_porter_sel_hft_export_test.cpp   :  8 启用 / 3 DISABLED_
r13_porter_expctx_forward_test.cpp   : 19 启用 / 1 DISABLED_
合计：34 启用（目标区间 20~35）、6 条 DISABLED_、3 个文件全部过门禁
```

## 四、留白的面（不硬闯）与估计行数

### 1) `WtPorter.cpp` 里被 R6/Q06 点名为「需要接缝」的入口 —— 全部留白

| 面 | 位置 | 估计未取样行数 | 卡在哪 |
| --- | --- | --- | --- |
| `init_porter/config_porter/run_porter/release_porter` | `WtPorter.cpp:239-289` | ≈ 24 | BUG-R6-001/002（config 不判空）+ BUG-R6-006（run 不判 `_engine`、release 里 `WTSLogger::stop()` 是进程级副作用）；`init_porter` 还带 `static bool inited` 的一次性门（:241-247），一进程只能用一次 |
| 引擎直取 getter 14 个 | `:857,872,883,894,905`、`1215,1226,1237,1409,1421`、`1743,1755,1767,1779` | ≈ 42 | `getRunner().getEngine()` 返回 NULL（BUG-Q06-002 / BUG-R6-003 同族） |
| 历史切片 9 个（cta/sel/hft 的 get_bars/ticks/ordque/orddtl/trans） | `:501,544`、`1295,1354`、`1797,1839,1881,1920,1959` | ≈ 150 | 需要 `WtDtMgr/_data_mgr` 夹具（q03/p09 那套 reader 接缝），未 config 时 NULL |
| 下单/仓位 11 个 | `:420,440,460,480,743`、`1333`、`2100,2121,2157,2193` | ≈ 80 | `WtEngine::do_set_position` 要 `_hot_mgr/_data_mgr`；HFT 那 5 个还要已绑定的 `_trader`（BUG-R13-001） |
| 订阅族 8 个 | `:985,1002`、`1391`、`2027,2044,2062,2080` | ≈ 35 | `WtEngine::sub_tick`（:779+）与 `sub_order_*` 依赖 `_hot_mgr`；`cta_sub_bar_events` 只改 `_kline_tags`（可取，但本轮预算内未做，留给下一波） |
| 图表/指标族 6 个 | `:1019,1037,1054,1072,1090,1108` | ≈ 45 | 未逐行核过 `set_chart_kline/register_index*` 是否触碰引擎指针，本轮按「不确证不启用」处理 |
| `get_raw_stdcode` | `:320-323` | ≈ 4 | BUG-Q06-002（`_engine->get_rawcode`） |
| 工厂注册 4 个 | `:352,364,376,388` | ≈ 16 | R6 已在 `WtRtRunner.cpp:1168-1186` 侧取过守卫支，导出层这一遍只是转发，未重复 |
| `feed_raw_bars/feed_adj_factors` | `:193-212` | ≈ 8 | BUG-Q06-004 / BUG-R6-009（`_feeder_bars/_feeder_fcts/_feed_obj` 未初始化即调用） |

**小计：`WtPorter.cpp` 本轮实际打开的 missed ≈ 130~170 行**（CTA 段 8 个入口的真支 + SEL 段 10 个 + HFT 段 4 个 + 3 个 create 入口，按每个 4~12 行折），
留白 ≈ 300 行上下，全部指向「能 config 成功的引擎夹具」这一个接缝。

### 2) `WtRtRunner.cpp/.h` —— 本轮一行没动

R6 实测把 `main`/`run()` 失败分支卡在 BUG-R6-001/002/003/006/009 上，`WtRtRunner.cpp` 491 行只进了约 30 行。
本轮按分工只借它的 `createXxxContext/getXxxContext/ctx_on_*/parser_*/executer_*`（R6、q06 已覆盖），
其余（`config()` 主体 :489-714、`initEngine/initDataMgr` :838-896、`run/release` :1091-1155、
`loadParsers/loadTraders/loadExecuters` :607-678、`init*Strategies` :918-1090）
**继续留白 ≈ 350 行**，需求是同一个：一个能在进程内跑通 `config()` 的最小 dtcfg 夹具（要 basefiles/holiday/parsers/traders 四段齐备，
并且不触发 `create_ext_parser` 的空 `_engine`）。这条接缝请主控或第八波统一给。

### 3) `Exp*Context` 的 on_init / on_session_end 与 HFT 的成交族 —— 留白

| 面 | 位置 | 估计行数 | 留白原因 |
| --- | --- | --- | --- |
| `ExpCtaContext::on_init` | `ExpCtaContext.cpp:48-58` | 3 | `CtaStraBaseCtx::init_outputs()`（CtaStraBaseCtx.cpp:77-83）无条件 `fs::create_directories`，`load_data()` 又要 `_data_mgr` → 进程内落文件 + 崩 |
| `Exp*Context::on_session_end` | `ExpCtaContext.cpp:81-88`、`ExpSelContext.cpp:76-83` | 4 | 基类 `save_data()`（CtaStraBaseCtx.cpp:1102、SelStraBaseCtx.cpp:709）落文件 |
| `ExpCtaContext::on_tick_updated` 的订阅支 | `ExpCtaContext.cpp:104-105` | 2 | 需要 `_tick_subs.insert`，唯一入口 `stra_sub_ticks` 带 `_engine->sub_tick`（CtaStraBaseCtx.cpp:1670） |
| `ExpSelContext` 同两处 | `ExpSelContext.cpp:124-125` | 2 | 同上 |
| `ExpHftContext::on_bar/on_tick/on_entrust/on_order/on_trade` 的非守卫段 | `ExpHftContext.cpp:36-43`、`:83-87`、`:144-147`、`:176-187`、`:238-241` | ≈ 20 | 尾段基类读未初始化的 `_ud_modified`（BUG-R13-002），`on_trade` 还要 `getOrderTag`/`stra_get_position` 走 `_trader` |
| `ExpHftContext::on_channel_ready/lost` | `ExpHftContext.cpp:50-57`、`:63-70` | ≈ 12 | `_trader->id()`（BUG-R13-001） |
| `ExpHftContext::on_init` | `ExpHftContext.cpp:93-100` | 3 | `load_userdata()` 落文件 |
| `ExpParser` 的 `registerSpi(非空)` 支 | `ExpParser.cpp:104-105` | 2 | 需要一个 `IParserSpi` 实现（含 `getBaseDataMgr`），本轮未造 |
| **小计** | | **≈ 46 行** | `ExpHftContext.cpp` 61 行里本轮取到约 14 行，另 47 行需要「输出目录夹具 + 已绑定交易通道」两个接缝 |

### 4) 交给主控的三件事

1. **本轮启用用例动过进程级状态**：`create_cta_context/create_sel_context/create_hft_context` 共 15 个上下文留在单例的 `_ctx_map` 里（无销毁入口，BUG-R13-006）；`register_*_callbacks` 在 TearDown 里全部撤销。请在「每用例一进程」口径下确认互不干扰。
2. **`ExpParser/ExpExecuter` 可直接栈构造**这件事值得复用：它绕开了 `create_ext_parser` 的空 `_engine`（BUG-R6-003），下一波若要把 `parser_push_quote` 的「命中 adapter」支打通，仍然需要 `ParserAdapterMgr::addAdapter` 的接缝，栈构造只能覆盖 ExpParser 自己这一侧。
3. 按提示专项核过 map 取值处的 `std::move`/erase-after-move：`WtCtaEngine::getContext`（:139-146）、`WtHftEngine::getContext`（:353-360）、`WtSelEngine::getContext`（:418-425）都是拷贝返回，**没有 R4 那种掏空缓存的形态**；porter 族里同类「返回值生命周期不成立」的是 `hft_buy/hft_sell` 的函数级 `static std::string`（BUG-R13-005）。
