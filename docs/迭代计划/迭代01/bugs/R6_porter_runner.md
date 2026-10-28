# R6 缺陷清单 — `src/WtPorter/WtRtRunner.cpp` + `src/WtRunner/`（第七波 lane R6）

> 只读生产代码，本文件只登记不修复；每条状态一律 `待审核`。
> 涉及用例见 `docs/迭代计划/迭代01/manifests/R6_porter_runner.md`。
> 崩溃/挂死类一律整条 `DISABLED_`，触发条件写在用例名字下面的注释里。

## BUG-R6-001 WtRtRunner::config 不校验加载结果，配置文件缺失即空指针解引用

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:489-494`（`_config = isFile ? WTSCfgLoader::load_from_file(...) : load_from_content(...)` 之后直接 `_config->get("basefiles")`）
- 现象: 加载失败时 `_config` 是 NULL，函数没有 `if(_config == NULL) return false;` 这一层，下一行就解引用；同族兄弟 `src/WtRunner/WtRunner.cpp:69-74` 有这个判空并返回 false，两条实现不对称。
- 影响范围: `WtPorter` 的 C 导出层 `init_porter/config_porter/run_porter` 任一入口在给定错误路径时直接把宿主打死（SIGSEGV，`catch(...)` 抓不到），实盘侧「配置写错」从可恢复错误变成进程崩溃；覆盖率侧导致 `config()` 整段 L489-714（约 120 行）无法在进程内取样。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterConfig.DISABLED_missing_config_file_derefs_null_config`（未启用）
- 备注: 不阻塞本轮其它用例；修复方向是照抄 WtRunner.cpp:70-74 的判空 + 错误日志（不实施）。

## BUG-R6-002 WtRtRunner::config 不校验 basefiles 段，合法但缺段的配置同样崩

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:494-495`（`cfgBF = _config->get("basefiles"); if (cfgBF->get("session"))`）
- 现象: `basefiles` 不存在时 `cfgBF` 为 NULL，紧接着无条件 `cfgBF->get("session")`；同函数后续对 `parsers`/`traders`/`executers`/`routers` 都判了 NULL（`:594/:627/:665/:702`），唯独 `basefiles` 没有。
- 影响范围: 一份语法正确但漏写 `basefiles` 的 dtcfg 会让实盘 runner 直接段错误；`config()` 前半段（L494-590）因此只能在进程外采样。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterConfig.DISABLED_valid_content_without_basefiles_derefs_null_section`
- 备注: `src/WtRunner/WtRunner.cpp:77-78` 是同构缺陷，另立 BUG-R6-012。

## BUG-R6-003 createExtParser 无条件解引用尚未赋值的 _engine

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:295-303`（`adapter->initExt(id, parser, _engine, _engine->get_basedata_mgr(), _engine->get_hot_mgr())`）；`_engine` 只有 `initEngine()`（`:838-882`，经 `config()` 才调用）会赋值
- 现象: 未 `config()` 就调 `create_ext_parser` 导出入口时 `_engine == NULL`，同一行表达式里连续解引用三次；紧邻的 `createExtExecuter`（`:305-312`）却完全不依赖 `_engine`，可安全调用 —— 同一族两个入口的健壮性差一个判空。
- 影响范围: 调用顺序稍有不同（先建 parser 再 config）就进程级崩溃；`WtPorter.cpp` 的 `create_ext_parser` 导出层没有任何前置校验能拦住它。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterExtParser.DISABLED_create_ext_parser_without_engine_crashes`；对照启用用例 `R6PorterExtExecuter.create_ext_executer_returns_true_without_engine`
- 备注: 与 `bugs/q06_porters.md` BUG-Q06-002 是同一类「引擎直取入口无 NULL 校验」，那条讲的是 `cta_get_price` 等 getter，本条讲的是构造入口，不重复。

## BUG-R6-004 parser_* 与 on_ext_parser_quote 对 id 参数不做 NULL 校验

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:1207-1242`（六个 `parser_` 入口）、`:1243-1256`（`on_ext_parser_quote` 的 `_parsers.getAdapter(id)`）
- 现象: `id` 一路透传给 `ParserAdapterMgr::getAdapter(const char*)`，那里以 `const char*` 构造 std::string 键；传 NULL 即未定义行为。同函数族里 `_cb_parser_evt` 有判空，参数本身没有。
- 影响范围: 外部语言绑定（Python/C# 回调层）传空串以外的小概率 NULL 会把实盘进程打挂；`WtPorter.cpp` 的 `parser_push_quote` 导出层同样不校验，直接把 NULL 递进来。
- 相关测试: `r6_porter_dispatch_silence_test.cpp:R6PorterNullArgs.DISABLED_parser_id_null_builds_string_from_nullptr`
- 备注: 启用用例 `R6PorterSilence.parser_and_executer_slots_are_silent_until_registered` 只用非空 id，不踩这条路。

## BUG-R6-005 ctx_on_tick / hft 微数据入口在回调已注册时对被测对象无判空

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:441-451`（`_cb_cta_tick(id, stdCode, &newTick->getTickStruct())`）、同形还有 `:1188-1192`（ordque）、`:1194-1198`（orddtl）、`:1200-1206`（trans）
- 现象: 指针参数只在「命中回调」时求值，所以未注册路径是安全的（本轮已用启用用例钉住），一旦注册了回调又传 NULL 就段错误；引擎侧的 `on_tick` 上游并无保证一定给非空。
- 影响范围: 分发层的健壮性取决于「有没有人注册回调」，同一份输入在不同注册状态下崩与不崩两种结果；集成方无法用一次冒烟测覆盖。
- 相关测试: `r6_porter_dispatch_silence_test.cpp:R6PorterNullArgs.DISABLED_ctx_on_tick_null_tickdata_derefs`
- 备注: 启用对照 `R6PorterSilence.tick_dispatch_with_null_callback_never_touches_the_argument`。

## BUG-R6-006 run() 不校验 _engine；release() 用 WTSLogger::stop 造成进程级副作用

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:1091-1124`（`:1098` 的 `_engine->run()`；`src/WtRunner/WtRunner.cpp:585` 是同形的一份）、`src/WtPorter/WtRtRunner.cpp:1141-1155`（`release()` 末行 `WTSLogger::stop()`，见 `src/WTSTools/WTSLogger.cpp:255-261`）
- 现象: `run()` 里 `_parsers.run()/_traders.run()` 之后无条件 `_engine->run()`，未 config 就 run 直接段错误；`release()` 虽然对 `_engine` 判了空（`:1151`），但它调 `WTSLogger::stop()` 会把静态 `m_bStopped` 置位并 `spdlog::shutdown()`，同进程内后续所有用例的日志通道被永久关掉。
- 影响范围: `run()` 的 34 行与 `release()` 的 15 行在本测试形态下不可取样（一个崩一个污染全局）；`while(!_to_exit) sleep(10ms)`（`:1112-1115`）在没有信号钩子的进程内是死循环。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterRunRelease.DISABLED_run_without_engine_and_release_global_logger_stop`
- 备注: 留给主控裁定 —— 建议 `release()` 拆成「停引擎」与「停日志」两级，或在「每用例一进程」采样口径下单独跑一条进程收这 15 行。

## BUG-R6-007 ExpExecuter::init 硬绑进程单例，多实例 createExtExecuter 语义不成立

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/ExpExecuter.cpp:24-28`（`getRunner().executer_init(name())`）与调用点 `src/WtPorter/WtRtRunner.cpp:305-312`
- 现象: 执行器初始化回调固定投递到 `getRunner()` 返回的那个静态单例；若 `createExtExecuter` 是在另一个 `WtRtRunner` 实例上调的（回测/多策略宿主、以及本项目的栈上测试夹具），注册在该实例上的 `_cb_exec_init` 永远收不到事件，只有单例的能收到。
- 影响范围: 「一个进程内跑多个 runner」这条路不成立；导出层 `create_ext_executer` 的语义与 `register_exec_callbacks` 的作用域不一致（后者是按实例的）。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterExtExecuter.executer_callbacks_fire_on_the_runner_that_registered_them`（钉住「按实例注册」这一半）
- 备注: 修复方向是给 `ExpExecuter` 注入所属 runner，不改本轮代码。

## BUG-R6-008 loadFactories 用文件存在性判目录，传文件路径时抛未捕获异常

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtCore/CtaStrategyMgr.cpp:30-41`（守卫 `StdFile::exists(path)` 见 `src/Share/StdUtils.hpp:137-145`，实现在 `std::filesystem::exists`；随后是 `boost::filesystem::directory_iterator`），经 `src/WtPorter/WtRtRunner.cpp:1168-1186` 四个 `addXxxFactories` 转发
- 现象: `std::filesystem::exists` 对「文件」也返回 true，守卫放行后 boost 的 `directory_iterator` 对文件路径抛 `filesystem_error`；`addCtaFactories` 返回 bool 却没有 try/catch，异常一路穿透到调用方（C 导出层跨 ABI 抛异常是 UB）。
- 影响范围: 配置里把策略工厂目录写成文件名（或写成一个已被替换成软链目标的空文件）时，实盘/回测的 `add_cta_factories` 入口行为未定义；`sel/hft/executer` 三族同形。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterFactories.factory_path_pointing_at_a_file_does_not_return_cleanly`（启用；断言「抛异常或返回 false，唯独不许返回 true」）
- 备注: 现状是抛异常，所以用例按「可捕获」写；若改为守卫返回 false，用例仍然成立。

## BUG-R6-009 WtRtRunner 构造列表漏初始化 4 个成员（含 _config 本身）

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtPorter/WtRtRunner.cpp:55-100`（初始化列表）对照成员声明 `src/WtPorter/WtRtRunner.h`（`_config`、`_feed_obj`、`_feeder_bars`、`_feeder_fcts`）
- 现象: 列表把 24 个回调槽和 `_data_store/_is_hft/_is_sel/_to_exit/_ext_*` 都置了 NULL/false，唯独没写 `_config`、`_feed_obj`、`_feeder_bars`、`_feeder_fcts`；`_config` 只在 `config()`（`:491`）里赋值，`_feeder_*` 只在外部 loader 回调里赋值。
- 影响范围: ①未 config 时 `_config` 是野指针，BUG-R6-001 的后果从「稳定 NULL 段错误」变成「随机地址解引用」；②`feedAdjFactors`（`:277-280`）不判空即调用 `_feeder_fcts`，未注册即跳飞指针（同一现象已由 `bugs/q06_porters.md` BUG-Q06-004 登记，本条给出根因）；③`WtRtRunner` 作为栈对象或第二个实例时不再有「静态零初始化」的兜底。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterFeed.DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage`
- 备注: 与 BUG-Q06-003（`WtBtRunner::_hft_mocker` 同类漏项）是同一次改造的成组缺陷，建议一并修。

## BUG-R6-010 LOG_TAGS 与 getBinDir 在多个模块里以外部链接重复定义

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtPorter/WtRtRunner.cpp:1126-1134`（`const char* LOG_TAGS[]`）、`src/WtRunner/WtRunner.cpp:614-622`（同名同形另一份）、`src/WtRunner/WtRunner.cpp:26-37`（`const char* getBinDir()`）对照 `src/Share/ModuleHelper.hpp:78`（`[[maybe_unused]] static const char* getBinDir()`）
- 现象: 三者都是全局/命名空间级实体而非 `static`/匿名命名空间；`WtRtRunner.cpp:124` 用的其实是头文件里那份 static 的 `getBinDir`，而 `WtRunner.cpp:26` 自己又发了一份外部链接的同名实现，两份实现的路径来源不同（一个来自 `ModuleHelper.hpp`，一个直接 `boost::filesystem::initial_path`）。
- 影响范围: 任何把这两个 TU 编进同一二进制的构建（覆盖率目标正是按「一目录一二进制」硬隔离的，见 `src/TestUnits/coverage/CMakeLists.txt` 顶部 BUG-L00-003 注释）都会撞 duplicate symbol；今天不炸只是因为拆分表把它们隔开了。
- 相关测试: `r6_porter_lifecycle_test.cpp:R6PorterLogRoute.level_to_tag_table_is_the_documented_seven_entries`、`r6_wtrunner_main_test.cpp:r6_wtrunner_config.log_tag_table_matches_the_seven_declared_levels`（两份表内容一致性由这两个用例分别钉住）
- 备注: 修复方向是把表与 `getBinDir` 收进匿名命名空间或 `inline`，不改本轮代码。

## BUG-R6-011 main 在 config 失败后照样 run，落到未赋值的 _engine 上

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtRunner/main.cpp:118`（`runner.config(filename);` 返回值被丢弃）→ `:125`（`runner.run(false)`）→ `src/WtRunner/WtRunner.cpp:585`（`_engine->run()`）
- 现象: `config()` 在加载失败时 `return false` 且 `_engine` 从未被 `initEngine()` 赋值（保持构造后的 NULL，`WtRunner.cpp:41-48` 也没初始化它），`main` 不看返回值继续 `run()`；`run()` 内部只有 `catch(...)`，对 SIGSEGV 无效。
- 影响范围: WtRunner 可执行体在「配置文件不存在/写错」这个最常见错误下不是打印错误退出，而是崩溃；同步 `run()` 还会在 `:600-603` 的 `while(!_to_exit) sleep(10ms)` 上永挂。这直接导致入口侧除 `--help` 以外的全部分支无法在进程内取样。
- 相关测试: `r6_wtrunner_main_test.cpp:r6_wtrunner_entry.DISABLED_bare_argv_falls_through_to_null_engine_run`、`DISABLED_missing_config_file_still_runs`
- 备注: 留给主控裁定。可执行的取样方案是「每用例一进程 + 预期崩溃码」或给 `main` 注入早退点；本轮按约束不启用。

## BUG-R6-012 WtRunner::config 不校验 basefiles 段（与 WtRtRunner 同构）

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtRunner/WtRunner.cpp:77-78`（`cfgBF = _config->get("basefiles"); if (cfgBF->get("session"))`）
- 现象: 与 BUG-R6-002 同形：`_config` 有判空（`:70-74`）而 `basefiles` 没有；同函数对 `parsers`（`:163-164`）、`traders`（`:196-197`）、`executers`（`:233-234`）、`routers`（`:270-271`）都判了空。
- 影响范围: 语法正确但缺 `basefiles` 的 config.yaml 让 WtRunner 段错误，`config()` 主体 L76-281（约 150 行）因此在进程内不可取样；本轮 WtRunner.cpp 能覆盖的只有 ctor/dtor、init、config 早退、handleLogAppend、LOG_TAGS、getBinDir 六小段。
- 相关测试: `r6_wtrunner_main_test.cpp:r6_wtrunner_config.DISABLED_empty_object_config_derefs_null_basefiles`
- 备注: 修复方向与 BUG-R6-002 一致（一次 PR 可同批处理）。

## BUG-R6-013 覆盖率 m_<模块> 目标不链本族核心静态库，CoverageTests_m_WtRunner 建不起来

- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/TestUnits/coverage/CMakeLists.txt` 末尾 `WT_MAIN_MODULES` 循环（`TARGET_LINK_LIBRARIES(CoverageTests_m_${mod} PRIVATE ${WT_COV_SHARED_LIBS})`），对照同文件里 porter/btporter 的专门补链（`if(suffix STREQUAL "porter") TARGET_LINK_LIBRARIES(... WtCore)`）
- 现象: `WT_COV_SHARED_LIBS` 列表里没有 `WtCore`，而本目标会把 `src/WtRunner/WtRunner.cpp` 与 `main.cpp` 一起编进二进制，二者必然引用 `wtp::WtCtaEngine/WtHftEngine/WtSelEngine/WtDtMgr/CtaStrategyMgr/EventNotifier/WtHelper` 等符号；`WtPorter` 是 hidden 可见性的 dylib（同文件顶部 BUG-L00-002 已实测「内部符号本来链不上」），不能替这些符号提供出处。`LoaderRunner` 那条已跑通只是因为它不碰核心。
- 影响范围: `m_WtRunner` 目录有 .cpp 后 configure 会建出目标，但链接期会报未定义符号；本轮 lane 只能交「语法门禁」，`src/WtRunner/WtRunner.cpp` 的 491 行取样要等这条打通。属于构建配置决定，不在我的改动权限内（禁改 CMakeLists）。
- 相关测试: 三个文件全部通过 `-fsyntax-only` 门禁，与本条无关
- 备注: 留给主控裁定 —— 建议在 `WT_MAIN_MODULES` 循环里按模块补一份核心库（WtRunner→WtCore），并同步核对 WtUftRunner/WtBtRunner 的对应库；不要一次补两个核心（BUG-L00-005 的静默择一）。

## BUG-R6-014 initActionPolicy 两份实现不对称，WtRunner 侧不校验配置项

- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtRunner/WtRunner.cpp:411-414`（`return _act_policy.init(_config->getCString("bspolicy"));`）对照 `src/WtPorter/WtRtRunner.cpp:1157-1166`（先 `strlen(action_file) <= 0` 才返回 false）
- 现象: 实盘侧在 `bspolicy` 缺省时直接返回 false 并跳过加载，WtRunner 侧把空串递给 `ActionPolicyMgr::init`，行为由那个函数内部决定（两条实现对「同一个缺省配置」给出不同结论）。
- 影响范围: 同一份 dtcfg 在 WtRunner 与 WtPorter 下的初始化结果不同；`WtRunner.cpp:159-160` 是 `config()` 里唯一的早退点之一，差异会级联到 parsers/traders 的加载。
- 相关测试: 无启用用例可达（`initActionPolicy` 是 private 且只能经 `config()` 触发，见 BUG-R6-012/013）
- 备注: 顺带登记 `initDataMgr`（`WtRunner.cpp:416-426` vs `WtRtRunner.cpp:884-896`）与 `initEngine`（`:365-409` vs `:838-882`）的同构不同形，方便一次评审收口。

## BUG-R6-015 handleLogAppend 对日志级别没有范围校验

- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtPorter/WtRtRunner.cpp:1136-1139`、`src/WtRunner/WtRunner.cpp:624-627`（两处都是 `LOG_TAGS[ll-100]`）
- 现象: 表长 7 项，取用式按 `ll-100` 索引，但没有任何 `if(ll < LL_ALL || ll > LL_NONE)` 守卫；`WTSLogLevel` 是 `: uint32_t` 的枚举，越界值（例如外部绑定传来的 0 或 200）就是数组越界读，读到的指针再交给 `std::string tag = tag` 构造。
- 影响范围: 日志转发是多线程共享路径（`ILogHandler` 由 spdlog sink 回调），一次越界读可能让实盘进程崩在日志上而不是崩在交易上；两处同形缺陷意味着「修一处忘一处」的概率高。
- 相关测试: 两个文件各有一条启用用例只遍历合法区间（`R6PorterLogRoute.handleLogAppend_accepts_every_declared_level`、`r6_wtrunner_init.ctor_dtor_roundtrip_routes_declared_levels_only`），越界值按规则不进启用用例
- 备注: 修复方向是加 clamp 或 `static_assert` 把表长与枚举尾对齐；本轮只登记。
