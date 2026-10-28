# R22 manifest — src/WtDtPorter（迭代01 · lane R22）

- 范围: `src/WtDtPorter/` 未覆盖 281 行（`api-brief-R22.md`：`WtDtRunner.cpp` 266/345＝22.90%、
  `WtDtPorter.cpp` 9/51、`ExpParser.h`/`ExpDumper.h`/`ExpParser.cpp` 若干）。
- 目标二进制: **主目标 `CoverageTests`**。新建目录
  `src/TestUnits/coverage/r22_dtporter/`，由
  `src/TestUnits/coverage/CMakeLists.txt:15` 的 `file(GLOB_RECURSE COV_SRCS ...)` 自动收；
  该文件 `:340-352` 已把 `src/WtDtPorter/*.cpp` 与 `src/WtDataStorage/*.cpp` 一起编进主目标，
  并在 `:356` 链 `WtDtCore WtCore` —— **本轮未改任何 CMakeLists**（主控需重新 configure 才生效）。
- 符号缝: 只引 WtDtPorter 这一族。`WtPorter`/`WtBtPorter`/`WtDtServo` 的同名 `extern "C"`
  入口（`initialize`/`start`/`get_version`/`write_log`）与同名 `wtp::ParserAdapter` 一概不碰。
  用例内不声明 `getRunner()`（该符号由 `WtDtPorter.cpp:48-52` 定义、
  由 ExpParser/ExpDumper 自己引用），一律经 C 入口或 `WtDtRunner` 的公开成员进出，避免跨族择一。
- 门禁（真实构建口径，未自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests.dir/l05_dt_servo_porter/l05_dt_porter_capi_test.cpp.o | tail -1`
  → 截掉 ` -MD ... -o ... -c ...` 尾巴 → 追加 `-fsyntax-only <我的cpp>`。
  **三个文件全部 exit 0、0 诊断**（含 `-Wall -Wextra -Werror`）。未跑构建、未跑二进制、未跑 ctest、无 git。
- 硬约束遵守情况: 不起线程、不 sleep、不建 socket/shm、不 dlopen 真插件、不读挂钟、
  不 `#define private public`、不改产品代码、不改他人用例、不放宽断言。
  起线程/挂死/段错误/越界四类路径整条 `DISABLED_`，触发条件写在各用例注释与 `bugs/R22_dtporter.md`。

## 观测口径（为什么不是「没崩就算过」）
`WtDtRunner` 的成员全是 private 且无 getter，`initialize` 返回 void。本轮找到的唯一
「可计数的外部证据」是产品自己的这条链：
`createExtParser`（`WtDtRunner.cpp:349-357`）-> `ParserAdapter::initExt`
（`WtDtCore/ParserAdapter.cpp:46-81`，其中 `:60-72` 取 `_bd_mgr->getContracts()` 全集）
-> `ExpParser::subscribe`（`ExpParser.cpp:75-80`）-> `parser_subscribe` -> 宿主回调。
于是「basefiles 到底装了哪几条合约」变成订阅回调的**集合**，期望值由每个用例
自己用一个独立 `WTSBaseDataMgr` 装载同一批夹具文件算出（`r22_runner_init_test.cpp:170-192`），
不抄产品实现、不写死字符串（full code 的拼法由 `WTSContractInfo::getFullCode()` 自己给出）。

## 一、用例 → 生产文件:行段（行号一律本轮 Read/Grep 实测）

### `src/TestUnits/coverage/r22_dtporter/r22_ext_porter_test.cpp`（19 启用 + 0 DISABLED）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `FiveDumpersReturnFalseBeforeAnyRegistration` | `WtDtRunner.cpp:385-394,396-405,407-416,418-427,429-438`（空槽支 `:387,398,409,420,431`） | 五个 `_dumper_for_*` 未注册即返回 false，回调 0 命中 |
| `RegisterExtDumperEnablesBarsAndTicksOnly` | `WtDtRunner.cpp:372-376` + `:396-405,385-394`；高频三支仍空 | bar/tick 两槽与 hft 三槽互不相通（注册函数不同族） |
| `RegisterHftDumperEnablesThreeHftSlotsOnly` | `WtDtRunner.cpp:378-383` + `:407-427,429-438` | 高频三槽各自 count/date 原样透出，bar 槽仍 false |
| `DumperForwardsPointerIdentityAndCallbackReturnValue` | `WtDtRunner.cpp:393,404,415,426,437` | 指针身份透出；回调返回 false 时 runner 也返回 false；`count=0`、空 stdCode 不做任何过滤 |
| `ReregisteringDumpersWithNullsClearsEverySlot` | `WtDtRunner.cpp:374-375,380-382`（覆盖式注册） | 传 NULL 撤销槽位，撤销后回到 `:387` 假支 |
| `SixParserEntriesAreSilentBeforeRegistration` | `WtDtRunner.cpp:298-302,304-308,310-314,316-320,322-326,328-332` 的 `if` 假支 | `_cb_parser_evt/_cb_parser_sub` 为空时六个入口全静默 |
| `FourParserEventsMapToPorterDefsConstants` | `WtDtRunner.cpp:290-296,298-320` + `PorterDefs.h:67-85` | 四个方法 -> `EVENT_PARSER_INIT/CONNECT/DISCONNECT/RELEASE`＝1/2/3/4 一一对应；注册语义是覆盖 |
| `SubscribeAndUnsubscribeCarryOneCallbackPerCode` | `WtDtRunner.cpp:322-326,328-332` | `isForSub` 分别 true/false，id 与 code 原样透传 |
| `SlotsOfTwoRunnersDoNotLeakIntoEachOther` | `WtDtRunner.h:92-93`（槽位是成员而非全局） | 两个 runner 的回调槽互不串扰（BUG-R22-001 的控制组） |
| `ExpParserLifecycleForwardsFourEventsAndReturnsTrue` | `ExpParser.cpp:29-34,40-44,51-56,63-68` + `ExpParser.h:30,36,73` | init/connect/disconnect/release 四个事件按序落到**单例**；三个方法恒返回 true（含 `isConnected`） |
| `ExpParserSubscribeWalksEveryCodeInSet` | `ExpParser.cpp:75-80,87-92` | 3 个代码 × 订阅/退订＝6 次命中；空集合 0 次；hashset 无序故按成员校验 |
| `ExpParserRegisterSpiPullsBaseDataMgrOncePerValidSink` | `ExpParser.cpp:99-107`（`:105` 的 `if (m_sink)` 两支） | NULL sink 不取 BDMgr；非空 sink 每次 registerSpi 取一次 |
| `CreateExtParserFiresInitThroughExpParserOnSingleton` | `WtDtRunner.cpp:349-357` -> `WtDtCore/ParserAdapter.cpp:46-58` | 建适配器即触发一次 EVENT_PARSER_INIT；未 initialize 的 runner 订阅扇出为 0（`getContracts()` 返回空集而非 NULL，`:61-62`） |
| `CreateExtDumperRegistersByIdOnSingleton` | `WtDtPorter.cpp:116-119` + `WtDtRunner.cpp:361-370` -> `WtDataStorage`/`WtDtCore/DataManager.h:71` | C 入口建 dumper 成功；同 id 的 ExpDumper 实例在全局槽位空/满两种状态下各 5 个方法的返回值与实参 |
| `ExtDumperIdIsIndependentOfCreateExtDumperId` | `ExpDumper.cpp:76-80` + `WtDtRunner.cpp:396-405` | 槽位是全局的，与 create_ext_dumper 的登记无关（导出面与登记面无校验） |
| `PushQuoteForUnknownParserLeavesTickUntouched` | `WtDtRunner.cpp:334-346`（`:345` warn 支） | 未知 id 不消费 tick，调用方缓冲逐字节不变 |
| `PushQuoteForKnownExtParserStopsAtZeroDates` | `WtDtRunner.cpp:336-342` -> `WtDtCore/ParserAdapter.cpp:47-52`（handleQuote 的零日期守卫） | action/trading date 为 0 时原样返回，不回写调用方缓冲 |
| `PushQuoteAfterInitResolvesContractAndWrites` | `WtDtRunner.cpp:334-342`（`WTSTickData::create` + `handleQuote`） | 已注册适配器 + 未知合约：走到 `contract == NULL` 前返回，字段不被改写 |
| `CApiCreateExtParserAndPushQuoteReachTheRegisteredSlots` | `WtDtPorter.cpp:97-100,102-105,107-110` | 三个 C 入口把调用透传到单例槽位；未知 id 的 push 不再产生事件 |

### `src/TestUnits/coverage/r22_dtporter/r22_runner_init_test.cpp`（11 启用 + 5 DISABLED）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `AlldayModeWithScalarBasefilesLoadsContractsAndSkipsParsers` | `WtDtRunner.cpp:88-103,105-173,175-199,221-252`（`:189-197` allday 真支、`:252` 无 parsers 告警支） | 装载出的合约全集＝独立 ref 的集合；只 1 个 INIT 事件；订阅无重复 |
| `NonAlldayModeInitializesStateMonitorAndStillLoadsBasefiles` | `:190-193`（`_state_mon.initialize`）+ `:199` -> `:257-260`（`&_state_mon`） | allday=false 走状态机 initialize（夹具 state.json 的两个 session 都在 sessions.json 里）；合约集不受影响 |
| `HolidayHotSecondAndCustomRulesSectionsAreLoadedWithoutBreakingBasefiles` | `:145-149,152-156,158-162,164-173`（rules 多 tag 循环） | 四个可选段（含一个指向不存在文件的 second）全走一遍，basefiles 结果不变 |
| `BasefilesCommodityAndContractAsArraysWalkBothBranches` | `:113-127,129-143` 的 `VT_Array` 支（`:120-126,136-142`） | 数组三项（末项文件不存在）循环不中断，合约全集与 ref 一致＝3 |
| `ConfigFileLoadedFromDiskPathWhenCfgFileFlagIsTrue` | `:93-95`（`bCfgFile=true` -> `load_from_file`） | 文件形态与内联形态装载结果同一期望值 |
| `UnparseableContentReturnsBeforeLoadingBasefiles` | `:96-103` | 解析失败 -> error + 早退；一条合约都不装（对照组证明夹具能装出 2 条） |
| `MissingConfigFileReturnsBeforeLoadingBasefiles` | `:94-95,99-103` | 文件不存在时 `load_from_file` 返回 NULL，走同一早退 |
| `IndexSectionWithAbsentFileIsReportedAndDoesNotBreakBasefiles` | `:201-219`（`:203-204` loadFactories、`:209-218` 的 `var == NULL` 支） | `{cwd}index/` 不存在 + index 文件不存在 -> 两段 error，basefiles 不受影响 |
| `ParsersAsInlineArrayRegistersActiveEntriesOnly` | `:221-250`（`VT_Array` -> `initParsers`）、`:262-287`（`:267-268` inactive continue、`:274-279` 自动 id、`:281-283`） | 三个条目（1 inactive + 1 无 id + 1 具名）都过；`createExtParser` 对已占用/未占用 id 一律 true（BUG-R22-007） |
| `ParsersAsStringPathToExistingFileIsExpanded` | `:224-235`（`:227` exists 真支、`:231-235` var 非空支） | parsers 文件被展开；其中 `r22_from_file` 已占名 -> 重复 create 仍 true |
| `ParsersAsStringPathToMissingFileIsSkipped` | `:226,241-244`（`Parser configuration {} not exists` 支） | parsers 文件缺失只打 error；后续 push 命中「Parser not exists」告警支 |
| `DISABLED_basefiles_section_missing_derefs_null_section` | `:106-107` | BUG-R22-003（触发：合法 json 无 basefiles 段 -> NULL 解引用） |
| `DISABLED_writer_section_missing_derefs_null_params` | `:199,257-260` -> `WtDtCore/DataManager.cpp:67-72` | BUG-R22-003（触发：无 writer 段 -> `params->getCString` 解引用 NULL） |
| `DISABLED_parsers_file_without_parsers_key_derefs_null` | `:233` -> `:262-264` | BUG-R22-003（触发：parsers 文件存在但无 `parsers` 键 -> `cfg->size()` 解引用 NULL） |
| `DISABLED_start_sync_branch_never_returns` | `:44-46,48-77`（`:50` install_signal_hooks、`:60-66` post+sleep、`:68-74` StdThread、`:76` join） | BUG-R22-002（触发：任何 `start(false)` -> 进程挂死） |
| `DISABLED_start_async_branch_spawns_state_monitor_thread` | `:78-85`（`:82` sleep 5ms、`:83` `_state_mon.run()`） | BUG-R22-002（触发：`start(true)` -> 起状态机线程） |

### `src/TestUnits/coverage/r22_dtporter/r22_porter_capi_test.cpp`（8 启用 + 3 DISABLED）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `VersionStringIsPlatformPrefixedAndBuiltOnce` | `WtDtPorter.cpp:25-27`（`PLATFORM_NAME="UNIX"`）、`:67-81` | 前缀 `UNIX `、含 ` Build@`、两次调用返回同一指针（static 缓存的第二支） |
| `WriteLogKeepsCallerBuffersUnchanged` | `WtDtPorter.cpp:83-93`（`:85` 真支 -> `WTSLogger.cpp:356`，假支 -> `:323`） | 带/不带类别两条分支都不改写调用方缓冲、不抛异常 |
| `WriteLogBelowLoggerThresholdIsDropped` | `WtDtPorter.cpp:85-92` + `WTSLogger.cpp:325,358`（`m_logLevel > ll` 守卫） | level=1/2 落在 `LL_ALL=100` 之下 -> 静默空转（BUG-R22-006） |
| `RegisterExtendedDumperStoresBothBarAndTickSlots` | `WtDtPorter.cpp:121-124` -> `WtDtRunner.cpp:372-376` + `ExpDumper.cpp:76-80,91-95` | bar/tick 两个槽各自返回值；置 NULL 后双双回落 false |
| `ExpParserEventsReachGlobalRunnerNotTheOwningRunner` | `ExpParser.cpp:21,29-34` + `WtDtPorter.cpp:48-52` + `WtDtRunner.cpp:349-357` | 正交对照：`owner.parser_init` 只命中 owner 槽；`owner.createExtParser` 的 INIT 只命中全局槽（BUG-R22-001） |
| `ExpDumperAlwaysForwardsToGlobalRunnerNotOwningRunner` | `ExpDumper.cpp:20,76-80` + `WtDtRunner.cpp:396-405` | owner 自己的 dumpHisBars 命中 owner 槽；ExpDumper 实例走全局槽返回 false（BUG-R22-001） |
| `CapiInitializeWithAbsentLogFileKeepsLoggerUninited` | `WtDtPorter.cpp:54-60`（`getBinDir()` 作 modDir）-> `WtDtRunner.cpp:88-255` | C 入口 initialize 的装载结果与独立 ref 集合相等；`:97-100` 的 `create_ext_parser` 扇出即证据；顺带钉住 `WtHelper::get_module_dir()` 已被改写（BUG-R22-004） |
| `CreateExtDumperIsIdempotentAndReturnsTrue` | `WtDtRunner.cpp:361-370`（`_dumpers[id]` 覆盖）+ `WtDtCore/DataManager.h:71` | 同 id 两次都 true；导出面仍按 id 走全局槽（悬挂指针面不可观测，见留白 5） |
| `DISABLED_write_log_with_braces_escapes_format_string` | `WtDtPorter.cpp:87,91` -> `WTSLogger.cpp:129-135,330,368`（`fmt::print(message)`） | BUG-R22-005（触发：日志未 init 且 level 达阈值，message 含 `{}`） |
| `DISABLED_write_log_with_null_category_hits_strlen_null` | `WtDtPorter.cpp:85` | BUG-R22-008（复述 L05-005；触发：catName==NULL） |
| `DISABLED_capi_start_always_spawns_state_monitor_thread` | `WtDtPorter.cpp:62-65` -> `WtDtRunner.cpp:80-85` | BUG-R22-002（触发：C 入口 `start(true)`，allday 形参缺失 -> 必起状态机线程） |

## 二、本轮打到 / 打不到的面（诚实口径）
- 应可转绿：`WtDtRunner.cpp` 的 `:290-438` 全段（扩展 Parser/Dumper 族）、`:88-287` 的
  initialize/initDataMgr/initParsers 主体（除 shmcaster/broadcaster/成功 dlopen 三支）；
  `ExpParser.cpp`/`ExpDumper.cpp` 两个文件的全部方法；`ExpParser.h:30,36,73`、`ExpDumper.h:31,37`；
  `WtDtPorter.cpp:48-52,54-60,67-81,83-93,97-100,102-105,107-110,116-119,121-124,126-129`。
- 留白（估计行数，按上面的可达性结论给）：
  `WtDtRunner.cpp:44-86` 约 35 行（线程/sleep/信号，BUG-R22-002）＋
  `:175-179,181-185` 约 8 行（shm/udp，前账 R5）＋
  `:224-240` 里「parsers 插件真加载」的 3~4 行（不 dlopen 真插件）＋
  `WtDtPorter.cpp` 的 Windows-only 12 行（BUG-R22-009，预处理即剔除）＝
  **约 55~60 行不可覆盖**，其余 220 行左右本轮有对应用例。
- 用例计数：3 个文件、**38 条启用 + 8 条 DISABLED**。启用条数超出简报给的 15~25 区间，
  原因是 `initialize` 的每个配置分支（scalar/array/文件/内联、4 个可选段、parsers 三种形态）
  都必须各配一份夹具才能用「自己喂的输入」算期望值（硬规则 6），合并成一条就会退化成弱断言。
