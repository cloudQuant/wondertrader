# 04_core_engines：185 条第二轮核实

## 结论

本组复核 11 条：`PRODUCT` 9 条，`NOT_BUG` 1 条，`NEEDS_DECISION` 1 条；11 条的证据等级均为 `STATIC`。已按正常调用链、配置/持久化入口、对象所有权和 stop/release 顺序重新核对源码与现有测试，没有把上轮结论直接改写。产品候选的修复方向和验收状态均列在 triage 中。

本轮使用主代理刷新后的 `CoverageTests_wtcore`（SHA-256 `779ecd25f633ec0b11f2cf44f0afc9f2c4edcf438b9851f8ac00d418940c30ce`）运行 5 个安全的单用例正控，均 PASS。它们分别覆盖通知工作线程发布、风控不兼容插件加载、有效执行器插件、解析器管理器常规释放和 diff 执行器语法错误快照处理。这些用例没有复现下列缺陷，因此各条仍标 `STATIC`。每次运行均独立 cwd、LLVM profile 和 GTest XML，命令、return code、用例数、超时标志及输出保存在 `evidence/04_core_engines/selected-runs.json` 和对应 `runs/*/` 目录。

## 逐项结果

### BUG-R10-005 — PRODUCT

`EventNotifier::init` 检查 `create_server`，但从动态库取得 `regiter_callbacks` 后直接调用；插件缺少这一必需导出时会空函数指针调用。通知活动配置和插件库是运行时加载边界，故不依赖人工调用内部方法。见 `src/WtCore/EventNotifier.cpp:58-111,214-227`。现有 R160 发布测试通过，但只覆盖完整插件正常发布，不覆盖缺少注册回调的插件。订单通知 `retain` 无对应 `release` 的子项与 BUG-R41-003 重复，本条仅将它指向该 canonical，不重复计为新缺陷。

修复应校验所有必需导出，在调用前释放模块并令初始化返回失败；订单引用归还仍由 canonical 项处理。验收需验证缺 `create_server` 或缺 `regiter_callbacks` 时不启动 worker、无残留句柄，完整插件仍能发布且订单引用最终释放。已向主代理提出新增隔离缺导出 dylib 的 `R160EventNotifier.MissingRequiredCallbackExportFailsInit`。

### BUG-R211-001 — PRODUCT

当 `riskmon.active=true` 且模块缺失或 ABI 不兼容时，`init_riskmon` 失败；`WtEngine::init` 丢弃该结果，Runner 仍保存 engine 并继续配置，最终可进入 `run`，CTA 仅在 `_risk_mon` 非空时启动风险线程。也就是说，明确请求的风控模块失败会静默变为无风控运行。源码链见 `src/WtCore/WtEngine.cpp:285-316,1143-1184`、`src/WtRunner/WtRunner.cpp:153-160,365-408,578-585`、`src/WtCore/WtCtaEngine.cpp:109-112`。

现有 `R211Engine.IncompatibleRiskMonModuleIsRejected` 断言没有创建 riskmon，本轮 PASS；它未断言 Runner 配置失败，不能验收 fail-closed。修复需逐层传播初始化失败，并阻止失败 engine 进入 `run`。验收需覆盖 active 风控的缺失/不兼容和有效模块，以及不配置风控的既有分支。已请求主代理扩展 Runner 链路测试。

### BUG-R31-002 — PRODUCT

Runner 按 `strategies.hft` 配置扫描目录；CTA 与 HFT 插件共用 `createStrategyFact` 导出名，HFT loader 将 CTA 工厂按 `IHftStrategyFact` 使用并进入虚接口调用，缺少族别核验。配置扫描目录中的非本族动态库即可到达该类型错配路径。见 `src/WtRunner/WtRunner.cpp:283-342`、`src/WtCore/HftStrategyMgr.cpp:57-87`、`src/WtCore/CtaStrategyMgr.cpp:61-85`。已知会崩溃的 disabled 用例未运行。

修复方向是增加族别专用符号或 ABI 类型标记，在调用工厂前安全拒绝错族插件，并保持有效 HFT 插件兼容或定义迁移策略。验收需同时验证正确族加载和错族库安全拒绝。本轮已请求主代理增加族标记错配的安全拒绝测试。

### BUG-R31-003 — PRODUCT

`WtExecuterFactory::loadFactories` 检查插件导出符号后直接调用 creator，随后对结果调用 `getName`，没有处理 creator 返回 `NULL`。同仓 CTA/HFT 策略 manager 已对空 creator 结果做清理与跳过，说明“插件工厂创建失败”可在扩展边界安全处理。见 `src/WtCore/WtExecuterFactory.cpp:49-65`、`src/WtCore/CtaStrategyMgr.cpp:61-85`、`src/WtCore/HftStrategyMgr.cpp:61-85`。

本轮有效 factory dylib 正控 PASS，不覆盖空结果。修复需先检查返回值，再写入 factory map；NULL 时释放库且不登记条目。已请求主代理增加 `createExecFact` 返回 NULL 的隔离插件测试，并与有效插件同目录验证后者仍可用。

### BUG-R42-003 — PRODUCT

`ParserAdapterMgr::getAdapter` 返回公开的 `shared_ptr`，所以调用者可以在 `mgr.release()` 清空 map 后继续持有 adapter。manager release 会释放底层 parser API，但 `ParserAdapter::release` 未清空 `_parser_api`；保留句柄调用公开 `run()` 会通过非空检查后对已释放 API 调用 `connect`。见 `src/WtCore/ParserAdapter.cpp:214-235,378-424`、`src/WtCore/ParserAdapter.h:40-46,110-119`。常规 manager release 正控 PASS；测试不保留 shared pointer，post-release 的 `handleQuote` 守卫也不能证明 `run` 安全。该顺序由公开生命周期与 shared ownership 自然到达，不是人工二次 release 前提。

修复时应先清空成员 API 指针，再释放底层 API，并让终态 `run()` 返回 false；若支持重启，应由重新 init 创建新 API。已请求主代理补 retained handle 的 manager-release→run 用例；当前潜在 UAF 序列未执行。

### BUG-R81-002 — PRODUCT

`CtaStraBaseCtx::load_data` 从本地 `stradata/<strategy>.json` 恢复条件，将 `alg/action` 转成枚举后用作长度为 5 的名称数组下标；超范围持久化整数可越界。加载入口由策略 `on_init` 正常触发，旧文件损坏、手改或跨版本都能通过文件边界到达。见 `src/WtCore/CtaStraBaseCtx.cpp:32-48,409-447,585-615,839-865`、`src/Includes/WTSTypes.h:328-335`。

现有测试用子类把 `alg=99` 直接写入内存，并没有经 JSON 恢复；已知危险文件输入未运行。修复应在枚举转换/数组访问前校验字段类型和值域，拒绝坏记录且不得产生交易动作。已请求主代理将 `alg=99` 与 `action=99` 分别写入真实 JSON fixture 后经 `on_init` 恢复。

### BUG-N02-002 — PRODUCT

`WtCtaRtTicker` 构造函数没有初始化 `_s_info`，但公开的 `is_in_trading()` / `time_to_mins()` 会先读 `_s_info`，并明确提供空 session 时的 fallback。fresh ticker 在 init 前查询时，连该 fallback 守卫本身都是未初始化读取；正常 `WtCtaEngine` 路径会先 init 再 run，因此本结论仅限公开的 CTA 预初始化查询，不外推 Sel/HFT 的 `on_tick(NULL)` 或 `stop`。见 `src/WtCore/WtCtaTicker.h:41-55,95-104`、`src/WtCore/WtCtaTicker.cpp:28-38,285-299`、`src/WtCore/WtCtaEngine.cpp:61-65,109-112,675-680`。

修复应在构造时把 `_s_info`/`_store` 置空，并保留现有查询 fallback。已请求主代理拆分 disabled 混合用例，单独验证 fresh CTA ticker 查询；本轮未运行未初始化读取用例。

### BUG-N02-003 — NEEDS_DECISION

`IExecCommand` 有虚方法但无虚析构，若经基类静态类型直接销毁派生对象，派生析构不会被调用；但仓内 Runner 从具体派生指针创建 `ExecCmdPtr`，其 shared pointer 控制块保留派生删除器，现有测试也未证明错误所有权序列会发生。公开 `ExecCmdPtr` 类型允许外部消费者按基类裸指针重新建立 owning `shared_ptr`；是否支持这种所有权方式及 ABI 约束不能由仓内调用唯一推断。见 `src/WtCore/IExecCommand.h:61-115`、`src/WtRunner/WtRunner.cpp:481-506,508-541`。

请发布方明确：是否承诺第三方可从 `IExecCommand*` 获得并释放派生对象所有权？推荐若这是第三方扩展接口则增加 virtual destructor 并统一重编；若不能破坏 ABI，则保留布局并禁止由基类裸指针重建默认删除器所有权，收紧文档/API。影响和选择已写入 triage 的 `decision_request`。决定前不补测试，也不改接口。

### BUG-P02-001 — NOT_BUG

正常 Runner 只在找到 `TraderAdapter` 后调用 `setTrader(adapter)` 并注册 sink；未找到 adapter 时 ready 保持 false。绑定后的断开事件会调用 `on_channel_lost` 并清 ready。仓内没有正常生命周期能产生候选要求的 `ready=true && trader==NULL`；该状态需直接调用 ready callback 或先 ready 再人工 `setTrader(NULL)`。见 `src/WtRunner/WtRunner.cpp:481-506,508-534`、`src/WtCore/TraderAdapter.cpp:1418-1423,1538-1542` 及两个执行器的 ready/lost 处理。

现有测试只把 ready+NULL 作为状态构造且不调用危险下单；这不是有效的正常可达性证据。当前候选按误报撤回；若未来把 `setTrader(nullptr)` 定为公开解除绑定操作，再单独规定它与 ready 状态的关系。

### BUG-P02-004 — PRODUCT（仅外部费率 key 校验子项）

Runner 正常向 engine 传入有效 `BaseDataMgr`；旧条目的手工 NULL hot/data/base manager 不构成正常输入缺陷。另一个可达问题是外部 fees JSON 的 member key 进入 `load_fees` 后被按点分割并直接读取 `ay[0]`/`ay[1]`，无点或单段 key 可越界。合法正控中的键为 `SHFE.ag`。见 `src/WtCore/WtEngine.cpp:285-316,843-877,956-1003`、`src/WtRunner/WtRunner.cpp:153-160,365-408`。

修复需验证 key 段数与标识，并让格式错误通过配置/初始化返回值向 Runner 传播，避免部分费率应用后继续交易。本轮运行的 `DISABLED_NullHotMgrAndUnseededBaseDataPaths` 是空体；R130 合法费率正控不覆盖错误 key，故不把这两者当否定证据。已请求主代理在 R130 增加真实外部文件 malformed-key 用例。

### BUG-P02-006 — PRODUCT（快照 schema 与写盘失败两个子项）

`WtDiffExecuter` 对语法合法的本地 JSON 假设根对象、数组和字段类型均有效，随后直接调用 RapidJSON `GetArray/GetString/GetDouble`；结构错误快照可能触发断言/未定义行为。正常保存路径则由 `create_new_file` 先截断目标文件，再调用返回 `bool` 的 `write_file`，但忽略写入结果；故障时 last-good 快照可能被空/部分文件取代且上层无失败状态。见 `src/WtCore/WtDiffExecuter.cpp:113-172,180-232,243-280,501-545,554-642,680-696`、`src/Share/BoostFile.hpp:44-50,147-155`。

语法错误文件正控本轮 PASS，但不覆盖字段结构；已知结构错误和写失败未运行。将这两项作为独立修复/验收：加载先完整 schema 校验再应用；保存先写临时文件、检查写完并原子替换，失败保留旧快照且返回/记录错误。插件缺 unit 后跳过仓位更新的路径有错误日志，现有测试还明确将跳过作为预期；本轮不将其误报为静默缺陷。已请求主代理补结构损坏 JSON、完整读回和写失败注入测试。

## 待本地闭环

本组本轮只读源码并运行 5 个已启用安全正控；没有运行已知会触发 UB/UAF 的 disabled 测试，没有构建，也未修改源码或测试。其余待办是按 triage 请求完成产品修复后运行对应回归；`BUG-N02-003` 需先取得 ABI/所有权选择。测试修改请求已发送主代理，按主代理统一审查/实施安排执行。
