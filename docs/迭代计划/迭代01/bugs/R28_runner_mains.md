# R28_runner_mains（WtRunner / WtBtRunner 可执行体）疑似缺陷

> 本文按条目标注复核状态；本次复核（2026-10-29）核对源码、CMake 和现有测试前提，未运行用例或构建。
> 静态可推导的空指针/编译路径不作为本轮运行复现。涉及用例见 `docs/迭代计划/迭代01/manifests/R28_runner_mains.md`。

## BUG-R28-001

- 状态: 待验证
- 严重级别: 待定（覆盖率现象报告，成因未明）
- 模块/文件: `src/WtRunner/WtRunner.cpp`（`WtRunner::~WtRunner` :45-48 及其按值持有的
  `WtCtaEngine _cta_engine` / `WtHftEngine _hft_engine` / `WtSelEngine _sel_engine`，声明见
  `src/WtRunner/WtRunner.h:151-166`）；崩溃点本身在 libWtCore 的引擎析构链里
- 相关测试: `src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp` →
  `r28_wtrunner_entry.DISABLED_stack_allocated_runner_is_sigkilled`；
  同目录 `r6_wtrunner_main_test.cpp` 原有 4 条（`ctor_dtor_roundtrip_*`、
  `init_with_missing_logcfg_*`、`missing_file_returns_false`、`config_error_path_*`）
  在改成堆构造之前全部 `CRASH(rc=137,sig=9)`
- 输入/前置条件: 报告称测试栈帧构造/析构时收到 SIGKILL，而堆上构造通过；本次未复跑该对照，也未核对到可独立读取的进程状态/日志附件。
- 期望行为: 同一类型的生命周期只取决于 ctor/dtor 本身，不因对象在栈上还是在堆上而改变进程存活。
- 实际行为/证据边界: 原 lane 笔记报告 SIGKILL(9)、无 core、peak RSS 32MB，并记录堆上/栈上结果不同；本次只核对到对应 `DISABLED_` 用例，不独立确认上述数值或信号来源。测试本身在 `EXPECT_TRUE(config(absent))` 后会因断言失败，但静态源码不能解释为何栈/堆位置会令进程被 SIGKILL。
- 影响范围: 当前不能把 SIGKILL 归因于栈容量、libWtCore 重复状态、coverage runtime，亦不能认定它是 WtRunner.cpp 覆盖为 0.00% 的第一原因。应先取得独立子进程的 wait status、超时/看门狗记录、栈与堆最小复现及崩溃现场，再决定是否登记产品缺陷；目前仅是覆盖率测试环境的待验证现象。
- 复现步骤（未执行）:
  `cmake --build build-cov --target CoverageTests_m_WtRunner`，把任一启用用例的
  `std::unique_ptr<WtRunner> runner(new WtRunner());` 改回 `WtRunner runner;` 并把 `->` 改成 `.`，
  用 `case_runner.sh` 单跑，看 `$rc/*.rc` 首词是 `CRASH(rc=137,sig=9)`；
  或用 `os.fork()/os.waitpid()` 取真实信号（shell 的 `$?`=137 只到信号号，看不到「无 core」）。
- 后续覆盖率影响: 只有在 SIGKILL 现象被独立复现且能稳定关联到栈构造时，才可据此判定相关进程内测试/入口覆盖受阻；目前不能外推到三个兄弟可执行体。
- 备注: WtRunner 的构造函数没有初始化 `_engine`，但该字段是否参与本用例结束时的崩溃链尚未证明；用 `unique_ptr` 绕行不能当作根因修复。上述 dylib/coverage runtime 说明仅是待检假设，本次不采信为因果。栈上对象测试当前为 `DISABLED_`，本次未运行。

## BUG-R28-002 cppcli 的参数解析错误以成功码退出（help 自退出是既定行为）

- 状态: 待验证
- 严重级别: 候选 Major（parse error 退出码契约待确认）
- 模块/文件: `src/Share/cppcli.hpp:1563-1582`（参数解析错误输出后无条件 `std::exit(0)`）；help 正常路径在 `:1884-1899` 单独处理
- 相关测试: `r28_runner_main_test.cpp` 的 `help_wins_even_when_c_and_l_are_present` 只确认 help 时立即退出；这与 `cppcli.hpp:1881-82` 的注释及实现一致，不作为缺陷证据。当前未见对非法参数/缺必需参数退出码的启用测试。
- 输入/前置条件: 遗漏 `necessary` 必选参数，或其值未通过 type/allowed-value/range 校验，触发 `Option::errorExitFunc()`；未知参数路径不据此推断。
- 期望行为: 帮助请求可按现有接口以 0 退出；解析错误应以非零状态退出，供 shell/supervisor 区分失败。
- 实际行为: `errorExitFunc()` 输出错误文本后无条件 `std::exit(0)`。这是“parse error 返回成功码”的静态源码证据。`printHelpDoc()` 也会 `std::exit(0)`，但该行为在 cppcli 注释中明确设计为 help 输出后退出，不将它或调用方后续的 `if (hParam->exists())` 死代码另算产品缺陷。
- 影响范围: 自动化调用者可能把参数错误误认为正常结束；具体 CLI 使用者影响和兼容码约定尚未通过运行/产品接口文档验证。
- 备注: 若要修复，给错误退出设置非零码或返回解析状态；保留 help 的 0 退出语义。help 的 subprocess 包装属于测试隔离，不是产品 bug。

## BUG-R28-003

- 状态: 解析失败受控返回契约不满足；本机定向测试 FAIL
- 严重级别: Major（配置语法错误没有受控错误返回；进程终止码未在本次验证）
- 模块/文件: `src/WtRunner/WtRunner.cpp:69`（`_config = WTSCfgLoader::load_from_file(filename);` 无
  try/catch）、`src/WtBtRunner/WtBtRunner.cpp:84`（同一形状）；抛出点在
  `src/WTSUtils/WTSCfgLoader.cpp` 的 `load_from_yaml` 分支
- 相关测试: `r28_runner_main_test.cpp` → `r28_wtrunner_config.unparsable_yaml_returns_false_without_throwing`
  （原测试以异常为预期，现要求受控返回 false）与对照用例 `unparsable_json_returns_false_without_throwing`；
  `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` →
  `r28_btrunner_entry.bad_yaml_config_terminates_the_process`（断言子进程 SIGABRT）
- 输入/前置条件: 给一个语法不对的 YAML 配置（例：`basefiles: [ this is : not valid` 少一个 `]`）。
- 期望行为: 和「文件不存在」「后缀不认识」「空文件」三条一样，返回 false / -1 并给出可诊断日志
  —— `load_from_file` 对这三条都老老实实返回 NULL，`WtRunner.cpp:71-73` 也会 error + return false。
- 复核判定: `load_from_yaml()` 直接调用 `YAML::Load()` 且无 catch，而 `load_from_file()` 文档称解析失败返回 NULL；YAML 语法异常因此不遵守该返回约定。修正后的 WtRunner 测试本机 FAIL：Google Test 捕获到了 yaml-cpp 异常。未运行 WtBtRunner 进程路径，不能声称观察到 SIGABRT。
- 对照边界: `load_from_json()` 在 `Parse()` 后检查 `HasParseError()` 并返回 NULL，是源码静态对照；本次未运行 JSON/YAML 对比。
- 复现步骤: 独立临时工作目录中运行 `CoverageTests_m_WtRunner --gtest_filter=r28_wtrunner_config.unparsable_yaml_returns_false_without_throwing`；当前异常信息取决于 yaml-cpp 版本及输入内容。
- 影响范围: 调用方可能收到异常而不是 loader 文档承诺的 NULL/受控失败；若异常未捕获，进程将走 C++ terminate 路径，但本次未确认终止信号、stderr 文案或日志表现。
- 备注: 修复方向：在 loader 或调用边界 catch YAML 异常，记录文件路径/解析摘要并返回受控失败；修复后回归 YAML 与 JSON 解析失败及正常配置。

## BUG-R28-004

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Major（coverage-only 条件构建风险）
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:160`（`main` 落尾无 `return`）× 
  `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（`-Dmain=wt_coverage_main_<模块>`）
- 相关测试: `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` 当前是 `.cpp.blocked`，glob 不会纳入它；本目录没有可见的 `.cpp` 测试源。
- 输入/前置条件: `WT_ENABLE_COVERAGE=ON`、`WT_WARNINGS_AS_ERRORS=ON`，并为 `m_WtBtRunner` 提供 `.cpp` 测试源使 CMake 生成该目标。
- 期望行为: 产品 TU 在测试目标里换名后仍然编得过（或者构建表对这类 TU 有与
  SEL/UFT/HFT 工厂同款的处置），使 `src/WtBtRunner/WtBtRunner.cpp` 能进入覆盖率统计。
- 复核判定: `WtBtRunner` 确在 `WT_MAIN_MODULES`，其 TU 通过 `-Dmain=wt_coverage_main_wtbtrunner` 改名，源码末尾无显式 `return`。macOS coverage cache 中 `WT_WARNINGS_AS_ERRORS=ON`，而 `-Wall` 含 `-Wreturn-type`，因此目标启用后有触发 `-Werror=return-type` 的静态构建风险。CMake 虽定义 `WT_COV_RELAX_WARNINGS`，但本文件没有调用该函数；它不会自动为此 TU 放宽告警。本次未生成/编译该目标，故不把早前的错误行或零告警说成本次复现。生产源码中的函数仍名为 `main`，标准的 `main` 隐式返回规则不适用于改名后的普通函数；本次未检查生产二进制构建结果。
- 复现步骤（未执行）: `mkdir -p src/TestUnits/coverage/m_WtBtRunner && 放入任意 *.cpp &&
  cmake --build build-cov --target CoverageTests_m_WtBtRunner`。
- 影响范围: 目前没有 `CoverageTests_m_WtBtRunner` 目标，故没有本条对应的当前 coverage 百分比证据。目标创建后若编译失败，会阻止该目标；是否中止全量构建取决于调用的 build target/并行构建策略，不能只由 Ninja 默认行为推断。
- 备注: 静态构建风险，不是生产运行缺陷。候选处理包括显式 `return 0`、只对改名 main 的 TU 设置精确的 `-Wno-error=return-type`，或调整 coverage 目标范围；选择前应以启用 `.cpp` 测试源后的真实编译命令与诊断验证。当前 `.blocked` 文件不会生成该 target。


- 迭代02复核: TEST。生产函数名是标准 C++ main，落尾隐式 return 0；源注释也说明 0 成功。风险只在 coverage 用 -Dmain 改普通函数后、warnings-as-errors harness；当前 `.cpp.blocked` 不生成目标。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待验证
## BUG-R28-005

- 状态: 待验证
- 严重级别: Major（router 条目缺必需键时未受控拒绝；运行信号未在本次复现）
- 模块/文件: `src/WtCore/WtExecMgr.cpp:216-219`（`load_router_rules` 循环体）
- 相关测试: `r28_runner_main_test.cpp` →
  `r28_wtrunner_config.DISABLED_router_rule_without_executer_key_derefs_null`；
  对照用例 `r28_wtrunner_config.routers_section_reaches_load_router_rules` 为启用用例，本次未运行
- 输入/前置条件: `WtRunner::config()` 的配置里有 `routers` 数组，但某个条目没写 `executer` 键
  （例：`routers:\n-   strategy: x\n    symbol: y`）。
- 期望行为: 拒绝缺少必需 `executer` 的 router 条目并给出可诊断错误；对 `itemExec` 做判空。若输入 schema 允许 null array elements，再为 `item` 增加守卫。
- 复核判定: `WTSVariant::get()` 缺键返回 `NULL`；`load_router_rules()` 在 `itemExec->isArray()` 前没有判空。因此配置数组元素缺少 `executer` 时源码会对空指针调用成员函数，属于静态确认的未受控输入路径；SIGSEGV 是常见结果但本次未运行，不能标为本次实测。
- 复现步骤（未执行）: 在有效的 runner 配置中提供 routers 数组，并让某个对象缺少 `executer` 键；需在隔离子进程验证具体信号，不能在 gtest 宿主直接运行潜在崩溃路径。
- 影响范围: 缺键的 router 条目会进入未定义行为而非返回配置错误；`WtRunner.cpp` 的 executers 侧与顶层 routers 入口都可能转发到此函数。`strategy` 缺键则由 `getCString()` 返回空串，但后续是否产生错误路由尚未验证。
- 备注: `DISABLED_` 负向用例和启用正向对照本次均未运行。建议将缺 `executer` 作为配置错误记录并使 `config()` 失败，而不是静默跳过；对缺失 `strategy` 存入空键的后续路由影响也尚未验证。

## BUG-R28-006

- 状态: 待验证
- 严重级别: Major（未知 mocker 配置导致未注册 sink 路径；具体信号未验证）
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:98-150`（五个 `strcmp(mode, ...)` 分支）与
  `src/WtBtCore/HisDataReplayer.cpp:669`（`_listener->handle_init()`）；
  `_listener` 只在 `HisDataReplayer.h:804-807` 的 `register_sink` 里被赋值，初值 NULL 见 `:146`
- 相关测试: `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` →
  `r28_btrunner_entry.DISABLED_unknown_mocker_leaves_listener_null_and_segv_in_prepare`
- 输入/前置条件: 含有效 `replayer` 段的配置，其 `env` 为对象且 `mocker` 是非空、但不属于 cta/hft/sel/exec/uft 的字符串。缺整个 `env` 段会在前面的 `cfgEnv->getCString()` 触发另一条未经检查的空值路径，不是本条专属前置条件。
- 期望行为: 认不出 mocker 时给出「不支持的回测引擎 xxx」之类的错误并退出非 0，
  而不是带着一个从未注册的 listener 继续跑 `prepare()`。
- 复核判定: 对非空但不匹配 `cta/hft/sel/exec/uft` 的 `mode`，五个分支都不注册 sink；`HisDataReplayer` 的 `_listener` 初值为 NULL，`prepare()` 随后无条件调用 `_listener->handle_init()`。这是静态可确认的空指针调用路径，实际终止信号未由本次运行确认。
- 复现步骤（未执行）: 为 `replayer` 和 `env` 提供有效配置，令 `mocker` 为未知字符串，再在隔离子进程调用入口；还需确保 `replayer.prepare()` 可通过 `calcTradingDate()` 前置数据读取。
- 影响范围: 对有效 `replayer` 配置加未知 `env.mocker` 时，回测入口不会受控拒绝；调用未注册 listener 后的进程行为仍待验证。测试前提还需确认 `configbt.yaml` 的 replayer/storage、交易日历/历史数据和 cwd 工厂路径是否足以到达 `prepare()`；仅有 `.blocked` 用例不证明该路径已被运行。
- 备注: 与 BUG-R6-011 同属启动前置失败后仍继续执行的家族，但本条是回测 `env.mocker` 未命中分支、并非 `config()` 返回 false 后继续 run，保留独立发生点。修复方向：未知类型时报错非零退出并跳过 `prepare()`；显式检查 `register_sink`/初始化结果。

## BUG-R28-007

- 状态: 本机缺配置回归 FAIL；notifier 必需性待确认
- 严重级别: Major（engine 初始化失败仍报告配置成功；notifier 是否必需待确认）
- 模块/文件: `src/WtRunner/WtRunner.cpp:154`（`initEngine();`）、`:157`（`initDataMgr();`）、
  `:228`（`initEvtNotifier();`）—— 三处返回值被丢弃；对照 `:157-158` 的
  `if (!initActionPolicy()) return false;` 是同一个函数里唯一被检查的 init
- 相关测试: `r28_wtrunner_config.missing_env_and_data_sections_make_config_fail` 和 `missing_data_section_makes_config_fail`；均将应失败写为期望。
- 输入/前置条件: 一份 basefiles/bspolicy 齐全而同时缺 `env` 和 `data` 的配置；另有 env 完整但缺 data 的独立负例。
  `initEngine()` 在 `:367-370` 因 `cfg==NULL` 返回 false；`_engine` 未被赋值，不能表述为 NULL。
- 期望行为: 必需的 engine/data 初始化失败时应令 `config()` 返回 false 或记录失败；不能让未初始化的 runner 被当作配置成功。notifier 是否为必需项尚未确认，不据其缺失单独判定配置失败。
- 复核判定: `config()` 对 `initEngine()`、`initDataMgr()`、`initEvtNotifier()` 的返回值不作检查。本机两个独立负例均 FAIL，实际 config 返回 true；缺 env 的负例同时缺 data，不能单独证明只缺 env 的行为。由于 `_engine` 是未初始化成员而非被置 NULL，后续 `run()` 的 `_engine->run()` 属未定义行为，不能表述为确定空指针或必然 SIGSEGV。
  `initDataMgr()` 同理（`:418-420` 无 data 段返回 false 被丢）。`initEvtNotifier()` 的返回值也被忽略，且无 notifier 的启用测试预期 `config()` 返回 true；因此其是否为必需初始化属于接口契约待确认项，不并入已确认的 engine/data 失败判断。
- 复现步骤: 分别运行上述两个测试；本机均未调用 `run()`。仅缺 env 而保留 data 的初始化路径可能传递未初始化 `_engine`，本次未运行。
- 影响范围: 缺少 env 时配置结果可能误报成功；继续运行会访问未初始化 `_engine`，但崩溃表现/信号不由源码静态证明。
- 备注: 修复方向：对必需初始化调用检查返回值并记录错误；为未初始化状态增加确定初始化/安全拒绝，失败时清理已创建资源。

## BUG-R28-008 [已合并] WtBtRunner 缺配置文件提示拼写错误并返回 0

- 状态: 已合并
- 严重级别: 不另计（与 BUG-R14-004 同类发生点）
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:80`（`fmt::print("confiture {} not exists", filename);`）
- 相关测试: `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` 中 `missing_config_prints_notice_and_returns_zero`；当前源文件不是 CMake glob 可见的 `.cpp`，本次未运行。
- 输入/前置条件: `WtBtRunner -c ./不存在的.yaml`。
- 期望行为: 一句拼写正确、以换行结尾的提示，例如 `config file {} not exists\n`。
- 复核判定: 源码确认字面量为 `confiture`、无换行并 `return 0`。该 occurrence 与 BUG-R14-004 的 QuoteFactory `coniture` / `return 0` 同属缺配置提示及退出码缺陷家族，已并入 R14-004；本条保留作发生点索引，不另计。具体 shell 提示符表现、脚本影响和运行退出码本次未复现。
- 复现步骤: `WtBtRunner -c ./nope.yaml`，或跑该用例看子进程 stdout 回收到的原文。
- 影响范围: 当调用方要求错误诊断进日志或以非零码失败时可能不满足；调用契约待确认，见 BUG-R14-004。缺失配置文案为源码静态证据。
- 备注: 修复方向与 BUG-R14-004 一并处理，修正文案/换行并按 CLI 契约选择退出码。本次未构建或运行该入口。
