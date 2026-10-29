# R22 缺陷清单（迭代01 · lane R22 · src/WtDtPorter 281 行未覆盖）

- 范围: 本 lane 只记录，未修改任何生产代码；`src/**` 除新建的
  `src/TestUnits/coverage/r22_dtporter/`（3 个用例文件）外一字未动，
  `src/TestUnits/coverage/CMakeLists.txt` 未改，未跑构建/二进制/ctest，无 git 操作。
- 关联用例: `r22_ext_porter_test.cpp`、`r22_runner_init_test.cpp`、`r22_porter_capi_test.cpp`
- 关联前账（不重复主张，只交叉引用）: `bugs/R5_wtdtcore.md`（StateMonitor/UDPCaster 的时钟与 socket 接缝）、
  `bugs/R6_porter_runner.md` BUG-R6-002/003/007（缺段无守卫、createExtParser 解引用、Exp* 硬绑单例同族）、
  `bugs/l05_dt_servo_porter.md` L05-005（write_log 的 NULL 类别）。

## BUG-R22-001 ExpParser/ExpDumper 硬绑 getRunner() 单例（多 runner 支持未确认）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 条件性设计限制；当前 C API 契约为单例，未确认是产品缺陷
- 位置: `src/WtDtPorter/ExpParser.cpp:21`（`extern WtDtRunner& getRunner();`）与
  `:29-34,40-44,51-56,63-68,75-80,87-92,99-107`；`src/WtDtPorter/ExpDumper.cpp:20` 与
  `:31-35,46-50,61-65,76-80,91-95`；单例定义 `src/WtDtPorter/WtDtPorter.cpp:48-52`；
  被错位影响的创建点 `src/WtDtPorter/WtDtRunner.cpp:349-357`（createExtParser）、`:361-370`（createExtDumper）
- 现象（仅针对人工构造的第二 runner）: `ParserAdapter::initExt`（`src/WtDtCore/ParserAdapter.cpp:46-81`）在**调用方那个 runner** 上把
  ExpParser 注册为回调桥，但桥的每一个方法都不携带 owner 引用，而是重新去取函数级 static 单例：
  `ExpParser::init` -> `getRunner().parser_init(...)`。于是 `WtDtRunner runner2; runner2.createExtParser("x")`
  产生的 EVENT_PARSER_INIT/CONNECT/SUBSCRIBE 落在**单例**的 `_cb_parser_evt` 槽上，
  `runner2` 自己注册的槽一次都不响。Dumper 侧同形：ExpDumper 只带 `_id`，导出永远走单例的
  `_dumper_for_*`。同一份配置在两个 runner 上跑，行情事件与宿主回调被混到一个通道里。
- 复核: 两个测试直接构造第二个 `WtDtRunner`，源码路径确实把桥对象回调转给 `getRunner()` 的函数级单例；这是人工多实例场景的源码/测试观察。C 导出也通过 `getRunner()` 提供单例，公开头文件未承诺多个 runner 的隔离语义，因此不能据此断言标准 C API 用法错误，也不能把覆盖率可测性表述为产品影响。只有确认本地 C++ 多 runner 是受支持用法时才应修复：把 owner 显式传给桥对象，并保留两个 runner 回调互不串线的测试。否则应把单例约束写入契约。与 BUG-R6-007 为同族，评审时交叉引用、避免重复计数。


- 迭代02复核: NOT_BUG。公开 C API 通过函数静态 getRunner() 暴露单个 WtDtRunner，parser/dumper 回调也绑定它。仅从可构造第二个内部 C++ 对象推导多实例支持，没有公开合同，不构成已证产品缺陷。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R22-002 C 入口 start(bAsync) 不透传 allday，全天候配置下仍然起状态机线程
- 状态: 待审核
- 严重级别: Major（源码确认启动路径不一致，特定配置组合未动态复现）
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
- 证据边界与修复方向: C 入口 `start(bool)` 不透传 `allday`，runner 的第二个参数默认 false；按源码调用顺序，C API 的 allday=true 配置仍会进入状态监视器分支。这是源码确认的产品行为差异，但本 lane 的 `DISABLED_capi_start_always_spawns_state_monitor_thread` 并未先 `initialize` 一份 allday 配置，也未运行，不能作为该组合场景的动态复现。测试应执行 initialize(allday=true)→C start(true)，再验证监视器未启动且没有启动延迟。修复方向是保存初始化所得 allday 状态供 start 使用，或明确修改公开契约；若状态监视器本来应始终启动，则应先修正配置注释。
- 复现记录（未运行）: `r22_runner_init_test.cpp::DISABLED_start_sync_branch_never_returns`
  （触发条件：任何 `start(false)` —— `:50` install_signal_hooks 往宿主装 SIGINT/SIGTERM，
  `:68-74` 的 `run_one()` 在唯一 post 任务跑完后永久阻塞，`:76` join 只有 `_to_exit` 被信号回调
  置位才返回 -> 整个测试进程挂死）与 `::DISABLED_start_async_branch_spawns_state_monitor_thread`
  （触发条件：`start(true)` —— sleep + StateMonitor 起线程）；
  `r22_porter_capi_test.cpp::DISABLED_capi_start_always_spawns_state_monitor_thread`
  （触发条件：C 入口 `start(true)`，专门钉住 allday 形参缺失）。

## BUG-R22-003 initialize 对缺失配置项未判空（有条件的非法配置风险）
- 状态: 待审核
- 严重级别: Major（源码确认的缺字段配置空指针风险；未动态复现）
- 位置: `src/WtDtPorter/WtDtRunner.cpp:106-107`（`config->get("basefiles")` 未判空即 `cfgBF->get("session")`）、
  `:199` + `:257-260` -> `src/WtDtCore/DataManager.cpp:67-72`（`config->get("writer")` 为 NULL 时
  `params->getCString("module")` 解引用）、`:233` -> `:262-264`（parsers 指向的 json 文件里没有
  `parsers` 键时 `initParsers(NULL)`，`cfg->size()` 解引用）
- 源码现象: `WTSVariant::get()` 对缺失键返回 NULL（`src/Includes/WTSVariant.hpp:364-375`），
  三处调用点都没有守卫。若运行配置允许缺少 `basefiles`、`writer`，或 parser 文件没有 `parsers` 键，源码存在空指针解引用路径；是否将其视作非法配置及初始化如何失败，应由公开配置契约明确。当前没有实际 SIGSEGV 运行证据。
  同文件的 `:99-103` 却对 `config == NULL` 做了 error+return —— 说明守卫是漏写的，不是设计取舍。
- 影响范围: 对允许传入的缺字段配置，初始化可能因未判空而异常终止；
  与 `bugs/R6_porter_runner.md` BUG-R6-001/002（WtRtRunner 同形）并条修复最省。
- 修复方向: 在入口验证 required sections/keys，记录缺失字段并返回失败；每个缺项用启用测试断言 `initialize` 失败且有可诊断错误，不依赖崩溃。以下测试均为 DISABLED、未运行，不能写成已复现崩溃。
- 复现用例（未运行）: `r22_runner_init_test.cpp` 三条
  `DISABLED_basefiles_section_missing_derefs_null_section` /
  `DISABLED_writer_section_missing_derefs_null_params` /
  `DISABLED_parsers_file_without_parsers_key_derefs_null`，触发条件逐条写注释。

## BUG-R22-004 initialize 写入进程级全局状态（失败路径影响未证实）
- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 进程级副作用由源码确认；失败后跨 runner 污染未证实
- 位置: `src/WtDtPorter/WtDtRunner.cpp:90-91`（`WTSLogger::init` + `WtHelper::set_module_dir(modDir)`）
  早于 `:93-103` 的 `config == NULL` 早退
- 复核: `WtHelper::set_module_dir(modDir)` 在配置校验前写进程级 static，源码副作用成立；但 C 入口按设计传 `getBinDir()`，现有 enabled 测试只证明成功进入 C initialize 后模块目录被设为 bin dir，使用不存在的 log 文件并未测试配置解析失败。没有证据表明这个目录值错误，也没有失败重试导致插件错路径的动态证据。多 runner 支持契约也未确认。当前应视为全局状态/初始化顺序设计限制，不应宣称“失败后永久污染导致插件加载失败”。若需支持隔离多实例或失败回滚，应补“失败配置→检查全局状态→第二次有效初始化并验证插件路径”的测试，再决定改为 runner 局部或失败时恢复。

## BUG-R22-005 write_log 的 message 原文/格式串语义需核对（崩溃主张证据不足）
- 状态: 待审核
- 严重级别: 未确认产品缺陷（保留格式串安全审查点）
- 位置: `src/WtDtPorter/WtDtPorter.cpp:83-93`（两个分支都不校验 message）->
  `src/WTSTools/WTSLogger.cpp:330`（`print_message(message)`，其实现是
  `WTSLogger.cpp:129-135` 的 `fmt::print(buffer)`）与 `:368`（`fmt::print(message)`）
- 复核更正: 当前 `WTSLogger` 默认阈值为 `LL_NONE`，`log_raw`/`log_raw_by_cat` 会先按阈值返回；因此“日志未 init”本身不能推出含花括号消息会进入 `fmt::print`，原报告触发链不成立。特定 logger 状态下的格式串调用仍应与 `WtDtPorter.h` 的 message 原文语义核对，但本次没有运行测试证明异常穿过 C ABI，也没有证明默认产品路径可达。若接口承诺原文，应使用 literal-safe 输出；定向测试需建立会进入该分支的 logger 状态并用大括号消息断言原文、安全返回，可用子进程检查终止边界。现有 DISABLED 用例未运行，不能称为进程崩溃证据。
- 原始主张（本复核不采纳其“未初始化必进入格式化”的前提）: 日志未 init（本轮的常态：logCfg 指向不存在的文件，`WTSLogger::init` 早退）时，
  `log_raw`/`log_raw_by_cat` 都走「直接把 message 当格式串 print」的支路。
  parser/trader 插件经 `write_log` 传一条含 `{}` 或 `{0}` 的原文（JSON 片段、错误串都很常见），
  fmt 抛 `fmt::format_error`，异常穿越 `extern "C"` 导出边界 -> `std::terminate`，宿主进程直接没了。
- 影响范围: 任何向 porter 转发第三方报文的插件都能一发打停 datakit；
  `write_log` 返回 void，调用方没有任何降级机会。
- 复现: `r22_porter_capi_test.cpp::DISABLED_write_log_with_braces_escapes_format_string`
  （触发条件：level >= 日志阈值且日志未 init 时 `write_log(LL_FATAL, "…{not a format}…", "parser")）。

## BUG-R22-006 write_log 的 level 使用 WTSLogLevel 原始值（0..5 输入不在当前契约内）
- 状态: 已撤回（0..5 不属于已文档化枚举值；保留非法值说明改进建议）
- 严重级别: 不成立为当前契约缺陷（可改进参数校验/说明）
- 位置: `src/WtDtPorter/WtDtPorter.cpp:85-92`（`(WTSLogLevel)level` 直转，无范围检查）->
  守卫在 `src/WTSTools/WTSLogger.cpp:325`、`:358`（`if (m_logLevel > ll || m_bStopped) return;`）
- 复核更正: `WtDtPorter.h` 将此参数描述为 `WTSLogLevel`，其枚举值从 `LL_ALL=100` 开始；C ABI 的 `unsigned int` 是承载类型。传入 0..5 不属于已文档化枚举值，不能以其他语言常见枚举编码推断它是合法输入。`WriteLogBelowLoggerThresholdIsDropped` 仅验证输入缓冲不变/调用不抛异常，未观察日志输出，不能证明“静默丢弃”。建议文档列出枚举原值，并可对非法值拒绝或告警；只有契约明确接受 0..5 后，才补映射测试。
- 原始主张（本复核不采纳）: `WTSLogLevel` 从 `LL_ALL=100` 起排（`src/Includes/WTSTypes.h:128-137`），
  而 C 入口收的是裸 `unsigned int`。宿主侧（Python/Rust/其它语言的枚举惯例是 0..5）
  传进来的 level 全部 < 100，于是被 `m_logLevel > ll` 一句吞掉：`write_log` 返回 void、
  不告警、不落盘，日志整体消失。
- 影响范围: 跨语言绑定的默认写法会「接口通、日志空」；排障成本与 BUG-R5-005 一类静默丢弃同级。
- 复现: `r22_porter_capi_test.cpp::WriteLogBelowLoggerThresholdIsDropped`。
  说明：「日志丢失」本身在本轮口径下不可观测（不能读 stdout/挂钟），
  该用例只钉住「入参缓冲不被改写、调用不抛异常」这一可断言面，级别语义丢失靠源码陈述支撑。

## BUG-R22-007 createExtParser 忽略重复有效 ID 的登记失败
- 状态: 本地重复 ID 返回值测试 FAIL；资源清理风险尚未动态测量
- 严重级别: Minor（含一处可达的内存泄漏）
- 位置: `src/WtDtPorter/WtDtRunner.cpp:349-357`、`src/WtDtCore/ParserAdapter.cpp:337-351`
  （`addAdapter` 的 `Same name of parsers` 分支返回 false，`:349` 正常入表）、
  `src/WtDtCore/ParserAdapter.cpp:42-44`（`~ParserAdapter()` 空实现）
- 复核: 在 `initExt` 成功后撞名且 `addAdapter` 返回 false 的场景，源码忽略该返回值并仍返回 true；局部适配器析构也不调用 `release`，因此有泄漏风险。此前“配置项初始化失败时未占用 ID”的判断有误：`initParsers` 不检查 `adapter->init()` 结果，仍将活跃 ID 加入 manager。原两条测试将重复注册返回 true 固化为通过；现增加首次创建唯一 ID 的正控并要求重复创建返回 false，修正后待运行。测试没有验证临时 API 的释放或真实插件行为。修复方向是在副作用前检查 ID，或在 addAdapter 失败时释放适配器并传回 false。
- 源码路径: `createExtParser` 先 `new ExpParser(id)`、`adapter->initExt(...)`（内部已经 registerSpi + init +
  对全量合约发起 subscribe），再 `_parsers.addAdapter(id, adapter)`，**且不检查返回值**，
  最后无条件 `return true`。id 撞名时 addAdapter 返回 false，局部 shared_ptr 析构，
  `~ParserAdapter` 是空的、`release()` 也无人调用，于是 `initExt` 里 new 出来的 ExpParser 直接泄漏；
  宿主每调一次 `create_ext_parser("<已存在的 id>")` 就漏一个对象，还会白白多发一轮全量订阅。
- 影响范围: C 入口 `create_ext_parser`（`WtDtPorter.cpp:97-100`）返回值恒真，
  宿主无法区分「新建成功」与「id 已被占用」；重复注册 = 无界泄漏 + 重复订阅扇出。
- 复现: `r22_runner_init_test.cpp::ParsersAsInlineArrayRegistersActiveEntriesOnly` 与 `::ParsersAsStringPathToExistingFileIsExpanded` 已按唯一 ID 和重复 ID 的成功/失败合同修正，两条隔离运行均 FAIL：首次唯一 ID 的正控成功，重复创建及配置中已登记的 ID 均仍返回 true。本地缺模块配置路径有 dlopen 失败诊断；未启动真实插件或网络。`r22_ext_porter_test.cpp::PushQuoteAfterInitResolvesContractAndWrites` 是另一个转发场景，不代证重复注册。

## BUG-R22-008 write_log 的 NULL catName（L05-005 同源，当前契约未承诺 NULL）
- 状态: 已合并（主记录 BUG-L05-005；同一入口与触发条件，NULL 输入合同仍待确认）
- 严重级别: 契约外输入风险；与 L05-005 重复，不单独计账
- 位置: `src/WtDtPorter/WtDtPorter.cpp:85`（`if (strlen(catName) > 0)`）
- 复核更正: 源码对 NULL 调用 strlen 有空指针风险，但公开头文件只承诺空字符串表示默认类别，没有承诺 NULL 可用。相关 DISABLED 测试未运行；将 NULL 当作合法输入并据此登记崩溃属于契约外推。与 L05-005 是同一位置、同一条件，应合并避免重复计数。若产品希望 C API 容忍 NULL，则需明确契约并增加入口判空；否则文档注明必须传非 NULL 字符串。
- 原始主张（本复核不采纳其 NULL 合法性假设）: 与 `bugs/l05_dt_servo_porter.md` 的 L05-005 同一处、同一问题。R22 侧只补充：
  `WtDtPorter.h:52` 的接口文档写明「空字符串表示使用默认类别」，但没有说明 NULL 不可传，
  而 `EXPORT_FLAG` 的 C 消费方（尤其自动生成绑定）传 NULL 的概率不低。
- 影响范围: 同上（段错误）。
- 复现: `r22_porter_capi_test.cpp::DISABLED_write_log_with_null_category_hits_strlen_null`
  （仅作复现位，合并建议：与 L05-005 并条，不重复计账）。

## BUG-R22-009 Windows-only 分支与覆盖率分母的关系待 artifact 证实
- 状态: 已撤回（属报表口径问题，非产品缺陷；平台 artifact 仍待验证）
- 严重级别: Minor
- 位置: `src/WtDtPorter/WtDtPorter.cpp:19-27`（`PLATFORM_NAME` 的 _WIN32 支）、
  `:29-46`（`#ifdef _MSC_VER` 的 `getModuleName()`，含 api-brief 点名的 `:40` `fs::path p(MODULE_NAME)`）、
  `:56-58`（同一 `#ifdef _MSC_VER` 内的 `CMiniDumper::Enable(...)`，含 `Common/mdump.h` 引入）
- 复核更正: `_MSC_VER` 分支在非 MSVC 构建中预处理掉这一源码事实成立；但仅凭 api-brief 的 missed 列表，不能确认这些行进入实际 gcov 分母，也不能确认 281 行统计使用的分母算法。归类为报表口径待核，不是产品缺陷。应检查 `.gcno/.gcov` 或原始 coverage artifact，按真实编译单元/预处理行集核对分母；核对前不要宣称分母被系统性抬高。
- 原始主张（分母影响待原始 coverage artifact 证实）: `api-brief-R22.md` 把 `WtDtPorter.cpp:40` 与 `:57` 列为 missed，但这两行连同
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
