# 12_porters 第二轮复核

## 结论

本组 10 项：PRODUCT 4，DUPLICATE 2，NOT_BUG 4；TEST、NEEDS_DECISION、EXTERNAL_BLOCKED、LOCAL_INCOMPLETE 均为 0。产品源码与测试文件本轮只读；测试修改建议已按精确路径、用例、改法、target/filter 提交主代理。所有产品缺陷均为 STATIC：本轮没有运行触发缺陷的测试，也没有将控制用例通过当作缺陷复现。

五个安全控制用例按独立进程运行，每个过滤器各运行 1 个测试，均为 return code 0、failures=0、errors=0、15 秒内完成。使用 Anaconda base 环境运行 evidence/12_porters/run_safe_tests.py；目标 SHA、命令、隔离 cwd、profile/XML 和原始 stdout/stderr 均在 evidence/12_porters/safe-tests.json 与 runs/ 下。危险 disabled 用例没有启用。

## 逐项裁定

### BUG-R125-001 — PRODUCT / STATIC

WtStraDtSel 将 codes 配置拆成多个代码并在 on_init 中全部订阅。on_schedule 对每只代码取 bars，但遇到 NULL 或空切片就从整个函数 return，后续代码不会处理。由于策略输入明确支持多代码，此早退把一个代码的缺片扩散成其他代码本轮不运行，按逐代码调度修复：释放已获取的空切片后 continue。现有 ScheduleEarlyReturns 是单代码测试，不能覆盖该影响；InitSubscribesAllCodes 只覆盖订阅数量。两个用例均已安全通过，但未复现跨代码短路。请求新增双代码、缺片首代码加有效次代码的回归。

证据：WtStraDtSel.cpp:88-95、104-111、121-150；ISelStraCtx.h:309-316；r125_sel_test.cpp:116-158。运行记录：runs/r125_schedule_early 与 runs/r125_multicode_subscription。

### BUG-R13-001 — DUPLICATE → BUG-R42-002 / STATIC

公开 create_hft_context 接收 trader 名称。runner 先把 HFT context 放进 engine map，再查找 trader；查找失败只记日志，仍返回 context ID。HftStraBaseCtx 构造没有初始化 _trader，策略入口随后直接解引用。未初始化 _trader 与未绑定调用安全性已由当前主编号 BUG-R42-002 覆盖，因此本条不重复计数；新增证据补明 WtRtRunner 的公开绑定失败路径仍返回并保留句柄，应并入 canonical 验收：未知 trader 返回失败且不在 map 留 context。已注册 trader 正常路径在测试中有覆盖；未绑定指针的 disabled 测试不执行。

证据：WtPorter.h:995-1005；WtRtRunner.cpp:321-335、791-833；HftStraBaseCtx.h:670-674、HftStraBaseCtx.cpp:41-48、60-63、325-337；ExpHftContext.cpp:50-69；缺陷裁定总表.json:2608-2623；r13_porter_sel_hft_export_test.cpp:257-275、431-455。已请求将 disabled UB 用例改为安全失败契约测试。

### BUG-R13-005 — PRODUCT / STATIC

hft_buy 和 hft_sell 都以函数级 static std::string 存储返回订单 ID；hft_cancel_all 已使用 thread_local。HFT 行情 tick 与 TraderAdapter 的委托/成交回报走直接派发路径，没有共同串行队列。不同回调线程并发调用买卖接口时，共享返回缓冲存在数据竞争。现有 R124 用例顺序调用并立即复制，未覆盖并发。本轮没有并发复现，TSan 未运行。建议 buy/sell 使用 thread_local，并明确返回指针在同线程下一次相同调用前有效；新增双线程 fake trader 回归及 TSan 变体。

证据：WtPorter.cpp:2121-2139、2157-2175、2193-2211；WtPorter.h:1232-1260；WtHftEngine.cpp:221-253；TraderAdapter.cpp:2133-2135、2321-2322；r124_porter_hft_orders_test.cpp:199-228。

### BUG-R13-006 — NOT_BUG

引擎 map 确实持有 context，release() 不清 map；但本条主张是没有单独 destroy 就必然导致产品泄漏。公开 release 文档将其定位为异步程序退出前的清理，且 release 后不能再用 Porter API；C API 没有运行期 context destroy 或热重载工作流。getRunner 是函数静态单例，三种引擎为 runner 的内嵌对象，进程退出时析构并释放 map 中的 shared_ptr。现有两 context 用户数据隔离测试不证明泄漏。故撤回“所有 create context 永久泄漏”的主张；该结论不覆盖未来若新增运行期热重载支持。

证据：WtPorter.h:218-224、995-1005；WtPorter.cpp:34-38、286-289；WtRtRunner.cpp:103-105、1141-1155；WtRtRunner.h:841-849；各引擎析构/release 与 map 持有位置见 triage JSON。若后续要求热重载，应另行设计 destroy，并在 HFT map 移除前从 trader sink 脱离。

### BUG-R46-003 — PRODUCT / STATIC

公开 config_backtest 接受配置文件或内容。WtBtRunner::config 只检查配置根对象，随后对 replayer 段调用 init，再无守卫地读取 env.mocker；HisDataReplayer::init 对 cfg 直接访问成员。可解析但缺少必需子段的外部配置会走空指针解引用。C ABI 返回 void，不要求改变签名：结构校验失败时记录配置错误、保持 runner 未就绪，且 run_backtest 拒绝执行。两个危险 disabled 用例未运行；建议修复后重写为安全 ready=false 断言，不以崩溃为预期。

证据：WtBtPorter.h:118-124、WtBtPorter.cpp:174-185；WtBtRunner.cpp:449-471；HisDataReplayer.cpp:171-189；r46_btporter_runner_test.cpp:159-179。

### BUG-R69-003 — PRODUCT / STATIC，范围收窄

本次只保留“用户显式配置的 store 插件无法加载，但 runner 仍报初始化成功”这一发生点。WtDtMgr::initStore/init 对无效 store module 返回 false；WtRtRunner::initDataMgr 丢弃该结果、打印 initialized 并返回 true，config 后续也不检查 initDataMgr 的结果。与此不同，data:{} 无 store 的现有用例确实通过，查询入口在 reader 为空时返回 NULL；该可选/无 reader 控制不在本条问题范围。此根因是内层 store 初始化失败被吞，与 BUG-R6-006 所列缺 data/env 时外层忽略 initDataMgr 失败不是同一发生点，不重复合并。

证据：WtRtRunner.cpp:489-491、583-588、884-895；WtDtMgr.cpp:50-102、277-281、378-399、421-447；r69_porter_runner_config_test.cpp:250-275；r42_dtmgr_test.cpp:52-80。无 store 与 manager 层 store failure 两个控制用例均通过，但 runner 层显式缺模块集成负例尚未运行。建议增设该集成测试，并保持 no-store 控制。

### Q06-001 — NOT_BUG

有效 context 没有相关持仓时，头文件承诺返回空字符串，基类实现也返回非 NULL 的空串。无效 context 时导出返回 NULL；公开接口没有承诺无效句柄必须与有效查询的空值表示相同。当前测试接受 NULL 或空串，不能证明违反有效输入合同。R35 的有效句柄/空持仓用例本轮通过，确认有效路径；这不是无效句柄合同的动态复现。

证据：WtPorter.h:553-562、WtPorter.cpp:839-845、CtaStraBaseCtx.cpp:1774-1784；q06_porter_capi_test.cpp:139-147、r35_porter_trail_getters_test.cpp:154-168。运行记录：runs/r35_entertag_contract。

### Q06-002 — DUPLICATE → BUG-R6-006

未就绪状态下多个导出直接解引用 getEngine，根因与 BUG-R6-006 相同：配置初始化失败没有传播为明确未就绪状态，随后运行/查询仍可能访问空 engine。主总表 BUG-R6-006 已有 config 失败传播与 run guard 的主修复，不重复计数；可把代表性 getter 纳入其验收。候选 disabled 用例主体只有 (void)0，不是实际测试，应改为安全门禁用例。

证据：WtPorter.cpp:857-907、1215-1239、1409-1423、1743-1781；WtRtRunner.cpp:838-895；缺陷裁定总表.json:8126-8153；q06_porter_capi_test.cpp:273-288。

### Q06-005 — NOT_BUG

CtxHandler 名称本身表示句柄，但回测 runner 对 CTA/SEL/HFT 分别只保留一个 mocker 指针；每次 init 都先删除并替换前一个对象。当前实现因此是单活动 mocker 模式，C ABI 使用当前 slot，不支持多个并行 mocker 按 handle 隔离。现有“无 mocker”测试的任意句柄只覆盖无实例中性返回，不能作为多实例路由证据；在单实例契约下忽略无效或被替换的旧句柄不是可证实的产品 bug。

证据：PorterDefs.h:27-31；WtBtPorter.h:141-176；WtBtRunner.h:505-524；WtBtRunner.cpp:266-313；WtBtPorter.cpp:354-360、558-564、574-581、729-736；q06_btporter_capi_test.cpp:112-137。

### Q06-006 — NOT_BUG

live/backtest write_log 都把 catName 当作 NUL 结尾字符串并用 strlen 区分默认类别与命名类别。头文件没有声明该字符串可为 NULL；backtest 注释明确空字符串使用默认分类，两侧测试都覆盖空串和命名分类。NULL 不等于被支持的空字符串，不按未声明的无效指针输入裁定为产品 bug。

证据：WtPorter.h:163-172、WtPorter.cpp:333-343；WtBtPorter.h:186-193、WtBtPorter.cpp:285-295；两侧 q06 C API 测试分别见 q06_porter_capi_test.cpp:95-106 和 q06_btporter_capi_test.cpp:83-90。

## 执行记录

以下 enabled 控制用例逐个独立运行，均 1/1 PASS：

| 目标 | 过滤器 | 结果 | 目标 SHA-256 |
|---|---|---|---|
| CoverageTests_selfthft | R125SelStrategy.ScheduleEarlyReturns | 1/1 PASS | 199edf7583f12426049163a78e74750837d56666ab4db71b5ed37f814906640f |
| CoverageTests_selfthft | R125SelStrategy.InitSubscribesAllCodes | 1/1 PASS | 199edf7583f12426049163a78e74750837d56666ab4db71b5ed37f814906640f |
| CoverageTests_porter | R69PorterRunnerConfig.MissingExecuterFileAndEmptyDataStillReturnTrue | 1/1 PASS | 2bdb9631c7b49b4b211d581fc069a7d604886cba4b304ad2e4ca5e9e090ab3c3 |
| CoverageTests_porter | R35PorterTrailGetters.CtaLastEnterTagRealHandleIsBlankUnknownAllowsNullOrEmpty | 1/1 PASS | 2bdb9631c7b49b4b211d581fc069a7d604886cba4b304ad2e4ca5e9e090ab3c3 |
| CoverageTests_wtcore | R42DtMgrTest.InitReportsStoreFailures | 1/1 PASS | 779ecd25f633ec0b11f2cf44f0afc9f2c4edcf438b9851f8ac00d418940c30ce |

每项原始命令、cwd、超时、XML、stdout/stderr、测试计数与 SHA 在 evidence/12_porters/safe-tests.json 及相应 runs/<case>/ 文件。R13 未绑定 trader、R46 缺段崩溃、并发竞态和 Q06 未就绪 getter 均没有运行危险或尚未修复的路径；没有执行构建或全量测试。
