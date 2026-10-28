# R14 缺陷登记 — 可执行体/工具模块（第七波 lane R14）

范围：`src/WtUftRunner/`、`src/WtLatencyHFT/`、`src/WtLatencyUFT/`、`src/QuoteFactory/`。
`src/**` 一行未改，本文件只做登记；级别按「对覆盖率取样 / 对生产运行」的影响给建议值，由主控裁定。

## BUG-R14-001 CoverageTests_m_WtUftRunner / m_WtLatencyUFT 没有 libWtUftCore.a 的出处，链接期必报未定义符号

- 状态: 待审核
- 严重级别: Blocker（挡住本 lane 四个目标里的两个）
- 位置: `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（只 `TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_SHARED_LIBS})`），对照同文件里 porter/btporter 的按后缀专门补链
- 现象: 实测依赖清单 —— `src/WtUftRunner/CMakeLists.txt:21-25` 与 `src/WtLatencyUFT/CMakeLists.txt:20-24` 的生产 LIBS 都是 `WtUftCore`（`src/WtUftCore/CMakeLists.txt:23` 是 **STATIC**）。而 `WT_COV_SHARED_LIBS` 里没有 `WtUftCore`，能传递带上核心的只有 `WtPorter→WtCore`（`src/WtPorter/CMakeLists.txt:22`）与 `WtDtPorter→WtDtCore`（`src/WtDtPorter/CMakeLists.txt:22-25`）；`WtUftStraFact/CMakeLists.txt:18-21` 只链 `${STDCXXFS} dl`，**不提供** WtUftCore 符号。于是 `WtUftRunner.cpp` 引用的 `wtp::WtUftEngine::*`、`ShareManager::self/initialize/init_domain/start_watching`、`wtp::ParserAdapter/TraderAdapter`（UFT 形）、`UftStraContext`、`wtp::WtHelper::_inst_dir/_gen_dir`（定义在 `src/WtUftCore/WtHelper.cpp:28-29`）全部没有出处。
- 影响范围: 这两个目标的取样拿不到（用例本身 4/4 过 `-fsyntax-only`）。另两个模块可传递拿到出处：`QuoteFactory/main.cpp` 需要的 WtDtCore 经 `WtDtPorter` 传递，`HftLatencyTool.cpp` 需要的 WtCore 经 `WtPorter` 传递 —— 但一旦 WtCore 与 WtDtCore 同时被抽进同一个二进制，就是本文件顶部 BUG-L00-005 描述的「同名强符号静默择一」，`m_QuoteFactory` 里 `wtp::ParserAdapterMgr/WtHelper` 的归因要复核（`nm` 级核对由主控做）。
- 相关测试: `m_WtUftRunner/r14_wtuftrunner_main_test.cpp` 全部 12 条启用用例、`m_WtLatencyUFT/r14_uftlatency_main_test.cpp` 全部 4 条启用用例
- 备注: 修复方向是照 porter/btporter 的做法，在 `WT_MAIN_MODULES` 循环里按模块补**唯一一个**核心库（WtUftRunner/WtLatencyUFT→WtUftCore），绝不同时补两个核心；本轮禁改 CMakeLists，故只登记。

## BUG-R14-002 WtUftRunner 的 main 丢弃 config() 返回值后照样 run(false)，落到未初始化引擎 + 看门狗线程 + 10ms 死循环

- 状态: 待审核
- 严重级别: Blocker（生产可用性 + 取样可行性双重）
- 位置: `src/WtUftRunner/main.cpp:75`（`runner.config(filename);` 返回值未接）→ `:78`（`runner.run(false);`）→ `src/WtUftRunner/WtUftRunner.cpp:355`（`_uft_engine.run();`）、`:360`（`ShareManager::self().start_watching(2);`）、`:362~365`（`while(!_to_exit) sleep_for(10ms)`）
- 现象: `config()` 在 `WtUftRunner.cpp:63~66` 加载失败时 `return false` 且 `_config` 保持 NULL，`initEngine()` 从未执行，但 main 不看返回值继续 `run(false)`；`run()` 外面只有 `catch(...)`，对 SIGSEGV 无效，而 `_to_exit` 只能由信号回调（`:42~45`）翻转。
- 影响范围: 配置写错/缺失时 WtUftRunner 不是报错退出而是崩溃或永挂；入口侧除 `-h/--help` 外没有任何可在进程内取样的分支（`main.cpp:56~79` 约 22 行因此锁死）。
- 相关测试: `r14_wtuftrunner_main_test.cpp:r14_uft_entry.DISABLED_non_help_argv_reaches_engine_run_and_watchdog_thread`、`r14_uft_runner.DISABLED_run_false_loops_until_signal`
- 备注: 同形的 WtRunner 版本已登记为 BUG-R6-011，建议同批修复；取样方案（每用例一进程 + 预期崩溃码）由主控裁定。

## BUG-R14-003 四个可执行体的配置解析都不校验 basefiles 段，缺段即空指针解引用

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtUftRunner/WtUftRunner.cpp:70~71`、`src/QuoteFactory/main.cpp:108~109`、`src/WtLatencyUFT/UftLatencyTool.cpp:212~213`、`src/WtLatencyHFT/HftLatencyTool.cpp:361~362`
- 现象: 四处都是 `WTSVariant* cfgBF = cfg->get("basefiles");` 后立刻 `cfgBF->get(...)` / `cfgBF->getBoolean(...)`；`WTSVariant.hpp:364~373` 的 `get()` 对缺失键返回 NULL，于是对 NULL 调成员函数。同一个函数里对 `parsers`/`traders`/`env`/`data` 却都判了空（如 `WtUftRunner.cpp:131/164/256/271`），说明是漏项而非有意。
- 影响范围: 一份「语法正确但没写 basefiles」的配置让四个进程全部段错误，`config()/initialize()/init()` 的成功支因此在进程内不可取样。本轮唯一能启用整链的 `WtUftRunner::config()` 是靠用例自己写 yaml、把 basefiles 写成只含占位键的对象（`r14_uft_config.authored_config_without_basefiles_children_walks_the_early_exits`）绕开——即「缺子键」安全而「缺整段」致命，这个反差本身就是缺陷证据。
- 相关测试: 上述用例 + `r14_lazy_hft_tool.DISABLED_init_success_dereferences_null_cfgbf_without_basefiles`、`r14_lazy_uft_tool.DISABLED_init_success_needs_real_config_and_engine`、`r14_quote_initialize.DISABLED_existing_config_enters_parsers_run_and_endless_loop`
- 备注: 与 BUG-R6-002 / BUG-R6-012 同族，建议一次 PR 收口（四处加同一句判空 + 明确错误返回）。

## BUG-R14-004 QuoteFactory 在配置文件缺失时用拼错的提示语返回成功码

- 状态: 待审核
- 严重级别: Major
- 位置: `src/QuoteFactory/main.cpp:304~308`（`fmt::print("coniture {} not exists", filename); return 0;`）
- 现象: ①`coniture` 是 `config`/`configuration` 的拼写错误，且这条输出绕过 `WTSLogger` 直接打 stdout，级别/时间与日志里那条 `:103` 的 `Loading config file {} failed` 对不上；②配置不存在属于致命启动错误，却 `return 0`（成功退出码），而同一进程对「-h」也返回 0，调用方无法区分「帮了」和「没起来」。对照 `dist/QuoteFactory/dtcfg.yaml` 是随包发布的真实配置，说明这条支在生产里真会被踩到。
- 影响范围: 脚本化部署/supervisor 会把启动失败当成成功；覆盖率取样侧也无法用退出码判定进了哪条支（本轮因此只能给 `EXPECT_EQ(rc, 0)`，并在用例注释里说明「不给 -c」的默认路径不可用，见 D2）。
- 相关测试: `r14_quotefactory_main_test.cpp:r14_quote_entry.missing_config_path_returns_zero_via_explicit_c`、`both_c_and_l_given_still_returns_zero_when_files_absent`、`r14_quote_entry.DISABLED_bare_argv_depends_on_cwd_contents`
- 备注: 建议改为 `WTSLogger::error` + 非零退出码（需要产品侧确认退出码约定后一并调整用例期望值，本轮按现状钉住）。

## BUG-R14-005 两个 LatencyTool 的入口用 getchar() 收尾并把 run() 的异常全吞，进程内/CI 都无法取样

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtLatencyUFT/main.cpp:25~32`、`src/WtLatencyHFT/main.cpp:25~32`（`test_uft()/test_hft()` 后 `printf("press enter key to exit\r\n"); getchar();`）；`src/WtLatencyUFT/UftLatencyTool.cpp:315~326`、`src/WtLatencyHFT/HftLatencyTool.cpp:494~505`（`try { ... } catch (...) {}`）
- 现象: 入口没有任何命令行参数（连 `-h` 都没有），无条件起引擎再等一个回车；`catch(...)` 空体意味着引擎里的真实异常（含配置错误、shm 失败）被静默吞掉，进程照样走到 `getchar()`。改名后的入口是 `int wt_coverage_main_wtlatencyuft()`（无参形态），本轮因此**刻意不**声明/调用它。
- 影响范围: 两个 `main.cpp` 各约 5 行 + `test_uft/test_hft`（`UftLatencyTool.cpp:28~34`、`HftLatencyTool.cpp:33~42`）共 11 行在任何进程内方案下都取不到（除非重定向 stdin）；更要紧的是延迟工具崩溃时没有任何可诊断信号。
- 相关测试: `r14_uftlatency_main_test.cpp:r14_lazy_uft_tool.DISABLED_entry_blocks_on_getchar`、`r14_hftlatency_main_test.cpp:r14_lazy_hft_tool.DISABLED_entry_blocks_on_getchar`
- 备注: 建议加 cppcli 参数（含 `--help`）+ 打印异常摘要后再退出；`catch(...)` 至少落一条 `WTSLogger::error`。

## BUG-R14-006 LatencyTool 把 logcfg.yaml / config.yaml 写死成相对路径，行为随运行目录漂移

- 状态: 待审核
- 严重级别: Major（覆盖率取样阻塞项）
- 位置: `src/WtLatencyUFT/UftLatencyTool.cpp:200`（`WTSLogger::init("logcfg.yaml")`）、`:202~204`（`std::string cfgFile = "config.yaml";`）；`src/WtLatencyHFT/HftLatencyTool.cpp:350`、`:353`、`:405`（`_act_mgr.init("actpolicy.yaml")`）
- 现象: 三处都是无目录前缀、无命令行开关、无环境变量的裸相对路径，进程 cwd 决定加载哪份（或加载不到）配置；同一份用例在 `build-cov/`、仓库根、`dist/WtLatencyUFT/` 下会进不同的分支，断言无法由输入算出。仓库里 `dist/` 下确实存在同名 yaml，所以「碰巧能跑」的假绿是真实风险。
- 影响范围: 本轮为了拿到确定分支，只能让用例自己 `mkdir/chdir` 进一个空的私有目录再还原（`ScopedEmptyCwd`），并在用例里先 `ASSERT_TRUE(guard.ok())` 把前提钉住；这属于测试侧绕行，不是修复。同类漂移在 `WtUftRunner/main.cpp:62/73` 的默认 `./logcfg.yaml`、`./config.yaml` 上也有，但那两处至少能被 `-c/-l` 覆盖。
- 相关测试: `r14_lazy_uft_tool.empty_cwd_is_really_empty_and_restored`、`r14_lazy_hft_tool.empty_cwd_is_really_empty_and_restored`
- 备注: 建议与 WtRunner/WtUftRunner 对齐，改为 cppcli 参数 + 默认落到 `WtHelper::getInstDir()` 下。

## BUG-R14-007 getBinDir() 在每个可执行体 TU 里各发一份外部链接同名实现，并被直接当成安装/模块目录

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtUftRunner/WtUftRunner.cpp:22~33`、`src/QuoteFactory/main.cpp:50~61`（两处同名同形、都是全局函数），对照 `src/WtRunner/WtRunner.cpp:26~37` 与 `src/Share/ModuleHelper.hpp:78` 的 `static` 版本（BUG-R6-010 已登记）
- 现象: 两份实现都是 `boost::filesystem::initial_path()` + `StrUtil::standardisePath()`，但被写进**两个不同核心的不同静态字段**：`WtUftRunner.cpp:57` 写 `wtp::WtHelper::_inst_dir`（WtUftCore），`QuoteFactory/main.cpp:98` 写 `wtp::WtHelper::_bin_dir`（WtDtCore 的 `set_module_dir`）；`initial_path()` 是进程启动时的 cwd 且只算一次，之后任何 chdir 都不会反映，`standardisePath()` 是否补尾斜杠也不确定（用例只能写成「等于 cwd 或 cwd+'/'，且两者恰有一个成立」）。
- 影响范围: 任何把这两个 TU 编进同一二进制的构建都会 duplicate symbol（今天靠 coverage 拆分表「一目录一二进制」硬隔离）；跨目录运行时安装目录与真实 cwd 脱钩，插件/dll 搜索路径随之错误。
- 相关测试: `r14_quote_binroot.*` 两条、`r14_uft_binroot.*` 两条、`r14_uft_init.init_with_missing_logcfg_stamps_inst_dir_with_binroot`、`r14_quote_initialize.missing_config_stamps_module_dir_then_returns`
- 备注: 建议收进匿名命名空间或复用 `ModuleHelper.hpp` 的那份，并明确返回带/不带尾斜杠的契约。

## BUG-R14-008 LOG_TAGS 全局表在 WtUftRunner 里无人引用，handleLogAppend 是空实现

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtUftRunner/WtUftRunner.cpp:375~383`（`const char* LOG_TAGS[]`，外部链接、全文件无使用点）与 `:385~387`（`void WtUftRunner::handleLogAppend(WTSLogLevel /*ll*/, const char* /*msg*/) {}`）
- 现象: 同一个字符串表已在 `src/WtRunner/WtRunner.cpp:614~622`、`src/WtPorter/WtRtRunner.cpp:1126~1134` 各发一份（BUG-R6-010）；而 UFT 侧的 `handleLogAppend` 连取表都没有，说明 WtUftRunner 作为 `ILogHandler` 注册的回调实际什么都不做——`WTSLogger::init()` 之后没有任何日志被路由给宿主进程。
- 影响范围: 表是死数据（三个 TU 同链即 duplicate）；空回调让「以 ILogHandler 方式接日志」这条 UFT 路径静默失效，排查时容易误判为日志丢失。
- 相关测试: `r14_uft_logtags.table_matches_the_seven_declared_levels`（钉住七个字面量与 `Includes/WTSTypes.h` 的 LL_ALL=100..LL_NONE=106 对齐）、`r14_uft_loghandler.handleLogAppend_is_a_noop_for_every_declared_level`
- 备注: 建议表收进 `inline constexpr` 或共享头，`handleLogAppend` 要么实现要么显式删掉接口实现并注释原因。

## BUG-R14-009 WtUftRunner::config() 重复调用会覆盖 _config 且不 release，QuoteFactory::initialize 的早退路径同样漏放

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtUftRunner/WtUftRunner.cpp:62`（`_config = WTSCfgLoader::load_from_file(...)`，函数内无一处 `_config->release()`，析构 `:49~51` 也不释放）；对照 `src/QuoteFactory/main.cpp:100`（`config` 局部变量）与 `:104`（`return` 前不需释放，但 `:252` 的 `config->release()` 只在成功路径末尾执行，中途任何新增早退都会泄漏）
- 现象: `WtUftRunner.cpp` 里对子节点是按 `var->release()`（`:144/:177`）显式管理的，唯独根 `_config` 没有；`WtLatencyUFT/UftLatencyTool.cpp:258`、`WtLatencyHFT/HftLatencyTool.cpp:417` 同样只在**成功路径末尾** release，`:213/:362` 之前的空指针崩溃路径与 `initEngine()` 之后新增的早退都会漏放。
- 影响范围: 进程一次性的 runner 影响很小，但「热重载配置」或多次 `config()` 会按次累积整棵 WTSVariant 树。本轮启用用例 `missing_then_authored_order_is_independent` 第二次调用就是这条路径（该用例只能证明「不崩、结论不变」，泄漏量需要 ASan 口径，不在本轮权限内量化）。
- 相关测试: `r14_uft_config.missing_then_authored_order_is_independent`
- 备注: 建议 `_config` 用 RAII（或析构里 release），并统一早退路径的释放。

## BUG-R14-010 WtUftRunner::run(bool bAsync) 的参数被完全忽略，异步模式静默失效

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtUftRunner/WtUftRunner.cpp:351`（`void WtUftRunner::run(bool /*bAsync*/ /* = false */)`）与头文件 `src/WtUftRunner/WtUftRunner.h`（`void run(bool bAsync = false);`），调用点 `src/WtUftRunner/main.cpp:78`（`runner.run(false);`）
- 现象: 实现里形参被注释掉、函数体（`:353~372`）无条件进入 `_uft_engine.run(); _parsers.run(); _traders.run(); start_watching(2); while(!_to_exit){ sleep_for(10ms); }`，没有任何按 `bAsync` 起线程或立即返回的分支；同时 `:352` 行首的默认值也被注释掉了，头文件与实现默认值靠「头文件那份」维持。
- 影响范围: 任何 `runner.run(true)` 的调用者以为拿到异步接口，实际会阻塞到信号到来（与 WtRunner 的 `run(bAsync)` 语义不一致，容易被照抄）；覆盖率侧则意味着「非 help 分支」必然落在死循环里，取样必须换进程。
- 相关测试: `r14_uft_runner.DISABLED_run_false_loops_until_signal`
- 备注: 建议要么实现 bAsync，要么把头文件签名改成 `run()` 并在注释里写明只能由信号退出。
