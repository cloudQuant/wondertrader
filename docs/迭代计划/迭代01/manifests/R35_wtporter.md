# R35 manifest — WtPorter/WtRtRunner 第三遍（第八波 lane R35）

- 波次: 迭代01 第八波，lane R35（范围 = 任务简报指定的 `src/WtPorter/`，测试目标 `CoverageTests_porter`）
- 用例目录: 已登记目录 `src/TestUnits/coverage/q06_porter/`，前缀 `r35_`；**未新建目录、未改任何 CMakeLists、未改 `src/**` 生产代码**
- 交付文件:
  - `src/TestUnits/coverage/q06_porter/r35_porter_feed_init_test.cpp`（8 启用 + 1 DISABLED_）
  - `src/TestUnits/coverage/q06_porter/r35_porter_trail_getters_test.cpp`（7 启用）
  - `src/TestUnits/coverage/q06_porter/r35_porter_chart_spi_test.cpp`（6 启用）
- 门禁（`docs/迭代计划/迭代01/tools/lane_gate.sh`，真实构建+逐用例一进程）:
  - `lane_gate CoverageTests_porter --build --jobs 3 --only '^R35'` → **21/21 全 PASS，exit 0**
  - 收尾整目标 `lane_gate CoverageTests_porter --jobs 3` → **exit 5**：110 条枚举、86 PASS、24 CRASH；
    24 条红的 id 集合与本轮动手前的基线（89 条枚举那次运行）**逐 id diff 完全一致**（`/tmp/base_red.ids` vs
    `/tmp/lane_gate_CoverageTests_porter.aPCBTP/rc`）⇒ 本 lane 引入的回归 = 0，24 条全部是兄弟 lane 既有红（第五节）。
- 先普查后动手的证据：本轮动手前逐条读过 `q06_porter_capi/runner/expwrapper`、`r6_porter_*`、`r13_porter_*`
  三个 lane 的全部 TU 与 `manifests/{R6_porter_runner,R13_wtporter,q06_porters}.md`，
  只补「R13 第四节留白清单 ∩ R6 第六节留给主控的三件事」之外、且能在**单例 + 真实句柄**形状下确证安全的面。

## 〇、本轮为什么全部走 getRunner() 单例、绝不栈构造 WtRtRunner（硬结论，先说）

动手前的基线整目标运行就有 23 条 R6 族 SIGSEGV/SIGBUS（简报里只登记了 1 条既有红）。
用 macOS crash report（`~/Library/Logs/DiagnosticReports/CoverageTests_porter-2026-10-28-012210.ips`，
EXC_BAD_ACCESS, KERN_INVALID_ADDRESS at 0x10）定位：**故障线程栈顶 =
`R6PorterSilence_cta_dispatch..._Test::TestBody → WtRtRunner::~WtRtRunner → wtp::WtCtaEngine::~WtCtaEngine`**。
即 R6 族在用例体末尾析构栈上 `WtRtRunner r;` 时必崩（断言还没打印就没了）。
嫌疑位置 `WtCore/WtCtaEngine.cpp:41-47 if (_cfg) _cfg->release();`（`_cfg` 在 WtCtaEngine.h:92，
构造列表 :35-39 未初始化；静态单例的同名成员先被零初始化所以单例路径不崩）。
对照证据：同批引擎 `WtHftEngine.cpp:34`、`WtSelEngine.cpp:35` 都写了 `_cfg(NULL)`，唯独 CTA 引擎漏，
崩溃帧恰好在 `~WtCtaEngine` ⇒ 收敛度高；但**基类其余未初始化裸指针未逐一排除**，仍按嫌疑登记（BUG-R35-001）。
本 lane 因此：启用用例 0 条触碰栈实例；该复现收在
`R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody`（实测现象如上）。
R6 的 23 条是兄弟用例，**本 lane 一条未代改**，处置建议见第五节。

## 一、`r35_porter_feed_init_test.cpp`（8 启用 + 1 DISABLED_）

| 用例 id | 生产文件:行段 | 钉住的行为（断言面） |
| --- | --- | --- |
| `R35PorterFeed.FeedRawBarsFiresCachedBarFeederWithExactPointers` | `WtPorter.cpp:193-196` → `WtRtRunner.cpp:283-292`（:291 命中支）+ `:203-226` | feeder 收到 `obj==&feedObj`、`bars==&bar`、`count==2` 的**同一指针**；loadFinalHisBars 返回 true 且 loader 收到 `("SHFE.rb.2510","d1")` |
| `R35PorterFeed.FeedAdjFactorsFiresLoadedFactorFeeder` | `WtPorter.cpp:209-212` → `WtRtRunner.cpp:277-280` + `:265-275` | 既有红 `Q06PorterRunner.FeedRawBarsUsesCachedFeeder` 的崩点函数（BUG-Q06-004）在**先 loadAdjFactors 再 feed** 的正确顺序下走通：dates/factors 指针原样、count==3、stdCode 逐字 |
| `R35PorterFeed.LoadAllAdjFactorsRequestsEmptyCodeThenFeedsPerCode` | `WtRtRunner.cpp:253-263`（:262 固定传 `""`）+ `WtPorter.cpp:209-212` | loader 入参精确空串；随后 feed_adj_factors 以另一 code 打到同一缓存 feeder，count==2 |
| `R35PorterFeed.LaterLoadReplacesCachedBarFeeder` | `WtRtRunner.cpp:210/235`（`_feeder_bars = cb` 两处覆盖写）+ `:228-251` | 第二次 load 后 `feed_raw_bars` 只打到 feeder_b（1 次），feeder_a 恒 0 次；period 串 "m1"→"m5" 逐次对 |
| `R35PorterFeed.RegisterExtDataLoaderReplacesAllLoaderSlots` | `WtRtRunner.h:242-247`（内联三槽赋值） | 换入第二组 loader 后旧 loader 计数不再增长（覆盖语义，不是追加） |
| `R35PorterFeed.RegFactoriesReturnFalseForAbsentDirectory` | `WtPorter.cpp:352-355/364-367/376-379/388-391` → `WtRtRunner.cpp:1168-1186` → `CtaStrategyMgr.cpp:30-36` 等四守卫 | 四个导出入口把 `loadFactories` 的 false 原样带回（R13 留白表「工厂注册 4 个 ≈16 行」收掉） |
| `R35PorterFeed.RegFactoriesReturnTrueForExistingEmptyDirectory` | `CtaStrategyMgr.cpp:41-93`、`SelStrategyMgr.cpp`、`HftStrategyMgr.cpp`、`WtExecuterFactory.cpp:15-54` 的 `return true` 收尾 | 真实存在的空目录（系统临时目录建/删，仓内零落盘）⇒ 四个入口恒 true，证明上一条的 false 不是恒 false 桩 |
| `R35PorterFeed.InitPorterStampsInstDirOnceAndSecondCallIsNoOp` | `WtPorter.cpp:239-249`（一次性门 :241-244）+ `WtRtRunner.cpp:113-126` | 首次 `init_porter(垃圾内容,false,"")` 后 `WtHelper::getInstDir()==getBinDir()` 且以 `/` 结尾；放哨兵值再调一次，哨兵原样 ⇒ `inited` 门生效。垃圾内容走 `WTSLogger.cpp:211-215` 的 cfg==NULL 早退（有效 JSON 缺 root 会 throw :239-242，已避开） |
| `R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody` | `WtRtRunner.h:841` → `WtCtaEngine.cpp:41-47` | 实测现象：打开即 SIGSEGV（crash report 见第〇节）；R6 族 23 条红同形 |

## 二、`r35_porter_trail_getters_test.cpp`（7 启用）

| 用例 id | 生产文件:行段 | 钉住的行为 |
| --- | --- | --- |
| `R35PorterTrailGetters.CtaTrailGettersOnVirginContextAreExactlyZero` | `WtPorter.cpp:595-602/615-622/763-770/782-789/801-808/820-827` → `CtaStraBaseCtx.cpp:1761-1821/1788-1795/1897-1932` | 7 个空白上下文读数逐字 0（含未知/空 openTag 两支），CTA init/bar 槽 0 次 |
| `R35PorterTrailGetters.CtaDetailProfitAcceptsEveryFlagAndReturnsExactZero` | `WtPorter.cpp:637-644` → `CtaStraBaseCtx.cpp:1935-1964`（switch {0,1,-1,2,-2} 全在 find-miss 之后） | flag∈{0,1,2,-1,-2,3,99} 七个值恒精确 0.0 |
| `R35PorterTrailGetters.CtaLastEnterTagRealHandleIsBlankStringUnknownIsNull` | `WtPorter.cpp:839-846`（拒绝支 `return 0`=NULL，:843）→ `CtaStraBaseCtx.cpp:1774-1780`（真句柄支 `""`） | 「非空指针且 STREQ ""」只有真句柄支成立 ⇒ 本文件对「真分支被走到」最硬的区分断言（NULL-vs-"" 现状即 BUG-Q06-001，未重复登记） |
| `R35PorterTrailGetters.SelTrailGettersOnVirginContextAreExactlyZero` | `WtPorter.cpp:1474-1481/1572-1579/1591-1598/1610-1617` → `SelStraBaseCtx.cpp:1108-1143/1206` | SEL 四个 R13 清单外读数逐字 0 |
| `R35PorterTrailGetters.SelLastEnterTagRealHandleIsBlankStringUnknownIsNull` | `WtPorter.cpp:1629-1636` → `SelStraBaseCtx.cpp:1094-1106` | 同上，SEL 族 |
| `R35PorterTrailGetters.CtaGetAllPositionEmitsTerminatorExactlyOnce` | `WtPorter.cpp:673-687`（真句柄支 :682-686）→ `CtaStraBaseCtx.cpp:1041-1072` | 空白上下文枚举=终结符 1 次 `("",0,true)`、内层 0 行、六个派发槽全 0（CTA 族第一次；SEL 同位置是 R13 的） |
| `R35PorterTrailGetters.TrailReadersArePerContextNotSharedAcrossTwoCtas` | `WtPorter.cpp:951-976` + `CtaStraBaseCtx.cpp:58-70/1746-1759` | A 写的用户数据 B 读回 defVal；B 的时间线读数与 entertag 不受 A 影响（提到静态即红） |

## 三、`r35_porter_chart_spi_test.cpp`（6 启用）

| 用例 id | 生产文件:行段 | 钉住的行为 |
| --- | --- | --- |
| `R35PorterChart.SubBarEventsDispatchesNothing` | `WtPorter.cpp:1002-1009` → `CtaStraBaseCtx.cpp:1675-1680`（只写 `_kline_tags[key]._notify`） | 三次订阅（同键两次）后六个派发槽恒 0、上下文仍在表（R13 留白表「cta_sub_bar_events 可取未做」收掉） |
| `R35PorterChart.IndexLineFollowsRegistrationOrder` | `WtPorter.cpp:1054-1061/1072-1079` → `CtaStraBaseCtx.cpp:1991-2012` | 未注册→false（:2000-2004）、注册后→true、同指标加第二条线→true、野指标名→false 五段次序 |
| `R35PorterChart.IndexBaselineFollowsRegistrationOrder` | `WtPorter.cpp:1090-1097` → `CtaStraBaseCtx.cpp:2014-2027` | baseline 入口同形 false/true/false |
| `R35PorterChart.ChartWritesRefusedOutsideScheduleWithoutTouchingEngine` | `WtPorter.cpp:1108-1115/1037-1044` → `CtaStraBaseCtx.cpp:2029-2032/1974-1977`（`!_is_in_schedule` 守卫） | 线已注册仍 false ⇒ false 只能来自守卫；add_chart_mark 非调度期 0 派发、守卫后状态完好（同线再注册 true）。`_is_in_schedule` 置位点 grep 实测仅 `CtaStraBaseCtx.cpp:939`，构造 :66 置 false |
| `R35PorterChart.SetChartKlineIsPureLocalState` | `WtPorter.cpp:1019-1026` → `CtaStraBaseCtx.cpp:1966-1970` | 两次设图 0 派发、句柄仍可查、后续指标注册次序不受影响（无暗通道） |
| `R35PorterExpParserSpi.RegisterSpiWithLiveSinkPullsBaseDataMgrOnce` | `ExpParser.cpp:98-106`（:104-105 命中支）+ `:26-91` 生命周期对照 | 非空 sink⇒`getBaseDataMgr()` 恰好 1 次；随后 `registerSpi(NULL)`+全生命周期不再拉取（对照 R13 的 NULL 支）；evt 槽 4 次（init/connect/disconnect/release）、sub 槽 3 次（2 订 1 退）、id 恒 `r35_spi_parser`、sink 的 quote/symbol/log 全 0 |

R13 留白表「图表/指标族 6 个 ≈45 行」与「ExpParser registerSpi(非空) 2 行」两块本轮收掉。

## 四、留白面（本轮仍未拿下，带原因与行数量级）

| 面 | 位置 | 量级 | 为什么留 |
| --- | --- | --- | --- |
| `config()` 主体 + `initEngine/initDataMgr/initParsers/initTraders/initExecuters/init*Strategies/initEvtNotifier` | `WtRtRunner.cpp:489-714, 716-896, 898-1089` | ≈ 500 行 | 需要「能在进程内跑通的 dtcfg 夹具」接缝（R13 第四节 2) 原样成立）：`_config/cfgBF` 无判空即崩（BUG-R6-001/002 已登记），且 `initEngine`→`WtEngine::init`+`_data_mgr.init` 起 ticker/store。这些是 **private**，只能经 `config()` 进，没有接缝就进不去。未使用任何布局镜像/type-punning（本轮禁用手法） |
| `run()/release()` 线程面 | `WtRtRunner.cpp:1091-1124, 1141-1155` | ≈ 45 行 | 硬约束 6：真线程+信号钩子；`release` 里 `WTSLogger::stop()` 是进程级副作用（BUG-R6-006） |
| 引擎直取 14 getter、历史切片 9 入口、下单/仓位 11 入口、HFT 绑通道族 | `WtPorter.cpp:420-568/743/857-905/1215-1421/1668-1732/1797-1983/2027-2212` | ≈ 270 行 | 全部依赖未 init 的 `_engine/_data_mgr/_hot_mgr/_trader`，SIGSEGV 类，R6/R13/q06 已逐族 DISABLED_ 登记，本轮不重复、不绕开 |
| `get_raw_stdcode` / `feed_*` 的未注册崩支 | `WtPorter.cpp:193-212`+`WtRtRunner.cpp:277/361` | ≈ 10 行 | BUG-Q06-002/004、BUG-R6-009 已登记。本轮把**正确顺序**收进启用用例（第一节表 1-3），崩支保持既有登记不动 |
| `Exp*Context::on_init/on_session_end`、HFT 成交族非守卫段 | `ExpCtaContext.cpp:48-58/81-88` 等 | ≈ 46 行 | R13 第四节 3) 原样：无条件 `create_directories`/落文件 + `_trader`/`_ud_modified` 未初始化 |
| `init_porter` 的 genDir 效果 | `WtRtRunner.cpp:125` | 1 行 | `WtHelper` 只有 `setGenerateDir`（WtHelper.h:132）**没有 getter**，`_gen_dir` 的可见 getter（getBaseDir 等）第一次调用就真建目录（WtHelper.cpp:55-97），与「不落盘」约束冲突 ⇒ 本轮只钉一次性门与 instDir |

## 五、与 R6/R13/Q06 的分工 + 交给主控的既有红

- **分工**：q06=「ctx==NULL 拒绝支 + register/load 族」；R6=「WtRtRunner 生命周期/dispatch 静默族（栈实例形状）」；
  R13=「真实句柄的 CTA/SEL/HFT 只读族 + Exp* 转发层」。本轮 R35=「导出层尾差」：
  feed 正确顺序链、reg_*_factories 转发双向、init_porter 一次性门、CTA/SEL 时间线 13 入口、
  图表/指标族、`cta_get_all_position` 真句柄支、`ExpParser::registerSpi` 命中支。
  与三者零用例重叠（断言面逐条比对过 R6/R13/q06 manifest 的第二/三节表格）。
- **兄弟 lane 既有红 24 条（本轮未代改，逐 id 与基线一致）**：
  1. `Q06PorterRunner.FeedRawBarsUsesCachedFeeder` —— 简报已登记的既有红（BUG-Q06-004），不动。
  2. **R6 族 23 条**（`R6PorterSilence.* ×7、R6PorterReregister.* ×4、R6PorterFactories.* ×5、
     R6PorterLifecycle.* ×3、R6PorterLogRoute.* ×1、R6PorterPassthrough.* ×1、
     R6PorterExtExecuter/R6PorterExtParserQuote ×2）—— 本轮实测现象：全部 SIGSEGV(sig=11)，
     其中 `R6PorterLifecycle.init_with_gen_dir_...` 是 SIGBUS(sig=10)；崩溃发生在用例体结束、
     析构栈上 `WtRtRunner r;` 时（crash report 栈顶 `WtCtaEngine::~WtCtaEngine`）。
     其中 `R6PorterSilence.registering_sel_slots_leaves_cta_and_hft_silent` 崩溃前还留有一处断言面
     （`r6_porter_dispatch_silence_test.cpp:198`，`g_p.sel_init` 实际 1）——那是「静默」断言被别的实例串扰，
     属 BUG-R6-007 家族，一并交主控裁定。
     **建议处置**（供主控选一，本 lane 不执行）：R6 的 porter 侧用例整体改走 `getRunner()` 单例，
     或给 `WtCtaEngine/WtHftEngine/WtSelEngine::WtXxxEngine()` 构造列表补 `_cfg(NULL)`（后者动 `src/**`，需产品侧评审）。

## 六、数字（单对象 `llvm-cov report CoverageTests_porter`，动手前/后）

```
文件名            Lines   Missed(前)  Missed(后)  差
WtPorter.cpp       848      381         322       -59
WtRtRunner.cpp     938      610         583       -27
```
（口径：`/opt/homebrew/opt/llvm/bin/llvm-cov report <bin> -instr-profile=<lane_gate 判决目录 profraw 合并>`，
前后两次分别用基线 89 条与收尾 110 条的 profraw；崩溃用例的 profraw 天然缺失，两口径同缺，差值可信。
按任务要求不报百分比。）

## 七、缺陷清单

`docs/迭代计划/迭代01/bugs/R35_wtporter.md`：1 条（BUG-R35-001，Blocker，待审核）。
既有 BUG-Q06-001/002/004、BUG-R6-001/006/007/009、BUG-R13-001/002/004 本轮只引用不重复登记。
