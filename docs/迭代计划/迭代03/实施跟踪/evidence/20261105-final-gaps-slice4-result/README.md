# 第 4 段（slice4-result）本地缺口修复证据（2026-11-05）

范围：`SCRATCH/review/slice4-result.json` 中标为 `C_LOCAL_GAP` 的基线条目与新增问题，以及主代理补充的 I03-NEW-089（仅执行器/HFT 上下文）与 I03-NEW-091（按 K02=B 作缺陷修复）。仓库 HEAD 见 `HEAD.txt`；工作区含大量他人未提交改动，测试期间其他代理仍在修改工作区，结论只对应下列哈希（`sources.sha256`：改前 `before`、改后 `after`），不代表最终冻结树。被改文件改前/改后副本在 `before/`、`after/`（文件名中 `/` 换成 `__`）。构建目录 `SCRATCH/slice4-result-build`（Apple clang、Debug、`WT_ENABLE_COVERAGE=ON`、默认 `-Werror`），运行目录 `SCRATCH/slice4-result-run/`。未自建 sanitizer 树；需要 sanitizer 的用例列在文末交主代理运行。未创建符号链接；未调用真实厂商 API。

文件锁：全部修改前经 `review/lock.py` 登记为 `slice4-result`。因锁冲突未做的子项在各条目“未完成”中写明。

（各条目结果见下文；构建与运行日志在 `logs/`。）

## BUG-I02-NEW-033：HFT 夹具 allocation-registry-failure 注入点修正（测试侧）

- 缺口：`HftStrategyMgr::createStrategy` 现在先在注册表中以空项预占策略 ID，再调用外部 creator；旧夹具在 creator 回调里武装“第 3 次分配失败”，已不会落在注册表插入点，`current-allocation-registry-failure` 在当前树失败（2026-11-04 `hft.out`），注册表分配失败的回滚路径实际没有被测到。
- 修改（只改夹具，不改产品）：
  - `src/TestUnits/fixtures/hft_factory_quarantine/check.cpp`：`allocation-registry-failure` 改为在调用前武装“下一次分配失败”（即注册表预占插入），断言：bad_alloc 传播、注入点为第 1 次分配、creator 未被调用、无策略删除且工厂保留、失败 ID 未发布、同一 ID 可重新预占并只发布一个策略。`allocation-wrapper-failure` / `allocation-control-failure` 另加“同一 ID 可重试”（证明预占已回滚），原断言不变。
  - `src/TestUnits/fixtures/hft_factory_quarantine/factory.cpp`：新增 `fixture_strategy_calls()`，统计进入策略 creator 的次数（原 `fixture_creations()` 统计的是工厂创建）。
- 红控：
  - 旧 `check.cpp`（`before/`）在当前产品上：`allocation-registry-failure` exit 1（`logs/hftq-red/old-check-current-product.txt`）。
  - 产品变异体（只在 SCRATCH 副本中修改，见 `logs/hftq-red/*.diff`）：`mut-norollback`（失败后不撤销预占）使 wrapper/control 两例 exit 1（“same ID can be retried …”）；`mut-latereserve`（creator 之后才插入注册表）使 registry 例 exit 1（“creator is not called …”）。结果 `logs/hftq-red/results.txt`。
- 绿控：`WT_BOOST_INCLUDE=/opt/homebrew/opt/boost/include WT_FMT_INCLUDE=/opt/homebrew/opt/fmt/include WT_SPDLOG_INCLUDE=/opt/homebrew/opt/spdlog/include sh src/TestUnits/fixtures/hft_factory_quarantine/run.sh SCRATCH/slice4-result-run/hftq-green` exit 0，26 个 current 场景全部 exit 0（`logs/hftq-green-run.log`、`logs/hftq-green/`）。
- 未覆盖：真实第三方策略插件、跨 DSO registry、Windows。夹具普通构建，未跑 sanitizer。

## I03-NEW-101：TraderOES 查询末行转换失败仍交付最终回调（产品修复 + 新隔离夹具）

- 隔离编译可行：OES 0.17.5.8 头文件在 macOS 上可编译（`TraderOES.cpp` 只有 21 条既有告警：未使用参数、成员初始化顺序、未使用私有字段，无错误），因此本项不是平台外部项。新夹具 `src/TestUnits/fixtures/i03_trader_oes_query_s4/`（README、`run.sh`、`oes_query_test.cpp`），测试以允许未解析符号的方式链接，`TraderOES` 从不 connect，不调用 OES API。
- 修改：`src/TraderOES/TraderOES.cpp` `handle_rsp_orders` / `handle_rsp_trades`：转换失败的行仍按 L01-014 上送原始结构，但不再 `return`；游标结束时总是投递最终回调。宿主声明 `WT_TRADER_SPI_FEATURE_QUERY_STATUS` 时委托查询走 `onRspOrdersWithStatus`（有丢弃行→`PARTIAL`，否则 `COMPLETE`），否则旧 `onRspOrders`；成交查询仍为旧 `onRspTrades`（K42 未版本化）。`src/TraderOES/TraderOES.h` 追加成员 `_qry_orders_dropped`。保留原混合行尾（cpp 主体 LF，头文件 CRLF）。
- 红控（改前产品）：`sh src/TestUnits/fixtures/i03_trader_oes_query_s4/run.sh SCRATCH/slice4-result-run/oes-red` exit 1，4/4 失败（最终回调计数 0），`logs/oes-red/`。
- 绿控（改后产品）：同命令输出到 `oes-green` exit 0，4/4 通过，`logs/oes-green/`；`TraderOES.cpp` 告警集合与改前完全相同（21 条，均为既有）。
- 新增用例（独立夹具，不在 CoverageTests 二进制中）：`OesQueryFixture.FailingLastOrderRowStillDeliversTheFinalCallback`、`StatusHostLearnsPartialSnapshotWhenARowWasDropped`、`FailingLastTradeRowStillDeliversTheFinalCallback`、`OnlyFailingRowsStillEndTheQuery`。
- 未覆盖：真实 OES SDK 的游标语义（失败行与 `isEnd` 的组合）、OES 查询失败的错误回调路径、Windows。`TraderOES` 仍不在 CMake 构建中，没有覆盖 lane。

## BUG-I02-NEW-032：只缺 sanitizer 运行（未改代码）

按主代理要求只列出 filter。用例本身在常规冻结/复修全量中通过；缺的是“无越界”的当前树插桩证据：

- `CoverageTests --gtest_filter='Q03RdmKlineTest.day_tail_fragment_is_dropped:Q03RdmKlineTest.day_dsb_raw_v2_full_range_roundtrip:Q03RdmKlineTest.day_damaged_block_leaves_no_cache_entry:Q03RdmKlineTest.day_size_check_failure_returns_null_and_logs_error'`，ASan+UBSan（含 alignment），期望 0 报告。
- 若 UBSan 开 alignment，同一二进制的 `Q03RdmTickTest.his_tick_old_struct_versions_are_upgraded:Q03ReaderFileTest.*` 依赖本段 I03-NEW-084 的夹具修正才能不在测试代码自身中止。

## I03-NEW-100：R41 线程池用例一次 SIGSEGV（只做代码审查，未改代码）

静态审查结论：未找到可确定的产品缺陷；以下是需在 ASan/TSan 下排除的候选。

- 用例 `R41EngineFixture.R41OnScheduleWithPoolCommitsLeftoverPositionToZero`（`src/TestUnits/coverage/n02_wtcore_engines/r41_engine_base_test.cpp`）在 `poolsize=2` 的 `WtCtaEngine` 上调用 `on_schedule`，随后引擎在函数结束时析构。
- 候选 1：`WtCtaEngine::on_schedule`（`src/WtCore/WtCtaEngine.cpp:421-428`）向引擎任务线程投递 `update_fund_dynprofit()` + `_adapter_mgr->refresh_funds()`，测试线程随即执行 `save_datas()`（`WtEngine.cpp` `save_datas` 读取 `_port_fund->fundInfo()` 与 `_pos_map`）。`update_fund_dynprofit` 只在遍历 `_pos_map` 时持 `_pos_mtx`，写 `WTSFundStruct` 字段不加锁，与 `save_datas` 读同一结构构成数据竞争（TSan 应可见；单靠它产生 SIGSEGV 的概率低）。
- 候选 2：`init` 创建的 `boost::threadpool::pool`（`WtCtaEngine.cpp:136`）在毫秒级寿命内创建、`wait()`、销毁；短寿命线程池的工作线程启动/退出与 `pool` 析构交错是已知的脆弱点。
- 已排除：`WtEngine::release()`（`WtEngine.cpp:1825` 起）在非任务线程上会 join 任务线程（`g_task_loop_engine` 为 `thread_local`，`WtEngine.cpp:172`）；R41 夹具的 generate 目录带进程号（`r41_wtcore_fixture.h:58`），并行进程不共享组合文件。
- 交主代理压测的 filter：`CoverageTests_wtcore --gtest_filter='R41EngineFixture.R41OnSchedule*:R90ExecPool.*' --gtest_repeat=50`，ASan+UBSan 与 TSan 各跑 12 路并行；期望：若出现报告，以栈定位候选 1/2 后再修；若 0 报告，记“未复现”并保留监测。

## I03-NEW-083：切片 create 超长代码按 WTSTickData::create 口径返回 NULL（产品修复）

- 修改：`src/Includes/WTSDataDef.hpp` `WTSKlineSlice::create`、`WTSTickSlice::create`：代码为 NULL 或长度 ≥ `MAX_INSTRUMENT_LENGTH`（放不下终止符）时返回 NULL，否则有界复制；`WTSKlineSlice::setCode`（旧 void 接口）对 NULL/超长输入保留原值。对象布局与 ABI 不变。
- 调用方核对（全部 `WTSKlineSlice::create(` / `WTSTickSlice::create(` 生产调用，静态逐个核对）：多数已对 NULL 判空或直接返回；4 处会解引用 NULL，已补 `if (slice == NULL) return NULL;`：`src/WtDataStorageAD/WtDataReaderAD.cpp` `readTickSlice`（tick 环形缓冲两段拼接分支，原 `ret->appendBlock` 无判空）、`src/WtDataStorage/WtDataReader.cpp` `readKlineSlice`（预建空切片）、`src/WtDataStorage/WtRdmDtReader.cpp` `readTickSliceByRange` 与 `readTickSliceByCount`（预建空切片）。`WtDtHelper.cpp:1606` 以空串创建，不受影响；`HisDataReplayer`、`WtDtMgr`、`WtDataManager`、`WtSimpDataMgr` 与 AD 读取器其余调用已判空或原样返回。
- 新增用例：`CoverageTests::S4SliceLongCode.*`（`src/TestUnits/coverage/r12_headers/s4_slice_long_code_test.cpp`，5 例：31 字节可用、32/63 字节与 NULL 返回 NULL、旧 setCode 保留原值）。
- 红控（改前产品）：覆盖率构建 `CoverageTests` 中 3 例失败——`KlineSliceRejectsCodeThatDoesNotFit` exit 134、`KlineSliceLegacySetCodeKeepsOldValueOnOverlongInput` exit 139、`TickSliceRejectsCodeThatDoesNotFit` exit 134（越界复制破坏对象后中止），`logs/red-main-083-082-084/`；独立单 TU 编译同样 3 例崩溃（`logs/s083-standalone-red/`）。
- 绿控：独立单 TU 5/5（`logs/s083-standalone-green/`）；覆盖率构建结果见下文“绿控汇总”。

## I03-NEW-084：q03 夹具不再对打包结构字段绑定引用（测试代码）

- 修改：`src/TestUnits/coverage/q03_storage3/q03_storage3_fixture.h` 的 `expect_bar_upgraded`、`expect_tick_upgraded`：`WTSBarStructOld`/`WTSTickStructOld` 为 `#pragma pack(1)`，调用方传入打包 vector 的元素，数值成员未对齐；gtest 比较宏以 const 引用绑定实参，对未对齐成员是未定义行为。改为按 `offsetof` 用 `memcpy` 逐字段按值读取后比较，比较的字段与期望值不变；`expect_bar_upgraded` 同类问题一并修正。
- 红/绿：只在 UBSan alignment 下可观察，常规构建无红控（补测）。常规构建中依赖这两个辅助函数的用例（`Q03RdmKlineTest.day_old_struct_versions_are_upgraded`、`Q03RdmTickTest.his_tick_old_struct_versions_are_upgraded`、`Q03ReaderFileTest.*`）改后通过，见“绿控汇总”。
- 交主代理：UBSan（`-fsanitize=alignment` 或完整 UBSan）`CoverageTests --gtest_filter='Q03RdmTickTest.his_tick_old_struct_versions_are_upgraded:Q03ReaderFileTest.*:Q03RdmKlineTest.day_old_struct_versions_are_upgraded'`，期望 0 报告（改前在测试自身中止）。

## I03-NEW-087：WtCore ParserAdapter 对无商品信息的合约不再解引用（产品修复）

- 修改：`src/WtCore/ParserAdapter.cpp`：`handleQuote` 与 `handleOrderQueue/handleOrderDetail/handleTransaction` 在 `cInfo->getCommInfo()` 为 NULL 时丢弃并记有界告警（每进程前 16 条，`warnContractWithoutCommodity`）；平码分支与三个 level2 分支对空标准代码直接返回（与分月分支一致）；`_bd_mgr` 为空（未初始化适配器）时同样丢弃，不再解引用（`n01_parser_adapter_test.cpp` 中 DISABLED 用例记录的同类崩溃，本次未启用该用例）。
- 新增用例：`CoverageTests_wtcore::S4ParserAdapterNullComm.*`（`src/TestUnits/coverage/n01_wtcore_tradepaths/s4_parser_adapter_null_comm_test.cpp`，4 例，每例同时有正常合约正控）。
- 红控（改前产品）：4/4 exit 139（SIGSEGV），`logs/red-wtcore-087-089/`。

## I03-NEW-089：执行器与 HFT 上下文在自身生命周期结束时归还认领（产品修复，人工认领未做）

- 修改：新增 `src/WtCore/OwnerClaimLease.h`（弱引用账户门禁，避免宿主先销毁非常驻适配器时悬空）。`WtLocalExecuter`、`WtArbiExecuter`、`HftStraBaseCtx` 各增加成员 `_owner_claims`，在 `setTrader` 时绑定当前适配器的 `accountGate()` 与 owner id；改绑到另一账户门禁或对象销毁时调用 `AccountTradingGate::releaseOwner(owner)`。旧 vtable 与导出不变（WtCore 内部类布局变化，需同树重编）。
- 未做：`WtDiffExecuter.{h,cpp}` 被 slice3-result 锁定（I03-NEW-072），Diff 执行器未接入；需主代理协调后按同一方式在 `WtDiffExecuter::setTrader` 绑定。人工（`manual`）账户级认领的释放方式未经裁定，未实现，控制用例 `ManualClaimIsNotReleasedByAnExecuterTeardown` 记录其现状——**待裁定**。
- 新增用例：`CoverageTests_wtcore::S4OwnerRelease.*`（`src/TestUnits/coverage/n01_wtcore_tradepaths/s4_owner_release_test.cpp`，HFT 销毁、HFT 改绑、Local 销毁、Arbi 销毁四例 + 人工认领控制例）。
- 红控（改前产品）：前 4 例 exit 1（断言“销毁/改绑后另一 owner 可交易该合约”失败），人工控制例 exit 0，`logs/red-wtcore-087-089/`。

## I03-NEW-091：CTPLoader/CTPOptLoader 配置错误返回非零（按 K02=B 作缺陷修复）

- 依据：用户裁定 K02=B（2026-11-04）“Loader 所有模式 `run()` 返回结果码、插件内不再 exit”，LoaderRunner 以 `run()` 返回值作为进程退出码。配置错误返回 0 使配置错误表现为成功，违反该裁定，故按缺陷修复，不另行裁定。
- 修改：`src/CTPLoader/CTPLoader.cpp` `run()`：`cfgfile` 为空、解析异常、校验失败、必填项缺失共 9 处 `return 0` 改为 `return EXIT_FAILURE`（模块/API 失败原已返回 `EXIT_FAILURE`）。`src/CTPOptLoader/CTPOptLoader.cpp` `run()`：5 处配置错误 `return 0` 改为 `return 1`（与该文件模块失败的返回值一致），并补 `cfgfile == NULL` 时返回 1（原实现会对 NULL 调用 `endsWith`）。
- 测试按合同改写（依据 K02=B，不弱化断言）：`t_CTPLoader/t6_ctploader_reachable_test.cpp` 的 `expect_rejected_without_mutation` 由 `EXPECT_EQ(0, result)` 改为 `EXPECT_EQ(EXIT_FAILURE, result)`（被 `t6_ctploader_run.*` 与 `t6_ctploader_config.*` 共 12 例使用），“不改全局状态”断言不变；`t_CTPOptLoader/ctpopt_loader_test.cpp` 11 处、`r114_optloader_test.cpp` 2 处配置错误断言由 0 改为 1，用例名中的 `returns_zero_without_touching_globals` 改为 `is_rejected_without_touching_globals`（7 个用例改名）。
- 红控（改前产品）：t_CTPLoader 12/12 exit 1；t_CTPOptLoader 改写的 13 例 exit 1、模块失败/成功 4 例 exit 0（未改），`logs/red-ctploader-091/`、`logs/red-ctpoptloader-091/`。

## I03-NEW-094：SEL/HFT ticker 与 WtEngine::handle_push_quote 的无合约 tick 回归用例（补测）

- 产品四处修复已在当前树；本次只补用例：`CoverageTests_wtcore::R66Fixture.S4SelTickerTickWithoutContractSkipsHotFanOut`、`S4HftTickerTickWithoutContractSkipsHotFanOut`、`S4EnginePushQuoteWithoutContractSkipsHotFanOut`（`src/TestUnits/coverage/q01_wtcore3/s4_ticker_null_contract_test.cpp`）：无合约信息的 tick 照常投递本代码、不扇出热合约、不崩溃；同代码带合约信息的 tick 作正控扇出到 HOT。三个实盘引擎都覆盖了 `handle_push_quote`（转给自身 ticker），基类修复点以 `engine.WtEngine::handle_push_quote(...)` 显式调用。
- 红控（SCRATCH 中把三处判空还原的变异体，`logs/red-variant-094/*.diff`；以 `review/s4work/relink.py` 用同一编译命令重编这几个 TU 并重新链接，不改仓库）：3/3 exit 139。
- 绿控（同法只换新测试 TU，产品为当前源码）：3/3 exit 0，`logs/green-test-only-094/`。

## BUG-I02-NEW-082：损坏日线块不在缓存留下条目（补测）

- 新增用例：`CoverageTests::Q03RdmKlineTest.day_damaged_block_leaves_no_cache_entry`（`q03_rdm_kline_file_test.cpp`）：带尾碎片的日线块读取得 NULL+ERROR；不 `clearCache` 再读仍被拒绝且再次报错（未命中缓存）；文件修复后不 `clearCache` 直接读到完整 K 线、逐字段一致。
- 当前产品通过（`logs/red-main-083-082-084/`）。红控（SCRATCH 变异体：普通合约历史块被拒时不删除缓存项，`logs/red-variant-082/WtRdmDtReader.082red.diff`）：新用例 exit 1（第二次读取未再报错、修复后仍读不到），而原 `day_tail_fragment_is_dropped` 在该变异体下仍 exit 0——证明新用例覆盖了原用例覆盖不到的缓存污染。
- sanitizer：见 BUG-I02-NEW-032 一节。

## BUG-I02-NEW-096：回放后两只非主合约都重新查询（补测）

- 修改：`src/TestUnits/coverage/p03_btcore_mockers2/r180_btcore_mockers2_test.cpp` `R180His_CacheClearJoinsMultipleCodes` 末尾追加：A、B 都重新查询并 `ASSERT_NE(nullptr, …)`，loader 调用次数仍为 1。原断言不变。
- 当前产品通过（`logs/pre-btcore-096/`）。红控（SCRATCH 中该用例的 `cache_clear_days` 由 2 改 1，会话开始即淘汰，`logs/red-variant-096/r180.096red.diff`）：exit 1；失败点包括原有 A 计数断言与新增 `ASSERT_NE(nullptr, reloadA)`，B 的新断言与 A 对称，未单独构造“只淘汰 B”的变异体。

## BUG-I02-NEW-155：K26 完整生命周期用例（补测，SEL 与 CTA）

- 新增用例：`CoverageTests_wtcore::S4K26Lifecycle.*`（`src/TestUnits/coverage/q01_wtcore3/s4_k26_lifecycle_test.cpp`，5 例）：引擎经 `init()`（基础数据、热合约管理器、数据管理器、记录型执行器）——①SEL/CTA 无价 +2/+3 挂起目标为 5、执行器收到两次增量、组合未记账；有效价格 tick 到来后组合恰好记 5 且挂起清空，之后有价增量直接记账；②SEL/CTA HOT 映射与换月（B2 现状）：旧合约挂起目标保留并在旧合约上执行，换月后增量进入新合约；③SEL ticker strand：另一线程的 tick 在 strand 工作（模拟 K25 schedule 中的组合增量）期间等待，结束后只应用一次累计目标，两者不重叠。
- 当前产品 5/5 通过（`logs/pre-wtcore-094-155-008/`）。红控：`WtSelEngine.cpp`/`WtCtaEngine.cpp` 被 slice1-result 锁定，不能临时还原，改用 SCRATCH 变异体（挂起目标基准还原为持仓，即 K26 前行为，`logs/red-variant-155-k26/*.diff`）重编链接：B1 相关 3 例与既有 `S5MainK26Sel` exit 1，B2 两例 exit 0（B2 维持现状，K26 前后相同）。
- 未覆盖：真实 SEL/CTA 策略插件、运行中的 ticker worker 定时闭合分钟触发 schedule 的完整线程时序（本例以未启动 ticker 的 strand 入口模拟）。

## BUG-I02-NEW-008：两个真实 Diff 执行器共享同一 TraderAdapter 的 owner 路由端到端（补测）

- 新增用例：`CoverageTests_wtcore::S4TwoDiffOwnerRouting.*`（`src/TestUnits/coverage/n01_wtcore_tradepaths/s4_two_diff_owner_routing_test.cpp`，3 例），真实 `WtDiffExecuter` ×2 + 真实 `WtExeFact.WtDiffMinImpactExeUnit`（声明主机订单身份能力）+ 本地 fake API，同一合约 `SHFE.ag.2512`：
  - ALL 路由：两执行器各收到增量、各自下单（卡5c 同账户同合约共享）；订单回报只进入发单者的 `getOrders` 快照；A 的成交只改 A 的差量（B 不变，即 2026-10-31 探针 3/5→2/4 串扰不再发生）；A 的 `cancel(code,isBuy)`（StockVWap 等单元使用的账户级撤单接口）只撤 A 的单，B 的同合约单保持；B 的委托失败只释放 B 的预占；owner 无法解析的成交（本适配器标签、无账本项）两边账本都不变并暂停账户（卡6/F4）。
  - 缓存目标：`add_target_to_cache` 一条显式（A）一条 ALL，`commit_cached_targets` 后 A 用显式项、B 用 ALL 项；B 的成交不进入 A。
  - 对照：owner 路由关闭（缺省）时 A 的成交仍广播进 B 的差量（K44 保留的旧口径），记录改前现象。
- 当前产品 3/3 通过（`logs/pre-wtcore-094-155-008/`）。产品早已实现（F1–F4、K44），本项是补测；路由关闭对照例即 2026-10-31 的红控现象。
- 未覆盖：真实 `WtStockVWapExeUnit` 在 Local 执行器上的端到端（需股票合约、VWAP 曲线与 T+1 夹具），本例验证的是该单元所用的 `ExecuteContext::getOrders/cancel(code,isBuy)` 在 owner 路由下的语义；真实 provider 保留 usertag/成交号的能力仍只由配置声明（外部）。

## BUG-I02-NEW-154：K42 带状态订单查询迁移（只完成 TraderXTP）

- 锁冲突：`src/TraderCTPMini/TraderCTPMini.cpp`、`src/TraderCTPOpt/TraderCTPOpt.cpp`、`src/TraderFemas/TraderFemas.cpp`、`src/TraderYD/TraderYD.cpp` 被 slice1-result（BUG-P02-005-card8）锁定，因文件被 slice1-result 锁定未做，需主代理协调。
- 修改：`src/TraderXTP/TraderXTP.cpp` `OnQueryOrder`、`src/TraderXTP/TraderXTP.h`（追加 `_qry_orders_failed`、`_qry_orders_dropped`）：宿主声明 `WT_TRADER_SPI_FEATURE_QUERY_STATUS` 后，末包按 COMPLETE（含空结果，空数组非 NULL）/ PARTIAL（有行无法转换）/ FAILED（带错误）交付 `onRspOrdersWithStatus`，下一次查询状态清零；未声明能力的宿主仍只收到旧 `onRspOrders`。XTP 头文件未约定委托查询的“无记录”错误码（仅其他查询注明 11000350），按卡14c 无契约前 fail-closed，任何错误判 FAILED。
- 新增用例：`CoverageTests_t_TraderXTP::TraderXTPTest.k42_order_query_reports_completeness_when_host_supports_it`（`t5_xtp_api_test.cpp`，测试 sink 增加 `onRspOrdersWithStatus` 记录）。
- 红控（改前产品）：exit 1（未收到带状态回调），`logs/red-xtp-154/`；既有 `query_order_accumulates_then_publishes_on_last` 改前通过。

## 绿控汇总（改后产品 + 新/改用例，构建 `logs/build-03-green.log`）

构建：`cmake --build SCRATCH/slice4-result-build --target CoverageTests_wtcore CoverageTests CoverageTests_btcore CoverageTests_t_CTPLoader CoverageTests_t_CTPLoaderYamlPositive CoverageTests_t_CTPOptLoader CoverageTests_t_TraderXTP CoverageTests_adstorage -j 3`，exit 0，告警 0（默认 `-Werror`）。每例单独进程、单独工作目录（`review/s4work/runcases.sh`，120 秒上限），XML/日志在 `logs/<标签>/`。

| 标签 | 范围 | 结果 |
|---|---|---|
| `green-wtcore-new` | 本段新增 wtcore 用例：`S4ParserAdapterNullComm.*`、`S4OwnerRelease.*`、`R66Fixture.S4*`、`S4K26Lifecycle.*`、`S4TwoDiffOwnerRouting.*` | 20/20 |
| `green-main-083-082-084` | `CoverageTests`：`S4SliceLongCode.*`、`Q03*`、`R1DataDefTest.*`、`R122*`、`R162*`、`R193*` | 82/82 |
| `green-adstorage-083` | `CoverageTests_adstorage` 全部（`WtDataReaderAD` 改动） | 311/311 |
| `green-btcore-096` | `R180LoaderFixture.*`、`R180ReplayFixture.*`、`R210Fixture.*` | 16/16 |
| `green-ctploader-091` / `green-ctploaderyaml-091` / `green-ctpoptloader-091` | 三个 loader 二进制全部 | 67/67、1/1、43/43 |
| `green-xtp-154` | `CoverageTests_t_TraderXTP` 全部 | 61/61 |
| `green-btcore-full` | `CoverageTests_btcore` 全部（`WTSKlineSlice/WTSTickSlice::create`、`setCode` 被回放器使用） | 721 例中 716 通过；5 例 `R111ExecMocker.*` 缺 `exec_v2_plugins` 假模块（本构建未建 `CoverageTests_execv2`，环境），建该目标后 `R111ExecMocker.*` 16/16（`green-btcore-r111-with-execv2`、`build-04-execv2-fixtures.log`） |
| `green-wtcore-related` | wtcore 相关既有套件（`N01ParserAdapter*`、`R110*`、`R42*`、`R71ExecTest.*`、`S5MainK44*`、`R15*`、`R66Fixture.*`、`S5MainK25/K26*`、`DiffOwnerSink*`、`R59*`、`P11*`、`R10ExecuterTest.*`、`R41*`、`R90ExecPool.*`、`r31_stra_mgr.*`、`R192*`、`R211*`、`WtDiffExecuterConcurrencyTest.*`、`R220*`、`R190*`） | 380 例中 378 通过；2 例 `R59Exec.diff_stale_target_*` 因未设 `WT_COV_PLUGIN_BIN` 找不到插件目录而失败（环境，回归工具会提供），设置后 2/2 通过（`green-wtcore-r59exec-with-plugin-bin`） |

另：OES 夹具 4/4、HFT 夹具 26/26、独立单 TU `S4SliceLongCode` 5/5（见各节）。未运行：`CoverageTests_porter`/`m_WtRunner` 等含 `ExpHftContext`、`WtRtRunner` 的二进制（`HftStraBaseCtx`、`WtLocalExecuter`、`WtArbiExecuter` 布局变化需同树重编，由主代理最终冻结构建覆盖）、全部 wtcore/main 二进制全量。

## 交主代理的 sanitizer 运行

| binary | gtest filter | 期望 |
|---|---|---|
| `CoverageTests` | `S4SliceLongCode.*:Q03RdmKlineTest.day_*:Q03RdmTickTest.his_tick_old_struct_versions_are_upgraded:Q03ReaderFileTest.*` | ASan+UBSan（含 alignment）0 报告（I03-NEW-083、BUG-I02-NEW-032/082、I03-NEW-084） |
| `CoverageTests_wtcore` | `S4ParserAdapterNullComm.*:S4OwnerRelease.*:R66Fixture.S4*:S4TwoDiffOwnerRouting.*` | ASan+UBSan 0 报告；`S4OwnerRelease.*` 另在 TSan 下 0 报告（OwnerClaimLease 的互斥与弱引用） |
| `CoverageTests_wtcore` | `S4K26Lifecycle.SelTickFromAnotherThreadWaitsForStrandWorkThenAppliesOnce` | TSan 0 报告（strand 跨线程交错） |
| `CoverageTests_wtcore` | `R41EngineFixture.R41OnSchedule*:R90ExecPool.*`，`--gtest_repeat=50`，12 路并行 | ASan+UBSan、TSan 定位或排除 I03-NEW-100 |
| `CoverageTests_adstorage` | `R4ReaderADTest.*` | ASan 0 报告（`readTickSlice` 新判空分支） |
| `CoverageTests_t_TraderXTP` | `TraderXTPTest.k42_*:TraderXTPTest.query_*` | ASan 0 报告 |
| 夹具 `i03_trader_oes_query_s4` | 全部 4 例 | 可按 README 用 LLVM 22 加 `-fsanitize=address,undefined` 重编运行，期望 0 报告 |

## 未完成与待协调

- BUG-I02-NEW-154：CTPMini/CTPOpt/Femas/YD 迁移因文件被 slice1-result 锁定未做。（第三轮锁转交后已完成，见文末“BUG-I02-NEW-154（续）”。）
- I03-NEW-089：`WtDiffExecuter` 接入因文件被 slice3-result 锁定未做；人工（manual）认领释放待裁定。（第三轮已接入 Diff，见文末；人工认领仍待裁定。）
- BUG-I02-NEW-008：真实 `WtStockVWapExeUnit` 端到端未做（见该节）。（第三轮补了通道就绪对账路径，见文末；发单路径仍未做。）
- I03-NEW-100：只做了代码审查，压测待主代理。
- BUG-I02-NEW-032：只列出 sanitizer filter。
- 本段未新发现需另立编号的问题（`R59Exec.diff_stale_target_*` 对 `WT_COV_PLUGIN_BIN` 的依赖与 I03-NEW-105 同类，但回归工具已提供该变量，不另立）。

---

# 第三轮（锁转交后的续做，2026-11-05）

协调者把 `src/WtCore/WtDiffExecuter.{h,cpp}`、`src/TraderCTPMini|TraderCTPOpt|TraderFemas|TraderYD` 四个 `.cpp` 及其 lane 用例的锁转给 slice4-result；本轮另登记 `TraderCTPMini.h`、`TraderCTPOpt.h`、`TraderFemas.h`、`TraderYD.h`、`src/CTPLoader/TraderSpi.{h,cpp}` 与新用例文件（`lock.py` 均返回 ok）。改前快照：`before/`（同名覆盖前已存在的不重拍；本轮中间态另存 `before/*.pre-I03-NEW-123`、`before/*.pre-R132`，即含第二轮 I03-NEW-083/091 改动、不含本轮改动的版本）。slice1 的卡8 整手拒绝与 slice3 的 I03-NEW-072 非阻塞回调排队均在改前快照内，本轮只做局部替换（`review/s4work/edit.py`，保留各区域行尾），未覆盖。构建：同一 `SCRATCH/slice4-result-build`，`-j 3`，默认 `-Werror`。

## BUG-I02-NEW-154（续）：CTPMini / CTPOpt / Femas / YD 订单查询迁移到带状态回调（产品修复）

- 修改：
  - `src/TraderCTPMini/TraderCTPMini.cpp`、`src/TraderCTPOpt/TraderCTPOpt.cpp`、`src/TraderFemas/TraderFemas.cpp` 的 `OnRspQryOrder`，以及三个头文件。头文件的 `protected` 段新增 `m_qryOrdersFailed`、`m_qryOrdersDropped`，做法与 `TraderCTP` 一致：
    - 本次查询出现任一错误帧，判 FAILED；
    - 有行无法转换（原始回报仍按 L01-014 上送），判 PARTIAL；
    - 否则判 COMPLETE。空结果交付空数组，不交付 NULL。
    - 宿主声明了 `WT_TRADER_SPI_FEATURE_QUERY_STATUS` 才走 `onRspOrdersWithStatus`；没有声明的宿主仍只收到旧 `onRspOrders`。
    - 末帧之后两个标志清零。
  - `src/TraderYD/TraderYD.cpp`、`TraderYD.h`：
    - 新增 `m_bOrderCacheDropped`。订单推送转换失败时置位；release 清缓存时复位。
    - `queryOrders` 的投递体：宿主声明能力后，只有 `update_cache` 打开且本会话没有丢弃过订单回报时判 COMPLETE，其余一律 PARTIAL。
    - 无 sink 时直接返回，临时数组照常释放。
- 判定依据（只读 SDK 头文件）：
  - CTP 系三家都没有“无记录”错误码。`CTPMini1.5.8/ThostFtdcTraderApi.h`、`CTPOpt3.5.8/ThostFtdcTraderApi.h` 与 `CTPOpt3.5.8/error.xml`（只有出入金的 `DEPOSIT_NOT_FOUND`），以及 `Femas3.02/USTPFtdcTraderApi.h`，都没有为报单查询约定“无记录”错误码；空结果就是一条不带行的末帧。因此按 XTP/CTP 的做法和卡14c 保守处理：任何错误帧都判 FAILED。
  - YD 没有可依据的查询结果。`yd1.108.360/ydApi.h` 没有报单查询，快照来自推送建的本地缓存。`update_cache=false` 时，追上后缓存不再更新（`notifyOrder` 的 `!m_bCatchup || m_bUpdateCache`）。而宿主 `TraderAdapter::reconcileOrders` 会把 COMPLETE 快照里缺席的活动单记成 Absent 墓碑，所以冻结或缺单的缓存绝不能报 COMPLETE。YD 不产生 FAILED，因为不存在失败的查询。
- slice1 卡8 的整手拒绝改动在改前快照中，本项只替换查询回调区段，未触及。
- 新增/改用例：
  - `CoverageTests_t_TraderCTPMini::S4CTPMiniQueryStatus.k42_order_query_reports_completeness_when_host_supports_it`（新文件 `t_TraderCTPMini/s4_ctpmini_query_status_test.cpp`）。
  - `CoverageTests_t_TraderCTPOpt::S4CTPOptQueryStatus.k42_order_query_reports_completeness_when_host_supports_it`（`t_TraderCTPOpt/s4_ctpopt_query_status_test.cpp`）。
  - `CoverageTests_t_TraderFemas::S4FemasQueryStatus.k42_order_query_reports_completeness_when_host_supports_it`（`t_TraderFemas/s4_femas_query_status_test.cpp`）。
  - 以上三例覆盖：旧宿主走旧回调；新宿主 COMPLETE；空结果为 COMPLETE 且是空数组；丢行判 PARTIAL；中途错误帧判 FAILED；下一次查询从干净状态开始。均不创建厂商 API。
  - `CoverageTests_t_TraderYD::R93YdTest.k42_cached_order_query_reports_completeness_when_host_supports_it`、`R93YdTest.k42_cache_frozen_after_catch_up_is_never_reported_complete`（`t_TraderYD/r93_traderYD_api_test.cpp`，测试 sink 增加带状态回调记录，探针增加 `setUpdateCache`）。
- 红控：用 `before/` 的改前 `.cpp` 重新编译并重链（`relink.py`，`logs/phase3-red-relink-154-123.log`）。
  - `red-ctpmini-154`、`red-ctpopt-154`、`red-femas-154`：各 1/1 exit 1，均因没有收到带状态回调。
  - `red-yd-154`：两例 k42 exit 1；既有 `cache_roundtrip_marks_orders_and_trades_and_queries_arrays` 在改前 exit 0，作为对照。
- 绿控：`green-ctpmini-154`、`green-ctpopt-154`、`green-femas-154` 各 1/1，`green-yd-154` 2/2。整条 lane 逐例运行（`green-full-*`）：CTPMini 57/57、CTPOpt 58/58、Femas 80/80、YD 58/58。

## I03-NEW-089（续）：WtDiffExecuter 销毁或换绑时归还认领（产品修复，人工认领仍未做）

- 修改：
  - `src/WtCore/WtDiffExecuter.h`：`#include "OwnerClaimLease.h"`，并新增成员 `OwnerClaimLease _owner_claims`，放在 `_owner_modes` 之后。
  - `src/WtCore/WtDiffExecuter.cpp` 的 `setTrader`：在 `declareOwnerSink` 之前，用 `ExecOwnerModes::ownerId(name())` 绑定新交易适配器的 `accountGate()`。换绑时归还上一个 gate 上的认领；析构时成员 lease 归还。
  - slice3 的 I03-NEW-072 非阻塞回调排队代码未改动。
- 新增用例：`CoverageTests_wtcore::S4OwnerRelease.DiffExecuterReturnsItsClaimsWhenDestroyed`、`S4OwnerRelease.DiffExecuterReturnsClaimsOfThePreviousTraderWhenRebound`（`n01_wtcore_tradepaths/s4_owner_release_test.cpp`）。执行器由 `WtExecuterMgr` 持有，quiesce 完成后才随 mgr 析构。
- 红控：变体 `logs/mutants-phase3/WtDiffExecuter.089red.cpp`（只删除 bind 调用）重链为 `red-wtcore-089-diff`。两例 Diff 用例 exit 1，第 196、214 行显示其他 owner 仍被阻塞；同进程的 `LocalExecuterReturnsItsClaimsWhenDestroyed` exit 0 作为对照。
- 绿控：`green-wtcore-089-008`，`S4OwnerRelease.*` 7/7，其中包括人工认领对照 `ManualClaimIsNotReleasedByAnExecuterTeardown`。
- 人工（manual）认领的释放仍待裁定，未实现。

## I03-NEW-123（slice2 发现）：CTPLoader 先停 worker 再 Release（产品修复）

- 问题：`run()` 原来先执行 `RegisterSpi(NULL)`、`Release()` 和 `pUserApi = NULL`，然后才 `WaitWorkersStopped`。已提交的查询任务在锁外调用 `pUserApi->ReqQry*`，可能落在已释放或已置空的 API 上。
- 修改：
  - `src/CTPLoader/CTPLoader.cpp`：抽出 `ctp_loader_release(spi, timeout)`，在 `src/CTPLoader/TraderSpi.h` 声明。
    - 先 `WaitWorkersStopped(timeout)`。
    - worker 在时限内退出，才执行 `RegisterSpi(NULL)`、`Release()`、`pUserApi = NULL` 和 `delete spi`，并返回 true。
    - 仍有线程忙（例如阻塞在 SDK 调用里）时，API 与 SPI 都不动，保留到进程退出（卡9a），返回 false。
    - `run()` 以 5 秒时限调用它。
  - I03-NEW-091 的返回码改动不受影响。`TraderSpi.cpp` 已登记锁，但未修改。
- 行为差异：worker 超时未停时，改前仍会 Release，即在 worker 底下释放 API；改后不 Release，保留到进程退出。这一行为与卡9a 一致。
- 新增用例：`CoverageTests_t_CTPLoader::S4CTPLoaderReleaseOrder.workers_are_stopped_before_the_api_is_released`、`S4CTPLoaderReleaseOrder.a_worker_still_busy_after_the_bound_keeps_the_api_alive`（新文件 `t_CTPLoader/s4_ctploader_release_order_test.cpp`）。
  - 本地桩 `S4HoldingApi` 经真实 SPI 登录路径让查询 worker 停在 `ReqQryInstrument` 调用里，等待有界：300 ms 或 1.5 s。期间若有 `Release()` 或 `RegisterSpi(NULL)` 到达，桩会记录下来。
  - 参考了 slice2 在 `t_CTPLoaderYamlPositive` 中给 `T26CtpApiStub` 加的有界等待。
- 红控：改前代码没有可调用的接缝，所以把改前 `run()` 的原样顺序搬进同名函数，作为变体 `logs/mutants-phase3/CTPLoader.123red.cpp`，重链为 `red-ctploader-123`。
  - 两例都 exit 1：Release 与 `RegisterSpi(NULL)` 发生在调用期间；忙 worker 情况下 API 被释放。
  - 第二例在红控下 SPI 已被删除，用例不再触碰它，没有 UAF。
- 绿控：`green-ctploader-123` 2/2；`green-full-t_CTPLoader` 69/69；`green-full-t_CTPLoaderYamlPositive` 1/1（真实 `run()` 端到端，使用 slice2 的桩）。

## BUG-R132-001 剩余部分：实盘读端与数据服务读端读取解压预算（产品修复）

- 按 slice2 方案 `../20261105-final-gaps-slice2-result/pending-patch-R132-locked-readers.md` 落地，复用 `src/WTSUtils/WTSCmpBudgetConfig.hpp`：
  - `src/WtDataStorage/WtDataReader.cpp`：引入头文件；`init` 在 `cfg == NULL` 判断之后、读 `path` 之前调用 `wt_cmp_budget::apply`，非法值记 ERROR（`pipe_reader_log`）。
  - `src/WtDataStorage/WtRdmDtReader.cpp`：同样插入，日志用 `pipe_rdmreader_log(_sink, ...)`。
  - 第二轮 I03-NEW-083 的判空改动保留不变。合并前的中间态见 `before/*.pre-R132`。两个文件都是 CRLF，行尾已保持。
- 说明：预算是本模块的全局量（K37=B，见 WTSCmpBudgetConfig.hpp 的说明）。同一 WtDataStorage 模块内多个读端各自 init 时，后执行的配置生效。
- 新增用例：`CoverageTests::S4R132LiveReaderBudget.*`，共 6 例，新文件 `l04_datastorage/s4_r132_live_reader_budget_test.cpp`，复用 q03 夹具。
  - WtDataReader 的 `readKlineSlice`（历史日线 CMP_V2 块）：缺省预算可读；`uncompress_max_ratio=5` 拒绝压缩块（ERROR），RAW 块仍可读；`-5` 非法时记 ERROR（点名键），fail-closed，RAW 块仍可读。
  - WtRdmDtReader 的 `readKlineSliceByRange`：缺省可读；`uncompress_max_bytes="4096"`（YAML 数字串）拒绝；`"1e9"` 非法时 fail-closed。
- 红控：用改前（含 083）源 `before/src__WtDataStorage__WtDataReader.cpp.pre-R132`、`before/src__WtDataStorage__WtRdmDtReader.cpp.pre-R132` 编译。为让编译器按 C++ 识别，先原样复制为 `.cpp` 扩展名（哈希不变：e302fcdb…、f4203f12…），再重链为 `red-main-r132`（`logs/phase3-red-relink-089-r132.log`）。第一次 relink 因扩展名未被识别而 exit 1，日志保留。
  - 4 个配置类用例 exit 1，因为配置被忽略。
  - 2 个缺省用例 exit 0，作为正控。
  - 同一红控二进制中 `I03R132BtReaderBudget.*` 4/4 通过，回测读端不受影响。
- 绿控：`green-main-r132` 6/6。

## BUG-I02-NEW-008（续）：真实 WtStockVWapExeUnit 端到端（补测，部分路径）

- 新增用例：`CoverageTests_wtcore::S4StockVWapOwnerE2E.*`，共 2 例，新文件 `n01_wtcore_tradepaths/s4_stock_vwap_owner_e2e_test.cpp`。组成如下：
  - 真实 `WtExeFact.WtStockVWapExeUnit`，通过 `WtExecuterFactory` V2 加载 libWtExeFact 副本；
  - 真实 `WtLocalExecuter`；
  - `TraderAdapter` 加本地 fake API，不接厂商；
  - 真实 VWAP v2 曲线文件 `Vwap_STK.txt`，与 SSE.STK 交易时段一致。
- 覆盖的是 StockVWap 在通道就绪时的对账路径：用账户级同合约快照 `getUndoneQty` 判断“未受管挂单”，并按 code/side 调 `cancel(code,isBuy)`。这正是任务清单第 752 行指向 008 的风险点。
  - owner 路由开启时：另一 owner 在同合约同方向有 100 股活动单，本单元的快照只看自身 owner 范围，结果为 0。不记“unmanaged orders”，不发撤单，另一单保持活动。执行器级 `cancel(code,true,0)` 也撤不到它。
  - 对照（K44 保留的旧口径，不是本项红例）：路由关闭时快照是账户级（100），单元把它当作未受管挂单记日志；owner 校验的撤单仍不会撤到这笔单（`cancelAdmissibleLocked`）。
- 结果：`green-wtcore-089-008` 2/2。产品早已实现（F1–F4、K44），本项是补测，没有改前红例。
- 仍未覆盖：StockVWap 的 `do_calc` 发单路径（行情时间桶、价格模式、T+1 卖出）没有驱动。原因是该路径的时间与行情门槛很多，本机负载高（load 40–50），每轮 wtcore 重链要 10 分钟以上，本轮来不及逐项调通。“单元自身委托只出现在本 owner 快照”这一点，目前只由 Diff 端到端用例覆盖（同一 `ExecuteContext::getOrders/cancel` 语义）。

## 第三轮相关套件回归（逐例单独进程）

| 标签 | 范围 | 结果 |
|---|---|---|
| `green-phase3-wtcore-related` | `R71ExecTest.*`、`R71ExecScheduleTest.*`、`S3New072DiffCallbacks.*`、`S4OwnerRelease.*`、`S4StockVWapOwnerE2E.*`、`S4TwoDiffOwnerRouting.*`、`S5MainK44OwnerParticipation.*`、`DiffOrderOwnershipJournalV1.*`、`DiffOwnerSink*`、`WtDiffExecuterConcurrencyTest.*`、`R120DiffTest.*`、`P02DiffExecuterTest.*`、`R59*`、`I03New014DiffConfig.*`、`R10ExecuterTest.*`、`R130DiffExecuterTail.*`、`R7VwapV2LocalIntegration.*`；设置了 `WT_COV_PLUGIN_BIN=<build>/build_x64/Debug/bin` | 159/159 |
| `green-phase3-main-readers` | `CoverageTests` 读端与预算相关全部套件：`I03R132*`、`S5MainK37*`、`L04BtReaderTest`、`S4R132*`、`P09*`、`I03New038*`、`I03S2R100*`、`Q03*`、`R100R*`、`R112R*`、`R122R*`、`R132R*`、`R142R*`、`R162*`、`R172R*`、`R182R*`、`R193*`、`R203*`、`R212*`、`ReaderHistorical*`、`R52R*`、`R70R*`、`R74R*`、`R94R*`、`S5MainK36*`、`S5StorageReaderTest`、`R195*`、`R1RdmHftTest`、`R1ReaderBarsTest`、`R23Rdm*` | 339/339 |
| `green-full-*` | `t_TraderCTPMini`、`t_TraderCTPOpt`、`t_TraderFemas`、`t_TraderYD`、`t_CTPLoader`、`t_CTPLoaderYamlPositive` 全部 | 57/57、58/58、80/80、58/58、69/69、1/1 |

构建日志：
- `logs/phase3-reconfigure.log`；
- `logs/phase3-green-build-adapters.log`（exit 0）；
- `logs/phase3-green-build-wtcore-main.log`：exit 1，因为新用例里误用了私有 `WTSVariant::create`。修正后见 `-2.log`（exit 0），读端用例 sink 改为记录全部 ERROR 后见 `-3.log`（exit 0）；
- `logs/phase3-product-modules-build.log`：产品模块 `WtCore`、`WtDataStorage`、`TraderCTPMini`、`TraderCTPOpt`、`TraderFemas`、`TraderYD`、`CTPLoader` 已是最新，exit 0。

以上构建在默认 `-Werror` 下告警均为 0。各标签的 `binary.sha256` 在对应 `logs/<标签>/` 目录中；HEAD 见 `HEAD-phase3.txt`（`ccaad010`）；改后快照在 `after/`，哈希在 `sources.sha256`。

未运行：`CoverageTests_porter`、`m_WtRunner` 等含 `WtDiffExecuter` 的其他二进制。`WtDiffExecuter` 新增成员后对象布局变化，需要同树重编，由主代理的最终冻结构建覆盖。全量回归也未运行。

## 第三轮补充的 sanitizer 运行（交主代理）

| binary | gtest filter | 期望 |
|---|---|---|
| `CoverageTests_t_CTPLoader` | `S4CTPLoaderReleaseOrder.*` | TSan 0 报告（worker 与 teardown 的先后关系）；ASan 0 报告 |
| `CoverageTests_t_CTPLoaderYamlPositive` | 全部 | TSan 0 报告（`run()` 新顺序端到端） |
| `CoverageTests_wtcore` | `S4OwnerRelease.Diff*:S4StockVWapOwnerE2E.*` | ASan+UBSan 0 报告 |
| `CoverageTests` | `S4R132LiveReaderBudget.*` | ASan+UBSan 0 报告 |
| `CoverageTests_t_TraderCTPMini` / `_t_TraderCTPOpt` / `_t_TraderFemas` | `S4*QueryStatus.*` | ASan 0 报告 |
| `CoverageTests_t_TraderYD` | `R93YdTest.k42_*:R93YdTest.cache_roundtrip*` | ASan 0 报告；TSan 下 `m_bOrderCacheDropped` 为原子量，期望 0 报告 |

## 第三轮后的未完成与待协调

- I03-NEW-089：人工（manual）认领的释放待裁定，未实现。
- BUG-I02-NEW-008：StockVWap 的 `do_calc` 发单路径端到端未做，原因见上。（第四轮已补，见下节。）
- 编号说明：协调者给的 I03-NEW-123 与基线中的 BUG-I02-NEW-123 前缀不同，不冲突，本文按 I03-NEW-123 记录。
- 本轮没有新发现需要另立编号的问题。

---

# 第四轮（协调者追加：BUG-I02-NEW-008 的 StockVWap 自身下单路径，2026-11-05，机器负载约 2–6）

## BUG-I02-NEW-008（续二）：两个真实 StockVWap 执行器在同一 TraderAdapter、同一合约上的 do_calc 端到端（补测）

- 本轮只改测试，未改产品。改的文件是 `n01_wtcore_tradepaths/s4_stock_vwap_owner_e2e_test.cpp`：
  - 原有两例保留，不改断言；
  - 改前快照：`before/src__TestUnits__coverage__n01_wtcore_tradepaths__s4_stock_vwap_owner_e2e_test.cpp.pre-docalc`（29747938…）；
  - 改后：`after/` 同名文件（07d40461…）。
- 夹具只做测试侧补充，没有降低任何产品门槛：
  - 可控执行器时钟 `S4ClockStub`（派生 R71ExecStub，`getCurTime` 随 tick 时间走）；
  - 累积型 VWAP v2 曲线，每个交易分钟累计 100 股，与 SSE.STK 时段 930–1130/1300–1500 一致；
  - 受控 tick，使用真实 `r71::makeTick`；
  - fake API 记录 usertag、entrust id、价格和数量；
  - 订单回报按 provider 回显 usertag 与 entrust id；
  - RAII 收尾 `S4ExecTeardown`：任何退出路径都先 removeSink 并 quiesce，再销毁执行器。
- 产品门槛及其在用例中的满足方式：
  - **会话、时间窗、时间桶**：tick 时间 09:31:00.000 落在交易时段，也落在配置的 930–1500 窗口内，对应桶 1；曲线值个数为交易分钟数加 1。
  - **tick 新鲜度与高水位**：每次都送入更新的 tick。
  - **发单间隔 fire span**：`total_times=240`，即每 60 秒一个切片；第二次计算的 tick 与时钟推进到 09:32:00，满足 `now - _last_fire_time >= fire_span*1000`。
  - **股票整手**：`minopenlots=100`，桶 1 的切片量为 100 股。
  - **通道就绪**：执行器调用 `on_channel_ready`。
  - **订单快照非 NULL**：登录订单查询走 K42 COMPLETE 空快照（`onRspOrdersWithStatus(空数组, COMPLETE)`），这正是已迁移 provider 对空账户的回报。
- 用例 `CoverageTests_wtcore::S4StockVWapOwnerE2E.TwoStockVWapExecutersSubmitReceiveAndCancelOnlyUnderTheirOwnOwner` 的流程：owner 路由开启；两个真实 StockVWap 执行器 `s4_vw_a`、`s4_vw_b`，均为 Local；同一 TraderAdapter，同一 `SSE.STK.600000`，同一方向；两者都按 WtRunner 的方式 addSink。
  1. **B 下单**：B 收到首个时段内 tick 后由 do_calc 自行发单，100 股、10.0（price_mode 0）。
  2. **A 下单**：此时 B 的活动单既不在 A 的 gross 订单扫描里，也不在 A 的未完成量里，所以 A 不被阻塞，自行发单。usertag 不同，各自的 owner 快照里各 1 单；账户未完成量为 200。
  3. **A 撤单**：A 的目标降到 50 股，下一切片（09:32）时 A 的 100 股活动单超出剩余量，A 经 `cancel(code,isBuy)` 撤单。发到 provider 的只有 A 自己的 entrust id，撤单未回报前不再发新单。撤单回报只到 A 的单元，B 的同合约单保持活动。
  4. **B 成交**：成交与全成回报只进入 B 的单元（日志 traded / has filled），A 的单元看不到；B 的未完成量归零。全程只有 1 次撤单。
- 对照用例（不计红例）：
  - `WithoutOwnerRoutingAnotherExecutersOrderHoldsTheStockVWap`：记录 K44 保留的旧口径。路由关闭时，A 的账户级快照把 B 的同合约单当成自己的未完成单，A 被挂起不发单，也就是审计所说的“持续阻塞”现象。
  - `WithoutAnOrderSnapshotTheUnitFailsClosedBeforeItsFirstOrder`：门槛说明。登录订单查询若是旧接口的 NULL（未迁移 provider 对空账户的回报），adapter 没有订单表，`getOrders` 返回 NULL，单元按设计 fail-closed（日志“getOrders returned null”），首单发不出。K42 COMPLETE 空快照可满足这个门槛。这不是本项缺陷，但未迁移 K42 的 provider 在空账户上会使 StockVWap 一直等到出现订单回报，提请主代理知悉，未另立编号。
- 红控：变体 `logs/mutants-phase3/WtLocalExecuter.008red.cpp`（a9aea708…）。它只把 `WtLocalExecuter::getUndoneQty/getOrders` 改回账户级快照，即审计所指、owner 过滤之前的口径。
  - 重链放在 `SCRATCH/slice4-result-run/red/wtcore008/TestUnits/`，同级复制同哈希的 `libWtExeFact.dylib`（bac792e7…），以满足 R71PluginDir 的查找。日志 `logs/phase4-red-relink-008.log`。
  - 结果（`red-wtcore-008-docalc`）：主用例 exit 1，第 422 行显示 A 被 B 的单阻塞、只有 1 笔下单；通道就绪路由用例 exit 1；三个对照 exit 0。
  - 第一次红控运行时，二进制不在 `TestUnits/` 子目录下，找不到插件，5 例全部在夹具前置处失败。改正目录后，主用例在 ASSERT 失败后因执行器未 quiesce 而 abort（exit 134）。此后加入 RAII 收尾并重建（`logs/phase4-docalc-build-2.log`），红控改为干净失败。以上过程日志均已被覆盖，只保留最终结果。
- 绿控：`green-wtcore-008-docalc` 5/5；`green-phase4-wtcore-related` 162/162（与第三轮同一过滤，含新增 3 例，并设置 `WT_COV_PLUGIN_BIN`）。构建 `logs/phase4-docalc-build-1.log`、`-2.log` 均 exit 0，告警 0。
- 仍未覆盖：
  - StockVWap 的 T+1 卖出切片、`ord_sticky` 宿主身份核验超时撤单路径、`tail_secs` 尾盘全量；
  - 真实 provider 对 usertag 与 entrust id 的回显保真（外部）；
  - 持仓仍是账户级口径。任务清单注明“账户持仓广播本身不由该探针判错”，本例不对它作结论。
- sanitizer 建议（交主代理）：`CoverageTests_wtcore`，过滤 `S4StockVWapOwnerE2E.*`，ASan+UBSan 期望 0 报告。

## 第四轮收尾时的快照核对

按 `sources.sha256` 最后的 after 记录逐一比对工作区，共 55 项，其中 4 项现在与本目录的 after 快照不同。这 4 项的锁均已由协调者从 slice4-result 转给其他工作包（见 `locks.txt`），属于转交后对方的后续修改，不是本段改动：
- `src/TraderCTPOpt/TraderCTPOpt.cpp`：slice3-result 加入了 I03-NEW-133 的结算查询日期状态，当前哈希 a1732f7e…。本段 K42 的 `OnRspQryOrder` 改动仍在。本段的 CTPOpt 绿控是对 after 快照版本做的，不覆盖对方的新版本。
- `src/TestUnits/coverage/t_TraderXTP/t5_xtp_api_test.cpp`：slice3-result 启用了一个原禁用用例，当前哈希 281fe52b…。
- `src/TestUnits/coverage/q03_storage3/q03_rdm_kline_file_test.cpp`：slice1-result 修改了 sign-compare 字面量，当前哈希 a9b5af14…。
- `src/TestUnits/coverage/p03_btcore_mockers2/r180_btcore_mockers2_test.cpp`：同上，当前哈希 c3232f9e….

本目录 after 快照与相应的红绿结论只对应本段提交时的版本。
