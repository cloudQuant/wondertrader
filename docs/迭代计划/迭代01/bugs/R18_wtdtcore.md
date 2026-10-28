# R18 缺陷清单（迭代01 · WtDtCore/ParserAdapter + IndexWorkerMgr + ShmCaster + DataManager + StateMonitor.h）

- 范围: 本 lane 只记录，未修改任何产品代码；`src/**`（除 `src/TestUnits/coverage/q05_dtsrv/r18_*.cpp`）一字未动。
- 关联用例: `r18_parser_adapter_test.cpp`、`r18_idxworker_mgr_test.cpp`、`r18_shm_caster_test.cpp`、`r18_state_info_datamgr_test.cpp`
- 交叉引用: R5 已登记的面（StateMonitor 状态机时钟接缝、UDPCaster 真 socket）本 lane 未触碰，见 `manifests/R5_wtdtcore.md`。
- 关于任务令提示的 BUG-L00-004（`ParserAdapter.h` 用了 `boost::noncopyable` 却没 include）：本轮实测当前树上 `src/WtDtCore/ParserAdapter.h:14` 已含 `#include <boost/core/noncopyable.hpp>`，用例直接 include 该头文件即可通过 `-Werror` 语法门禁，未复现缺 include；若主控账上仍挂着该条，建议按「已在树内修复」处置，本 lane 不重复主张。

## BUG-R18-001 DataManager::init 不校验 params，NULL 即空指针解引用
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtCore/DataManager.cpp:67-72`
- 现象: `init(WTSVariant* params, ...)` 第一句可用代码就是 `std::string module = params->getCString("module");`（:72），前面没有任何 `params == NULL` 守卫；`_bd_mgr/_state_mon` 也一律照收。dt_main/wtdtserver 的配置装配若在 cfg 缺失时调用（例如 caster/store 段落读不到却仍调 init），进程直接 SIGSEGV，而不是返回 false。
- 影响范围: 启动路径上任何「配置树缺 store 节点」的部署都会崩进程而非报可读错误。
- 复现: `r18_state_info_datamgr_test.cpp::DISABLED_InitCrashesWhenParamsIsNull`（触发条件：`dm.init(nullptr, nullptr, nullptr)` 即崩在 :72）。

## BUG-R18-002 DataManager::init 在「模块加载成功但找不到写手符号」时无条件解引用 _writer
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtCore/DataManager.cpp:81-97`、`:106`
- 现象: `:82-85` 找不到 `createWriter` 只打一条 error 日志，`:88-91` 找不到 `deleteWriter` 同样只打日志，`:93` 的 `if (pFuncCreateWriter && pFuncDeleteWriter)` 只保护 `_writer` 的赋值；函数体在 `:98` 打完「module loaded」info 日志后，`:106` 无条件 `return _writer->init(params, this);`。因此 dlopen 成功但符号不齐（插件版本与主程序不匹配、被 strip、或装错目录的第三方 dylib）时 `_writer` 保持 NULL → 空指针解引用；`createWriter` 返回 NULL 时同样崩。更糟的是日志顺序把失败伪装成「模块已加载」。
- 影响范围: 插件/主程序版本错配是最常见的现场故障之一，表现为崩溃而非「存储不可用」的降级；`_remover` 也未赋值，`release()`（:127-134）在 `_writer` 非空但 `_remover` 为空时同样会调空函数指针。
- 复现: `r18_state_info_datamgr_test.cpp::DISABLED_InitCrashesWhenLoadedModuleHasNoWriterSymbols`（触发条件：把任一真实 Mach-O dylib 复制到 `<module_dir>/libWtDataStorage.dylib`，再 init 空配置；本机无该样本时用例自动跳过）。

## BUG-R18-003 ParserAdapter 全程不校验 _dt_mgr / _bd_mgr
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:243-249`、`:260-266`、`:277-283`、`:294-308`；构造 `:30-39`
- 现象: 构造函数照收 `DataManager*`/`WTSBaseDataMgr*` 且从不判空，四个 handle 入口的尾部 `_dt_mgr->writeTransaction/writeOrderDetail/writeOrderQueue/writeTick`（:249/266/283/304）与 `_bd_mgr->getContract/getContracts`（:61/160/180/243/260/277/297）都是裸解引用。守卫链只覆盖「停止位、日期为 0、合约查不到」三种情况，合约命中后必 deref。
- 影响范围: 任何把 ParserAdapter 挂在未装配 DataManager 上的用法（单测、离线灌数、解析插件自检）在第一帧合法行情即崩；`handleQuote` 里 `writeTick` 返回 false 直接 return（:304-305），也意味着 `_idx_fact->handle_quote`（:307-308）在没有存储写手时永远不会被驱动。
- 复现: 启用侧 `r18_parser_adapter_test.cpp::HandleQuoteRejectedWhenDatesZeroOrContractUnknown`、`ThreeLevel2HandlersRejectBeforeDataManager`（同一用例把 `_dt_mgr` 置 NULL，只有守卫生效才不崩）；放行侧的崩溃见 `DISABLED_HandleQuoteCrashesWhenDataManagerIsNull`。

## BUG-R18-004 ParserAdapter::release 不置空指针，二次 release 即 double free
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:207-219`
- 现象: `release()` 置 `_stopped=true` 后调 `_parser_api->release()`，再按 `_remover ? _remover(_parser_api) : delete _parser_api` 释放，但 `_parser_api` 与 `_remover` 都不清空，`ParserAdapter` 也没有幂等标志。第二次 `release()`（或 `release()` 后析构再手工调一次）会对同一对象再 `delete` 一次 → double free 崩溃；DLL 路径下 `_remover(_parser_api)` 二次调用同样重复释放。
- 影响范围: `ParserAdapterMgr::release()`（:327-335）本身安全（清表），但停服流程里「先停单个 parser 再整体 release」的写法直接踩坑。
- 复现: `r18_parser_adapter_test.cpp::DISABLED_DoubleReleaseDoubleDelete`（触发条件：`initExt` 后连续两次 `release()`）。

## BUG-R18-005 ParserAdapterMgr::addAdapter 先用 id 后判空
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:337-352`
- 现象: `if (adapter == NULL || strlen(id) == 0)` —— `&&`/`||` 短路只保护了「adapter 为空」这一路，`adapter` 非空而 `id == NULL` 时先执行 `strlen(NULL)`，UB/崩溃；`id` 指向空串时返回 false 但调用方（parser_mgr 装配处）不看返回值，静默丢一个解析器。
- 影响范围: 配置里 parser 条目缺 id 时崩溃而不是报错。
- 复现: `r18_parser_adapter_test.cpp::DISABLED_AddAdapterWithNullIdCrashesInStrlen`。

## BUG-R18-006 ParserAdapter::initExt 失败也返回 true，且跨模块所有权交给 delete
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:46-81`、`:215-218`
- 现象: `:58` 的 `_parser_api->init(NULL)` 失败只打一条日志（:76），函数照旧 `return true`（:80）；上层无法区分「api 起不来」与「起好了」。同时 `_remover` 只在 `init()` 的 DLL 路径里赋值（:136），`initExt` 走的注入路径下 `release()` 落到 `delete _parser_api`（:218）——若 api 由插件模块创建，用主程序的 `delete` 释放模块内对象属跨模块所有权错误。
- 影响范围: 解析链路静默失联（行情零帧但服务健康）；插件释放路径与主程序不一致可造成堆损坏。
- 复现: 启用用例 `InitExtStillReturnsTrueWhenApiInitFails`（断言现状 `EXPECT_TRUE` + `subscribe` 未被调用），锁死语义；改判为 `return false` 时该断言即失败。

## BUG-R18-007 IndexWorkerMgr::handle_quote 同步分支 retain 无配对 release
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:170-203`
- 现象: `:180` 对命中 `_subbed` 的行情统一 `retain()`；只有线程池分支（:184-193）在任务末尾 `newTick->release()`（:192），`else` 同步分支（:195-202）遍历完 `_workers` 后直接落空返回，没有任何 release。`poolsize==0`（默认不建池）时每处理一帧就多 1 个引用，`WTSTickData` 走 `WTSPoolObject` 池化，永不归还即池只增不减。
- 影响范围: `IndexWorkerMgr` 常驻内存随行情条数线性增长，长时间运行后池耗尽；`_pool` 存在时反而正确。
- 复现: `r18_idxworker_mgr_test.cpp::SyncHandleQuoteKeepsOneExtraRetainPerQuote`：自己 create 的 tick 引用数 1 → 2 → 3，逐次断言。

## BUG-R18-008 IndexWorkerMgr::handle_quote 不判合约信息空指针
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:172-178`
- 现象: `:173` 只判 `newTick == NULL`，紧接 `:175` 就是 `newTick->getContractInfo()->getFullCode()`，没有对 `getContractInfo()` 判空。解析侧 `handleQuote` 只有在 `_bd_mgr` 查得到合约时才会走到 `_idx_fact->handle_quote`，但 `IndexWorkerMgr` 也暴露在 `IIndexContext` 的其他调用点与测试/灌数路径上。
- 影响范围: 空合约信息的行情（灌数、跨市场未知品种）直接崩。
- 复现: `r18_idxworker_mgr_test.cpp::DISABLED_HandleQuoteCrashesWhenContractInfoMissing`。

## BUG-R18-009 IndexWorkerMgr::sub_ticks 不校验切分结果，无点号代码越界
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:210-216`
- 现象: `:214` `StrUtil::split(fullCode, ".")` 之后直接 `ay[1]`/`ay[0]`，既不校验 `ay.size()` 也不校验空串：传 `"600000"`（无点号）时 `size()==1`，`ay[1]` 越界访问 `std::vector`（`operator[]` 无检查）→ UB/崩溃；传空串时 `ay` 可能为空，`ay[0]` 同样越界。另外按 `CodeHelper.hpp:601-616` 的分月合约键是三段式，用「取第 2 段当 code」的做法对 `EXCHG.PROD.MONTH` 形式会取到品种而非合约。
- 影响范围: 指标工厂侧任何不带点的代码（虚拟货币/内部符号）订阅即崩或订阅到错误键；`_subbed` 已在 `:212` 先写入，越界后集合已被污染。
- 复现: `r18_idxworker_mgr_test.cpp::DISABLED_SubTicksWithoutDotCrashesOnIndex`。

## BUG-R18-010 ShmCaster::init 不校验映射结果就在空地址上 placement-new
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/ShmCaster.cpp:35-58`
- 现象: `:35` 直接取配置里的 `path` 字符串，不校验是否为空/是否可写；`:40-42` 的 `create_or_open_file`/`truncate_file` 返回值被忽略，`:47` ` _mapfile->map(_path.c_str())` 的返回值同样被忽略，`:48` 立刻把 `addr()` 当 `CastQueue*` 用，`:51` 在该地址上 `new CastQueue()`。映射失败（文件不存在、权限、盘满、路径是目录）时 `addr()` 为 NULL → 在 NULL 上构造 3MB+ 对象。`:31` 的 `active` 与 `:27` 的 `cfg == NULL` 是函数里仅有的两处真校验。
- 影响范围: 共享内存广播器起不来时表现为崩溃或写入非法地址；本 lane 按任务令不建真映射，只用拒绝分支证明其余面无校验。
- 复现: 启用用例 `InitRejectsNullConfig`、`InitRejectsInactiveAndTouchesNoFile`（拒绝路径不落任何文件）；崩溃面见 `DISABLED_InitWithActiveTrueNeedsRealMappedFile`（触发条件：active=true 时 create/truncate 约 3MB 映射文件并 mmap）。

## BUG-R18-011 ShmCaster 写侧注释与实现不符：所谓原子自增只是 volatile
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/ShmCaster.h:71-73`、`src/WtDtCore/ShmCaster.cpp:83-91`（同形状见 `:110-118`、`:137-145`、`:164-172`）
- 现象: 四处广播实现的注释都写「原子操作获取并且递增写指针」，但 `_writable` 声明为 `volatile uint64_t`（`_readable` 同），`_queue->_writable++` 是 volatile 上的读-改-写，不是原子；且 `_items[realIdx]._type`（:87）在 payload 拷贝（:89）之前写，`_readable` 之后（:91）才更新，跨进程读者没有任何屏障保证看到「类型已更新、数据仍是旧的」这一中间态。多生产者（同一路数据被多个 caster 线程广播）时下标可丢。
- 影响范围: 消费端偶发读到半新半旧的包；结构体大小/下标回绕本身是确定的（本 lane 已用纯内存用例锁死算式）。
- 复现: 结构性证据由 `r18_shm_caster_test.cpp::RingIndexModuloWrapsWithinSmallQueue`（下标回绕与 `_readable = wIdx` 的实际语义：第 i 次写入后可读位是 i-1）与 `QueueTemplateDefaultsCapacityReadableWritablePid`（`_readable` 初值 UINT64_MAX）提供。

## BUG-R18-012 ParserAdapter::init 早退路径泄漏 _cfg，_filters 不重建
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/ParserAdapter.cpp:91-137`、`:140-149`、`:207-219`
- 现象: `:94-95` 先 `_cfg = cfg; _cfg->retain();`，随后 `:99-100`（module 为空）、`:104-116`（模块加载失败）、`:122-134`（入口函数缺失/parser 创建失败）三条早退都不释放 `_cfg`，`release()`（:207-219）也从不 `release _cfg` 或置空 → 每次失败的 init 泄漏一棵配置树。`_filters`（:140-149）只在 init 里累加、从不清空，二次 init 时旧过滤集合仍在（虽然 `:91` 的重复 init 守卫让这基本不可达，但 `initExt` 与 `init` 混用时语义不确定）。
- 影响范围: 长跑服务反复重连/重建 parser 时的稳定内存增长；日志刷屏但不影响正确返回值。
- 复现: 启用用例 `InitRejectsNullCfgEmptyModuleAndMissingModule` 锁定「首次 init 失败后二次 init 必走 :91 的重复守卫返回 false」，即 `_cfg` 已被 retain 这一副作用的外在表现。

## 本 lane 未攻的面（避免下波重复踩）
1. `ParserAdapter::init` 的 dlopen 成功尾段（`:122-137`）与 `:140-204` 的 filter/订阅全流程 —— 需要一个能加载的真实 parser 插件；本轮一律用不存在的模块名走 `:111-116`。估计 40~55 行。
2. `ParserAdapter.cpp:307-308`（`_idx_fact->handle_quote`）—— `writeTick` 在 `_writer == NULL` 时恒 false，`DataManger::writeTick` 需要真实存储写手插件才能放行。约 2 行但卡在整条链上。
3. `IndexWorkerMgr.cpp:66-91` 的「工厂命中」分支（`creator()` 返回非空、`_factories` 登记）与 `:116-126` 的 `createWorker` 成功路径 —— 需要真实 `create_index_factory` 插件；`_factories/_workers` 私有且无注入点，本轮只打到「dlopen 成功但缺符号 → free_library → continue」。约 25~30 行。
4. `IndexWorkerMgr.cpp:136-139`（`poolsize > 0` 建线程池）与 `:182-194`（池分支的异步任务）—— 任务令禁止起线程。约 15 行。
5. `ShmCaster.cpp:35-64`（真映射尾段）与四个 `broadcast` 的写入体（`:83-91` 等）—— 任务令明令不建共享内存段；`_queue` 私有且无 setter，无法注入本地队列。约 35~45 行。
6. `StateMonitor.h` 的 `isAnyInState/isAllInState/isInState` 在**非空表**上的命中/未命中两侧，与 `StateInfo::_sections` 在真实配置下的装配 —— 已由 R5 覆盖到 `initialize` 极限，剩下的仍卡在 `StateMonitor.cpp:145-530` 的时钟接缝（BUG-R5-002）。本 lane 只补了空表分支与 `isInSections` 纯内存分支。
7. `DataManager.cpp:93-98`、`:106` 的成功尾段（真实 `createWriter`/`deleteWriter` + `_writer->init`）—— 需要存储插件；本轮把两条失败路径与守卫缺口留在 BUG-R18-002。约 20 行。
