# R18 缺陷清单（迭代01 · WtDtCore/ParserAdapter + IndexWorkerMgr + ShmCaster + DataManager + StateMonitor.h）

- 范围: 本 lane 只记录，未修改任何产品代码；`src/**`（除 `src/TestUnits/coverage/q05_dtsrv/r18_*.cpp`）一字未动。
- 关联用例: `r18_parser_adapter_test.cpp`、`r18_idxworker_mgr_test.cpp`、`r18_shm_caster_test.cpp`、`r18_state_info_datamgr_test.cpp`
- 交叉引用: R5 已登记的面（StateMonitor 状态机时钟接缝、UDPCaster 真 socket）本 lane 未触碰，见 `manifests/R5_wtdtcore.md`。
- 关于任务令提示的 BUG-L00-004（`ParserAdapter.h` 用了 `boost::noncopyable` 却没 include）：本轮实测当前树上 `src/WtDtCore/ParserAdapter.h:14` 已含 `#include <boost/core/noncopyable.hpp>`，用例直接 include 该头文件即可通过 `-Werror` 语法门禁，未复现缺 include；若主控账上仍挂着该条，建议按「已在树内修复」处置，本 lane 不重复主张。

## BUG-R18-001 DataManager::init 不校验 params，NULL 即空指针解引用
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Blocker
- 位置: `src/WtDtCore/DataManager.cpp:67-72`
- 现象: `init(WTSVariant* params, ...)` 第一句可用代码就是 `std::string module = params->getCString("module");`（:72），前面没有任何 `params == NULL` 守卫；`_bd_mgr/_state_mon` 也一律照收。dt_main/wtdtserver 的配置装配若在 cfg 缺失时调用（例如 caster/store 段落读不到却仍调 init），进程直接 SIGSEGV，而不是返回 false。
- 影响范围: 启动路径上任何「配置树缺 store 节点」的部署都会崩进程而非报可读错误。
- 复现: `r18_state_info_datamgr_test.cpp::DISABLED_InitCrashesWhenParamsIsNull`（触发条件：`dm.init(nullptr, nullptr, nullptr)` 即崩在 :72）。
- 复核：直接传 `nullptr` 会在 :72 解引用，源码结论成立。更具体的生产装配链在 `src/WtDtPorter/WtDtRunner.cpp:199,257-260`：`config->get("writer")` 的结果无条件传给 `_data_mgr.init`，中间没有空值守卫；若有效运行配置缺 `writer` 节点，该条件可由仓库调用链触发。本次仅静态核对，没有运行缺节点启动用例。建议在 `init` 首行拒绝空配置并给调用方可读错误，同时确认 writer 是否为配置必需项。


- 迭代02复核: TEST。参数是配置对象；候选把 init(null) 等同配置缺 data/store。runner 缺 data 时 initDataMgr 早退，不会调用此 init；null 不是已证有效输入。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 静态缺陷成立；缺失配置路径未运行
## BUG-R18-002 DataManager::init 在「模块加载成功但找不到写手符号」时无条件解引用 _writer
- 状态: 静态缺陷成立；DISABLED 动态用例未运行
- 严重级别: Blocker
- 位置: `src/WtDtCore/DataManager.cpp:81-97`、`:106`
- 现象: `:82-85` 找不到 `createWriter` 只打一条 error 日志，`:88-91` 找不到 `deleteWriter` 同样只打日志，`:93` 的 `if (pFuncCreateWriter && pFuncDeleteWriter)` 只保护 `_writer` 的赋值；函数体在 `:98` 打完「module loaded」info 日志后，`:106` 无条件 `return _writer->init(params, this);`。因此 dlopen 成功但符号不齐（插件版本与主程序不匹配、被 strip、或装错目录的第三方 dylib）时 `_writer` 保持 NULL → 空指针解引用；`createWriter` 返回 NULL 时同样崩。更糟的是日志顺序把失败伪装成「模块已加载」。
- 影响范围: 成功 `dlopen` 但缺少任一必需符号，或 `createWriter()` 返回 NULL 时，:106 对空 `_writer` 解引用。代码只有在 `createWriter` 与 `deleteWriter` 都存在时才同时赋 `_writer/_remover`，所以“`_writer` 非空而 `_remover` 为空”的同次初始化路径不能由这里的缺符号条件推出；`release()` 的这一附带描述撤回。
- 复现: `r18_state_info_datamgr_test.cpp::DISABLED_InitCrashesWhenLoadedModuleHasNoWriterSymbols`（触发条件：把任一真实 Mach-O dylib 复制到 `<module_dir>/libWtDataStorage.dylib`，再 init 空配置；本机无该样本时用例自动跳过）。
- 复核：函数没有在缺符号或工厂返回 NULL 时失败返回，静态空指针链成立；测试是 DISABLED 且本次未运行，故无崩溃动态证据。建议对符号、工厂返回值和 writer 初始化结果逐项检查并在失败时返回 false；不要在错误日志后继续调用 `_writer->init`。

## BUG-R18-003 ParserAdapter 全程不校验 _dt_mgr / _bd_mgr
- 状态: 已撤回（把无效空指针注入当作正常生产调用）
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:243-249`、`:260-266`、`:277-283`、`:294-308`；构造 `:30-39`
- 现象: 构造函数照收 `DataManager*`/`WTSBaseDataMgr*` 且从不判空，四个 handle 入口的尾部 `_dt_mgr->writeTransaction/writeOrderDetail/writeOrderQueue/writeTick`（:249/266/283/304）与 `_bd_mgr->getContract/getContracts`（:61/160/180/243/260/277/297）都是裸解引用。守卫链只覆盖「停止位、日期为 0、合约查不到」三种情况，合约命中后必 deref。
- 影响范围: 任何把 ParserAdapter 挂在未装配 DataManager 上的用法（单测、离线灌数、解析插件自检）在第一帧合法行情即崩；`handleQuote` 里 `writeTick` 返回 false 直接 return（:304-305），也意味着 `_idx_fact->handle_quote`（:307-308）在没有存储写手时永远不会被驱动。
- 复现: 启用侧 `r18_parser_adapter_test.cpp::HandleQuoteRejectedWhenDatesZeroOrContractUnknown`、`ThreeLevel2HandlersRejectBeforeDataManager`（同一用例把 `_dt_mgr` 置 NULL，只有守卫生效才不崩）；放行侧的崩溃见 `DISABLED_HandleQuoteCrashesWhenDataManagerIsNull`。
- 复核：空 manager 确会使相关解引用崩溃，但仓库装配调用 `WtDtPorter::initParsers/createExtParser` 均以 `&_bd_mgr`、`&_data_mgr` 构造适配器。有效行情调用 `IndexWorkerMgr` 前先经基础数据管理器命中合约；写手未装配时 `DataManager::writeTick` 返回 false，适配器提前返回，不会到 index 分发。所述 DISABLED 用例是直接违反注入前置条件的构造，启用守卫用例只覆盖日期/合约拒绝路径。本项所称“正常行情/离线灌数因此崩溃”未获调用链支持；保留空指针防御可作健壮性改进，不计已证产品缺陷。

## BUG-R18-004 ParserAdapter::release 不置空指针，二次 release 即 double free
- 迭代02第二轮前状态（历史）: - 状态: 待验证（重复 release 的调用契约与生产路径未证）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:207-219`
- 现象: `release()` 置 `_stopped=true` 后调 `_parser_api->release()`，再按 `_remover ? _remover(_parser_api) : delete _parser_api` 释放，但 `_parser_api` 与 `_remover` 都不清空，`ParserAdapter` 也没有幂等标志。第二次 `release()`（或 `release()` 后析构再手工调一次）会对同一对象再 `delete` 一次 → double free 崩溃；DLL 路径下 `_remover(_parser_api)` 二次调用同样重复释放。
- 影响范围: 同一对象再次调用 `release()` 会对旧指针再调用虚函数/删除；该危险性由源码可见。但 `ParserAdapter` 析构函数为空，`ParserAdapterMgr::release()` 只在清表前调用一次，仓库未找到“单个释放后再整体释放”的生产调用链；是否要求 `release()` 幂等需按生命周期契约确认。
- 复现: `r18_parser_adapter_test.cpp::DISABLED_DoubleReleaseDoubleDelete`（触发条件：`initExt` 后连续两次 `release()`）。
- 复核：该 DISABLED 用例只定义了调用两次的输入，本次未运行；其旁注把该项误写成 BUG-R18-003，当前清单编号为 BUG-R18-004。若接口要求可重复释放，应在释放后置空 `_parser_api/_remover` 并验证重复调用；若契约仅允许一次，则应明确一次性生命周期并审查调用者。

## BUG-R18-005 ParserAdapterMgr::addAdapter 先用 id 后判空
- 状态: 已撤回（缺 id 配置崩溃主张不成立；直接 NULL 输入合同未约定）
- 严重级别: 不适用（直接 NULL 可作为防御性改进）
- 位置: `src/WtDtCore/ParserAdapter.cpp:337-352`
- 现象: `if (adapter == NULL || strlen(id) == 0)` —— `&&`/`||` 短路只保护了「adapter 为空」这一路，`adapter` 非空而 `id == NULL` 时先执行 `strlen(NULL)`，UB/崩溃；`id` 指向空串时返回 false 但调用方（parser_mgr 装配处）不看返回值，静默丢一个解析器。
- 影响范围: 仅已证实非空 adapter 配合直接 NULL id 时存在源码级未定义行为；常规缺 id 配置会生成默认 id，不属于此路径。
- 测试: 原禁用测试直接传 NULL，已替换为 `MgrRejectsEmptyIdAndAcceptsUniqueId`，验证非空 adapter 配空字符串时被拒绝及唯一 id 可登记；不再把未约定的 NULL 输入或崩溃当作预期。
- 复核：`strlen(NULL)` 的直接调用形态是源码可确认的危险输入，但常规 `WtDtRunner::initParsers` 对空配置 id 会先生成 `auto_parser_*`，不会把 NULL 传给 `addAdapter`；缺 id 配置因而不按登记中描述的路径崩溃。可以增加 `id == NULL` 防御，但在确认 API 是否承诺接收 NULL 前，不计生产故障。

## BUG-R18-006 ParserAdapter::initExt 失败也返回 true，且跨模块所有权交给 delete
- 状态: 静态缺陷成立（失败返回值）；所有权子项待验证
- 严重级别: Major
- 位置: `src/WtDtCore/ParserAdapter.cpp:46-81`、`:215-218`
- 现象: `initExt()` 的 `_parser_api->init(NULL)` 失败后只打日志，仍 `return true`；同文件配置式 `init()` 在 `_parser_api->init(cfg)` 失败时也落到函数末尾 `return true`。调用方由此无法区分已初始化和失败对象，后续 `run()` 仍可能调用 `connect()`。`initExt()` 下 `_remover` 为空，`release()` 会用主程序 `delete` 释放传入 API；但该注入接口的所有权/分配模块契约未给出，不能据此断言一定跨模块堆错误。配置式插件路径未检查 `deleteParser` 符号，缺失时也会落到 `delete`，这应与插件工厂/销毁函数契约一起核实。
- 影响范围: 解析链路静默失联（行情零帧但服务健康）；插件释放路径与主程序不一致可造成堆损坏。
- 复现: 旧用例固定了失败返回 true，且在释放 API 后读取其成员；现改为 `InitExtReturnsFalseWhenApiInitFails`，使用外部统计对象并避免对失败后 SPI、ID 或所有权策略作无依据断言。重建后单例 FAIL：预期 false、实际 true；成功订阅对照 PASS。
- 复核：源码明确让两个 init 入口在 API 初始化失败后返回 true。修复建议：失败时返回 false，清理/保留 API 的策略与返回值保持一致；为 `initExt` 明确传入对象的所有权或 deleter，并在配置式 DLL 加载时验证销毁符号。

## BUG-R18-007 IndexWorkerMgr::handle_quote 同步分支 retain 无配对 release
- 状态: 静态缺陷成立；未运行 refcount 用例
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:170-203`
- 现象: `:180` 对命中 `_subbed` 的行情统一 `retain()`；只有线程池分支（:184-193）在任务末尾 `newTick->release()`（:192），`else` 同步分支（:195-202）遍历完 `_workers` 后直接落空返回，没有任何 release。`poolsize==0`（默认不建池）时每处理一帧就多 1 个引用，`WTSTickData` 走 `WTSPoolObject` 池化，永不归还即池只增不减。
- 影响范围: `IndexWorkerMgr` 常驻内存随行情条数线性增长，长时间运行后池耗尽；`_pool` 存在时反而正确。
- 复现: `r18_idxworker_mgr_test.cpp::SyncHandleQuoteKeepsOneExtraRetainPerQuote`：自己 create 的 tick 引用数 1 → 2 → 3，逐次断言。
- 复核：在 `_pool == nullptr` 且行情命中 `_subbed` 时，静态控制流显示一次 `retain()` 后同步分支没有配对 `release()`；线程池分支有 release。默认 `poolsize==0` 会保留同步分支。引用数用例定义了可测断言，本次未运行；也未做长跑池增长测量。建议同步循环后用 RAII/配对释放，并在正常同步路径验证多帧 retainCount 不增长。

## BUG-R18-008 IndexWorkerMgr::handle_quote 不判合约信息空指针
- 状态: 已撤回（仓库现行调用链已保证合约信息非空）
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:172-178`
- 现象: `:173` 只判 `newTick == NULL`，紧接 `:175` 就是 `newTick->getContractInfo()->getFullCode()`，没有对 `getContractInfo()` 判空。解析侧 `handleQuote` 只有在 `_bd_mgr` 查得到合约时才会走到 `_idx_fact->handle_quote`，但 `IndexWorkerMgr` 也暴露在 `IIndexContext` 的其他调用点与测试/灌数路径上。
- 影响范围: 空合约信息的行情（灌数、跨市场未知品种）直接崩。
- 复现: `r18_idxworker_mgr_test.cpp::DISABLED_HandleQuoteCrashesWhenContractInfoMissing`。
- 复核：`IndexWorkerMgr::handle_quote` 在读取合约前确未判空，但仓库唯一调用点是 `WtDtCore::ParserAdapter::handleQuote`；该调用先通过 `_bd_mgr` 找到并确认合约，才在写入成功后分发给 index manager。DISABLED 用例直接传入无合约行情，不对应当前调用链。本项作为内部防御可加判空，不把该输入形状计为现行产品缺陷。

## BUG-R18-009 IndexWorkerMgr::sub_ticks 不校验切分结果，无点号代码越界
- 状态: 已撤回（无点号输入不符合 fullCode 前置条件；内置调用先规范化）
- 严重级别: Major
- 位置: `src/WtDtCore/IndexWorkerMgr.cpp:210-216`
- 现象: `:214` `StrUtil::split(fullCode, ".")` 之后直接 `ay[1]`/`ay[0]`，既不校验 `ay.size()` 也不校验空串：传 `"600000"`（无点号）时 `size()==1`，`ay[1]` 越界访问 `std::vector`（`operator[]` 无检查）→ UB/崩溃；传空串时 `ay` 可能为空，`ay[0]` 同样越界。另外按 `CodeHelper.hpp:601-616` 的分月合约键是三段式，用「取第 2 段当 code」的做法对 `EXCHG.PROD.MONTH` 形式会取到品种而非合约。
- 影响范围: 指标工厂侧任何不带点的代码（虚拟货币/内部符号）订阅即崩或订阅到错误键；`_subbed` 已在 `:212` 先写入，越界后集合已被污染。
- 复现: `r18_idxworker_mgr_test.cpp::DISABLED_SubTicksWithoutDotCrashesOnIndex`。
- 复核：直接传不带分隔符的字符串会越界，但 `sub_ticks` 的唯一内置调用来自 `WtBlkIdxWorker`，两条配置分支都生成/归一为 `EXCHG.code` 两段形式后再调用；三段式标准码亦先经 `CodeHelper::extractStdCode` 归一为两段码。因此测试的坏格式不对应内置产品路径。可为外部 index worker 增加入参校验，但当前不是合法 `fullCode` 路径缺陷。

## BUG-R18-010 ShmCaster::init 不校验映射结果就在空地址上 placement-new
- 状态: 已合并（主记录 BUG-R68-002）
- 严重级别: Minor
- 位置: `src/WtDtCore/ShmCaster.cpp:35-58`
- 现象: `:35` 直接取配置里的 `path` 字符串，不校验是否为空/是否可写；`:40-42` 的 `create_or_open_file`/`truncate_file` 返回值被忽略，`:47` ` _mapfile->map(_path.c_str())` 的返回值同样被忽略，`:48` 立刻把 `addr()` 当 `CastQueue*` 用，`:51` 在该地址上 `new CastQueue()`。映射失败（文件不存在、权限、盘满、路径是目录）时 `addr()` 为 NULL → 在 NULL 上构造 3MB+ 对象。`:31` 的 `active` 与 `:27` 的 `cfg == NULL` 是函数里仅有的两处真校验。
- 影响范围: 共享内存广播器起不来时表现为崩溃或写入非法地址；本 lane 按任务令不建真映射，只用拒绝分支证明其余面无校验。
- 复现: 启用用例 `InitRejectsNullConfig`、`InitRejectsInactiveAndTouchesNoFile`（拒绝路径不落任何文件）；崩溃面见 `DISABLED_InitWithActiveTrueNeedsRealMappedFile`（触发条件：active=true 时 create/truncate 约 3MB 映射文件并 mmap）。
- 复核：`BoostMappingFile::map` 在文件不存在时返回 false，而 `ShmCaster::init` 忽略返回值并继续取 `addr()` placement-new；无父目录路径的空地址崩溃与目录路径抛异常已由 BUG-R68-002 更完整登记。本 lane 的用例只运行拒绝分支定义，本次未运行映射失败崩溃；按 BUG-R68-002 统一修复/计数。

## BUG-R18-011 ShmCaster 写侧注释与实现不符：所谓原子自增只是 volatile
- 状态: 待验证（静态并发风险成立；并发条件未运行验证）
- 严重级别: Minor
- 位置: `src/WtDtCore/ShmCaster.h:71-73`、`src/WtDtCore/ShmCaster.cpp:83-91`（同形状见 `:110-118`、`:137-145`、`:164-172`）
- 现象: 四处广播实现的注释都写「原子操作获取并且递增写指针」，但 `_writable` 声明为 `volatile uint64_t`（`_readable` 同），`_queue->_writable++` 是 volatile 上的读-改-写，不是原子；且 `_items[realIdx]._type`（:87）在 payload 拷贝（:89）之前写，`_readable` 之后（:91）才更新，跨进程读者没有任何屏障保证看到「类型已更新、数据仍是旧的」这一中间态。多生产者（同一路数据被多个 caster 线程广播）时下标可丢。
- 影响范围: 消费端偶发读到半新半旧的包；结构体大小/下标回绕本身是确定的（本 lane 已用纯内存用例锁死算式）。
- 复现: 尚无生产广播及并发运行时复现。`r18_shm_caster_test.cpp::RingIndexModuloWrapsWithinSmallQueue` 在测试内复制下标算式而没有调用 `ShmCaster::broadcast`；`QueueTemplateDefaultsCapacityReadableWritablePid` 只检查默认值。两者均不能证明并发行为。
- 复核：源码属性确认：`volatile` 不提供原子读改写或跨进程 acquire/release 屏障，且 `_type` 在 payload 前写；“原子”注释不准确。现有两项测试只覆盖默认值/环形下标，不验证并发读写或半包可见性；本次未运行并发测试，也未证明生产中同一队列有多个并发写者/读者。建议在确认共享协议与进程数后，按兼容的跨进程原子/锁协议发布数据，并以多写者/读者验证；在此之前不要把所述竞争、丢位或半新半旧包记为已实测现象。

## BUG-R18-012 ParserAdapter::init 未释放 retained `_cfg`
- 状态: 静态资源泄漏成立；现有用例未直接核对引用计数
- 严重级别: Minor
- 位置: `src/WtDtCore/ParserAdapter.cpp:91-137`、`:140-149`、`:207-219`
- 现象: `:94-95` 对 `_cfg` retain；无论 module 为空、模块加载失败、后续 parser init 成功或失败，析构函数为空，`release()`（:207-219）也不释放 `_cfg`。因此 adapter 生命周期结束后 retained 引用没有配对释放。`_filters` 不清空是另一个状态维护观察，但 `init()` 的 `_cfg != NULL` 守卫禁止同对象重入，`initExt()` 又不接收 filter 参数；本次没有证据表明这造成当前有效调用路径错误，不并入资源泄漏结论。
- 影响范围: 每个走过 `init()` 的 adapter 至少遗留一份配置引用；配置树何时释放取决于外部持有者，本次没有测量 RSS/长期增长。
- 复现: 启用用例 `InitRejectsNullCfgEmptyModuleAndMissingModule` 锁定「首次 init 失败后二次 init 必走 :91 的重复守卫返回 false」，即 `_cfg` 已被 retain 这一副作用的外在表现。
- 复核：该用例只证明 `_cfg` 保持非空并阻止重试，未检查 `WTSVariant` refcount 或内存释放；泄漏来自源码中 retain 无任何 release 对应。建议失败回滚与 `release()` 都配对释放 `_cfg`、置空，并用引用计数/析构替身验收。`_filters` 子项不作为已确认缺陷。

## 本 lane 未攻的面（避免下波重复踩）
1. `ParserAdapter::init` 的 dlopen 成功尾段（`:122-137`）与 `:140-204` 的 filter/订阅全流程 —— 需要一个能加载的真实 parser 插件；本轮一律用不存在的模块名走 `:111-116`。估计 40~55 行。
2. `ParserAdapter.cpp:307-308`（`_idx_fact->handle_quote`）—— `writeTick` 在 `_writer == NULL` 时恒 false，`DataManger::writeTick` 需要真实存储写手插件才能放行。约 2 行但卡在整条链上。
3. `IndexWorkerMgr.cpp:66-91` 的「工厂命中」分支（`creator()` 返回非空、`_factories` 登记）与 `:116-126` 的 `createWorker` 成功路径 —— 需要真实 `create_index_factory` 插件；`_factories/_workers` 私有且无注入点，本轮只打到「dlopen 成功但缺符号 → free_library → continue」。约 25~30 行。
4. `IndexWorkerMgr.cpp:136-139`（`poolsize > 0` 建线程池）与 `:182-194`（池分支的异步任务）—— 任务令禁止起线程。约 15 行。
5. `ShmCaster.cpp:35-64`（真映射尾段）与四个 `broadcast` 的写入体（`:83-91` 等）—— 任务令明令不建共享内存段；`_queue` 私有且无 setter，无法注入本地队列。约 35~45 行。
6. `StateMonitor.h` 的 `isAnyInState/isAllInState/isInState` 在**非空表**上的命中/未命中两侧，与 `StateInfo::_sections` 在真实配置下的装配 —— 已由 R5 覆盖到 `initialize` 极限，剩下的仍卡在 `StateMonitor.cpp:145-530` 的时钟接缝（BUG-R5-002）。本 lane 只补了空表分支与 `isInSections` 纯内存分支。
7. `DataManager.cpp:93-98`、`:106` 的成功尾段（真实 `createWriter`/`deleteWriter` + `_writer->init`）—— 需要存储插件；本轮把两条失败路径与守卫缺口留在 BUG-R18-002。约 20 行。
