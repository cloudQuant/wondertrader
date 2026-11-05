# 2026-11-04 s5 子任务 engine-lifecycle（引擎、策略管理器、策略插件 ABI、事件通知与风控监控）

macOS arm64，HEAD `ccaad010e9169f610aded4d8dc0f5647f70a5cff`（工作区含大量他人未提交改动，见 `hashes/start-owned-sha256.txt`）。本目录只记录本子任务的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实插件/供应商或最终整合树。未修改任何跟踪文件。改动前副本在 `before/`（保留相对路径，哈希见 `hashes/before-sha256.txt`），改后哈希见 `hashes/after-sha256.txt`。

构建目录：覆盖率 `/tmp/wt-i03-s5-engine-lifecycle-cov`（默认 `-Werror`）；ASan `/tmp/wt-i03-s5-engine-lifecycle-asan`（Homebrew LLVM 22，实体 rsync 快照，`WT_WARNINGS_AS_ERRORS=OFF` 仅为 sanitizer 局部构建）；运行目录 `/tmp/wt-i03-s5-engine-lifecycle-run/`。机器负载持续 100–180，构建很慢。

## 1. 卡4b=A 策略插件版本化导出与能力协商（NEW-002、L03-002 插件部分、R31-002、NEW-033 部分）

新增 `src/Includes/IStrategyPluginV2.h`（不改旧 `CtaStrategy`/`I*StrategyFact` vtable 与旧 C 导出）：

- 每族独立 getter：`get{Cta,Sel,Hft,Uft}StrategyPluginApiV2(requested_major)`，返回静态表 `WtStrategyPluginApiV2{struct_size, abi_version, family, object_revision, capabilities, reserved[3], create_factory, destroy_factory}`。
- 宿主校验：主版本 2；表族别与 getter 族别一致；`object_revision` 必须**完全等于**宿主 `WT_STRA_PLUGIN_V2_OBJECT_REVISION`（20261104，对应卡4a 的 `WTSCommodityInfo` ready/TM_None 改动；插件内联旧 getter 的问题由此拒绝）；保留字段 0；create/destroy 非空；`struct_size` 覆盖 2.0 布局。
- `wt_stra_plugin::probeModule`：不调用任何 v1 函数即可分类：本族通过（Accepted）/拒绝（旧导出或表非法，Rejected）/他族版本化插件（ForeignFamily，跳过）/非插件。
- 能力 `WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN`（卡9a）：只有声明者，宿主才调用 `destroy_factory` 并卸载；否则工厂与模块隔离到进程退出。

宿主改动：实盘 `CtaStrategyMgr`/`SelStrategyMgr`/`HftStrategyMgr`（`HftStrategyMgr::FactoryOwner` 改为按能力决定最后释放时是否销毁工厂）；回测 `CtaMocker::init_cta_factory`、`SelMocker::init_sel_factory`、`HftMocker::init_hft_factory`、`UftMocker::init_uft_factory` 及各自 `_StraFactInfo::release`（新增 `_api`，旧测试注入的 `init_*_factory_impl(creator, remover)` 接缝保留并经新的 `*_core` 走旧路径）。产品插件 `WtCtaStraFact`、`WtCtaTwoMaFact`、`WtSelStraFact`、`WtHftStraFact`、`WtUftStraFact` 追加 V2 表（保留旧导出），均无异步工作，声明同步拆除能力。

返回值合同：CTA/HFT 拒绝时 `loadFactories` 返回 false（与既有拒绝合同一致）；SEL 对旧 SEL 二进制/非法表返回 false（部署错误须可见），对他族插件维持既有“忽略并返回 true”。实盘管理器拒绝后按既有隔离规则保留模块引用（dlopen 可能已运行静态初始化）；回测 mocker 沿用既有“拒绝即卸载”。

**未改（非本包文件）**：实盘 UFT `src/WtUftCore/UftStrategyMgr.cpp` 仍按旧 `createStrategyFact` 加载。所需改动：按 `CtaStrategyMgr` 同样调用 `wt_stra_plugin::probeModule(..., WT_STRA_PLUGIN_V2_FAMILY_UFT, ...)`，以 `api->create_factory()` 建工厂，仅在 `hasSyncTeardown(api)` 时用 `api->destroy_factory` 释放；`fixtures/r95_uft_strategy.cpp` 需同步追加 `getUftStrategyPluginApiV2`。另 `fixtures/i03_exec_eager_validation_20261103/sel_factory.cpp`（他包夹具）为旧 SEL 二进制，按新合同会被拒绝，需追加 V2 表。

### 结果（`runs/abi-all/`，夹具 `src/TestUnits/fixtures/i03_strategy_plugin_abi_20261104s5/`）

4 族 × 12 变体 fake 插件。每例独立进程/cwd、60 s alarm。

| 二进制 | 宿主源码 | 结果 |
|---|---|---|
| live-green | 当前 Cta/Sel/HftStrategyMgr | 11/11 exit 0 |
| live-red | `before/` 中改前管理器 | 11/11 exit 1（旧 creator 被调用 `fixture_legacy_creates()==1`；HFT 管理器调用 CTA 旧 creator；非法表被接受） |
| mocker-green | 当前构建树 `libWtBtCore.a`（覆盖率插桩，默认 -Werror 构建 exit 0） | 5/5 exit 0 |
| mocker-red | 改前快照的 `libWtBtCore.a`（`/tmp/wt-i03-s5-engine-lifecycle-red/libs`，哈希见 `mocker-red-libs.sha256`） | 5/5 exit 1（同上） |

补充运行 `runs/abi-byref/`：增加 `LEGACY_BYREF`（历史按引用 deleter 的旧二进制）变体后 4 族 × 13 变体复跑：live-green 11/11、mocker-green 5/5；live-red 10 例 exit 1、`HftInvalidVersionTablesRejected` 在改前 HFT 管理器下 SIGBUS（exit 138，旧宿主把非法/他族表模块当作旧 HFT 插件调用 creator 后崩溃，即 R31-002 现象）。改前 mocker 静态库副本在第二次运行前已不在 `/tmp`（原目录被清空），故此次未重跑 mocker-red，首次 mocker-red 5/5 失败结果保留在 `runs/abi-all/`。

NEW-033 既有夹具（追加 V2 getter 后用当前管理器复跑，`runs/new033-fixtures/`）：CTA 17/17 current 通过；SEL 19/19 current 通过；HFT 25/26 current 通过、`allocation-registry-failure` 失败——用改前 HFT 管理器同夹具同样失败（`hftbefore`，exit 1），为既有夹具 oracle 与策略 ID 预占位改动不一致，非本次回归。CTA/HFT 脚本的 `head-red-*` 历史对照因 HEAD(ccaad010) 已含 CTA/HFT guard 修复而不再为红（脚本既有陈旧，未改）。SEL `missing-remover` 旧断言按卡4b=A 改写（V2 表缺 `destroy_factory` 为非法表、拒绝返回 false；原“忽略返回 true”与合同冲突），原文件在 `before/`。`i03_strategy_id_20261103` HFT/SEL 均 PASS。

## 2. NEW-130：未初始化引擎元数据访问 SIGSEGV（卡15 明确缺陷，已修）

根因：`WtEngine` 构造未初始化 `_base_data_mgr/_hot_mgr/_data_mgr`（及 `_risk_fact` 各指针），访问器无条件解引用。`WtRtRunner` 只初始化 env 对应的引擎，二次 `config` 按卡9c 被拒，同进程在未配置的 SEL 引擎上创建 context 即在 `WtEngine.cpp:132` 解引用空指针（z15 `runs/new130_lldb`）。修复：构造置空；`get_commodity_info/get_contract_info/get_session_info/get_rawcode/get_tick_slice/get_last_tick/get_kline_slice/get_cur_price/get_day_price/get_exright_factor/sub_tick` 在缺管理器时返回空/0/1.0 而不解引用。新用例 `n02_wtcore_engines/s5_engine_uninit_metadata_test.cpp`（子进程 death-test，零填充与 0xA5 填充两种存储）。红绿结果见第 7 节。`R82PorterCapiReal` 同进程顺序依赖二次 `config`，与卡9c 冲突，属测试前提过时（q06_porter，非本包），建议逐进程运行或改为每族独立进程。

## 3. I03-NEW-008：实盘费用消费接入 FeeSchedule（卡13d）

- `WtEngine::load_fees`：识别版本化信封（`version/contracts/products`），JSON 直接、YAML 经严格转换后交 `wt_fee::Schedule::loadJson`；任一错误整份拒绝（`fee_config_valid()==false`，引擎/runner 既有门禁拒绝启动）。旧格式（仅品种行）保持既有兼容（null 跳过、未注册品种告警跳过），新增保存 `minfee`。成功加载替换旧表。
- 新 `calc_fee_checked(stdCode, price, qty, offset, fee, error)`：实际成交合约（连续/自定义规则代码经 `getCustomRawCode` 映射为当日实际合约）；合约级优先、品种级回退；合法零费率有效；缺费/元数据不符/非法输入返回 false；计算用 `wt_fee::calculate`（与回测相同的最低费与分位取整）。
- `calc_fee()`（旧签名）与实盘消费者 `WtEngine::do_set_position`、`CtaStraBaseCtx::do_set_position`、`SelStraBaseCtx::do_set_position` 统一经它计费；失败时**成交/持仓变化保留**、费用记 0，并经 `report_fee_error` 可观察（每代码首次 error 日志、`fee_error_count()`、`last_fee_error()`）。
- 复核中发现并修复：实盘 SEL context 与引擎组合账本开仓/反手费用按目标量 `abs(qty)` 计（日志记的是增量 `abs(diff)/abs(left)`，回测 SelMocker 用增量）→ 改为增量。
- 行为变化（需在发布说明注明）：缺少合约元数据的代码不再按品种费率计费而是报错记 0；合约无自身 volscale 时按 `WTSBaseDataMgr::loadContracts` 同规则取品种 volscale。`minfee` 按既有实盘行为只解析校验、不应用（回测应用），是否对齐回测列为决策项（`decision-drafts.md` G）；首轮实现曾应用 minfee，`R130EngineFee.LoadFeesMakesTemplateAvailableForEngineCalculation` 旧断言（实盘不应用）据此失败，已撤回该行为而非改断言。
- **未改（非本包）**：`HftStraBaseCtx`（wp1-ledger）真实成交仍用 `commInfo->calcFee`；所需改动：三处改为 `_engine->calc_fee(stdCode, ...)` 或 `calc_fee_checked` 并在失败时保留成交、调用 `report_fee_error`。`CTPLoader` 生成侧无需改动（v2 合约级键 `交易所.合约` 现被实盘消费）。
- 新用例 `n02_wtcore_engines/s5_live_fee_schedule_test.cpp`（9 例），结果见第 7 节。

## 4. NEW-034：风控监控器停止/排空与资金对象借用（卡9a=A+C）

- 新增 `src/Includes/IRiskMonPluginV2.h`：导出 `getRiskMonPluginCapsV2`，能力 `WT_RISKMON_PLUGIN_V2_CAP_SYNC_STOP`（`stop()` 幂等、外部线程调用时返回即全部活动停止、此后不再调用上下文、不保留 `getFundInfo()` 借出的指针）。`WtRiskMonFact` 导出该能力（`WtSimpleRiskMon::stop()` 外部调用 join）。
- `WtEngine`：插件监控器拿到的是 `WtRiskContextProxy`（转发到引擎）；`release()` 在 join task worker 后停止监控器恰一次（析构再调用幂等）；已证明停止者在析构时先删监控器再删代理、释放资金引用；未证明者隔离：代理脱离引擎并持有资金引用，迟到调用得到惰性结果（资金指针仍有效、`isInTrading=false`），监控器对象、模块、代理与资金常驻到进程退出；`init_riskmon` 二次初始化按卡9c 拒绝。`setRiskMonitor`（仅测试/宿主进程内使用）视为调用方担保同步停止。
- 夹具 `src/TestUnits/fixtures/i03_riskmon_quiesce_20261104s5/`（真实 dlopen fake 插件、ASan/UBSan），结果见第 7 节。

## 5. R10-005：LSan exit 23 的归因（已确证为 profile 运行时）

`runs/r10-005-profile-lsan/`：与 WonderTrader 无关的一行函数模块，`-fprofile-instr-generate -fcoverage-mapping` 编译后由 ASan 宿主 dlopen/dlclose：设置 `LLVM_PROFILE_FILE`（69 字符）时 LSan 报 70 字节 `strdup`（栈在已卸载模块的初始化器中），不设时 0 泄漏；同源未插桩模块设置与不设置均 0 泄漏。大小=路径长+1、栈形状与 11-03/11-04 复验现象一致，确证 R10-005 两例缺导出用例的 exit 23 来自被 dlopen/dlclose 的插桩模块的 profile 运行时，不是 EventNotifier 产品代码。

## 6. 评估与决策卡草稿（D/Z 类）

见本目录 `decision-drafts.md`。

## 7. ASan 定向结果与覆盖率目标

### 7.1 ASan/UBSan（Homebrew LLVM 22，实体快照全量重编 `WtCore WTSTools WTSUtils`）

绿相位（当前源码）：

| 范围 | 结果 | 证据 |
|---|---|---|
| NEW-034 riskmon 夹具 | 3/3 exit 0，无 ASan/UBSan/LSan 报告（隔离例 LSan 关，见夹具 README） | `runs/riskmon-asan/green/` |
| NEW-130 `S5EngineUninitMetadata.*` | 3/3 exit 0，无报告（death-test 子进程 fork 时 libsystem 的 `_notify_fork_child` 分配会被 LSan 报告，故此 3 例 LSan 关） | `runs/lane-asan/green/` |
| I03-NEW-008 `S5LiveFeeSchedule.*` | 9/9 exit 0，LSan 开，无报告 | `runs/lane-asan/green/` |

首轮绿运行 `VersionedYamlTemplateIsAccepted` 失败（YAML 转 JSON 时 `version: 2` 被写成 2.0，严格解析要求整数）；修正转换（整数保留为整数）后通过，首轮日志未保留为通过证据。

红相位（`before/` 中改前 `WtEngine.{cpp,h}`，同一快照重编）：见 7.2。

### 7.2 红控（改前 WtEngine，同一 ASan 快照）

| 范围 | 结果 | 证据 |
|---|---|---|
| NEW-034 `UnprovenMonitorIsQuarantinedAndOutlivesEngineSafely` | exit 134：引擎析构后监控线程经 `getFundInfo()` 进入已销毁引擎，UBSan `member call on null pointer of type 'wtp::WTSPortFundInfo'`（`WtEngine.cpp:237 update_fund_dynprofit`） | `runs/riskmon-asan/red/` |
| NEW-034 `ProvenMonitorIsStoppedThenFundAndMonitorReleased` | exit 0（正控：同步停止的监控器改前同样安全） | 同上 |
| NEW-034 `RepeatedRiskMonitorInitIsRejected` | exit 1：第二次 `init_riskmon` 返回 true 并再创建一个监控器（卡9c 未实施） | 同上 |
| NEW-130 `S5EngineUninitMetadata.*` | 3/3 exit 1：death-test 子进程在 `WtEngine.cpp:132` UBSan `member call on null pointer`/`misaligned address 0xa5a5…`（与 z15 lldb 位置一致） | `runs/lane-asan/red/` |
| I03-NEW-008 `S5LiveFeeSchedule.*` | 编译失败（改前无 `calc_fee_checked`/`fee_error_count`，且改前 `load_fees` 拒绝 v2 信封）——为接口级红控，非运行红控 | `runs/lane-asan/red-s5_live_fee_schedule_test-link.log` |

### 7.3 覆盖率目标 `CoverageTests_wtcore`（默认 `-Werror`，构建 exit 0）

逐过滤器独立 cwd、300 s alarm（`runs/lane-cov/`）：

| 过滤器 | 结果 | 说明 |
|---|---|---|
| `S5EngineUninitMetadata.*` | 3/3 | NEW-130 |
| `S5LiveFeeSchedule.*` | 9/9 | I03-NEW-008 |
| `r31_stra_mgr.*` | 13/14 | 新增 `hft_refuses_cta_plugin_before_creator` 通过；失败的 `sel_manager_loads_real_sel_plugin_and_creates_strategy` 断言“插件返回空策略时仍发布空 wrapper”，与 11-03 HFT/SEL 策略 ID 改动（空策略不发布、可同 ID 重试）冲突，`SelStrategyMgr::createStrategy` 本轮未改，属既有 oracle 冲突（非本包回归，未改断言） |
| `R202HftStrategyFactoryAbi.*` | 1/1 | 产品 HFT 插件经 V2 加载与按能力销毁 |
| `R130CtaStrategyMgr.*` | 3/3 | 产品 CTA 插件 |
| `R41EngineFixture.*` | 30/30 | 引擎、费用、风控相关 |
| `R101EngineGap.*` | 2/2 | 费用文件错误/缺商品 |
| `R211Engine.*`、`R211StrategyMgr.*` | 2/2、1/1 | 含 z15 空监控器拒绝、非插件目录 |
| `R110EngineRiskMon.*` | 1/1 | 产品 `WtRiskMonFact` 经 init_riskmon |
| `R130MiscTail.*` | 4/7 | `CtaRunStartsRiskMonitor` 通过；3 例 `Parser*` 失败为 ParserAdapter V2 生命周期（他包）与旧 fake 不匹配，与本包无关 |
| `R10EventNotifier.*` | 2/2 | R10-005 两例 |
| `R41EventNotifierTest.*`/`R160EventNotifier.*`/`R120Notifier.*`/`R101EventNotifierLoader.*` | 6/6、12/12、2/2、1/1 | R41-003 相关回归，EventNotifier 本轮未改 |
| `R42ParserAdapterTest.*` | 4/11 | 7 例为 R42-003 旧 fake 无 V2 生命周期证明，被当前 ParserAdapter 按卡9a 隔离保留（如 `mgr.getAdapter` 释放后仍非空），旧 oracle 与卡9a 冲突，需 ParserAdapter 所属包更新夹具 |
| `R130EngineFee.*` | 1/6（首轮，旧实现） | 首轮暴露三处与旧合同冲突的实现：合约 volscale=0 判非法、实盘应用 minfee、成功加载清空旧表破坏“部分重载只更新出现的品种”。均改回既有合同（volscale 按 `loadContracts` 规则回退品种值；minfee 不应用、列决策 G；旧格式重载保持合并语义），旧断言未改。最终源码复跑见 7.4 |

## 8. 其他条目评估（无产品改动）

- **BUG-R42-003**：`WtCore/ParserAdapter.cpp`（非本包文件）已由解析器生命周期 V2 工作改为：`requestStopLifecycle` 先置 `_stopped`；`finishReleaseLifecycle` 在同一生命周期锁与 operation 标志下经 `request_stop → can_wait → wait_drained → destroy_api` 成功后才清空 `_parser_api`，回调抛异常或状态不明则 `_release_uncertain` 隔离、保留原句柄；`run()` 以 try_lock + `_stopped` 拒绝。原条目“release 后才清空指针”的竞态窗口由生命周期锁关闭，异常 quarantine 已实现；initExt 外部 API 无排空证明则不销毁。本包未改动该文件；定向复跑结果见第 7 节（若已运行），真实 provider 排空仍为 X。
- **BUG-I02-NEW-111**：`HftStraBaseCtx::stra_get_position` 在未绑定 trader 时按 I03-NEW-002 返回 0 并记错误日志，不崩溃；`WtRtRunner::createHftContext` 按 R42-002 拒绝不存在的 trader。合同已由 R42-002/I03-NEW-002 确定，`R124PorterExpHft.SubscribedTickAndTradeAreForwarded` 失败是测试前提（以不存在的 trader 建 context）过时（q06_porter，非本包），需改为绑定本地 TraderMocker 或断言拒绝。
- **BUG-R111-001**：回测四族 mocker 现只经 V2 表建工厂，声明同步拆除能力的产品插件在释放时“策略删除 → destroy_factory → dlclose”，fake 矩阵证明删除各恰 1 次、无能力插件隔离（`runs/abi-all/mocker-green`）。产品 UFT/HFT 插件在 LSan 下的整链释放未在本轮重跑。

### 7.4 最终源码复跑（ASan/UBSan 独立链接，`runs/lane-asan/`）

`WtEngine.cpp` 在首轮绿运行后又两次修改（minfee 不应用、旧格式重载合并），最终 sha256 前缀 `f756f9579048`；ASan 快照与仓库源码一致性见 `runs/lane-asan/binaries.sha256` 末两行。以最终源码重编 ASan `libWtCore.a` 后：`S5EngineUninitMetadata.*` 3/3、`S5LiveFeeSchedule.*` 9/9（LSan 开）、`R130EngineFee.*` 6/6（该夹具的 `makeCt/makeComm` 对象从不释放，属测试侧泄漏，LSan 关），均 exit 0、无 ASan/UBSan 报告。riskmon 夹具以最终源码复跑见 `runs/riskmon-asan/green/`。覆盖率二进制中的 `S5LiveFeeSchedule.*`/`R130EngineFee.*`（7.3）早于这两处修改，最终源码的覆盖率目标重编（`build-CoverageTests_wtcore-4.log`）在本报告时仍在进行，未用于结论。

`CoverageTests_btcore`（默认 -Werror 构建 exit 0，`runs/lane-cov/btcore/`）：`R202HftMockerFactoryAbi.*` 3/3、`Q02HftMockerFactoryInit.*` 4/4、`Q02FactoryTest.*` 7/7、`R198SelMockerFactory.*` 2/2；`R111{Cta,Hft,Sel,Uft}Mocker.*` 全部在夹具 SetUp 的 `HisDataReplayer::reset` 中 `pointer being freed was not allocated` 中止（`r111-hft-lldb.log`）——他包于 20:24:51 修改 `HisDataReplayer.h`，而该用例目标文件编译于 20:24:18、`HisDataReplayer.cpp.o` 编译于 20:28，同一二进制混入两种布局，属构建期并发改动造成的陈旧对象，非产品缺陷；R111 需在 btcore 稳定后重编复跑。`CoverageTests_porter`（R6/R82）未构建完成，未运行。

## 9. 主线程追加（I03-NEW-066 / 065 / 062 及 R111、最终费用复跑）

改前副本：`before/src/WtCore/WtSelEngine.{h,cpp}`、`before/src/WtCore/EventNotifier.cpp`、`before/src/WtRiskMonFact/WtRiskMonFact.cpp`（会话开始时备份，至本轮修改前未变）；NEW-062 改前的 `WtEngine.{cpp,h}`（sha256 前缀 `f756f9579048`/`f373d348dded`）另存 `before-062/`。哈希见 `hashes/before-after-sha256-prefix.txt`。夹具 `src/TestUnits/fixtures/i03_engine_followup_20261104s5/`；SEL 用例同时放在覆盖率 lane `q01_wtcore3/s5_sel_schedule_thread_test.cpp`。

### 9.1 I03-NEW-066：SEL 任务派发线程（明确崩溃缺陷，已修）

运行时红控（改前 `WtSelEngine`，ASan 快照库，`runs/followup/red-asan/`）：`FiredTaskRunsOnceOnItsContextAndIsJoined`、`TasksOnSeveralDaysAreAllJoinedByRelease` 均 exit 1，死亡测试子进程输出 `libc++abi: terminating`——单次触发即终止，确证每个触发任务都会 `std::terminate`。

修复（不预判卡 A，保持现有异步语义）：派发线程由引擎持有（`_sched_workers` + 完成标志，互斥保护），后续派发时回收已结束线程，`release()` 在停止 ticker 后 join 剩余线程（自线程调用时分离并记错误）；线程体捕获策略异常并记日志，避免逃逸异常再触发 terminate；按任务登记键（context id）而非任务序号查找 context，找不到时记错误跳过。未改变 schedule 与 tick 的并发关系（仍待卡 A）。

绿控：ASan/UBSan（`runs/followup/green-asan/`）与 TSan（Homebrew LLVM 22 `-fsanitize=thread`，独立实体快照 `/tmp/wt-i03-s5-engine-lifecycle-tsan` 重编 `WtCore WTSTools WTSUtils`，`runs/followup/green-tsan/`）两例均 exit 0，TSan 0 报告（`TSAN_OPTIONS=halt_on_error=1`，死亡测试子进程若有报告会以非 0 退出而断言失败）。TSan 只覆盖派发线程登记/回收/join 本身；schedule 与 tick 对同一 context 的并发属卡 A，未在此验证。

### 9.2 I03-NEW-065：`EventNotifier::init` 在 `create_server` 返回 0 时报失败（已修）

修复：先用局部变量完成 `regiter_callbacks` 与 `create_server`，返回 0 时记错误、返回 false、不覆盖既有 notifier 状态、不启动 worker；成功后才提交成员。红控（改前）：`CreateServerFailureIsReportedAndStartsNothing` exit 1（`init` 返回 true）；正控 `SuccessfulCreateStillPublishes` 改前改后均 exit 0。绿：ASan/UBSan+LSan 2/2、TSan 2/2，无报告。

### 9.3 I03-NEW-062：风控工厂删除函数 ABI 与泄漏（已修，旧 ABI 不变）

`RiskMonDefs.h` 未改（旧 `FuncDeleteRiskMonFact` 按引用的 typedef 保留，宿主不调用）。`IRiskMonPluginV2.h` 的能力表追加 `destroy_factory`（按值，`struct_size` 覆盖时才读取；`factoryDestroyer()` 校验），`WtRiskMonFact` 导出之。引擎在已证明同步停止的监控器停止并删除后、或创建监控器失败时，经该函数释放工厂；未声明者工厂随模块常驻（卡9a 隔离）。夹具去掉 LSan 抑制并断言工厂删除次数。红控（`before-062/` 引擎，`runs/riskmon-asan/red-062/`）：`ProvenMonitorIsStoppedThenFundAndMonitorReleased` exit 1（`fixture_factory_deletes` 为 0，LSan 报 `createRiskMonFact` 8 字节直接泄漏），`RepeatedRiskMonitorInitIsRejected` exit 1（同一 8 字节工厂泄漏，断言本身通过），隔离例 exit 0（正控）。

绿控（`runs/riskmon-asan/green-062/`，当前源码，无 LSan 抑制）：riskmon 3/3 exit 0，0 报告。

### 9.4 I03-NEW-075：CTA/SEL/HFT ticker `run()` 空 session 解引用（已修）

`init()` 传入未知 session 时 `_s_info` 为空（SEL/HFT ticker 构造函数还未初始化 `_s_info/_store`）。红控（改前 ticker，ASan 快照库，`runs/ticker075/red-asan/`）：`S5{Cta,Sel,Hft}TickerRunWithoutSessionStartsNothing` 3/3 exit 1，子进程 UBSan `member call on null pointer of type 'wtp::WTSSessionInfo'`，分别位于 `WtCtaTicker.cpp:234`、`WtSelTicker.cpp:234`、`WtHftTicker.cpp:274`（`calcTradingDate(_s_info->id(), …)`）。

修复：三个 ticker 的 `run()` 由 `void` 改为 `bool`（宿主内部类，调用方忽略返回值仍可编译），session 或基础数据缺失时记错误、返回 false，不调用 `on_init/on_session_begin`、不启动 worker；HFT 保持 `Idle` 以便之后重新 `init`；已启动返回 true。SEL/HFT ticker 构造函数将 `_s_info/_store` 置空。三个引擎在 `run()` 返回 false 时记错误日志。新增 `n02_wtcore_engines/s5_ticker_null_session_test.cpp`（3 个死亡测试 + 1 个返回值用例）。

绿控（ASan/UBSan，`runs/ticker075/green-asan/`）：4/4 exit 0，0 报告。

### 9.5 最终源码一致性复跑（ASan/UBSan，`runs/final-asan/`）

ASan 快照同步全部本包改动后重编 `WtCore`（库哈希 `libs.sha256`；快照与仓库逐文件对照 `snapshot-vs-repo.txt`：26 个相关源一致，唯一差异 `SelStraBaseCtx.cpp` 为快照早于 SEL 费用增量修正，下列用例不经过该路径）。一次连续运行：riskmon 3/3（含无抑制 LSan）、I03-NEW-065 2/2、I03-NEW-066 2/2、NEW-130 3/3、`S5LiveFeeSchedule` 9/9（LSan 开）、`R130EngineFee` 6/6（夹具泄漏，LSan 关），全部 exit 0、无 ASan/UBSan/LSan 报告。`WtEngine.cpp` 现为 `ca96588e5f66`（`f756f9579048` 之后为 NEW-062 加入工厂释放，费用代码未再改动）。

### 9.6 R111 套件与费用用例在新构建中的复跑

- 费用用例（最终源码）：见 9.5（`S5LiveFeeSchedule` 9/9、`R130EngineFee` 6/6，ASan/UBSan 独立重编 `WtCore`）。覆盖率目标 `CoverageTests_wtcore` 以最终源码的重编在机器负载 150+ 下未完成，已中止，不作结论。
- `CoverageTests_btcore` 以当前树全新增量重编（默认 `-Werror`，`build exit 0`，`runs/lane-cov/btcore-final/build.log`；`HisDataReplayer.cpp.o` 与 R111 用例对象均晚于他包对 `HisDataReplayer.{h,cpp}` 的最后修改，已排除陈旧对象）：`R202HftMockerFactoryAbi` 3/3、`Q02HftMockerFactoryInit` 4/4、`Q02FactoryTest` 7/7、`R198SelMockerFactory` 2/2、`R111HftMocker` 1/1 通过；`R111CtaMocker` 0/7、`R111SelMocker` 0/1、`R111UftMocker` 0/7、`R111ExecMocker` 0/16、`R111ReplayerEnv` 0/3——全部失败点均为夹具 `SetUp` 中 `HisDataReplayer::init(cfg)` 返回 false（`r62_mocker_common.h:218`、`r111_replayer_notifier_env_test.cpp:104`），在进入 mocker 工厂加载之前，与本包改动无关。`HisDataReplayer.cpp`（20:24）与 `WTSBaseDataMgr.cpp`（21:17）为他包当晚修改；需回放器/基础数据所属包定位 `init` 失败原因后再复跑 R111。用 lldb 在 `init` 各 `return false` 处设断点的尝试在高负载下超时，未取得具体位置。

### 9.7 本轮删除的路径（仅本子任务自建）

本子任务只删除过自己创建的 `/tmp/wt-i03-s5-engine-lifecycle-*` 下的子目录/文件：`/tmp/wt-i03-s5-engine-lifecycle-run/abi-byref`（重跑前）、`/tmp/wt-i03-s5-engine-lifecycle-run/asan-lane/run-green`（重跑前）、`/tmp/wt-i03-s5-engine-lifecycle-asan-libs/green`、`green-src`、`green2`、`green2-src`（重建前）、`/tmp/wt-i03-s5-engine-lifecycle-run/asan-lane/green-s5_*`；证据目录内删除过 `*.profraw`、被替换的旧 `runs/riskmon-asan/green`、`runs/lane-asan/green`，以及误复制进 `runs/final-asan/followup/` 的两个可执行文件（哈希保留在 `runs/final-asan/binaries-removed-from-evidence.sha256`）。未删除任何他人目录。此前报告的 `/tmp/wt-i03-s5-engine-lifecycle-red/libs` 消失并非本子任务所为（未执行过针对它的删除命令）。
