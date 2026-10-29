# 12_porters 候选审阅

## 范围与证据口径

本报告对 `/tmp/wondertrader-review-20261029/groups/12_porters.json` 中 27 条候选逐项核对生产实现、公开头文件/调用路径及当前测试前置与断言。只新增本报告和配套 triage JSON；未修改原候选、测试或生产文件，未运行测试、构建或网络操作。所有结论均标记 `STATIC`，因此不声称本轮有本地复现或 PASS 证据。测试文件行号只说明测试内容，不代表测试已运行。

## 分类汇总

| 判定 | 数量 | 含义 |
|---|---:|---|
| PRODUCT | 12 | 有效输入、对象生命周期或公开接口契约可支持产品缺陷结论 |
| TEST | 1 | 候选测试绕过生产可达前置，当前产品缺陷主张不成立 |
| DUPLICATE | 4 | 同一根因已有主记录，不重复计数 |
| UNDECIDED | 9 | 需要确认有效输入、调用顺序、并发或生命周期契约 |
| NOT_BUG | 1 | 无效句柄的空值行为没有违反已知公开承诺 |
| **合计** | **27** | |

## 逐条结论

### R125

- **BUG-R125-001 — UNDECIDED / STATIC。** `WtStraDtSel::on_schedule` 对当前无 bars 提前返回（`src/WtSelStraFact/WtStraDtSel.cpp:121-150`），但当前策略上下文契约未明确要求其他标的继续处理，唯一关联测试只覆盖单标的（`src/TestUnits/coverage/r125_selfthft/r125_sel_test.cpp:133-158`）。先确认多标的缺 bar 语义；若要求容错，再测试空标的后仍调度有 bar 标的。

### R134

- **BUG-R134-001 — DUPLICATE / STATIC，归并 BUG-R9-001。** wrapper 的 once 标志不置位（`src/WtBtPorter/WtBtPorter.cpp:172-185`）；现有缺文件用例不能验证成功重配（`src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:376-386`）。
- **BUG-R134-002 — DUPLICATE / STATIC，归并 BUG-R9-001。** runner `_inited` 初始化为 false 且成功配置未置位（`src/WtBtPorter/WtBtRunner.cpp:47-89,449-455`）；重复配置测试目前禁用（`src/TestUnits/coverage/q06_btporter/r205_btporter_residual_test.cpp:687-722`）。
- **BUG-R134-003 — PRODUCT / STATIC。** config 路由没有调用 SEL 自动初始化；遗留函数读错 `strategies.cta`（`src/WtPorter/WtRtRunner.cpp:708-711,745-789`），而现有 bars 测试是显式创建 SEL context，不覆盖配置加载（`src/TestUnits/coverage/q06_porter/r134_porter_bars_test.cpp:220-240`）。按 SEL 路由读取 `strategies.sel` 并验证策略初始化回调。
- **BUG-R134-004 — PRODUCT / STATIC。** 公共接口接收 tickLoader，但 runner 签名/存储路径未保留该回调（`src/WtPorter/WtPorter.h:93-103`、`src/WtPorter/WtRtRunner.h:234-247`）；测试注册 NULL loader（`src/TestUnits/coverage/q06_porter/r134_porter_bars_test.cpp:171-190`）。补上 tick 请求至 feeder 的完整路径，或撤销并明确不支持的 API 承诺。

### R13 Porter

- **BUG-R13-001 — UNDECIDED / STATIC。** 未找到 trader 时仍返回 context ID，而 `_trader` 未初始化；相关读者会解引用它（`src/WtPorter/WtRtRunner.cpp:321-335`、`src/WtCore/HftStraBaseCtx.h:670-674`、`src/WtCore/HftStraBaseCtx.cpp:641-679`）。需先确定无 trader context 是否允许；若不允许，创建失败要回滚并返回失败句柄。未初始化成员与 BUG-R42-002 重叠。
- **BUG-R13-004 — PRODUCT / STATIC。** 公开示例 `d1/w1/m1` 没有被 parser 识别，未知输入被成功创建为 `TPT_None`，而 scheduler 会跳过该类型（`src/WtPorter/WtPorter.h:686-700`、`src/WtPorter/WtRtRunner.cpp:338-358`、`src/WtCore/WtSelEngine.cpp:260-265,325-333`）。现有测试只验证已识别 token 注册（`src/TestUnits/coverage/q06_porter/r13_porter_sel_hft_export_test.cpp:324-345`）。统一文档、解析和调度周期，并为未知周期提供明确失败行为。
- **BUG-R13-005 — UNDECIDED / STATIC。** buy/sell 返回函数级 static string（`src/WtPorter/WtPorter.cpp:2157-2217`），并发调用可能数据竞争，连续同线程调用会覆盖旧内容；但公开契约未说明并发支持及指针有效期，也无启用测试。先明确这两项契约，再按调用者缓冲/线程局部方案验收。
- **BUG-R13-006 — UNDECIDED / STATIC。** create 把 context 保存在 engine map，头文件没有 destroy，release 不清该 map（`src/WtPorter/WtPorter.cpp:404-407,1135-1138,1652-1655`、`src/WtPorter/WtRtRunner.cpp:1141-1155`）。但是否支持动态销毁/重载尚不明确；先确定生命周期，之后补 destroy/解绑或写清 runner/进程生命周期约束。现有 userdata 隔离测试不证明释放。
- **BUG-R13-007 — DUPLICATE / STATIC，write_log 部分归并 Q06-006。** `write_log` 行与 Q06-006 完全相同（`src/WtPorter/WtPorter.cpp:333-343`）；另一个 cfgfile NULL 候选没有契约证明它是有效输入，不应据此把必填字符串的非法 NULL 泛化为产品缺陷。

### R173 / R35 / R46

- **BUG-R173-005 — DUPLICATE / STATIC，归并 BUG-R46-001。** `_hft_mocker/_exec_mocker/_loader_auto_trans` 构造遗漏与 R46 主记录同根因（`src/WtBtPorter/WtBtRunner.cpp:47-89`、`src/WtBtPorter/WtBtRunner.h:633-638`）。
- **BUG-R35-001 — PRODUCT / STATIC。** `WtCtaEngine` 构造未初始化 `_cfg`，析构读取该裸指针；按值成员使普通未配置 `WtRtRunner` 可走到此析构路径（`src/WtCore/WtCtaEngine.cpp:34-47`、`src/WtPorter/WtRtRunner.h:841-849`）。在构造时初始化 `_cfg`，启用未 init 的普通对象安全析构回归。既有 BUG-R144-003 已归并到此主编号。
- **BUG-R46-001 — PRODUCT / STATIC。** 实盘 runner `_engine` 缺确定初值，公开 `getEngine()` 可在 config 前读取；BT runner 中与 Q06-003、R9-006、R173-005 重复的指针字段及 `WtCtaEngine::_cfg` 分别归到既有主记录（`src/WtPorter/WtRtRunner.cpp:55-100`、`src/WtPorter/WtRtRunner.h:321-325,822-853`）。初始化剩余可读成员；测试使用普通构造对象，不让函数静态 singleton 的零填充掩盖问题。
- **BUG-R46-002 — PRODUCT / STATIC。** `config()` 忽略 `initEngine()` 失败；缺 `env` 时 initEngine 返回 false，外层仍可走到 true 返回（`src/WtPorter/WtRtRunner.cpp:583-588,840-842,708-713`）。现有测试提供其他有效配置字段并仅省 env（`src/TestUnits/coverage/q06_porter/r46_porter_config_test.cpp:237-253`），前置足以支持该主张。传播失败并断言 engine 未就绪。
- **BUG-R46-003 — UNDECIDED / STATIC。** 缺少 `replayer` 或 `env` 会在 config 下游空指针解引用（`src/WtBtPorter/WtBtRunner.cpp:457-471`），但配置 schema 的必填段和错误返回契约未确立；候选测试仍禁用且构造不完整配置（`src/TestUnits/coverage/q06_btporter/r46_btporter_runner_test.cpp:159-179`）。确认 schema 后验证配置失败，不把崩溃作为测试预期。

### R69

- **BUG-R69-001 — PRODUCT / STATIC。** `WTSHotMgr::getHotTags()` 的函数内 static 缓存跨实例且不随规则加载更新（`src/WTSTools/WTSHotMgr.cpp:148-159`），runner 会使用此标签集合（`src/WtPorter/WtRtRunner.cpp:562-580`）。已有定向回归禁用（`src/TestUnits/coverage/q06_porter/r69_porter_runner_config_test.cpp:300-324`）。改为实例数据/实例缓存失效，并测试 A/B 实例及后加载规则。
- **BUG-R69-003 — UNDECIDED / STATIC。** config 忽略 `initDataMgr()` 结果，内部又忽略 `_data_mgr.init()` 结果（`src/WtPorter/WtRtRunner.cpp:583-588,884-895`、`src/WtCore/WtDtMgr.cpp:89-101`）。但现有用例是空 data 对象且无 store，并非缺 `data` 段（`src/TestUnits/coverage/q06_porter/r69_porter_runner_config_test.cpp:250-275`）；外部 loader/无 reader 模式是否可用仍需契约确认。

### R9 回测 Porter

- **BUG-R9-001 — PRODUCT / STATIC。** wrapper once 守卫和 runner `_inited` 守卫成功后均未置位，重复配置会继续覆盖状态（`src/WtBtPorter/WtBtPorter.cpp:172-185`、`src/WtBtPorter/WtBtRunner.cpp:47-89,449-507`）。现有缺文件测试不覆盖成功重复调用（`src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:376-386`）。明确状态机，并用有效配置调用两次验收；BUG-R134-001/002 合并至此。
- **BUG-R9-003 — PRODUCT / STATIC。** 回放器传 base period + times（`src/WtBtCore/HisDataReplayer.cpp:2034-2044,2065-2070`）；回测 wrapper 对 d 以外均映射 m，而实盘 wrapper 保留 h/f（`src/WtBtPorter/ExpHftMocker.cpp:14-27`、`src/WtPorter/ExpHftContext.cpp:29-43`）。现有测试用 `m1/d1` 字符串，绕开生产参数形状（`src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:410-435`）。修正测试输入为 `h,1`、`f,1`、`m,5`、`d,3` 等实际形状。建议父任务将 `R9Wired.HftBarPeriodRenamedByTimes` 过滤器作为修复目标。
- **BUG-R9-004 — TEST / STATIC。** 测试直接把 NULL 传给 `on_bar_close`，绕过 CtaMocker/SelMocker `on_bar` null guard；正常 replay 路径以有效 bar 调 close（`src/WtBtCore/CtaMocker.cpp:945-960`、`src/WtBtCore/SelMocker.cpp:382-398`、`src/WtBtCore/HisDataReplayer.cpp:2029-2044,2065-2069`）。修改测试走真实入口并断言不派发，或将其限定为 wrapper 直接调用特征测试。建议父任务使用 `R9Wired.CtaBarClosePassesPeriodUnchanged` 过滤器检查。
- **BUG-R9-007 — PRODUCT / STATIC。** `_running` 在线程 lambda 内才置真；run 后立即 stop 可先观察到 false 并跳过 replayer stop，仍只 join 线程（`src/WtBtPorter/WtBtRunner.cpp:509-560`）。公共 stop_backtest 承诺停止异步回测（`src/WtBtPorter/WtBtPorter.cpp:219-226`）；现有测试只是 `EXPECT_TRUE(true)`（`src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:740-750`）。增加有 barrier 的 run-then-stop 回归，并同步状态读写。

### Q06 Porter

- **Q06-001 — NOT_BUG / STATIC。** 只涉及无效句柄/未注册 mocker；未发现有效 context 下必须非空的 API 保证，当前 live/backtest 测试也接受 NULL 或空串（`src/TestUnits/coverage/q06_porter/q06_porter_capi_test.cpp:139-146`、`src/TestUnits/coverage/q06_btporter/q06_btporter_capi_test.cpp:112-137`）。建议统一文档化空值策略。
- **Q06-002 — UNDECIDED / STATIC。** 多个 C 导出在 engine 未就绪时直接解引用，engine 只在初始化后设定（`src/WtPorter/WtPorter.cpp:857-907,1215-1239,1409-1423,1743-1781`、`src/WtPorter/WtRtRunner.cpp:361-366`）。头文件规定 init/config 顺序但未清楚约束成功 config 前的查询；现有 placeholder 无调用（`src/TestUnits/coverage/q06_porter/q06_porter_capi_test.cpp:273-287`）。确认调用前置后决定入口 guard 和错误返回。
- **Q06-004 — PRODUCT / STATIC。** raw loader 注册路径设置 raw feeder/回调，而 `feedRawBars` 检查 final loader 槽（`src/WtPorter/WtRtRunner.cpp:203-250,277-292`）。现有 raw-only 用例前置有效且针对该错配（`src/TestUnits/coverage/q06_porter/q06_porter_runner_test.cpp:628-642`），但本轮未运行。主任务可用过滤器 `Q06PorterRunner.FeedRawBarsUsesRawOnlyLoader` 定向验证。
- **Q06-005 — UNDECIDED / STATIC。** 多个 backtest 导出忽略 CtxHandler 并用 runner 单 mocker slot（`src/WtBtPorter/WtBtPorter.cpp:589,729,913`、`src/WtBtPorter/WtBtRunner.cpp:266-309,449-505`）；句柄类型描述为 context 标识（`src/WtBtPorter/PorterDefs.h:28-31`），但多实例隔离支持未确认。先决定单/多实例契约，再测试多实例路由或单实例下拒绝非当前句柄。
- **Q06-006 — UNDECIDED / STATIC。** live/backtest `write_log` 对 `catName` 调 `strlen`（`src/WtPorter/WtPorter.cpp:333-343`、`src/WtBtPorter/WtBtPorter.cpp:285-295`），头文件未标明可空，测试只覆盖空串和命名值（`src/TestUnits/coverage/q06_porter/q06_porter_capi_test.cpp:95-106`、`src/TestUnits/coverage/q06_btporter/q06_btporter_capi_test.cpp:83-90`）。先决定 NULL 是否有效输入，再统一两侧处理；BUG-R13-007 对此不另计数。
- **Q06-007 — PRODUCT / STATIC。** 回测 runner 发 READY=1000，但回测公开 defs 缺通道常量，实盘 defs 有 READY/LOST（`src/WtBtPorter/WtBtRunner.cpp:413-417`、`src/WtBtPorter/PorterDefs.h:33-41`、`src/WtPorter/PorterDefs.h:41-46`）；现有测试用裸数字（`src/TestUnits/coverage/q06_btporter/q06_btporter_runner_mocker_test.cpp:417-420`）。公开回测 READY 符号并用仅包含 public header 的消费者编译验证。

## 交接给父任务

本轮没有编辑或运行测试。已识别的测试前置修正及可用过滤器：

1. `src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:414-435`：周期重命名测试输入改成实际 dispatcher 的 base+times；过滤器 `R9Wired.HftBarPeriodRenamedByTimes`。
2. `src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp:599-602`：NULL close 测试需从 `cta.on_bar(..., NULL)` 进入正常 guard，或标注为 wrapper 直调特征；过滤器 `R9Wired.CtaBarClosePassesPeriodUnchanged`。
3. `src/TestUnits/coverage/q06_porter/q06_porter_runner_test.cpp:628-642`：raw-only loader 回归前置正确，可作 Q06-004 修复验收；过滤器 `Q06PorterRunner.FeedRawBarsUsesRawOnlyLoader`。
