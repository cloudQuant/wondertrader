# Lane R9 交付清单 — WtBtPorter（迭代01 第七波）

- 派生自: `manifests/api-brief-R9.md`（范围来自全量报表 UNCOVERED 段：`WtBtPorter.cpp` 433/846、`WtBtRunner.cpp` 189/442、`ExpHftMocker.cpp` 56/64、`WtBtRunner.h` 36/36、`ExpCtaMocker.cpp` 26/46、`ExpSelMocker.cpp` 23/38）
- 落点目录: `src/TestUnits/coverage/q06_btporter/`（已登记目录，前缀 `r9_`；未新建目录、未改 `src/TestUnits/coverage/CMakeLists.txt`）
- 生产代码改动: **无**。`src/**`（除本 lane 两个新 .cpp）一字未动。
- 门禁（真实构建口径，非自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_btporter.dir/q06_btporter/q06_btporter_capi_test.cpp.o | tail -1`
  → 剥掉 `-o X -c Y` 后对两个新 .cpp 加 `-fsyntax-only -c` 执行；**2/2 exit 0**（同目标同目录命令行，含 `-Werror`）。
  迭代记录：`r9_exp_mocker_bridge_test.cpp` 首跑 1 error（`WTSVariant::create` 是 private，见 WTSVariant.hpp:63/120）→ 改 `WTSVariant::createArray()`（公开，:147）后通过；`r9_mocker_wired_capi_test.cpp` 首跑 3 errors（`WtBtRunner.h:36-37` 只有前向声明，`mocker->id()` 需要完整类型）→ 补 `WtBtPorter/ExpCtaMocker.h`/`ExpHftMocker.h` 后通过。
- 用例总数 32：**启用 30**，`DISABLED_` 2。不起线程、不 sleep、不跑 run/config 成功路径、不落盘、不建 socket/共享内存；无 `#define private public`。
- 符号缝: 只引用 `WtBtPorter.h` 声明的 WtBtPorter 一族入口（`register_*`/`init_*_mocker`/`config_backtest`/`get_raw_stdcode`/`cta_*`/`sel_*`/`hft_*`），未 include 任何 `WtPorter`/`WtCore`/`WtUftCore` 头；核心类只取 `WtBtCore` 侧（经 `WtBtPorter/Exp*.h` 的相对路径），未触碰 BUG-L00-005 的同名 `wtp::` 兄弟核心。
- 夹具复用: 沿用 q06 的「全局 `getRunner()` + 注册替身回调 + `WTSTickData::create(WTSTickStruct&)` 池工厂」思路与「未挂 ext loader 时拒绝」前置，探针与替身自建（匿名 namespace，不与 q06 的 static 冲突）；未改写 q06 两个文件。
- 订阅位接缝: `_tick_subs` 实测是 `CtaMocker.h:786(protected)/1045`、`SelMocker.h:566(protected)/836`、`HftMocker.h:1216(protected)/1258` 的 **protected** 成员 → 用测试本地派生类 `SubbedCta/SubbedSel/SubbedHft` 直接 insert，不需要 `#define private public`（旧「成员全 private 进不去」结论已被推翻）。
- 文件顺序约束: `r9_mocker_wired_capi_test.cpp` 会把 `_cta_mocker/_hft_mocker` 永久置为非空，因此命名排在 `r9_exp_*`（以及 q06 两个文件）之后，避免反向污染「未初始化」断言。

## 1. `src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp`（750 行 / 23 例，22 启用 + 1 DISABLED）

夹具 `R9Wired::SetUp` 全程经 C 导出接线（不是直接调 runner），使 L51-123 五个 register 口在每个用例里都被真实执行一次；`TearDown` 用 NULL 反注册复原。

| 用例（行号=测试文件内） | 生产文件:行段 | 期望值来源 / 打到的分支 |
| --- | --- | --- |
| `RegisterExportsWireRunnerSlots` L323 | `WtBtPorter.cpp:51-54,67-71,83-87,104-109,120-123`、`WtBtRunner.cpp:218-227,349-359`、`WtBtRunner.h:202-205,548-552` | `ctx_on_init(77,ET_CTA)`→`_cb_cta_init` 恰好 1 次且 `last_handle==77`；`on_initialize_event()`→evt 1 次、id=`EVENT_ENGINE_INIT`；`isAutoTrans()` 为 true（第 5 个入参喂 true） |
| `LoaderAutoTransFlagFollowsLastRegistration` L343 | `WtBtPorter.cpp:120-123`、`WtBtRunner.h:217-224,122-125` | 同一入口先 true 后 false 再 true，逐次覆盖 `_loader_auto_trans`（无累加、无默认回落） |
| `MockersAreNullAndAsyncFalseBeforeInit` L356 | `WtBtRunner.h:510,517,524,539`、`WtBtRunner.cpp:82-87` | 三个槽位 nullptr（构造列表）＋`_async(false)` L87 |
| `ReplayerAccessorIsStableReference` L366 | `WtBtRunner.h:531,639` | 两次取址必等（返回成员引用而非临时对象）且非空 |
| `ConfigBacktestMissingFileLeavesMockersNull` L376 | `WtBtPorter.cpp:174-185`、`WtBtRunner.cpp:449-462` | 不存在路径→`load_from_file` L300-301 返 NULL→L458-462 早退，三个槽位仍 nullptr；连喂两次证明守卫不生效（BUG-R9-001） |
| `InitEvtNotifierRejectsNullAndNonObject` L389 | `WtBtRunner.cpp:626-630` | `NULL` 与 `VT_Array` 各命中 `\|\|` 的一侧，均 false；不触发 `_notifier.init` |
| `HftBarNullGuardSkipsDispatch` L402 | `ExpHftMocker.cpp:16-17` | `newBar==NULL`→bar 回调 0 次、evt 0 次（与 CTA 侧对照见 BUG-R9-004） |
| `HftBarPeriodRenamedByTimes` L414 | `ExpHftMocker.cpp:19-27`、`WtBtRunner.cpp:315-325` | `("m1",5)→"m5"`(L23)、`("d1",3)→"d13"`(L21)、`("w1",2)→"m2"`(L23)；`close=88.5` 与 `hft.id()` 原样透传 |
| `HftTickNeedsSubscription` L438 | `ExpHftMocker.cpp:78-83` | 未订阅→0 次；`_tick_subs.insert` 后→1 次、`price==12.75`（本用例自己喂的 tick） |
| `HftChannelReadyDispatch` L458 | `ExpHftMocker.cpp:32-34`、`WtBtRunner.cpp:413-417` | trader 是 L34 的字面量 `""`；evtid 是 L416 的字面量 `1000` |
| `HftEntrustOrderTradeArgsPassthrough` L469 | `ExpHftMocker.cpp:39-41,71-73,103-105`、`WtBtRunner.cpp:419-435`、`HftMocker.cpp:1107-1109` | entrust `localid=4101/bSuccess=false/msg="rejected by mock"`；order `3+1+100=104`、`isBuy&&\|isCanceled` 编码=1；trade `2+70.25=72.25`（`do_set_position` 因 `commInfo==NULL` 直接 return，不影响外发） |
| `HftSessionBeginEndFireCtxAndGlobal` L497 | `ExpHftMocker.cpp:55-58,63-66`、`WtBtRunner.cpp:371-381`、`WtBtRunner.h:577-583` | begin(20240611)→sess(isBegin=1,date=20240611)+`EVENT_SESSION_BEGIN`；end(20240612)→flag=0+`EVENT_SESSION_END`；两次日期互不串用 |
| `HftMicroStructIgnoresSubscription` L523 | `ExpHftMocker.cpp:88,93,98`、`WtBtRunner.cpp:395-411` | 只订阅 `SSE.600000`，仍收到 `SHFE.rb2410` 的 ordque/orddtl/trans 各 1 次（BUG-R9-005 的直接证据） |
| `HftBacktestEndOnlyGlobal` L559 | `ExpHftMocker.cpp:110`、`WtBtRunner.h:592-596` | evt 1 次且 id=`EVENT_BACKTEST_END`；bar/tick/chnl/ord/trd 计数和为 0 |
| `HftInitFiresCtxInitAndEngineEvent` L569 | `ExpHftMocker.cpp:46-50`、`WtBtRunner.cpp:349-359` | `hft_init==1`、`last_handle==hft.id()`、evt=`EVENT_ENGINE_INIT` |
| `CtaBarClosePassesPeriodUnchanged` L583 | `ExpCtaMocker.cpp:135-138`、`CtaMocker.cpp:1021-1025` | `("SSE.600519","m5",close=15.25)`→period 原样 `"m5"`（与 HFT 的 L21/L23 对照）；第二次喂 `NULL` 仍回调 1 次且探针 `last_bar_null==true`（BUG-R9-004） |
| `CtaCalcAndCalcDoneForwardTime` L606 | `ExpCtaMocker.cpp:153-154,170-173`、`WtBtRunner.cpp:327-347`、`WtBtRunner.h:562-566`、`ICtaStraCtx.h:124` | calc(20240603,93000)→date/time 相等且 evt==0；calc_done(20240603,145900)→补 `EVENT_ENGINE_SCHDL` 带同 date/time；父类是空实现 |
| `CtaTickNeedsSubscription` L628 | `ExpCtaMocker.cpp:115-120`、`CtaMocker.cpp:1027-1035` | 未订阅 0 次；订阅后 1 次、`price==9.6` |
| `CtaCondTriggerAndBacktestEnd` L648 | `ExpCtaMocker.cpp:186,203`、`WtBtRunner.cpp:361-369` | cond 1 次、target=12.5/price=12.0/tag="r9tag"；`on_bactest_end`→`EVENT_BACKTEST_END` 且 SEL 计数 0（只有 CTA 一条口） |
| `SelBarAndScheduleForward` L667 | `ExpSelMocker.cpp:124-126,140-145`、`SelMocker.cpp:458-462,474-478` | bar `("SSE.600519","d1",close=7.125)`；schedule(20240614,153000)→`ctx_on_calc(ET_SEL)`+`EVENT_ENGINE_SCHDL` 同 date/time |
| `SelSessionEndAndTickGuard` L691 | `ExpSelMocker.cpp:88-91,105-110`、`SelMocker.cpp:553-560` | end(20240615)→sess flag=0/date=20240615+`EVENT_SESSION_END`；tick 未订阅 0 次、订阅后 1 次 `price==100.5` |
| `SelInitSessionBeginAndBacktestEnd` L717 | `ExpSelMocker.cpp:54-59,72-75,156` | init→sel_init 1 次+evt#1 `EVENT_ENGINE_INIT`；begin(20240616)→flag=1+evt#2 `EVENT_SESSION_BEGIN`；backtest_end→evt#3 `EVENT_BACKTEST_END`（evt 序号 1/2/3 递增即双发契约的证据） |
| `DISABLED_RunConfigSuccessPathsNeedStorageAndThreads` L747 | `WtBtRunner.cpp:509-548,550-583,437-447,299-313` | 触发条件：`run(false,false)` 走 `_replayer.prepare()+run()` 同步阻塞整段区间并读存储；`run(false,true)` 起 `StdThread` 且 `stop()` 要 join；`init_backtest` 里 `WTSLogger::init` 接管全局日志并按 outDir 建目录；`initSelMocker` 还要 `register_task(trdtpl,session)`。关联 BUG-R9-007 |

## 2. `src/TestUnits/coverage/q06_btporter/r9_mocker_wired_capi_test.cpp`（181 行 / 9 例，8 启用 + 1 DISABLED）

q06 只打了 `_mocker == NULL` 的中性兜底；本文件先用 `init_cta_mocker/init_hft_mocker`（已核对唯一副作用是 Exp*Mocker 构造 + `HisDataReplayer.h:804-807` 两行赋值）把槽位置真，再打 mocker 非空的真实分支。

| 用例（行号=测试文件内） | 生产文件:行段 | 期望值来源 / 打到的分支 |
| --- | --- | --- |
| `InitCtaMockerWiresSlotAndReturnsContextId` L65 | `WtBtPorter.cpp:308-311`、`WtBtRunner.cpp:266-283`、`HisDataReplayer.h:804-807`、`CtaMocker.cpp:55-56,60-78,80` | 返回值>0（`fetch_add` 从 1 起）且 `== cta_mocker()->id()`；`hook/persistData/bIncremental` 全 false ⇒ 跳过 `install_hook`(L280) 与 `load_incremental_data`(L278) |
| `InitCtaMockerTwiceReplacesPreviousContext` L79 | `WtBtRunner.cpp:269-273,282` | `id2 == id1+1`（static atomic 每次构造 +1）；槽位指向新实例；`sel_mocker()` 仍 nullptr（L82-83 只换 CTA） |
| `InitHftMockerWiresSlotAndIsRepeatable` L93 | `WtBtPorter.cpp:320-323`、`WtBtRunner.cpp:285-297`、`HftMocker.cpp:114-118,120-133` | 同上，HFT 段 id 从 6000 起、`id2==id1+1`、`hft_mocker()->id()==id2` |
| `EnumPositionWithWiredMockerStillTerminates` L106 | `WtBtPorter.cpp:589-603`、`CtaMocker.cpp:1241-1249` | 空 `_pos_map` ⇒ `enum_position` 不回调，L602 的终止标记仍必须恰好 1 次：code=""、qty=0.0、isLast=true |
| `CtaPositionGetterRunsMockerOnEmptyBook` L119 | `WtBtPorter.cpp:614-...`、`CtaMocker.cpp:1999-2008` | 走真实 `stra_get_position`（非 q06 的 nullptr 兜底）：`_sig_map`/`_pos_map` 均无该 code ⇒ `bOnlyValid` 两个取值都是 0.0 |
| `HftPositionAndUndoneAreZeroOnEmptyBook` L130 | `WtBtPorter.cpp:1470-1476,1519-1525`、`HftMocker.cpp:899-907,533-545` | `_pos_map[code]` 新建零值 PosInfo、`_orders` 空 ⇒ position(true/false)、undone 全 0.0 |
| `GetRawStdCodeIsEmptyWithoutRulesLoaded` L145 | `WtBtPorter.cpp:243-246`、`WtBtRunner.cpp:609-614`、`HisDataReplayer.cpp:2921-2931`、`CodeHelper.hpp:97-100,645-656` | 热点表为空 ⇒ `getRuleTag(ext)` 返回 ""⇒`_ruletag` 空⇒`hasRule()` 假⇒落到 L2930 `return ""`；推翻 q06 「依赖回放器代码表、进不去」的旧结论 |
| `ReplayerClockAndPriceStayZeroAfterSinkRegister` L152 | `WtBtRunner.h:531`、`WtBtRunner.cpp:590-602`、`WtBtPorter.cpp:243-246` | 接线只改 `_listener/_stra_name` ⇒ date/tdate/time/secs 与 `get_cur_price` 仍 0；`isAsync()` 仍 false |
| `DISABLED_OrderAndSlicePathsNeedLoadedStorage` L178 | `WtBtRunner.cpp:299-313`、`WtBtPorter.cpp:431-458,469-497,798-820,1560-1760` | 触发条件：`init_sel_mocker`→`register_task(trdtpl,session)` 需交易时段表；`cta_get_bars/cta_get_ticks`→`stra_get_bars` 走 K 线/Tick 存储（外层虽有 `catch(...)`，段错误不可捕获）；`hft_buy/sell/cancel*` 需 `get_commodity_info`，空存储下 `HftMocker.cpp:1107-1109` 直接 return，拿不到可核对的量价；`cta_log_text/*_save_userdata` 会往 `./outputs_bt` 落盘 |

## 3. 覆盖增量与仍进不去的面

- `ExpHftMocker.cpp`：本轮把 L14-111 的 11 个包装函数全部打到（q06 只有 `on_init`/构造）。
- `ExpCtaMocker.cpp` L113-204、`ExpSelMocker.cpp` L86-157：除 `on_bar`（未被 Exp 层覆写）外全部有真实调用点。
- `WtBtRunner.h`：新增 L122-125（`isAutoTrans`）与 L539（`isAsync`）两处此前无人触碰的内联；L202-205/217-224/510-539/548-596 用第二组输入重钉。
- `WtBtRunner.cpp`：L449-462、L626-630、L266-297（含 delete-旧实例分支）、L590-614（`get_raw_stdcode`）。
- `WtBtPorter.cpp`：L51-123（5 个 register 口）、L174-185、L243-246、L308-323、L589-603、L614+、L1470-1525 的 mocker 非空分支。
- 仍进不去（估计行数与验证方式）：
  `WtBtRunner.cpp` L437-447/463-507/509-583 ≈ 130 行（init/config 成功路径 + run/stop/release，需存储与线程，见 DISABLED 与本文件 BUG-R9-007）；
  `WtBtPorter.cpp` L431-497/798-1010/1560-1760 ≈ 300+ 行（K 线/Tick 切片、下单撤单、落盘类 userdata/log，均需 `get_commodity_info`/存储/输出目录；验证方式是让 `HisDataReplayer` 支持注入内存存储或提供 commodity 注册的公开接缝）；
  `Exp*Mocker` 的 `on_bar`/父类持仓结算路径 ≈ 40 行（要 `_strategy` 非空或 commodity 规则齐备）。
