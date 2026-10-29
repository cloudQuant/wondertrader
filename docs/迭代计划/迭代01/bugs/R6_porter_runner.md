# R6 缺陷清单 — `src/WtPorter/WtRtRunner.cpp` + `src/WtRunner/`（第七波 lane R6）

> 只读生产代码，本文件只登记不修复；每条状态一律 `待审核`。
> 涉及用例见 `docs/迭代计划/迭代01/manifests/R6_porter_runner.md`。
> 崩溃/挂死类一律整条 `DISABLED_`，触发条件写在用例名字下面的注释里。
> 本次复核以当前源码为准。manifest 记载本 lane 只做 `-fsyntax-only`，未跑构建或测试；下文逐项标为源码证据、测试前提问题或待验证，不把禁用用例当作实测。

## BUG-R6-001 WtRtRunner::config 不校验加载结果，配置文件加载失败时解引用空指针

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:489-494`（`_config = isFile ? WTSCfgLoader::load_from_file(...) : load_from_content(...)` 之后直接 `_config->get("basefiles")`）
- 现象: `WTSCfgLoader::load_from_file` 对不存在文件返回 NULL（`src/WTSUtils/WTSCfgLoader.cpp:298-302`）；`WtRtRunner::config` 随后在 `:494` 直接 `_config->get(...)`，没有判空。`WtRunner::config` 的 `:69-74` 已展示可用的失败处理。
- 影响范围: `config_porter` 传入无法加载的配置文件时会进入空指针解引用；这是配置错误路径，不能外推为 `init_porter/run_porter` 的所有调用都会崩。YAML 语法错误可能抛异常，是 R28-003 的另一条路径。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterConfig.DISABLED_missing_config_file_derefs_null_config`，前置条件与源码相符，但标记为 `DISABLED_`；manifest 明确未运行本 lane 测试。
- 证据等级: 源码静态证据；本次未复现 SIGSEGV。
- 修复方向: 检查 loader 返回值，记录文件路径并返回 `false`；C 导出 `config_porter` 目前返回 `void`，如需调用方获知失败，应另评估 API 返回状态。

## BUG-R6-002 WtRtRunner::config 不校验必需的 basefiles 段

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:494-495`（`cfgBF = _config->get("basefiles"); if (cfgBF->get("session"))`）
- 现象: 可解析但缺少 `basefiles` 对象的配置会令 `cfgBF == NULL`，`WtRtRunner.cpp:495` 随即解引用；同函数对 `parsers/traders/executers/routers` 都先检查是否存在。
- 影响范围: 配置 schema 缺项时崩溃，而不是返回配置错误；此处不应称该配置为“合法”，其语法可解析但缺少 runner 必需段。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterConfig.DISABLED_valid_content_without_basefiles_derefs_null_section` 通过 `config("{}", false)`，确实走到缺段分支；测试未启用、未实测。
- 证据等级: 源码静态证据；未复现。
- 备注: WtRunner 对应路径 BUG-R6-012 是同一 schema 校验问题的另一处实现，合并计为一个缺陷家族、保留两处源码锚点。建议在读取 `cfgBF` 前校验非空及对象类型，缺失时记录配置项名并返回 `false`。

## BUG-R6-003 createExtParser 在引擎初始化前无条件解引用 _engine

- 状态: 待审核
- 严重级别: Major（调用时序与配置契约待确认）
- 位置: `src/WtPorter/WtRtRunner.cpp:295-303`（`adapter->initExt(id, parser, _engine, _engine->get_basedata_mgr(), _engine->get_hot_mgr())`）；`_engine` 只有 `initEngine()`（`:838-882`，经 `config()` 才调用）会赋值
- 现象: `initEngine()` 在成功配置 `env` 后才给 `_engine` 赋值；`createExtParser()` 却直接把 `_engine` 及其 manager 传给 adapter。C API 的 `getRunner()` 是函数静态对象，尚未配置时 `_engine` 静态零初始化为 NULL；非静态 `WtRtRunner` 的 `_engine` 则未初始化，不能断言必为 NULL。
- 影响范围: 若调用方在有效配置前创建 parser，或 `config()` 未成功建立 engine 后继续创建，C API 路径会解引用空指针；`create_ext_parser` 的公开调用时序契约尚未查到，故不把“先建 parser”描述为受支持正常流程中的必然故障。
- 相关测试: `DISABLED_create_ext_parser_without_engine_crashes` 使用栈上 `WtRtRunner`，其 `_engine` 是未初始化值，不是可靠的 NULL 复现；`create_ext_executer_returns_true_without_engine` 走静态单例且不访问 `_engine`，不是等价对照。manifest 未运行测试。
- 证据等级: 解引用和赋值位置为源码静态证据；是否违反 C API 的调用契约、实际崩溃未验证。
- 备注: 与 BUG-Q06-002（未初始化引擎的 getter）及 BUG-R46-001（非静态实例 `_engine` 漏初始化）同属 engine 尚未就绪路径，但不是同一消费点。若创建 parser 要求先成功配置，建议在此返回可诊断失败；若该 API 被规定只在成功配置后调用，应补清晰的调用顺序文档。

## BUG-R6-004 on_ext_parser_quote 对 id / curTick 的空指针缺少守卫

- 状态: 待审核
- 严重级别: 待定（NULL 参数契约未确认）
- 位置: `src/WtPorter/WtRtRunner.cpp:1243-1250`（先以 `id` 查 `_parsers`，命中后解引用 `curTick`）；`src/WtCore/ParserAdapter.cpp:406-414`（map 以 `std::string` 为键）；C 导出 `src/WtPorter/WtPorter.cpp:2265-2267` 直接转发。
- 现象与边界: `on_ext_parser_quote(NULL, ...)` 会把 NULL 传到 `_adapters.find(id)`；该 map 的 key 为 `std::string`，不能以 NULL 构造 key。另当 parser 已命中时，`:1248` 会解引用 `curTick`，没有 NULL 守卫。`parser_init/connect/disconnect/release/subscribe/unsubscribe` 本身只把 `id` 交给宿主注册的回调，本函数内没有同样的字符串构造，不能一概称六个入口都会崩。
- 影响范围: 仅限调用方违反非空参数前提、或 C ABI 未定义 NULL 输入时的鲁棒性问题；C API 对这些参数的 nullability 契约未找到。不要延伸成“Python/C# 必然崩溃”。
- 相关测试: `DISABLED_parser_id_null_builds_string_from_nullptr` 先调用 `parser_connect(NULL)`（测试回调本身能安全处理），再调用 `on_ext_parser_quote(NULL, NULL, 0)`；第二次调用混合了 `id` 与 `curTick` 两个变量，且未注册 adapter，因此不能单独证明 `curTick` 分支。未运行。
- 证据等级: 源码静态证据；NULL 是否为受支持输入及运行结果待验证。R13-007 的 parser ID 引用限于此 quote 查表入口。
- 修复方向: 若 C API 允许 NULL，分别校验 `id` 和已命中 adapter 时的 `curTick` 并安全返回；若参数必填，在头文件明确契约并让绑定层拒绝空指针。测试应分开覆盖两个输入。

## BUG-R6-005 ctx_on_tick / HFT 微数据回调对数据对象无判空（输入保证待验证）

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（取决于内部回调是否允许 NULL）
- 位置: `src/WtPorter/WtRtRunner.cpp:441-451`（`_cb_cta_tick(id, stdCode, &newTick->getTickStruct())`）、同形还有 `:1188-1192`（ordque）、`:1194-1198`（orddtl）、`:1200-1206`（trans）
- 现象: `ctx_on_tick` 在命中 callback 后才执行 `newTick->getTickStruct()`；HFT ordque/orddtl/trans 同理。未注册 callback 的启用测试只证明短路分支不会解引用 NULL。生产中这些指针来自 `Exp*Cta/Hft/SelContext` 的引擎回调，当前没有找到合法生产路径会传 NULL。
- 影响范围: 源码确有“注册 callback 且传 NULL”时的未定义行为；但相关方法是内部 C++ 分发点，不是宿主直接提供指针的 C 导出接口。本条尚未证明这是产品可达输入，而非测试注入的非法参数。
- 相关测试: `DISABLED_ctx_on_tick_null_tickdata_derefs` 直接调用内部方法并手工传 NULL；启用对照只覆盖 callback 未注册的短路支。两者均未在本次审查运行。
- 证据等级: 静态分支证据；内部数据非空保证、NULL 可达性待验证。
- 备注: 暂不按已证实产品缺陷计数。先确认 WtCore 对 `on_tick/on_order_queue/on_order_detail/on_transaction` 的参数契约和所有调用点；若允许 NULL，增加入口守卫与注册/未注册两种回归测试，否则将测试前置条件改成符合契约的数据。


- 迭代02复核: NOT_BUG。内部回调在 callback 注册且数据为空时会解引用，但参数来自引擎策略上下文回调；静态未找到合法生产 caller 传 null。disabled 用例手工注入非法空值，不能证明产品可达。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R6-006 run() 需要已初始化引擎；release() 停止进程级日志（可复用契约待确认）

- 状态: 待审核
- 严重级别: Major（run）/待验证（release 生命周期）
- 位置: `src/WtPorter/WtRtRunner.cpp:1091-1124`（`:1098` 的 `_engine->run()`；`src/WtRunner/WtRunner.cpp:585` 是同形的一份）、`src/WtPorter/WtRtRunner.cpp:1141-1155`（`release()` 末行 `WTSLogger::stop()`，见 `src/WTSTools/WTSLogger.cpp:255-261`）
- 现象: `run()` 无条件调用 `_engine->run()`（`:1098`）；未成功配置时没有可用 engine。C API 的 `config_porter` 又丢弃 `config()` 返回值（`WtPorter.cpp:260-266`），下游调用 `run_porter` 前无法从接口读回失败状态。`release()` 调 `WTSLogger::stop()` 会设置静态 `m_bStopped` 并执行 `spdlog::shutdown()`（`WTSLogger.cpp:255-261`）。
- 影响范围: `run()` 是配置失败后继续启动时的未定义行为；与 R46-002（env 缺失导致 config 错报成功）及 R28-007（WtRunner 内部 init 失败被忽略）共享“未就绪仍启动”家族，但消费点/路径不同。`run(false)` 的等待循环在已安装 signal hooks 后运行，是同步运行语义，不能据此登记为死循环缺陷。
- 相关测试: `DISABLED_run_without_engine_and_release_global_logger_stop` 先在栈上 runner 调 `run(true)`，其 `_engine` 是未初始化值；若该调用崩溃，后续 `release()` 根本不会执行。该用例既未可靠证明 run 崩溃，也没有证明 logger 的后续进程副作用；本次未运行。
- 证据等级: 无 engine 守卫和 logger 全局状态修改为源码静态证据；是否支持同进程 release 后重初始化尚未验证。
- 修复方向: run 入口验证 engine 已就绪并报告错误；若 `release_porter` 是终结性 API，在文档写明不可复用；若允许 release 后重初始化，则为日志资源实现可验证的重启路径或避免此处关闭进程级 logger。

## BUG-R6-007 ExpExecuter 回调硬绑 getRunner（多实例支持契约待确认）

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（仅在支持多个 WtRtRunner 实例时成立）
- 位置: `src/WtPorter/ExpExecuter.cpp:24-28`（`getRunner().executer_init(name())`）与调用点 `src/WtPorter/WtRtRunner.cpp:305-312`
- 现象: `ExpExecuter::init`、`set_position`、`on_position_changed` 都直接使用 `getRunner()`；`createExtExecuter` 却是 `WtRtRunner` 实例方法。若以非单例 runner 创建该 executer，回调路由会落到全局单例。
- 影响范围: 源码证明实现没有多实例隔离；但公开 C 导出函数都通过静态 `getRunner()`，类的多实例运行支持契约未找到。不能从“可构造第二个 C++ 对象”推断多 runner 是生产支持场景。
- 相关测试: `executer_callbacks_fire_on_the_runner_that_registered_them` 只直接调用 local runner 的 `executer_init/set_position`，不创建 `ExpExecuter`，不能证明或否定本条；`create_ext_executer_returns_true_without_engine` 则只走单例。
- 证据等级: callback 单例路由为源码静态证据；生产多实例需求待验证，未复现。
- 修复方向: 若支持多实例，将所属 runner 注入 `ExpExecuter` 并测试实例隔离；若 Porter 设计上只支持全局单例，应明确限制非单例实例的扩展执行器创建。


- 迭代02复核: NOT_BUG。ExpExecuter 回调绑全局 getRunner，但公开 C API 就是静态单例；未找到多 runner 合同。启用测试直接 local.executer_init，不创建 ExpExecuter，不能证明其路由；内部可构造第二对象不是产品支持证据。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R6-008 loadFactories 用 exists 判目录；无效文件路径会抛异常

- 状态: 本地隔离测试复现；其它工厂与真实插件未运行
- 严重级别: Major（无效路径错误处理）
- 位置: `src/WtCore/CtaStrategyMgr.cpp:30-41`（守卫 `StdFile::exists(path)` 见 `src/Share/StdUtils.hpp:137-145`，实现在 `std::filesystem::exists`；随后是 `boost::filesystem::directory_iterator`），经 `src/WtPorter/WtRtRunner.cpp:1168-1186` 四个 `addXxxFactories` 转发
- 现象: `std::filesystem::exists` 对「文件」也返回 true，守卫放行后 boost 的 `directory_iterator` 对文件路径会报告非目录错误，通常抛 `filesystem_error`；`addCtaFactories` 返回 bool 却没有 try/catch。这里不把“异常跨 ABI 必然是 UB”作为已证事实，只指出导出层没有把无效路径转成受控失败。
- 影响范围: CTA/SEL/HFT/Executer 四个 manager 都以 `exists` 后直接构造 `directory_iterator`（分别见 `CtaStrategyMgr.cpp:30-41`、`SelStrategyMgr.cpp:30-41`、`HftStrategyMgr.cpp:30-41`、`WtExecuterFactory.cpp:15-25`）。传入存在的普通文件会进入目录迭代器并可能抛 `filesystem_error`；导出层没有异常转为 `false` 的守卫。
- 相关测试: 原 `factory_path_pointing_at_a_file_does_not_return_cleanly` 将抛异常也视为通过。现改为子进程中要求不抛且返回 false，异常以退出码 2 表示失败；本地定向 FAIL，子进程实际退出码 2。子进程通过 `_Exit` 绕过 runner 析构，故不验证析构安全；首次直接运行曾在观察到 filesystem_error 后 signal 11，未获得栈，不能确定其具体因果。
- 证据等级: CTA 工厂入口本地隔离复现异常，SEL/HFT/Executer 同形路径为源码证据，未分别运行。
- 备注: 建议先用 `is_directory`（带 `error_code`）拒绝非目录并返回 `false`；如需防止插件枚举/装载异常逃逸，再在 C API 边界定义异常处理策略。保留子进程回归，另独立排查 runner 析构。

## BUG-R6-009 WtRtRunner 构造列表漏初始化成员（与 R46/Q06 已登记路径重叠）

- 状态: 已合并
- 严重级别: 已有条目覆盖，不另计
- 位置: `src/WtPorter/WtRtRunner.cpp:55-100`（初始化列表）对照成员声明 `src/WtPorter/WtRtRunner.h:824`（`_config`）、`:853`（`_engine`）、`:924-932`（`_feed_obj/_feeder_bars/_feeder_fcts`）
- 现象与范围: `_engine/_feed_obj/_feeder_bars/_feeder_fcts` 未在 `WtRtRunner` 构造函数中初始化；`_config` 也未初始化，但 `config()` 在首次读取前先赋 loader 结果，本身不能据此声称“config 前读未定义”。feed 函数的实际未注册/未 load 路径见 Q06-004。
- 重复关系: `_feed_obj/_feeder_bars/_feeder_fcts` 的危险消费路径与 BUG-Q06-004 重复；构造成员及非静态实例的 `_engine` 等消费面已由 BUG-R46-001 逐项登记。不要将 R6-009 作为独立 blocker 计数。BUG-Q06-003 是回测 `WtBtRunner::_hft_mocker`，只是同类，不是同一字段/发生点。
- 相关测试: `DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage` 在栈上对象上调用未初始化 callback；该用例未运行，也不能稳定把“垃圾值”预测为跳飞指针。
- 证据等级: 构造列表与具体读取为源码静态证据；具体运行表现待验证。
- 修复方向: 合并到 R46-001/Q06-004 的修复项：为所有会被读取的指针和函数指针提供确定默认值，并在 feed 入口检查对应 feeder；不要把 `_config` 作为独立未定义读取缺陷登记。

## BUG-R6-010 LOG_TAGS / getBinDir 名称在独立模块间重复（仅合并编译时的 ODR 风险）

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: 待定（当前产品/coverage 目标分开编译）
- 位置: `src/WtPorter/WtRtRunner.cpp:1126-1134`（`const char* LOG_TAGS[]`）、`src/WtRunner/WtRunner.cpp:614-622`（同名同形另一份）、`src/WtRunner/WtRunner.cpp:26-37`（`const char* getBinDir()`）对照 `src/Share/ModuleHelper.hpp:78`（`[[maybe_unused]] static const char* getBinDir()`）
- 现象: `LOG_TAGS` 在 WtPorter/WtRunner/WtUftRunner/WtBtPorter 各 TU 中是外部链接数组；WtRunner 与 WtUftRunner 的 `getBinDir` 也分别有外部定义。`WtRtRunner.cpp` 使用的 `ModuleHelper.hpp` 版本为 `static` 内部链接，不与 `WtRunner.cpp` 的外部定义直接冲突。
- 影响范围: 若自定义目标把多个这些生产 TU 编进同一链接单元，会有 ODR/duplicate symbol 风险；当前产品构建分别生成 WtPorter、WtRunner、WtUftRunner 等目标，coverage CMake 也为 `CoverageTests_porter` 与 `CoverageTests_m_WtRunner` 分别建目标，因此本条没有证明当前受支持构建失败。
- 相关测试: 两个测试只核对表项内容，不验证合并链接；manifest 未跑链接目标。
- 证据等级: 外部链接定义与目标隔离为源码/CMake 静态证据；本次未尝试合并链接。
- 备注: 若未来需要将这些 TU 合并，统一 `LOG_TAGS` 为内部链接表/共享唯一声明，避免重复定义；当前按条件性构建风险记录，不按运行期产品缺陷计数。


- 迭代02复核: TEST。同名外链符号在自定义合并目标可能冲突，但正式 CMake 分别建 WtPorter shared library/WtRunner executable，coverage 也分别建目标。未证明受支持构建合并这些 TU；现存测试只比表内容。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R6-011 main 忽略 config 失败后继续 run（与 R28 启动失败家族关联）

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtRunner/main.cpp:118`（`runner.config(filename);` 返回值被丢弃）→ `:125`（`runner.run(false)`）→ `src/WtRunner/WtRunner.cpp:585`（`_engine->run()`）
- 现象: `WtRunner::config()` 对文件加载失败返回 `false`（`:69-74`）；`main.cpp:118` 忽略结果并总是执行 `runner.run(false)`（`:125`），而 `WtRunner::run` 在 `:585` 直接调用 `_engine->run()`。`WtRunner::_engine` 构造时未初始化，故结果是未定义行为，不应写成稳定 NULL 或必然 SIGSEGV。
- 影响范围: 静态控制流证明可执行体在配置失败后仍启动未就绪 runner。R28-007 覆盖“配置返回 true 但内部 init 失败”的另一条上游路径，二者共用未就绪后继续 run 的下游风险，不重复计算同一启动失败家族；R6-011 的具体上游是 config 返回 false。
- 相关测试: 两条 `DISABLED_` 测试调用 main；但 main 栈构造 `WtRunner`。R28-001 记录该栈对象测试在进入 config 前即 SIGKILL，且因果仍未确定，因此这些用例不能证明 R6-011 的 config/run 路径。本次未运行。
- 证据等级: 返回值被丢弃及 `_engine` 未初始化为源码静态证据；main 进程的具体失败位置/信号待验证。
- 修复方向: `main` 检查 `config()` 结果并报告失败后退出；同时让 `WtRunner` 构造把 `_engine` 置空、`run()` 对未初始化状态安全失败。不要把同步模式的 signal-hook 等待循环描述成缺陷。

## BUG-R6-012 WtRunner::config 不校验必需的 basefiles 段（R6-002 的同源 occurrence）

- 状态: 已合并（主记录 BUG-R6-002；WtRunner 同一 basefiles 段缺失校验问题，保留本处源码锚点）
- 严重级别: 已有条目覆盖，不另计
- 位置: `src/WtRunner/WtRunner.cpp:77-78`（`cfgBF = _config->get("basefiles"); if (cfgBF->get("session"))`）
- 现象: 成功加载配置后 `cfgBF = _config->get("basefiles")`，`WtRunner.cpp:78` 未判空即调用 `cfgBF->get`；这与 WtRtRunner 的 R6-002 是相同 schema 校验缺陷的另一实现。
- 相关测试前提有误: `DISABLED_empty_object_config_derefs_null_basefiles` 调用 `runner.config("./r6_empty_cfg.yaml")`，但仓库没有该文件，测试也没有创建它；在通常测试 cwd 下会先由 `:70-74` 因文件不存在返回 false，根本到不了 basefiles 分支。只有外部 harness 提供同名文件时才另当别论，本次未验证。
- 证据等级: `cfgBF` 解引用为源码静态证据；现有测试不是有效复现，未运行。
- 备注: 与 R6-002 合并计数；应先让测试写出/提供一个内容为 `{}` 的有效配置文件，再断言配置校验返回失败，而不是以缺失文件代替缺段。

## BUG-R6-013 CoverageTests_m_WtRunner 缺 WtCore（当前源码显示旧候选已过时）

- 状态: 已撤回
- 严重级别: 当前不成立（重新配置/构建证据待补）
- 位置: `src/TestUnits/coverage/CMakeLists.txt:408-441`（`WT_MAIN_MODULES` 自动提取模块 CMake 中的核心库并链接），`src/WtRunner/CMakeLists.txt:20-24`（`WtCore` 位于 `LIBS`）
- 当前源码证据: `coverage/CMakeLists.txt:408-441` 的 `WT_MAIN_MODULES` 循环读取模块自己的 CMakeLists（`:431-437`），将匹配到的核心库放入 `WT_M_CORES` 并在 `:441` 链入；`src/WtRunner/CMakeLists.txt:20-24` 明确把 `WtCore` 列在 `LIBS`。因此对当前 `CoverageTests_m_WtRunner`，自动提取会加入 `WtCore`，旧文所称“只链 WT_COV_SHARED_LIBS”的前提已过时。
- 证据等级: 当前 CMake 静态证据说明链接缺口不成立；本次没有重新 configure 或执行链接，不能宣称目标实际 build PASS。
- 备注: 保留编号作审计结论，不作为待修构建缺陷计数。若重新 configure 后仍有 unresolved symbols，应按实际链接日志判断具体缺失库；当前没有源码依据再建议补 `WtCore`。

## BUG-R6-014 initActionPolicy 对 bspolicy 的处理表面不同，当前证据不支持功能差异

- 状态: 已撤回
- 严重级别: 候选不成立（除非另有可观察副作用契约）
- 位置: `src/WtRunner/WtRunner.cpp:411-414`（`return _act_policy.init(_config->getCString("bspolicy"));`）对照 `src/WtPorter/WtRtRunner.cpp:1157-1166`（先 `strlen(action_file) <= 0` 才返回 false）
- 现象与证明: `WTSVariant::getCString` 缺省返回 `""`（`Includes/WTSVariant.hpp:346-352`）。WtPorter 对空 `bspolicy` 在 `WtRtRunner.cpp:1159-1161` 返回 false；WtRunner 把空串传给 `ActionPolicyMgr::init`，而 `WTSCfgLoader::load_from_file("")` 因路径不存在返回 NULL（`WTSCfgLoader.cpp:298-302`），`init` 也返回 false（`ActionPolicyMgr.cpp:47-52`）。有非空路径时两边都调用同一个 mgr `init`。因此现有证据显示成功/失败结果一致。
- 影响范围: 未证明“同一配置得到不同初始化结论”。可能存在多一次空路径加载/日志等副作用，但本次未找到日志或 API 契约要求，不能定为功能缺陷。
- 证据等级: 源码静态比较；没有运行差分测试。
- 备注: 当前候选不单独计数。若 `bspolicy` 是可选项，需先确认期望行为；随后应一致地让两侧跳过加载并继续，或明确两侧都必须配置。`initDataMgr/initEngine` 返回值被忽略属于其他候选（如 R28-007 / R46-002），不应塞入本条扩展范围。

## BUG-R6-015 handleLogAppend 对越界日志级别没有守卫（输入来源待验证）

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（防御性检查候选）
- 位置: `src/WtPorter/WtRtRunner.cpp:1136-1139`、`src/WtRunner/WtRunner.cpp:624-627`（两处都是 `LOG_TAGS[ll-100]`）
- 现象: `LOG_TAGS` 有 7 项，`handleLogAppend` 用 `LOG_TAGS[ll-100]`；任意传入越界枚举会越界访问。`WTSLogLevel` 的声明范围为 LL_ALL=100 到 LL_NONE=106（`Includes/WTSTypes.h:128-137`）。
- 影响范围与边界: 当前可见生产调用来自 `WTSLogger` 的 `debug/info/warn/error/fatal_imp`（`WTSLogger.cpp:271-320`），它们传入固定有效枚举。未找到 C 导出入口把外部数值传给 `handleLogAppend`；因此“外部绑定传 0/200 导致实盘崩溃”没有证据。
- 相关测试: 启用用例只覆盖合法枚举，不验证非法值；manifest 未运行。
- 证据等级: 索引缺少守卫为源码静态证据；越界调用在当前产品是否可达待验证。
- 备注: 暂按防御性硬化候选处理，不计为已证实产品缺陷。可增加范围检查并记录非法值；测试应明确它验证的是内部接口的防御行为，而非声称外部 C API 能传此参数。

- 迭代02复核: NOT_BUG。越界枚举会越界访问，但 WTSLogLevel 合法范围为100..106，未找到生产调用给内部 handler 传范围外 enum。现测试只覆盖合法输入，不能据此算用户可达 bug。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
