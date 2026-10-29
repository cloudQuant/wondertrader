# R10 WtCore 缺陷登记（lane R10，只登记不改产品代码）

范围：`HftStraBaseCtx.cpp` `EventNotifier.cpp` `WtDtMgr.cpp` `WtArbiExecuter.cpp` `WtSelEngine.cpp` `WtDiffExecuter.cpp`。
分级：P0 崩溃/进程终止 · P1 功能错误 · P2 健壮性/资源 · P3 一致性/可读性 · 需接缝（可达性阻塞，非缺陷但挡覆盖率）。
所有行号取自本 lane 实读的源码；未修改任何产品代码，未放宽任何断言。
复核口径（2026-10-29）：初次复核仅静态检查源码、接口和现有测试源文件；后续 BUG-R10-004 定向运行证据见对应条目。其他测试存在不代表已通过。

---

## BUG-R10-001 【P0】WtArbi/WtDiff 执行器的 `_config` 未初始化 + policy 链无判空
- 位置：`src/WtCore/WtDiffExecuter.cpp` L41-50（构造函数初始化列表缺 `_config`）、L243-250（`_config->get("policy")` → `policy->has()` → `policy->get(des)` → `cfg->getCString("name")`）；`src/WtCore/WtArbiExecuter.cpp` L33-42、L163-171、L183-185
- 触发条件：任何人在 `init()` 之前（或 `init(NULL)` 之后，见 L84-88/L74-77 的早退分支）调用 `on_tick/on_trade/on_order/on_entrust/on_position_changed/set_position/on_*` —— 这些入口第一个动作就是 `getUnit()`，而 `getUnit` 无条件解引用 `_config`（未初始化的指针值）。第二种触发：`policy` 子节点缺失，或 commID 与 `"default"` 都不在 policy 里 ⇒ `policy->get("default")` 返回 NULL 后 `cfg->getCString` 崩溃。
- 期望：`_config` 应在构造列表里置 NULL，`getUnit()` 开头对 `_config==NULL || policy==NULL || cfg==NULL` 三个形态返回空 `ExecuteUnitPtr`。
- 测试侧处置：本 lane 所有启用用例都先 `init()` 且 policy 必带 `default.name`；未构造上述形态（会崩进程），因此该路径**无用例覆盖**。
- 状态: 待验证（源码显示无效初始化/缺失 policy 链可能空指针；配置输入合同与负例尚待确认）

## BUG-R10-002 【需接缝】WtExecuterFactory 无进程内工厂注入口；剩余执行单元覆盖依赖插件
- 位置：`src/WtCore/WtExecuterFactory.h` L77-182（`_factories` 私有、只有 `loadFactories(path)` 扫目录 dylib 一条填充路径、无 `registerFactory`/无虚函数）；受影响 `WtArbiExecuter.cpp` L186-196、L452-510、L555-567、L601-602、L622-633、L655-666、L686-698、L711-728、L742-758、L783-800、L874-888；`WtDiffExecuter.cpp` L261-285、L508-545、L564-595、L617-639、L657-668、L690-708、L727-738、L754-765、L782-813、L833-843、L871-881
- 现象：`create*ExeUnit()` 在未加载插件时恒返回空（`_factories.find` 命中不到，行为正确），于是 `getUnit()` 恒空，两个执行器里「价差/多腿撮合、价量约束、部分成交推进、超时撤单」全部落在 `if (unit == NULL) return;` 之后。`ExeUnitWrapper`/`IExecuterFact` 本身都是公开可继承的，但没有任何地方能把替身塞进 `_factories`。
- 估计阻塞行数：arbi ~90、diff ~120（另有线程池分支 ~70，规则 2 禁止起线程）。
- 建议接缝（择一）：`WtExecuterFactory` 增加 `bool registerFact(IExecuterFact*, const char* name)`；或 `Wt*Executer` 暴露 `setUnitForTest(stdCode, ExecuteUnitPtr)` 直接写 `_unit_map`。
- 状态: 已撤回（这是测试接缝/覆盖范围项，不是产品故障；现有插件路径未在本轮运行）

## BUG-R10-003 【P2】HftStraBaseCtx 未绑定 `_trader` 时多个公开 API 仍会解引用
- 位置：`src/WtCore/HftStraBaseCtx.cpp`：判空的有 L228（`if (_trader && !_trader->checkOrderLimits(...))`）、L292、L312；**不判空**的有 L190（`stra_cancel(uint32_t)` 直接 `return _trader->cancel(localid)`）、L196/L199（`stra_cancel(code,isBuy,qty)`）、L234（ruletag 命中分支里 `_trader->buy` 在 `_trader` 判空之后仍被无条件调用）、L249（else 分支 `if (!_trader->checkOrderLimits(...))` 无判空）、L255、L641、L645、L659、L683（`stra_get_position`/`stra_get_undone`）。
- 连带后果：`on_trade` L544-548 在 `_data_agent==true` 时会先调 `stra_get_position(stdCode)`；若上下文没有有效 trader 绑定，该调用不安全。`_data_agent` 本身不代表没有 trader：Runner 对 agent/non-agent 使用相同的查找与绑定流程。R10 裸上下文用例采用 `_data_agent=false`，没有覆盖未绑定 trader 路径。
- 期望：与 L228/L292/L312 统一——`_trader` 为空时记 error 并返回空 `OrderIDs()`/0.0。
- 状态: 已合并（同一 `_trader` 构造漏初始化/未绑定后仍可调用问题已登记于 BUG-R42-002；Runner 绑定失败路径见 BUG-R13-001，R214 已复核同一路径）

## BUG-R10-004 【P0】WtSelEngine::on_minute_end 的任务派发线程在局部作用域结束时 joinable ⇒ std::terminate
- 位置：`src/WtCore/WtSelEngine.cpp` L330-333
  ```cpp
  StdThreadPtr thrd(new StdThread([ctx, curDate, curTime, nextTime](){ ... }));
  ```
  `StdThread` = `std::thread`（`src/Share/StdUtils.hpp` L23-24），`thrd` 是 for 循环体内的局部对象；没有 `detach()`、没有 `join()`，迭代结束即析构 ⇒ 按标准「joinable 的 std::thread 析构调用 std::terminate」，整个进程 abort。
- 触发条件：任一注册任务到期（`_time == getNextMinute(curTime,1)`、当分钟未执行过、非假日、period 判定 `bIgnore==false`，如 TPT_Daily）。此时局部 `std::thread` 在循环体结束时仍是 joinable，析构即调用 `std::terminate`；线程函数是否已执行完不改变 joinable 状态。
- 测试侧处置：原禁用用例改为 `R10SelEngineTest.minute_end_fires_task_without_terminating_process`，在子进程要求到期任务正常派发及退出；原子计数避免测试线程间竞争。未命中时间的对照不派发。
- 期望：改为引擎持有并在 release 时安全收尾的任务执行器/线程池；避免仅 detach 后悬空访问 ctx。
- 状态: 本机子进程在到期调用前标记后 signal 6；具体终止栈未采集

## BUG-R10-005 【P1】EventNotifier：委托回报引用计数泄漏 + `_register` 符号未判空
- 位置：`src/WtCore/EventNotifier.cpp` L214-230（`notify(..., WTSOrderInfo*)`）：L221 `ordInfo->retain()` 后，L222-227 的 lambda 里只调 `orderToJson` 与 `_publisher`，**没有 `ordInfo->release()`**；对照成交分支 L204 `retain()` ↔ L210 `release()` 成对。⇒ 每笔委托回报泄漏一个引用计数，`WTSObject` 永不归零，长时间运行必然单调增长（委托回报量级远大于成交）。
- 位置：L86-91 —— `destroy_server/publish_message/regiter_callbacks` 查找后未统一校验；其中 `_register(on_mq_log)` 是无条件调用，符号缺失会空函数指针调用。`_publisher` 在发布 lambda 中有判空，析构的 `_remover` 也有判空；不能把三者都描述为必然空指针崩溃。
- 可达性：两段都在 `_mq_sid != 0` / 插件装载成功之后，本 lane 无法在不加载 WtMsgQue 的前提下用测试证明（见 BUG-R10-006），故只登记。
- 状态: 待验证（引用泄漏子项已合并到 BUG-R41-003；剩余符号问题依赖不完整/不兼容 MQ 插件，未构造该 ABI 形态）

## BUG-R10-006 【需接缝】EventNotifier 的同步测试受插件与 worker 约束，当前已存在集成测试路径
- 位置：`src/WtCore/EventNotifier.h` L38-67（`_mq_sid`、`_asyncio`、`_publisher`、`_worker` 全私有）；`EventNotifier.cpp` L58-112（`_mq_sid` 只能由插件导出的 `create_server` 赋值，L94；worker 线程 L98-109 每 2ms 轮询一次 `_asyncio.run_one()`）。
- 现象：9 个公开 `notify*` 的第一行都是 `if (_mq_sid == 0) return;`（L116/145/171/199/216/309/340/371），因此 L118-400 的 9 个 lambda（rapidjson 组包 + `_publisher` 广播，含 `tradeToJson` L232-265、`orderToJson` L267-305）在「不起线程、不建真实 shm/socket」的约束下完全不可达；即便能置 `_mq_sid`，也没有公开手段排空 `_asyncio` 来同步执行 lambda。
- 估计阻塞行数：**~340 行**（该文件 405 行里的绝大部分）。
- 建议接缝：`init` 拆出可注入的 publisher 回调（`std::function` 或 `IMqPublisher`），并把 `_asyncio` 的排空暴露成 `poll()`/`drainForTest()`；`tradeToJson/orderToJson` 降为 `protected` 以便直接断言 JSON 字段。
- 状态: 已撤回（测试边界项，不是产品缺陷；插件/集成覆盖另行验证）

## BUG-R10-007 【需接缝】WtDtMgr 的 `_reader` 只能由 WtDataStorage 插件装载，无 setter
- 位置：`src/WtCore/WtDtMgr.h` L239-244（`_reader` 私有，只有 `reader()` getter；`regsiter_loader` 只能喂 `_loader`）；`WtDtMgr.cpp` L50-87 `initStore()` 走 `DLLHelper::load_library` + `createDataReader`。
- 后果：`init(cfg,...)` 在 `cfg->get("store")==NULL` 时只完成 `_engine/_force_cache/_align_by_section` 赋值就返回 false（L89-102），此后 L277-400 五个切片查询、L109-117 的 `_bar_notifies` 派发、L183-217 的 on_bar 重采样、L229-239 的 `_ticks_adjusted`、L287-375 后复权 tick 缓存与 `std::lower_bound` 切片、L426-542 的 K 线重采样缓存全部走 `_reader==NULL` 早退 ⇒ 约 **230 行**不可达（本 lane 已把早退分支逐条打满）。
- 建议接缝：`void regsiter_reader(IDataReader*)`（与 `regsiter_loader` 对称），使回测/单测能挂进程内替身 reader。
- 状态: 已撤回（“所有 reader 相关路径不可达”的系统级覆盖结论已过时；见复核说明中的 R73/R160 测试路径）

## BUG-R10-008 【P3】WtDtMgr::get_adjusting_flag 用函数内 static 记忆化，跨实例生效
- 位置：`src/WtCore/WtDtMgr.cpp` L263-275
  ```cpp
  static uint32_t flag = UINT_MAX;   // 函数内 static，不是成员
  ```
- 现象：`WtDtMgr` 不是单例（回测引擎与实盘引擎可各持一个，`_reader` 各自的复权模式不同），但复权标志第一次取值后被整个进程缓存 ⇒ 第二个实例永远沿用第一个实例（甚至 `_reader==NULL` 时算出的 0）的模式，静默改变复权口径。
- 测试侧处置：`r10_wtcore_dtmgr.cpp` 的 `adjusting_factor_and_flag_fall_back_without_reader` 只断言 0（本进程内所有 WtDtMgr 都没有 reader，结论与实现一致），未额外放宽。
- 期望：改成成员变量（或按 `_reader` 指针为键的缓存）。
- 状态: 待验证（共享 static 的实例串扰可静态推导；双实例负例尚未运行）

## BUG-R10-009 【P1】WtDiffExecuter::set_position 的「不在目标里就自动清零」用标准代码去查合约
- 位置：`src/WtCore/WtDiffExecuter.cpp` L598-608
  ```cpp
  WTSContractInfo* cInfo = _bd_mgr->getContract(stdCode);   // stdCode 形如 SHFE.ag.2512
  if (cInfo == NULL) continue;
  ```
- 现象：`IBaseDataMgr::getContract(code, exchg, uDate)` 的入参是**交易所原始代码 + 交易所**（同文件其他地方都先 `CodeHelper::extractStdCode` 再查，例如 L141-142、L160-161），这里直接把标准代码当 code 且 exchg 用默认 `""`。真实 `WtBaseDataMgr` 的键是 `code|exchg`，必然查不到 ⇒ L610-638 的「目标里已删除的合约自动平掉」逻辑对生产基础数据管理器**恒不生效**（仓位残留）。
- 对照：`WtArbiExecuter.cpp` L482-509 的同一逻辑没有做合约校验，直接 `getUnit` —— 两者行为不一致。
- 测试侧处置：`diff_load_data_then_set_position_roundtrip` 清空目标后调用 `set_position(empty)`，但该替身的 `getContract` 前缀查找也无法用 `SHFE.ag.2512` 命中 `ag2512|SHFE`；所以测试会在 `cInfo == NULL` 处跳过，实际没有覆盖 `getUnit` 和归零分支。原文“带前缀匹配才使 L598-616 可达”不成立。
- 期望：与 L141-142 一致先 `extractStdCode`。
- 状态: 待验证（源码与生产键结构支持该缺陷；现有 roundtrip 用例没有到达清零分支）

## BUG-R10-010 【P2】WtArbiExecuter::writeLog 用 strcat 拼固定 2048 缓冲，无长度上限
- 位置：`src/WtCore/WtArbiExecuter.cpp` L352-358：`static thread_local char szBuf[2048]`；`fmtutil::format_to(szBuf, "[{}]", _name)` 之后 `strcat(szBuf, message)`。
- 现象：`message` 长度不受限（ExecuteUnit 的日志、错误回报文案都可能很长），`strcat` 越界写 thread_local 数组 ⇒ 栈/线程局部破坏。对照 `WtDiffExecuter.cpp` L440-445 同一函数没有 `strcat`（直接 `format_to` 整串），说明是遗漏统一。
- 期望：使用动态字符串，或有显式容量参数的格式化 API 并定义截断行为；无容量参数的单步 `format_to` 仍不构成边界保护。
- 状态: 待验证（固定缓冲区无界拼接的静态风险成立，长消息边界未运行验证）

## BUG-R10-011 【P3】WtSelEngine::_terminated 遮蔽基类同名成员
- 位置：`src/WtCore/WtSelEngine.cpp` L33-37（`WtSelEngine` 自己初始化 `_terminated(false)`）与 L45-57 `release()` 置自己的 `_terminated`；`src/WtCore/WtEngine.h` L314 基类另有 `bool _terminated;`（由 `WtEngine::release()` 置位）。
- 现象：两层各有一份同名标志，`WtSelEngine::release()` 先置派生份再调基类份，读取方若拿的是基类指针看到的是另一份；当前 `task_loop` 在基类里读基类份所以暂无实害，但属于典型遮蔽陷阱。
- 状态: 已撤回（派生标志遮蔽当前没有可观察行为影响；详见复核说明）

## BUG-R10-012 【待验证】WtSelEngine::on_bar 空键插入与 `newBar` 可空性合同待核
- 位置：`src/WtCore/WtSelEngine.cpp` L87-90、L102
- 现象：(1) `const SubList& sids = _bar_sub_map[key];` 对未订阅的 key 也会插入空条目；正常 `WtDtMgr` 订阅通知会先登记对应 key，因此目前只证明直接异常调用的行为，不能据此推导生产 map 无界增长。(2) L102 无条件读 `newBar->date/time`；但 `WtDtMgr` 上游也会解引用 bar，当前接口合同未证明 `newBar` 允许为 NULL。
- 期望：先确认合法调用合同；若允许未知 key 或空 bar，再按其语义分别防御并补覆盖。
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）


- 迭代02复核: NOT_BUG。未知 key 由 map operator[] 插入空列表，但正常数据管理器先登记 bar 订阅再派发；目前只证明单个异常调用创建惰性空项，不能推出正常路径无界增长。上游也解引用 bar，未证明 NULL 是合同允许输入。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待验证（正常派发路径下的缺陷主张未成立，空指针合同待确认）
## 2026-10-29 复核结论

以下分类绑定到源码和测试前提；初次复核没有运行测试，后续定向结果记在相应条目。

### BUG-R10-001 — 条件性空指针缺陷，输入合同待核

- 源码：Arbi/Diff 构造函数都没有初始化 `_config`；`getUnit()` 在策略查找前直接 `_config->get("policy")`，随后也不检查 `policy`、默认配置 `cfg` 是否为空。`init(NULL)` 返回失败后仍不能安全调用事件接口；这属于违反 init 生命周期。更现实的失败形态是缺 `policy` 或缺 `default` 的配置被 `init()` 接受，后续自动建单元时解引用空指针。
- 测试前提：R10 启用用例和 R110 的策略配置都带有 `policy.default.name`；未见缺 policy/default 的负例。当前 `R10` 的测试注释不能当作该失败分支已验证。
- 修复方向：构造时把 `_config` 置空，并在 `init()` 验证 policy/default 必需结构；失败时返回 false；`getUnit()` 保留空指针兜底。优先定义无效配置是拒绝初始化还是允许空策略。
- 证据边界：源码空指针链可静态确认；无效配置是否属于外部配置合同尚未在本轮确认，也未运行崩溃用例。
- 状态: 待验证

### BUG-R10-002 — 测试接缝/覆盖范围项，不是产品故障

- 接口：`WtExecuterFactory` 只公开 `loadFactories(path)`，通过插件 ABI 装配 `_factories`，没有进程内 `registerFact`。这与生产插件加载模式一致，不能单凭缺少 setter 判为功能缺陷。
- 现有测试：`r31_exe_fact_pluginload.cpp` 用真实 `libWtExeFact` 覆盖工厂注册和创建；`r110_diff_executer_test.cpp` 用真实 `WtMinImpactExeUnit` 覆盖多段 Diff 与部分 Arbi/Local 路径。R31 对照表明当前 WtExeFact 的 `createArbiExeUnit` 返回空，因此它没有提供可驱动 Arbi 单元主体的工厂。
- 修复/验证方向：不必新增生产 API；若需要验证剩余 Arbi 单元回调，可构建测试专用插件或受限测试 seam。重新核对“~210 行不可达”清单，不能把现有 R110 覆盖重新计入缺口。
- 证据边界：上述测试源存在，但本次没有运行；依赖插件产物的用例可能按注释 `GTEST_SKIP`。未据此宣称完整撮合行为已验收。
- 状态: 已撤回（覆盖范围仍待验证，不作为产品缺陷）

### BUG-R10-003 — 重复条目，补充调用点并入 BUG-R42-002

- 源码：`HftStraBaseCtx` 构造函数未初始化 `_trader`，成员声明也无默认值；`WtRunner` 仅在找到 trader adapter 时调用 `setTrader`，否则只记日志后仍把上下文加入引擎。多处公开 HFT API 在未绑定场景会读/调 `_trader`。`_data_agent == true` 本身不等于 `_trader == NULL`：Runner 对 agent/non-agent 使用相同的 trader 查找与绑定流程；未绑定时成员是未初始化指针，不应描述为稳定空指针。
- 测试前提：R10 HFT 账本用例构造裸上下文并刻意用 `_data_agent=false`；它没有覆盖 `_trader` 调用点。R42-002 已涵盖 `_trader` 漏初始化和无 trader 调用，R13-001 补充 Runner 绑定失败仍返回上下文，R214 有既有复核。
- 修复方向：按 BUG-R42-002 统一初始化 `_trader = NULL`，并在交易 API 使用前统一检查；Runner 是否允许无 trader 建立上下文另按配置合同处理。
- 状态: 已合并（BUG-R42-002；本条的 cancel/getPosition/getUndone 调用点作为补充证据保留于此复核记录）

### BUG-R10-004 — 到期任务进程终止，本机隔离复现

- 源码：局部 `StdThreadPtr` 在循环体结尾释放其 `std::thread`；无论工作函数届时是否已执行完，未 join/detach 的 `std::thread` 仍是 joinable，析构都会调用 `std::terminate`。原文“可能依赖线程抢先退出”不准确，退出不改变 joinable 状态。
- 测试前提：日任务时间设为下一分钟、非假日，满足创建线程条件；修正后的用例使用 threadsafe 子进程、调用前标记和原子回调计数，要求正常派发并退出，不把 abort 当作成功。
- 修复方向：由引擎持有可 join 的任务执行器/线程池，并在 `release()` 中收尾；只 detach 会把上下文生命周期和引擎关闭顺序留成风险。
- 本机证据：重建 `CoverageTests_wtcore` 后运行 `R10SelEngineTest.minute_end_fires_task_without_terminating_process`，未到期对照未失败；到期子进程输出调用前标记后 signal 6，并显示 libc++abi terminating，测试 FAIL。没有调试器栈，具体终止指令与线程由源码推断；修复后的成功调度和正常关闭尚未验证。测试给回调设置 500 ms 上限，未来通过时仍须结合更完整的生命周期验证。
- 状态: 本机子进程复现到期后终止；生产场景与完整生命周期未验证

### BUG-R10-005 — 引用泄漏重复；插件符号守卫仍待验证

- 重复项：`WTSOrderInfo::retain()` 后 lambda 没有 `release()`，与 BUG-R41-003、BUG-R160-001 是同一文件/同一行/同一缺陷，合并到 BUG-R41-003；R66/R160 的发布测试各发有限订单数，覆盖发布体不证明长期对象回收。
- 独立项：`regiter_callbacks` 符号查找结果直接调用，缺该符号的已加载模块可导致空函数指针调用。当前产品 `WtMsgQue` 导出该拼写；没有证据表明旧插件 ABI 允许只导出 creator。`_publisher` 和 `_remover` 缺失分别会被判空，不应并入同一崩溃主张。
- 修复方向：发布 lambda 用作用域守卫确保 `release()`（含异常/取消路径）；初始化时一次校验必需导出符号并在不兼容时释放库并失败。
- 状态（引用泄漏）: 已合并（BUG-R41-003）
- 状态（符号缺失）: 待验证（需要确认插件 ABI 版本合同并构造缺 `regiter_callbacks` 的模块）

### BUG-R10-006 — 集成覆盖条件成立；“系统内不可达”说法不成立

- 接口与测试：正常路径通过公开 `init()` 加载 MQ 插件、获得 `_mq_sid` 并启动 worker，之后公开 `notify*` 入队；`_asyncio` 私有只限制同步单测排空，不代表生产路径不可调用。现有 `r66_event_notifier_test.cpp`、`r160_event_notifier_test.cpp` 经插件与线程驱动发布体；R160 在 init 前保留 work guard，明确绕过了生产代码里的局部 work guard 问题。
- 分类：R10 的“无线程/无 MQ 插件”限制确实形成 lane 级覆盖边界，但本条不是产品缺陷，也不能据此把全文件 340 行算作全仓不可达。R160 条件用例可能因缺插件而 skip，本轮没有执行结果。
- 关联：生产 worker 的局部 `work_guard` 在 `init()` 返回时析构，导致首次无任务的 `run_one()` 可能把 io_context 置为 stopped；这一实际行为已在其他记录中以 BUG-R120-001 跟踪，应沿该条接受，不与本测试接缝混为一谈。
- 建议：保留为 lane 级覆盖说明；无须仅为此增加产品 getter。运行时验收需记录 R160 插件可用/skip 状态及 work guard 修正后的发布证据。
- 状态: 待验证（非产品缺陷；现有集成路径未在本轮运行）

### BUG-R10-007 — 读接口缺少公开注入口成立；“全路径不可覆盖”已过时

- 接口：`WtDtMgr::_reader` 私有，公开只有 getter；正常生产装配通过 `initStore()` 动态加载 WtDataStorage，这是设计路径，不是运行故障。
- 现有测试：`r160_dtmgr_test.cpp` 通过短范围 `#define private public` 注入 `R160FakeReader` 并覆盖后复权 tick/缓存/kline 分支；`r73_dtmgr_test.cpp` 通过 reader 插件覆盖委托、复权和 bar 通知。原“约 230 行整体不可达”不再适用于现有测试树。用例可能依插件构建布局运行；本轮未执行。
- 修复方向：不要求增加生产 setter；如目标是无插件确定性测试，可沿用 fake reader seam 或抽取纯计算帮助函数。按实际尚未覆盖行重算差额。
- 状态: 已撤回（系统级全不可达/全缺覆盖的结论已被现有测试路径否定；本轮不声称测试通过）

### BUG-R10-008 — 函数内静态缓存问题成立，主消费链被另一缺陷遮蔽

- 源码：局部 `static flag` 第一次按某个 `_reader` 取值，此后所有 `WtDtMgr` 实例共享。不同 reader 返回不同调整模式时，后建实例会沿用首个实例的值；无 reader 首次调用还会永久锁成 0。
- 接口/消费边界：`IDataReader::getAdjustingFlag()` 属实例接口，`WtDtMgr` 也公开实现了 `IDataManager::get_adjusting_flag()`；但当前 `WtEngine::get_adjusting_flag()` 调用数据管理器后丢弃返回值，相关引擎行为已有 BUG-P02-003 / BUG-R192-001。修复该缺陷前，不应把当前策略复权错误归因于 R10-008。
- 测试前提：R10 只在 reader 为空时断言 flag 0，未用两个返回不同 flag 的 reader 检验隔离性。建议加双实例单测；本轮未运行。
- 修复方向：按实例缓存，或直接从各自 `_reader` 读取；空 reader 返回 0。
- 状态: 待验证（源码共享状态明确；引擎可观察影响需与 BUG-P02-003 联合验收）

### BUG-R10-009 — 目标清零查询键错误成立；现有测试未到达目标分支

- 源码/接口：`IBaseDataMgr::getContract(code, exchg="")` 的生产 `WTSBaseDataMgr` 空交易所分支查 `_code_map` 原始/别名代码；建表键来自 `WTSContractInfo::getCode()`（如 `ag2512`），标准代码 `SHFE.ag.2512` 不命中。Diff 文件内其他 lookup 已用 `extractStdCode` 后传 `_code/_exchg`。这使有效、仍在基础数据中的旧目标也被误当成未知合约并跳过。
- 测试前提：`diff_load_data_then_set_position_roundtrip` 先加载旧 targets 再清空 targets，但替身同样无法把标准码映射到 `ag2512|SHFE`；它在 `cInfo == NULL` 处早退，没有覆盖 L610 后的自动归零，也没有证明订单/账本往返通过了该行为。
- 修复方向：拆出 stdCode 的 raw code/exchange 后查询；构造生产键语义的替身，断言仍有效的旧 target 被归零，再单测已失效合约的预期政策。
- 证据边界：键结构与 lookup 对照已静态确认；用例没有运行，测试注释所称的路径可达条件有误。
- 状态: 待验证

### BUG-R10-010 — 固定缓冲区拼接越界成立

- 源码/接口：`fmtutil::format_to(char*)` 无容量参数，`strcat` 也无上限；`IExecuter` 日志消息是 C 字符串，调用接口没有 2048 字节约束。`message` 足够长时会越过 thread-local buffer，属于内存越界。
- 测试前提：已有执行器测试只写短日志，没有边界长度输入。本轮未运行。
- 修复方向：改成动态字符串格式化，或有明确容量参数的有界格式化并显式处理截断；简单换成同一个无界 `fmtutil::format_to` 单步格式化仍不能证明安全。
- 状态: 待验证（静态越界链成立，长消息用例未执行）

### BUG-R10-011 — 派生标志遮蔽目前是误报

- 源码：`WtSelEngine` 的派生 `_terminated` 只在构造和派生 `release()` 中写；线程循环在 `WtEngine` 作用域读取基类自己的成员，随后 `WtEngine::release()` 也会置基类标志。当前没有派生侧读取造成分叉状态的路径。
- 结论：这是容易误读的命名/冗余字段，但现有调用顺序没有行为错误，不作为缺陷登记。
- 状态: 已撤回

### BUG-R10-012 — 正常通知合同下的缺陷主张未成立，空指针合同待验证

- 源码/接口：`WtSelEngine::on_bar` 的普通生产调用来自 `WtDtMgr` 的订阅通知队列；策略 `sub_bar` 会先登记相同 `(code,period,times)` key，因此 `operator[]` 未命中属于未订阅/直接异常调用，现有证据不足以推导生产 map 无界增长。`WtDtMgr::on_bar` 及 forced-cache 路径会直接读 `newBar`，接口也未发现允许 NULL 的说明；源头合同仍需确认。
- 测试前提：`on_bar_without_subscription_only_logs` 用非空 bar 直接调用，是为了覆盖日志路径；它不证明生产会派发未知 key 或空 bar。
- 修复方向：先确认 reader/bar callback 的 nullability 与取消订阅流程；若允许空 bar，再在消息边界防御；仅在合法未订阅事件能进入时改用 `find` 并评估 map 增长。
- 状态: 待验证（当前材料不足以确认生产缺陷；不将其当已证实误报）

---

## 复核前人结论（规则：引用前先自验）
- R3 修正过的「风控频率限流键空间不一致可达」结论，本 lane 未再触碰该面（不在本轮 6 个文件内）。
- 本 lane 对 `WtArbiExecuter::on_position` 的自动清理链路（L821-889）先自行验证过：`_auto_clear` 来自 `clear.active`（L101）、`_clear_includes/excludes` 支持 string 与 array 两种配置形态（L105-124）、`_stub->get_hot_mon()` 可经 `IExecCommand::setStub` 注入 ⇒ 判定矩阵 5 条全部可达且已启用，**不属于**不可达面。
- `p01_cta_env.h`/`p11_trader_fixture.h` 确认为可复用头文件；`p11_trader_fixture.h` 本轮未接入（`TraderAdapter::buy` 之后的回报推进需要连带读 TraderAdapter 语义，预算不足，登记为 BUG-R10-003 的测试缺口而非「不可达」）。
