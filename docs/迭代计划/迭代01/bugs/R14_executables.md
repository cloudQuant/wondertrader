# R14 缺陷登记 — 可执行体/工具模块（第七波 lane R14）

范围：`src/WtUftRunner/`、`src/WtLatencyHFT/`、`src/WtLatencyUFT/`、`src/QuoteFactory/`。
`src/**` 一行未改，本文件只做登记；级别按「对覆盖率取样 / 对生产运行」的影响给建议值，由主控裁定。
本次复核（2026-10-29）只核对源码、CMake 和现有测试前提，未运行用例；下列静态路径不记作本轮运行复现。

## BUG-R14-001 [已撤回] CoverageTests_m_WtUftRunner / m_WtLatencyUFT 缺少 libWtUftCore.a（原判断不成立）

- 状态: 已撤回
- 严重级别: 不适用（依赖缺失判断为误报）
- 位置: `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（只 `TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_SHARED_LIBS})`），对照同文件里 porter/btporter 的按后缀专门补链
- 复核判定: 生产 CMake 确实把 `WtUftCore` 列为 `WtUftRunner` / `WtLatencyUFT` 依赖，但 coverage CMake 在 422-441 行扫描各模块自己的 `CMakeLists.txt`，把命中的核心库加入 `WT_M_CORES`，并在 441 行链接 `${WT_COV_SHARED_LIBS} ${WT_M_CORES}`。两个模块的 CMake 都包含 `WtUftCore`，因此「共享库集合没有它」不足以推出缺链；原缺链结论撤回。`WtUftCore` 为静态库（其 CMake 第 23 行）。
- 影响范围: 本次未执行 coverage 目标链接，故不对实际链接成功作运行结论。当前这条“必然未定义符号”的登记不成立；若目标链接仍失败，应基于该目标生成后的完整链接命令和 linker diagnostics 另行登记。
- 相关测试: `m_WtUftRunner/r14_wtuftrunner_main_test.cpp` 全部 12 条启用用例、`m_WtLatencyUFT/r14_uftlatency_main_test.cpp` 全部 4 条启用用例
- 备注: 本条是对 CMake 依赖判读的误报，不需要按本登记修改 CMake。

## BUG-R14-002 WtUftRunner 的 main 丢弃 config() 返回值后照样启动 runner

- 状态: 待审核
- 严重级别: Major（配置失败未阻止启动；具体进程表现尚未运行验证）
- 位置: `src/WtUftRunner/main.cpp:75`（`runner.config(filename);` 返回值未接）→ `:78`（`runner.run(false);`）→ `src/WtUftRunner/WtUftRunner.cpp:355`（`_uft_engine.run();`）、`:360`（`ShareManager::self().start_watching(2);`）、`:362~365`（`while(!_to_exit) sleep_for(10ms)`）
- 复核判定: `main.cpp:75-78` 确认忽略 `config()` 的 `false` 并继续调用 `run(false)`。但当前源码不支持“必然 SIGSEGV”的说法：`_uft_engine` 是值成员，其构造函数将 `_cfg` 置空；`WtUftEngine::run()` 在 `_cfg == NULL` 时会走 `ALLDAY` 分支并启动 ticker。若运行器停在该路径，之后会进入等待退出信号的循环；本次未运行该入口，实际进程行为仍待验证。`ShareManager::start_watching()` 在未初始化时返回 `false`，不等于必然起看门狗线程。
- 影响范围: 可静态确认的是配置加载失败后程序没有受控失败返回，仍尝试启动一个未按配置初始化的运行器；不能据此断言必崩或“看门狗线程已启动”。
- 相关测试: `r14_wtuftrunner_main_test.cpp:r14_uft_entry.DISABLED_non_help_argv_reaches_engine_run_and_watchdog_thread`、`r14_uft_runner.DISABLED_run_false_loops_until_signal`
- 证据边界: 相关 `DISABLED_` 用例未在本次运行；源码控制流可确认，信号/挂起结果待验证。与 BUG-R6-011 是同类“config 失败仍 run”问题，但作用于不同 runner 与入口，保留为独立发生点。修复方向：main 检查 `config()` 返回值并以失败码退出。

## BUG-R14-003 四个可执行体未校验 basefiles 必需段

- 状态: 待验证
- 严重级别: Major（缺必需配置段时的崩溃路径；运行信号未复现）
- 位置: `src/WtUftRunner/WtUftRunner.cpp:70~71`、`src/QuoteFactory/main.cpp:108~109`、`src/WtLatencyUFT/UftLatencyTool.cpp:212~213`、`src/WtLatencyHFT/HftLatencyTool.cpp:361~362`
- 复核判定: 四处源码都在取得 `basefiles` 后直接访问成员；`WTSVariant::get()` 对缺失键返回 `NULL`。因此“可解析配置对象缺少 `basefiles` 时会走到空指针成员调用”是源码静态成立的条件路径。是否属于产品接受的输入、以及是否实际以 SIGSEGV 结束，未由本次运行验证；建议将它表述为“缺必需配置段未受控拒绝”，不要把测试规避方式当运行复现。
- 影响范围: 影响 `WtUftRunner`、`QuoteFactory`、UFT/HFT latency 初始化的四个独立调用点。测试中的 authored config 通过提供 `basefiles` 对象只验证子键缺省分支，不覆盖整段缺失。`DISABLED_` 用例也不构成运行证据。
- 相关测试: 上述用例 + `r14_lazy_hft_tool.DISABLED_init_success_dereferences_null_cfgbf_without_basefiles`、`r14_lazy_uft_tool.DISABLED_init_success_needs_real_config_and_engine`、`r14_quote_initialize.DISABLED_existing_config_enters_parsers_run_and_endless_loop`
- 证据边界/备注: 与 BUG-R6-002 / BUG-R6-012 是同一类 schema 守卫问题，但这里是四个不同 executable/tool 的代码位置，保留为 R14 的独立发生点。修复方向：检查段存在且为 object；缺失时记录字段名并返回失败。

## BUG-R14-004 QuoteFactory / WtBtRunner 缺配置文件时提示拼错且返回 0

- 状态: 待验证
- 严重级别: 候选 Major（退出码契约待确认）
- 位置: `src/QuoteFactory/main.cpp:304~308`（`fmt::print("coniture {} not exists", filename); return 0;`）
- 复核判定: 源码确认 `QuoteFactory` 对显式不存在的 `-c` 路径打印拼错的 `coniture` 并返回 0；`WtBtRunner` 有同类路径（原 BUG-R28-008：`confiture`、返回 0）。这两处合并为一个“缺配置文件提示/退出码”缺陷家族，R28-008 保留为已合并发生点。帮助选项也可能正常返回 0，不能单独证明缺配置成功码一定违约；仍需确认部署/CLI 对缺文件的退出码契约。
- 影响范围: 若调用方依赖非零码识别启动失败，会将这两种缺配置路径误判为成功；此影响依赖调用方契约，当前源码本身只证明输出与退出码。
- 相关测试: `r14_quotefactory_main_test.cpp:r14_quote_entry.missing_config_path_returns_zero_via_explicit_c`、`both_c_and_l_given_still_returns_zero_when_files_absent`、`r14_quote_entry.DISABLED_bare_argv_depends_on_cwd_contents`
- 证据边界/备注: 文案与 `return 0` 为源码静态证据；本次未执行可执行体。修复方向是拼写正确、保留换行并使用诊断日志；退出码调整以 CLI 契约为准。

## BUG-R14-005 LatencyTool 的 run() 捕获异常后不记录

- 状态: 待验证
- 严重级别: 待定（异常是否代表不可忽略的运行失败待验证）
- 位置: `src/WtLatencyUFT/main.cpp:25~32`、`src/WtLatencyHFT/main.cpp:25~32`（`test_uft()/test_hft()` 后 `printf("press enter key to exit\r\n"); getchar();`）；`src/WtLatencyUFT/UftLatencyTool.cpp:315~326`、`src/WtLatencyHFT/HftLatencyTool.cpp:494~505`（`try { ... } catch (...) {}`）
- 复核判定: 两个 `main()` 的 `getchar()` 前有明确“press enter key to exit”提示，属于当前交互式工具行为，单凭它不能定为产品缺陷；它会影响不提供 stdin 的自动化测试，但那是测试前提。`run()` 的 `catch (...) {}` 确实吞掉所有 C++ 异常而没有日志，不过本次未触发异常，是否覆盖可恢复情况或产品约定需确认。
- 影响范围: 可确认的是异常分支缺少可诊断输出；不能从空 catch 推断实际配置错误或共享内存错误会以异常形式出现，也不能将 `getchar()` 描述为崩溃/不可测试。
- 相关测试: `r14_uftlatency_main_test.cpp:r14_lazy_uft_tool.DISABLED_entry_blocks_on_getchar`、`r14_hftlatency_main_test.cpp:r14_lazy_hft_tool.DISABLED_entry_blocks_on_getchar`
- 证据边界/备注: 测试列出的 `DISABLED_` 用例本次未运行。若 latency 工具需要非交互自动化，再增加可选退出/参数接口；若 `run()` 可遇到异常，建议记录 `std::exception::what()`，另对非标准异常留通用错误日志。

## BUG-R14-006 LatencyTool 使用 cwd 相对配置路径（产品目录契约待确认）

- 迭代02第二轮前状态（历史）: - 状态: 待验证
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（依赖工具的工作目录契约）
- 位置: `src/WtLatencyUFT/UftLatencyTool.cpp:200`（`WTSLogger::init("logcfg.yaml")`）、`:202~204`（`std::string cfgFile = "config.yaml";`）；`src/WtLatencyHFT/HftLatencyTool.cpp:350`、`:353`、`:405`（`_act_mgr.init("actpolicy.yaml")`）
- 复核判定: UFT/HFT latency 工具源码使用相对路径，文件解析会受 cwd 影响；这点静态可确认。相对路径本身是否错误取决于产品是否承诺可从任意目录启动。`dist/` 同名文件和用例创建空 cwd 只说明不同 cwd 有不同文件可见性，不证明当前工作目录约定违约；`WtUftRunner` 的默认配置虽相对 cwd，但提供 `-c/-l` 覆盖。
- 影响范围: 这是启动目录契约/自动化夹具确定性问题的候选，不以目前证据判为已证实产品缺陷。
- 相关测试: `r14_lazy_uft_tool.empty_cwd_is_really_empty_and_restored`、`r14_lazy_hft_tool.empty_cwd_is_really_empty_and_restored`
- 证据边界/备注: `ScopedEmptyCwd` 的测试本次未运行。先确认工具应相对 cwd 还是可执行文件目录解析配置；若要求安装目录稳定解析，再加目录解析接口或 CLI 参数。

## BUG-R14-007 [已合并] getBinDir() 外部链接重复定义

- 状态: 已合并
- 严重级别: 条件性链接风险（不另计）
- 位置: `src/WtUftRunner/WtUftRunner.cpp:22~33`、`src/QuoteFactory/main.cpp:50~61`（两处同名同形、都是全局函数），对照 `src/WtRunner/WtRunner.cpp:26~37` 与 `src/Share/ModuleHelper.hpp:78` 的 `static` 版本（BUG-R6-010 已登记）
- 复核判定: 两处 `getBinDir()` 都是外部链接自由函数，确有自定义合并目标时的 ODR/重复符号风险；但 `WtUftRunner` 与 `QuoteFactory` 当前由各自可执行目标编译，coverage 也按模块拆目标。该同名定义风险与 BUG-R6-010 是同一家族，合并计数，R6-010 已包含 WtUftRunner 的 occurrence，本条补记 QuoteFactory occurrence。`initial_path()` 得到启动时 cwd，但“模块目录必须等于可执行文件目录”的运行契约未由此次静态检查确认。
- 影响范围: 当前受支持的分目标构建没有由此证明链接失败；不将自定义合并目标风险写成当前构建故障，也不把 cwd 差异直接认定为产品路径错误。
- 相关测试: `r14_quote_binroot.*` 两条、`r14_uft_binroot.*` 两条、`r14_uft_init.init_with_missing_logcfg_stamps_inst_dir_with_binroot`、`r14_quote_initialize.missing_config_stamps_module_dir_then_returns`
- 证据边界/备注: 本次未做合并链接或目录运行验证。后续若目标合并，改内部链接或共享唯一实现；若要求按可执行文件目录定位，应单独确认并测试该契约。

## BUG-R14-008 WtUftRunner 的日志转发回调为空（生产调用路径待确认）

- 迭代02第二轮前状态（历史）: - 状态: 待验证
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（取决于是否有受支持的 handler 注册路径）
- 位置: `src/WtUftRunner/WtUftRunner.cpp:375~383`（`const char* LOG_TAGS[]`，外部链接、全文件无使用点）与 `:385~387`（`void WtUftRunner::handleLogAppend(WTSLogLevel /*ll*/, const char* /*msg*/) {}`）
- 复核判定: `LOG_TAGS` 表未在本 TU 使用，其同名外部定义的条件性冲突已并入 BUG-R6-010；不另计。`handleLogAppend` 为空是源码事实，但 `WtUftRunner::init()` 调 `WTSLogger::init(filename)` 时使用默认空 handler，当前入口没有注册 `this`；未找到 main 启动路径通过 `registerHandler(&runner)` 注册它。因此不能据此说正常 WtUftRunner 日志被丢失。若其他受支持调用者显式注册该对象，回调将不转发日志，注册契约/消费者仍待确认。
- 影响范围: 只对显式注册该 ILogHandler 的调用路径可能有影响；本次没有查到生产注册点，未证明实盘日志路由受损。
- 相关测试: `r14_uft_logtags.table_matches_the_seven_declared_levels`（钉住七个字面量与 `Includes/WTSTypes.h` 的 LL_ALL=100..LL_NONE=106 对齐）、`r14_uft_loghandler.handleLogAppend_is_a_noop_for_every_declared_level`
- 证据边界/备注: 现有测试直接调用空方法并断言不抛异常，不能证明生产侧已注册 handler 或期望转发。若该接口由外部注册调用，补接口契约测试；若无人注册，可移除接口继承与 dead callback。

## BUG-R14-009 WtUftRunner 未释放其持有的根配置引用

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtUftRunner/WtUftRunner.cpp:49-51,60-67`（析构无 release，`config()` 覆盖 `_config`）与 `src/WtUftCore/WtUftEngine.cpp:308-320,58-70`（engine retain/release 自己的引用）
- 复核判定: `WtUftRunner::config()` 每次把 loader 返回的根指针写入 `_config`，类析构函数为空且没有释放旧/当前 `_config`。成功配置时 `WtUftEngine::init()` 对 `env` 增加一次引用，engine 析构只释放该次引用；runner 自己的 loader 引用仍无对应 release。重复调用还会覆盖旧根指针。此为源码生命周期缺口；本次未用 ASan/运行内存测量。
- 影响范围: 每次成功配置后，runner 自持的根配置树引用未释放；对象复用/多次 config 时会累积。QuoteFactory 在成功解析后走到 `config->release()`，其“中途早退泄漏”说法没有当前源码路径支持；LatencyTool 的 `NULL` 早退发生在 loader 返回空时，且成功路径末尾 release，本条不覆盖这两处。
- 相关测试: `r14_uft_config.missing_then_authored_order_is_independent`
- 证据边界/备注: `missing_then_authored_order_is_independent` 只会调用两次并检查返回/顺序，不量化泄漏；本次未运行。建议明确 `_config` 所有权，使用 RAII 或在替换前及析构时配对 release。

## BUG-R14-010 WtUftRunner::run(bool bAsync) 的参数被完全忽略，异步模式静默失效

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtUftRunner/WtUftRunner.cpp:351`（`void WtUftRunner::run(bool /*bAsync*/ /* = false */)`）与头文件 `src/WtUftRunner/WtUftRunner.h`（`void run(bool bAsync = false);`），调用点 `src/WtUftRunner/main.cpp:78`（`runner.run(false);`）
- 现象: 实现里形参被注释掉、函数体（`:353~372`）无条件进入 `_uft_engine.run(); _parsers.run(); _traders.run(); start_watching(2); while(!_to_exit){ sleep_for(10ms); }`，没有任何按 `bAsync` 起线程或立即返回的分支。默认实参只需在头文件声明，实现在定义处未重复写默认值是正常 C++ 用法，不作为缺陷。
- 影响范围: 任何 `runner.run(true)` 的调用者以为拿到异步接口，实际会阻塞到信号到来（与 WtRunner 的 `run(bAsync)` 语义不一致，容易被照抄）；覆盖率侧则意味着「非 help 分支」必然落在死循环里，取样必须换进程。
- 相关测试: `r14_uft_runner.DISABLED_run_false_loops_until_signal` 只覆盖同步参数的禁用挂起路径，不能验证 `run(true)`。
- 证据边界/备注: 参数被忽略且循环无条件执行是源码静态证据；头文件明示 `bAsync` 控制异步模式，WtRunner 对 `true` 则跳过等待循环。当前没有本次运行证据。修复方向：实现异步分支，或删除/更名此参数并明确同步语义。
