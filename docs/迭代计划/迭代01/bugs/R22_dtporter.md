# R22 缺陷清单（迭代01 · lane R22 · src/WtDtPorter 281 行未覆盖）

- 范围: 本 lane 只记录，未修改任何生产代码；`src/**` 除新建的
  `src/TestUnits/coverage/r22_dtporter/`（3 个用例文件）外一字未动，
  `src/TestUnits/coverage/CMakeLists.txt` 未改，未跑构建/二进制/ctest，无 git 操作。
- 关联用例: `r22_ext_porter_test.cpp`、`r22_runner_init_test.cpp`、`r22_porter_capi_test.cpp`
- 关联前账（不重复主张，只交叉引用）: `bugs/R5_wtdtcore.md`（StateMonitor/UDPCaster 的时钟与 socket 接缝）、
  `bugs/R6_porter_runner.md` BUG-R6-002/003/007（缺段无守卫、createExtParser 解引用、Exp* 硬绑单例同族）、
  `bugs/l05_dt_servo_porter.md` L05-005（write_log 的 NULL 类别）。

## BUG-R22-001 ExpParser/ExpDumper 硬绑 getRunner() 单例，第二个 WtDtRunner 实例的事件全部投递到别人身上
- 状态: 待审核
- 严重级别: Major（既是产品归属错误，也是本文件 266 行未覆盖的根因之一）
- 位置: `src/WtDtPorter/ExpParser.cpp:21`（`extern WtDtRunner& getRunner();`）与
  `:29-34,40-44,51-56,63-68,75-80,87-92,99-107`；`src/WtDtPorter/ExpDumper.cpp:20` 与
  `:31-35,46-50,61-65,76-80,91-95`；单例定义 `src/WtDtPorter/WtDtPorter.cpp:48-52`；
  被错位影响的创建点 `src/WtDtPorter/WtDtRunner.cpp:349-357`（createExtParser）、`:361-370`（createExtDumper）
- 现象: `ParserAdapter::initExt`（`src/WtDtCore/ParserAdapter.cpp:46-81`）在**调用方那个 runner** 上把
  ExpParser 注册为回调桥，但桥的每一个方法都不携带 owner 引用，而是重新去取函数级 static 单例：
  `ExpParser::init` -> `getRunner().parser_init(...)`。于是 `WtDtRunner runner2; runner2.createExtParser("x")`
  产生的 EVENT_PARSER_INIT/CONNECT/SUBSCRIBE 落在**单例**的 `_cb_parser_evt` 槽上，
  `runner2` 自己注册的槽一次都不响。Dumper 侧同形：ExpDumper 只带 `_id`，导出永远走单例的
  `_dumper_for_*`。同一份配置在两个 runner 上跑，行情事件与宿主回调被混到一个通道里。
- 影响范围: 1) 宿主只想用 `initialize()+start()` 单实例模型时行为正确，多实例（测试、多 datakit 并存、
  嵌入式复用）语义是错的；2) 覆盖率口径下 `WtDtRunner` 的可测面被强制绑到全局单例，
  单例状态跨用例累积，`_parsers`/`_bd_mgr` 无法复位，只能靠 createExtParser 的订阅扇出间接观测。
- 复现: `r22_porter_capi_test.cpp::ExpParserEventsReachGlobalRunnerNotTheOwningRunner`
  （owner 槽位 0 命中 / 全局槽位 1 命中，并带 `owner.parser_init` 的正交对照）与
  `::ExpDumperAlwaysForwardsToGlobalRunnerNotOwningRunner`（owner 的 bar 槽命中 1 次，
  ExpDumper 走全局槽返回 false）。与 BUG-R6-007（WtPorter 侧 ExpExecuter 同族）合并评审。

## BUG-R22-002 C 入口 start(bAsync) 不透传 allday，全天候配置下仍然起状态机线程
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtPorter/WtDtPorter.cpp:62-65`（`getRunner().start(bAsync)`，形参里没有 allday）、
  `src/WtDtPorter/WtDtRunner.cpp:44-46,60-66,68-76,80-85`、`src/WtDtPorter/WtDtRunner.h:38`
- 现象: `initialize()` 已经从配置里读了 `allday`（`WtDtRunner.cpp:189-197`），并在 allday=true 时
  **刻意跳过** `_state_mon.initialize(...)`（注释「全天候模式，不需要再使用状态机」）。
  但 `WtDtRunner::start(bool bAsync, bool bAlldayMode)` 的第二个参数只能由 C++ 侧传入，
  而导出给宿主的 C 入口 `start(bool bAsync)` 根本没有这个形参 -> bAlldayMode 恒为 false ->
  `:80-84`（同步分支 `:61-65` 同形）无条件执行 `sleep_for(5ms)` + `_state_mon.run()`。
  此时 `_state_mon` 从未 initialize（`_map` 为空），状态机线程在空表上按 2ms 轮询空转；
  也就是说「配置说不要状态机」和「运行时起状态机线程」这两个真源在同一个类里互相矛盾，
  且 `WtDtPorter.h:36` 的接口文档对此毫无提示。
- 影响范围: dt_porter 宿主按 allday 配置部署时白起一条线程并固定 5ms 启动延迟；
  同时 `start()` 的全部 43 行（`:44-86`）在本轮「不起线程/不 sleep」口径下不可测。
- 复现: `r22_runner_init_test.cpp::DISABLED_start_sync_branch_never_returns`
  （触发条件：任何 `start(false)` —— `:50` install_signal_hooks 往宿主装 SIGINT/SIGTERM，
  `:68-74` 的 `run_one()` 在唯一 post 任务跑完后永久阻塞，`:76` join 只有 `_to_exit` 被信号回调
  置位才返回 -> 整个测试进程挂死）与 `::DISABLED_start_async_branch_spawns_state_monitor_thread`
  （触发条件：`start(true)` —— sleep + StateMonitor 起线程）；
  `r22_porter_capi_test.cpp::DISABLED_capi_start_always_spawns_state_monitor_thread`
  （触发条件：C 入口 `start(true)`，专门钉住 allday 形参缺失）。

## BUG-R22-003 initialize 的三处缺段/缺键直接 NULL 解引用，段错误而不是报错返回
- 状态: 待审核
- 严重级别: Blocker（配置驱动的必崩路径）
- 位置: `src/WtDtPorter/WtDtRunner.cpp:106-107`（`config->get("basefiles")` 未判空即 `cfgBF->get("session")`）、
  `:199` + `:257-260` -> `src/WtDtCore/DataManager.cpp:67-72`（`config->get("writer")` 为 NULL 时
  `params->getCString("module")` 解引用）、`:233` -> `:262-264`（parsers 指向的 json 文件里没有
  `parsers` 键时 `initParsers(NULL)`，`cfg->size()` 解引用）
- 现象: `WTSVariant::get()` 对缺失键返回 NULL（`src/Includes/WTSVariant.hpp:364-375`），
  三处调用点都没有守卫。`wtdtserver.json` 少写 `basefiles`、少写 `writer`，
  或把 `parsers` 指到一个「存在且能解析、但没有 parsers 数组」的文件，进程立即 SIGSEGV。
  同文件的 `:99-103` 却对 `config == NULL` 做了 error+return —— 说明守卫是漏写的，不是设计取舍。
- 影响范围: 部署期最常见的一类错误（改配置漏一行）表现为崩溃而非可读报错；
  与 `bugs/R6_porter_runner.md` BUG-R6-001/002（WtRtRunner 同形）并条修复最省。
- 复现: `r22_runner_init_test.cpp` 三条
  `DISABLED_basefiles_section_missing_derefs_null_section` /
  `DISABLED_writer_section_missing_derefs_null_params` /
  `DISABLED_parsers_file_without_parsers_key_derefs_null`，触发条件逐条写注释。

## BUG-R22-004 initialize 先改进程级全局再校验配置：失败早退时副作用不可回滚（WtRdmDtReader 同形态）
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtPorter/WtDtRunner.cpp:90-91`（`WTSLogger::init` + `WtHelper::set_module_dir(modDir)`）
  早于 `:93-103` 的 `config == NULL` 早退
- 现象: 前账 `src/WtDataStorage/WtRdmDtReader.cpp:76,87` 登记的是「早退发生在起线程之前」这一形态，
  WtDtRunner 里是同一形态的更严重版本：`set_module_dir` 写的是 `WtHelper` 的**进程级 static**，
  `WTSLogger::init` 会改全局日志阈值/stop 位。一旦后面 `load_from_file` 失败，函数直接 return，
  但模块目录已经被改成这次调用的 modDir 且没人复位；同进程里后建的 runner（或另一个 datakit 实例）
  会拿这个错目录去拼 parsers/writer/index 的插件路径，表现为「插件莫名加载失败」。
  C 入口 `initialize`（`WtDtPorter.cpp:54-60`）传的是 `getBinDir()`，所以宿主只要有一次
  配置解析失败，模块目录就被永久钉在可执行体目录上。
- 影响范围: 单进程多实例 / 失败重试场景的静默串扰；本轮用例只能靠 TearDown 手工把模块目录写回来
  （见 `r22_porter_capi_test.cpp` 的 TearDown），这本身就是接缝缺失的证据。
- 复现: 启用用例 `r22_porter_capi_test.cpp::CapiInitializeWithAbsentLogFileKeepsLoggerUninited`
  钉住「C initialize 之后 `WtHelper::get_module_dir()` 非空且已被改写」这一事实；
  跨实例串扰需要第二个 runner 才能观测，受 BUG-R22-001 阻塞，故只登记不硬塞断言。

## BUG-R22-005 write_log 把调用方 message 当格式串使用，带大括号的报文使异常越过 extern "C" 边界
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtPorter/WtDtPorter.cpp:83-93`（两个分支都不校验 message）->
  `src/WTSTools/WTSLogger.cpp:330`（`print_message(message)`，其实现是
  `WTSLogger.cpp:129-135` 的 `fmt::print(buffer)`）与 `:368`（`fmt::print(message)`）
- 现象: 日志未 init（本轮的常态：logCfg 指向不存在的文件，`WTSLogger::init` 早退）时，
  `log_raw`/`log_raw_by_cat` 都走「直接把 message 当格式串 print」的支路。
  parser/trader 插件经 `write_log` 传一条含 `{}` 或 `{0}` 的原文（JSON 片段、错误串都很常见），
  fmt 抛 `fmt::format_error`，异常穿越 `extern "C"` 导出边界 -> `std::terminate`，宿主进程直接没了。
- 影响范围: 任何向 porter 转发第三方报文的插件都能一发打停 datakit；
  `write_log` 返回 void，调用方没有任何降级机会。
- 复现: `r22_porter_capi_test.cpp::DISABLED_write_log_with_braces_escapes_format_string`
  （触发条件：level >= 日志阈值且日志未 init 时 `write_log(LL_FATAL, "…{not a format}…", "parser")）。

## BUG-R22-006 write_log 不校验 level，小整数级别被静默丢弃
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtPorter/WtDtPorter.cpp:85-92`（`(WTSLogLevel)level` 直转，无范围检查）->
  守卫在 `src/WTSTools/WTSLogger.cpp:325`、`:358`（`if (m_logLevel > ll || m_bStopped) return;`）
- 现象: `WTSLogLevel` 从 `LL_ALL=100` 起排（`src/Includes/WTSTypes.h:128-137`），
  而 C 入口收的是裸 `unsigned int`。宿主侧（Python/Rust/其它语言的枚举惯例是 0..5）
  传进来的 level 全部 < 100，于是被 `m_logLevel > ll` 一句吞掉：`write_log` 返回 void、
  不告警、不落盘，日志整体消失。
- 影响范围: 跨语言绑定的默认写法会「接口通、日志空」；排障成本与 BUG-R5-005 一类静默丢弃同级。
- 复现: `r22_porter_capi_test.cpp::WriteLogBelowLoggerThresholdIsDropped`。
  说明：「日志丢失」本身在本轮口径下不可观测（不能读 stdout/挂钟），
  该用例只钉住「入参缓冲不被改写、调用不抛异常」这一可断言面，级别语义丢失靠源码陈述支撑。

## BUG-R22-007 createExtParser 的返回值不携带任何信息，撞名时旧适配器仍在表内且 ExpParser 泄漏
- 状态: 待审核
- 严重级别: Minor（含一处可达的内存泄漏）
- 位置: `src/WtDtPorter/WtDtRunner.cpp:349-357`、`src/WtDtCore/ParserAdapter.cpp:337-351`
  （`addAdapter` 的 `Same name of parsers` 分支返回 false，`:349` 正常入表）、
  `src/WtDtCore/ParserAdapter.cpp:42-44`（`~ParserAdapter()` 空实现）
- 现象: `createExtParser` 先 `new ExpParser(id)`、`adapter->initExt(...)`（内部已经 registerSpi + init +
  对全量合约发起 subscribe），再 `_parsers.addAdapter(id, adapter)`，**且不检查返回值**，
  最后无条件 `return true`。id 撞名时 addAdapter 返回 false，局部 shared_ptr 析构，
  `~ParserAdapter` 是空的、`release()` 也无人调用，于是 `initExt` 里 new 出来的 ExpParser 直接泄漏；
  宿主每调一次 `create_ext_parser("<已存在的 id>")` 就漏一个对象，还会白白多发一轮全量订阅。
- 影响范围: C 入口 `create_ext_parser`（`WtDtPorter.cpp:97-100`）返回值恒真，
  宿主无法区分「新建成功」与「id 已被占用」；重复注册 = 无界泄漏 + 重复订阅扇出。
- 复现: `r22_runner_init_test.cpp::ParsersAsInlineArrayRegistersActiveEntriesOnly`
  （对已被 initParsers 占用的 `r22_p2`、`auto_parser_1000`，以及从未用过的 id，三条 `EXPECT_TRUE`
  一起钉住「返回值零信息量」的现状；产品侧若改为返回 false，这三条会响亮失败）+
  `::ParsersAsStringPathToExistingFileIsExpanded`、`r22_ext_porter_test.cpp::PushQuoteAfterInitResolvesContractAndWrites`。

## BUG-R22-008 write_log 的 catName 为 NULL 时 strlen(NULL)（交叉引用 L05-005，不重复计账）
- 状态: 待审核
- 严重级别: Minor（复述）
- 位置: `src/WtDtPorter/WtDtPorter.cpp:85`（`if (strlen(catName) > 0)`）
- 现象: 与 `bugs/l05_dt_servo_porter.md` 的 L05-005 同一处、同一问题。R22 侧只补充：
  `WtDtPorter.h:52` 的接口文档写明「空字符串表示使用默认类别」，但没有说明 NULL 不可传，
  而 `EXPORT_FLAG` 的 C 消费方（尤其自动生成绑定）传 NULL 的概率不低。
- 影响范围: 同上（段错误）。
- 复现: `r22_porter_capi_test.cpp::DISABLED_write_log_with_null_category_hits_strlen_null`
  （仅作复现位，合并建议：与 L05-005 并条，不重复计账）。

## BUG-R22-009 覆盖率分母含 Windows-only 死码，macOS 口径永不可达
- 状态: 待审核（属报表口径问题，非产品缺陷）
- 严重级别: Minor
- 位置: `src/WtDtPorter/WtDtPorter.cpp:19-27`（`PLATFORM_NAME` 的 _WIN32 支）、
  `:29-46`（`#ifdef _MSC_VER` 的 `getModuleName()`，含 api-brief 点名的 `:40` `fs::path p(MODULE_NAME)`）、
  `:56-58`（同一 `#ifdef _MSC_VER` 内的 `CMiniDumper::Enable(...)`，含 `Common/mdump.h` 引入）
- 现象: `api-brief-R22.md` 把 `WtDtPorter.cpp:40` 与 `:57` 列为 missed，但这两行连同
  `#ifdef _MSC_VER` 整块在本机 macOS/arm64 预处理阶段就被剔除，不产生 gcno 行，
  「281 行未覆盖」的分母里混有这类不可达行；同理 `WtDtRunner.cpp` 里 `#region "Extended Parser"`
  之外的 Windows 分支。
- 影响范围: lane 目标达成度会被这类行系统性拖低；建议在报表侧按预处理后行集重新对齐分母。
- 复现: 不可复现（编译期即不存在）。本轮不写用例，仅登记口径。

## 需要接缝/环境、留给主控裁定的面（非缺陷，登记以免下波重复踩）
1. `WtDtRunner::start` 全段（`WtDtRunner.cpp:44-86`，约 35 行）：见 BUG-R22-002。
   需要 (a) 一个「单步」入口（把 post 的任务体抽成 `runStartupOnce()`）与
   (b) `_state_mon.run()` 的可注入时钟接缝（已由 BUG-R5-002 登记，此处不重复主张）。
   `_parsers.run()`（`:46` -> `WtDtCore/ParserAdapter.cpp`）在 start 首行，同样只在起线程路径可达。
2. `initialize` 的 `shmcaster`（`WtDtRunner.cpp:175-179`）与 `broadcaster`（`:181-185`）两支：
   `ShmCaster::init` 建共享内存、`UDPCaster::init` 的 `active=true` 支会走到 bind 面
   （见 BUG-R5-001/R5-004 的 socket 结论），本轮硬约束禁止，未写用例。
3. `initParsers` 的**加载成功**支（`:281-283` 之后 `ParserAdapter::init` 返回 true）：
   必须先 dlopen 一个真的 parser 插件 dylib（`libParserUDP.dylib` 之类）才能进，
   属于「不加载真实插件」的约束外；若要覆盖，需要给 `DLLHelper::load_library` 提供可注入替身。
4. `install_signal_hooks`（`WtDtRunner.cpp:50-58`）与 `_to_exit` 的三处读点：需要信号接缝
   （`raise(SIGINT)` 会打到整个测试进程），已并入 BUG-R22-002 的 DISABLED_ 说明。
5. `createExtDumper` 的第二次同 id 调用（`WtDtRunner.cpp:363-366` 的 `_dumpers[id]` 覆盖 +
   `DataManager::add_ext_dumper` 重复登记）是否会造成 writer 侧悬挂指针：`_dumpers` 从不 erase，
   当前不可达；若产品侧将来加 erase，需要接缝才能观测，本轮只在
   `r22_porter_capi_test.cpp::CreateExtDumperIsIdempotentAndReturnsTrue` 里钉住「两次都返回 true」。
