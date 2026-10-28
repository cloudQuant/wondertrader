# R8 缺陷清单（迭代01 第七波 · WtDtServo/WtDtRunner.cpp + WtDataManager.cpp + ParserAdapter.cpp）

- 范围: 本 lane 仅记录，未修改任何生产代码；`src/**`（除 `src/TestUnits/coverage/q05b_dtservo/r8_*.cpp`）一字未动。
- 关联用例: `r8_parseradapter_test.cpp`（13 条全启用）、`r8_wtdtrunner_test.cpp`（13 启用 + 3 DISABLED）、`r8_wtdatamgr_sinks_test.cpp`（6 启用 + 2 DISABLED）
- 关联既有记录: BUG-Q05-001（`q05_servo_wtdatamgr_test.cpp::DISABLED_InitStoreLoadFailureDerefsNullReader`）已登记 initStore 空 reader 解引用；本文件 BUG-R8-002 只做形态补全（默认模块名），不重复主张新发现。
- 同名类隔离: 本文件全部行号指向 `src/WtDtServo/`，与 `src/WtDtCore/ParserAdapter.h:28-30`（3 参构造）不是同一个类，未引用 WtDtCore 的任何符号。

## BUG-R8-001 initialize 未判 basefiles 段，缺段即空指针解引用
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDtRunner.cpp:73-74`
- 现象: `WTSVariant* cfgBF = config->get("basefiles");` 之后直接 `if (cfgBF->get("session"))`，中间没有判空。`WTSVariant::get` 未命中返回 NULL 是本文件的既有语义（同函数 `:152-153` 对 parsers、`WtDataManager.cpp:104` 对 store 都是靠这个返回值判空的），所以任何一份漏写 `basefiles` 的 wtdtserver 配置（或把段名写成 `basefile`/`bases`）都会在 `initialize()` 的第一条成员访问上 SIGSEGV，且崩溃发生在 `WTSLogger::init` 之后，日志里连一行 error 都不会有。
- 影响范围: 数据服务启动路径的唯一入口，配置文件笔误 = 进程直接段错误退出而不是「Loading config failed」那样的可诊断早退（对照 `:59-64` 的处理口径，同函数内前后不一致）。
- 复现: `r8_wtdtrunner_test.cpp::DISABLED_InitializeWithoutBasefilesSectionSegfaults`（根配置只含 `disable_dump`，触发条件：启用该用例即崩，禁止在 CI 打开）。

## BUG-R8-002 initStore 加载失败后无条件解引用 _reader（默认模块名形态）
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtServo/WtDataManager.cpp:54-56`、`:81-88`
- 现象: `module` 为空时回落到默认名 `WtDataStorage`，随后 `DLLHelper::load_library` 失败只打一条 error（`:83`）就落到 `:87 _reader->init(cfg, this)`，此时 `_reader` 仍是构造函数里的 NULL。同理，模块加载成功但 `createRdmDtReader`/`deleteRdmDtReader` 任一符号缺失时（`:63-72` 只打日志、`:74` 的 `if` 不进），`:87` 同样解引用 NULL。返回值恒为 `true`，调用方拿不到任何失败信号。
- 影响范围: 与 BUG-Q05-001 同根因，本条补的是「`store` 段写了但没写 module」这一最常见形态（dist 里的样例配置就是把 storedir/path 写全而模块名留空）。这条路径在真实部署里等价于「插件没装 = 服务段错误」，而 `init()` 还报告成功。
- 复现: `r8_wtdatamgr_sinks_test.cpp::DISABLED_InitStoreWithDefaultModuleNameDerefsNullReader`（`{"store":{}}` + 空模块目录；触发条件：启用即崩）。

## BUG-R8-003 WtDataManager 所有 _reader 委托入口零判空，452 行在测试口径下不可达
- 状态: 待审核
- 严重级别: Blocker（覆盖率阻塞，非产品缺陷）
- 位置: `src/WtDtServo/WtDataManager.cpp:116`、`:121`、`:128`、`:135`、`:142`、`:170`、`:203`、`:215`、`:247`、`:341`、`:357`、`:398`、`:487`、`:496`、`:516`、`:543`
- 现象: 除 `clear_cache`（`:609-612` 有判空并降级为 warn）之外，全部取数/订阅入口都无条件 `_reader->xxx()`。`_reader` 只有在 `initStore` 全链路成功时才非空，而这条链路要么加载真实 `libWtDataStorage.dylib`，要么走到上面 BUG-R8-002 的崩溃。真实插件又不可用：`src/WtDataStorage/WtRdmDtReader.cpp:87` 在 `init()` 里无条件 `StdThread` 起 5 秒巡检线程（`:76-77` 的 `cfg==NULL` 早退在起线程之前，且 `initStore` 永远传非空 cfg），本 lane 硬规则禁止起线程，因此没有任何合规方式拿到一个可用的 `_reader`。
- 影响范围: 实测不可达行数 452 行（`112-117,119-122,124-129,131-136,138-143,145-156,158-189,191-197,199-335,337-482,484-488,507-558,572-604`，含按日期/区间读取、缓存首次装配与增量更新、二分定位、实时 K 线订阅与合成），占本文件 missed 434 行的几乎全部。WtDtRunner 侧连带 121 行（`210-239,258-269,276-281,292-297,308,319-348,359-364,375,607-635`）只能停在守卫层。
- 建议: 给 `WtDataManager` 一个受控注入点（`IRdmDtReader` 已是纯虚接口，加 `void set_reader_for_test(IRdmDtReader*)` 或让 `initStore` 接受外部 reader 工厂即可），或在 `initStore` 里把「插件起线程」变成 cfg 开关；否则这两块只能由主控裁定是否放宽「不起线程」。已用 `r8_wtdatamgr_sinks_test.cpp::DISABLED_TickSliceDelegationWithoutReaderSegfaults` 与 `r8_wtdtrunner_test.cpp::DISABLED_GetBarsByRangeWithoutReaderSegfaults`、`DISABLED_SubBarWithoutReaderSegfaults` 占位。

## BUG-R8-004 WtDtRunner 构造函数漏初始化两个回调指针
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtServo/WtDtRunner.cpp:28-32`（初始化列表只有 `_data_store`、`_is_inited`），消费点 `:463`、`:474`、`:481`、`:490`、`:501`、`:639-642`
- 现象: `_cb_tick` / `_cb_bar` 是 POD 成员函数指针，不在初始化列表里也不在类内默认成员初始化器里；`initialize()` 在 `:52-53` 才赋值。任何「构造了 WtDtRunner 但还没 initialize」就调用 `proc_tick` / `trigger_tick` / `trigger_bar` 的用法（`WtDtServo.cpp` 的 C 导出层就是先建对象再按顺序初始化，顺序写反即命中）都会读到栈上垃圾指针：`if (_cb_tick != NULL)` 对垃圾值几乎必然为真，于是通过一个野指针发起调用。
- 影响范围: 未定义行为，通常表现为随机崩溃而不是明确错误；本 lane 无法用启用用例证明（一旦命中就是随机的，正是不允许它的原因），只锁源码事实：`WtDtRunner()` 的初始化列表里确实没有这两个成员。修复成本一行（`FuncOnTickCallback _cb_tick { nullptr };`）。
- 复现: 未写用例（触发条件：默认构造 `WtDtRunner` 后直接 `proc_tick`/`trigger_bar`，命中与否取决于栈垃圾，属不可复现崩溃，不适合作为 CI 用例）。

## BUG-R8-005 proc_tick 对 commInfo 零判空，合约未挂品种时段错误
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtServo/WtDtRunner.cpp:422-428`
- 现象: `cInfo` 在 `:419` 判过空，但紧接着 `WTSCommodityInfo* commInfo = cInfo->getCommInfo();`（`:422`）与 `commInfo->getSessionInfo()`（`:423`）、`commInfo->getCategoty()`（`:428`）全部裸用。`WTSBaseDataMgr::loadContracts` 是按 `product` 字段去 `_comm_list` 里挂品种的，合约表引用了 commodities 表里不存在的产品（漏配一个 product 是最常见的基改事故）时，合约能被 `getContract` 命中但 `getCommInfo()` 为 NULL —— 于是第一个 tick 就把数据服务打挂，而且是在「合约存在、品种缺失」这种最需要清晰报错的场景。
- 影响范围: 行情主链路（`ParserAdapter::handleQuote` → `proc_tick`，ParserAdapter.cpp:281-282）；基改配置漏配品种的爆炸半径是整进程，不是单合约。
- 复现: 未写用例（需要先构造「已入库但未挂 commodity 的 WTSContractInfo」，`WTSBaseDataMgr` 没有这种半装配状态的公开入口，需要接缝；留给主控裁定）。

## BUG-R8-006 ParserAdapter::init 的 _parser_api 守卫恒真导致 4 行死代码 + 拒绝态污染 _id + _cfg 泄漏
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtServo/ParserAdapter.cpp:128-132`、`:149` 与 `:197-200`；`:87-91`；`:92-93` 与 `:205-217`
- 现象: 三处同源小缺陷。① `:128-132` 里 `pFuncCreateParser()` 返回 NULL 已经 `return false`，因此到 `:149 if (_parser_api)` 时 `_parser_api` 必非空，`:197-200` 的 else 分支「Parser initializing failed: creating api failed」是**不可达死代码**（4 行），`:52`（initExt 里 `if (api==NULL) return false; _parser_api = api;` 之后再 `if (_parser_api)`）是同一形状的恒真判断。② `:87` 的 `_id = id` 排在 `:89` 的 `_cfg != NULL` 拒绝之前，于是一次失败的重复 init 会把已有适配器名字改成第二次调用的入参，而 `_parser_api`/`_cfg` 仍是第一次的——观测面 `id()` 与内部状态不一致（`r8_parseradapter_test.cpp::InitWithEmptyModuleRejectsButStillBindsIdAndCfg` 已把这个行为钉死为 `EXPECT_STREQ(pa.id(), "pa_second")`，产品侧修正顺序后该断言会失败并暴露改动）。③ `:93 _cfg->retain()` 之后 `release()`（`:205-217`）从不 `release()` `_cfg`，每个适配器泄漏一份解析器配置对象；`WtDtRunner::~WtDtRunner()`（WtDtRunner.cpp:35-37）是空实现，也不会调 `_parsers.release()`，所以数据服务退出时全部 parser 配置与 DllHandle 都留在原地。
- 影响范围: 死代码虚增覆盖率分母；id 污染影响任何按 id 排障的日志对齐；泄漏只在全局析构阶段，长跑进程一次性的。
- 复现: `r8_parseradapter_test.cpp::InitWithEmptyModuleRejectsButStillBindsIdAndCfg`（②，启用）；①③未写用例（①不可达、③需要观测分配器）。

## BUG-R8-007 initParsers 的自动 id 是函数级静态，跨实例只增不减
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtServo/WtDtRunner.cpp:390-395`（`static uint32_t auto_parserid = 1000;`）
- 现象: 配置里不写 `id` 时用 `auto_parser_{n++}` 兜底，但这个计数器是函数级静态、无复位入口，且 `_tick_sub_map`/`ParserAdapterMgr::_adapters` 都是按 id 建键。同一进程里多次 `initialize()`（WtDtServo 的 C 导出层支持重载配置）或多 runner 实例时，第二个实例的同一条配置会得到完全不同的 id（1001、1002…），`addAdapter` 的同名拒绝（ParserAdapter.cpp:317-321）因此永远不会触发，热重载里「旧适配器没清就装新的」也查不出来——旧 id 的适配器还在 `_adapters` 里继续跑。
- 影响范围: id 不可复现使日志/订阅对不上号；重载路径下解析器实例累积（配合 BUG-R8-006③ 的 release 缺失）。
- 复现: `r8_wtdtrunner_test.cpp::InactiveAndAutoIdParserEntriesAreBothHandled`（锁定「无 id 项仍会登记并让 initialize 正常收尾」这一现状，断言 initialize 后 tick 链路可用；若改成 id 从 1000 复位或拒绝无名项，本用例的行为描述需要一起复核）。
