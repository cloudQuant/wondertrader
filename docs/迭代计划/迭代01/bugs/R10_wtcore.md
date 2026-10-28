# R10 WtCore 缺陷登记（lane R10，只登记不改产品代码）

范围：`HftStraBaseCtx.cpp` `EventNotifier.cpp` `WtDtMgr.cpp` `WtArbiExecuter.cpp` `WtSelEngine.cpp` `WtDiffExecuter.cpp`。
分级：P0 崩溃/进程终止 · P1 功能错误 · P2 健壮性/资源 · P3 一致性/可读性 · 需接缝（可达性阻塞，非缺陷但挡覆盖率）。
所有行号取自本 lane 实读的源码；未修改任何产品代码，未放宽任何断言。

---

## BUG-R10-001 【P0】WtArbi/WtDiff 执行器的 `_config` 未初始化 + policy 链无判空
- 位置：`src/WtCore/WtDiffExecuter.cpp` L41-50（构造函数初始化列表缺 `_config`）、L243-250（`_config->get("policy")` → `policy->has()` → `policy->get(des)` → `cfg->getCString("name")`）；`src/WtCore/WtArbiExecuter.cpp` L33-42、L163-171、L183-185
- 触发条件：任何人在 `init()` 之前（或 `init(NULL)` 之后，见 L84-88/L74-77 的早退分支）调用 `on_tick/on_trade/on_order/on_entrust/on_position_changed/set_position/on_*` —— 这些入口第一个动作就是 `getUnit()`，而 `getUnit` 无条件解引用 `_config`（未初始化的指针值）。第二种触发：`policy` 子节点缺失，或 commID 与 `"default"` 都不在 policy 里 ⇒ `policy->get("default")` 返回 NULL 后 `cfg->getCString` 崩溃。
- 期望：`_config` 应在构造列表里置 NULL，`getUnit()` 开头对 `_config==NULL || policy==NULL || cfg==NULL` 三个形态返回空 `ExecuteUnitPtr`。
- 测试侧处置：本 lane 所有启用用例都先 `init()` 且 policy 必带 `default.name`；未构造上述形态（会崩进程），因此该路径**无用例覆盖**。
- 状态: 待审核

## BUG-R10-002 【需接缝】WtExecuterFactory 无进程内工厂注入口 ⇒ 两个执行器的撮合主体不可达
- 位置：`src/WtCore/WtExecuterFactory.h` L77-182（`_factories` 私有、只有 `loadFactories(path)` 扫目录 dylib 一条填充路径、无 `registerFactory`/无虚函数）；受影响 `WtArbiExecuter.cpp` L186-196、L452-510、L555-567、L601-602、L622-633、L655-666、L686-698、L711-728、L742-758、L783-800、L874-888；`WtDiffExecuter.cpp` L261-285、L508-545、L564-595、L617-639、L657-668、L690-708、L727-738、L754-765、L782-813、L833-843、L871-881
- 现象：`create*ExeUnit()` 在未加载插件时恒返回空（`_factories.find` 命中不到，行为正确），于是 `getUnit()` 恒空，两个执行器里「价差/多腿撮合、价量约束、部分成交推进、超时撤单」全部落在 `if (unit == NULL) return;` 之后。`ExeUnitWrapper`/`IExecuterFact` 本身都是公开可继承的，但没有任何地方能把替身塞进 `_factories`。
- 估计阻塞行数：arbi ~90、diff ~120（另有线程池分支 ~70，规则 2 禁止起线程）。
- 建议接缝（择一）：`WtExecuterFactory` 增加 `bool registerFact(IExecuterFact*, const char* name)`；或 `Wt*Executer` 暴露 `setUnitForTest(stdCode, ExecuteUnitPtr)` 直接写 `_unit_map`。
- 状态: 待审核

## BUG-R10-003 【P2】HftStraBaseCtx 对 `_trader` 的判空策略前后不一致，可空指针解引用
- 位置：`src/WtCore/HftStraBaseCtx.cpp`：判空的有 L228（`if (_trader && !_trader->checkOrderLimits(...))`）、L292、L312；**不判空**的有 L190（`stra_cancel(uint32_t)` 直接 `return _trader->cancel(localid)`）、L196/L199（`stra_cancel(code,isBuy,qty)`）、L234（ruletag 命中分支里 `_trader->buy` 在 `_trader` 判空之后仍被无条件调用）、L249（else 分支 `if (!_trader->checkOrderLimits(...))` 无判空）、L255、L641、L645、L659、L683（`stra_get_position`/`stra_get_undone`）。
- 连带后果：`on_trade` L544-548 在 `_data_agent==true` 时会先调 `stra_get_position(stdCode)` ⇒ 未挂通道的 agent 模式高频 ctx 收到成交回报即崩（本 lane 的 `do_set_position` 主用例因此只能用 `_data_agent=false` 驱动）。
- 期望：与 L228/L292/L312 统一——`_trader` 为空时记 error 并返回空 `OrderIDs()`/0.0。
- 状态: 待审核

## BUG-R10-004 【P0】WtSelEngine::on_minute_end 的任务派发线程在局部作用域结束时 joinable ⇒ std::terminate
- 位置：`src/WtCore/WtSelEngine.cpp` L330-333
  ```cpp
  StdThreadPtr thrd(new StdThread([ctx, curDate, curTime, nextTime](){ ... }));
  ```
  `StdThread` = `std::thread`（`src/Share/StdUtils.hpp` L23-24），`thrd` 是 for 循环体内的局部对象；没有 `detach()`、没有 `join()`，迭代结束即析构 ⇒ 按标准「joinable 的 std::thread 析构调用 std::terminate」，整个进程 abort。
- 触发条件：任一注册任务到期（`_time == getNextMinute(curTime,1)`、当分钟未执行过、非假日、period 判定 `bIgnore==false`，如 TPT_Daily）。选股组合每分钟都可能命中 ⇒ 生产上应为必现崩溃（或依赖线程抢先退出，属未定义行为）。
- 测试侧处置：整条用例 `R10SelEngineTest.DISABLED_minute_end_fires_task_thread_terminates_process` 已 DISABLED；其余 `on_minute_end` 用例刻意选择**必然 bIgnore** 的 period/day 组合，只覆盖 L224-326 的判定链。
- 期望：改为成员级线程池/单次 tick 定时器，或至少 `thrd->detach()`（并处理 ctx 生命周期）。
- 状态: 待审核

## BUG-R10-005 【P1】EventNotifier：委托回报引用计数泄漏 + `_register` 符号未判空
- 位置：`src/WtCore/EventNotifier.cpp` L214-230（`notify(..., WTSOrderInfo*)`）：L221 `ordInfo->retain()` 后，L222-227 的 lambda 里只调 `orderToJson` 与 `_publisher`，**没有 `ordInfo->release()`**；对照成交分支 L204 `retain()` ↔ L210 `release()` 成对。⇒ 每笔委托回报泄漏一个引用计数，`WTSObject` 永不归零，长时间运行必然单调增长（委托回报量级远大于成交）。
- 同一函数还有第二处：L229-230 多余空行里的 lambda 之后无任何清理，与上面同源。
- 位置：L86-91 —— `destroy_server/publish_message/regiter_callbacks` 三个 `get_symbol` 结果都不判空，紧接着 `_register(on_mq_log)` 直接调用；只导出 `create_server` 的旧插件会以 NULL 函数指针崩溃（L79-84 只校验了 `_creator`）。
- 可达性：两段都在 `_mq_sid != 0` / 插件装载成功之后，本 lane 无法在不加载 WtMsgQue 的前提下用测试证明（见 BUG-R10-006），故只登记。
- 状态: 待审核

## BUG-R10-006 【需接缝】EventNotifier 全部事件体被 `_mq_sid` 与私有 `_asyncio` 双重挡住
- 位置：`src/WtCore/EventNotifier.h` L38-67（`_mq_sid`、`_asyncio`、`_publisher`、`_worker` 全私有）；`EventNotifier.cpp` L58-112（`_mq_sid` 只能由插件导出的 `create_server` 赋值，L94；worker 线程 L98-109 每 2ms 轮询一次 `_asyncio.run_one()`）。
- 现象：9 个公开 `notify*` 的第一行都是 `if (_mq_sid == 0) return;`（L116/145/171/199/216/309/340/371），因此 L118-400 的 9 个 lambda（rapidjson 组包 + `_publisher` 广播，含 `tradeToJson` L232-265、`orderToJson` L267-305）在「不起线程、不建真实 shm/socket」的约束下完全不可达；即便能置 `_mq_sid`，也没有公开手段排空 `_asyncio` 来同步执行 lambda。
- 估计阻塞行数：**~340 行**（该文件 405 行里的绝大部分）。
- 建议接缝：`init` 拆出可注入的 publisher 回调（`std::function` 或 `IMqPublisher`），并把 `_asyncio` 的排空暴露成 `poll()`/`drainForTest()`；`tradeToJson/orderToJson` 降为 `protected` 以便直接断言 JSON 字段。
- 状态: 待审核

## BUG-R10-007 【需接缝】WtDtMgr 的 `_reader` 只能由 WtDataStorage 插件装载，无 setter
- 位置：`src/WtCore/WtDtMgr.h` L239-244（`_reader` 私有，只有 `reader()` getter；`regsiter_loader` 只能喂 `_loader`）；`WtDtMgr.cpp` L50-87 `initStore()` 走 `DLLHelper::load_library` + `createDataReader`。
- 后果：`init(cfg,...)` 在 `cfg->get("store")==NULL` 时只完成 `_engine/_force_cache/_align_by_section` 赋值就返回 false（L89-102），此后 L277-400 五个切片查询、L109-117 的 `_bar_notifies` 派发、L183-217 的 on_bar 重采样、L229-239 的 `_ticks_adjusted`、L287-375 后复权 tick 缓存与 `std::lower_bound` 切片、L426-542 的 K 线重采样缓存全部走 `_reader==NULL` 早退 ⇒ 约 **230 行**不可达（本 lane 已把早退分支逐条打满）。
- 建议接缝：`void regsiter_reader(IDataReader*)`（与 `regsiter_loader` 对称），使回测/单测能挂进程内替身 reader。
- 状态: 待审核

## BUG-R10-008 【P3】WtDtMgr::get_adjusting_flag 用函数内 static 记忆化，跨实例生效
- 位置：`src/WtCore/WtDtMgr.cpp` L263-275
  ```cpp
  static uint32_t flag = UINT_MAX;   // 函数内 static，不是成员
  ```
- 现象：`WtDtMgr` 不是单例（回测引擎与实盘引擎可各持一个，`_reader` 各自的复权模式不同），但复权标志第一次取值后被整个进程缓存 ⇒ 第二个实例永远沿用第一个实例（甚至 `_reader==NULL` 时算出的 0）的模式，静默改变复权口径。
- 测试侧处置：`r10_wtcore_dtmgr.cpp` 的 `adjusting_factor_and_flag_fall_back_without_reader` 只断言 0（本进程内所有 WtDtMgr 都没有 reader，结论与实现一致），未额外放宽。
- 期望：改成成员变量（或按 `_reader` 指针为键的缓存）。
- 状态: 待审核

## BUG-R10-009 【P1】WtDiffExecuter::set_position 的「不在目标里就自动清零」用标准代码去查合约
- 位置：`src/WtCore/WtDiffExecuter.cpp` L598-608
  ```cpp
  WTSContractInfo* cInfo = _bd_mgr->getContract(stdCode);   // stdCode 形如 SHFE.ag.2512
  if (cInfo == NULL) continue;
  ```
- 现象：`IBaseDataMgr::getContract(code, exchg, uDate)` 的入参是**交易所原始代码 + 交易所**（同文件其他地方都先 `CodeHelper::extractStdCode` 再查，例如 L141-142、L160-161），这里直接把标准代码当 code 且 exchg 用默认 `""`。真实 `WtBaseDataMgr` 的键是 `code|exchg`，必然查不到 ⇒ L610-638 的「目标里已删除的合约自动平掉」逻辑对生产基础数据管理器**恒不生效**（仓位残留）。
- 对照：`WtArbiExecuter.cpp` L482-509 的同一逻辑没有做合约校验，直接 `getUnit` —— 两者行为不一致。
- 测试侧处置：`diff_load_data_then_set_position_roundtrip` 用带前缀匹配实现的替身 BaseDataMgr，才使 L598-616 可达；换生产实现即断言失败，故该用例只断言「未产生委托 + 账本原样回写」，未掩盖此缺陷。
- 期望：与 L141-142 一致先 `extractStdCode`。
- 状态: 待审核

## BUG-R10-010 【P2】WtArbiExecuter::writeLog 用 strcat 拼固定 2048 缓冲，无长度上限
- 位置：`src/WtCore/WtArbiExecuter.cpp` L352-358：`static thread_local char szBuf[2048]`；`fmtutil::format_to(szBuf, "[{}]", _name)` 之后 `strcat(szBuf, message)`。
- 现象：`message` 长度不受限（ExecuteUnit 的日志、错误回报文案都可能很长），`strcat` 越界写 thread_local 数组 ⇒ 栈/线程局部破坏。对照 `WtDiffExecuter.cpp` L440-445 同一函数没有 `strcat`（直接 `format_to` 整串），说明是遗漏统一。
- 期望：`fmtutil::format_to(szBuf, "[{}] {}", _name, message)` 单步格式化并截断。
- 状态: 待审核

## BUG-R10-011 【P3】WtSelEngine::_terminated 遮蔽基类同名成员
- 位置：`src/WtCore/WtSelEngine.cpp` L33-37（`WtSelEngine` 自己初始化 `_terminated(false)`）与 L45-57 `release()` 置自己的 `_terminated`；`src/WtCore/WtEngine.h` L314 基类另有 `bool _terminated;`（由 `WtEngine::release()` 置位）。
- 现象：两层各有一份同名标志，`WtSelEngine::release()` 先置派生份再调基类份，读取方若拿的是基类指针看到的是另一份；当前 `task_loop` 在基类里读基类份所以暂无实害，但属于典型遮蔽陷阱。
- 状态: 待审核

## BUG-R10-012 【P3】WtSelEngine::on_bar 用 `operator[]` 组订阅键 + 无条件解引用 newBar
- 位置：`src/WtCore/WtSelEngine.cpp` L87-90、L102
- 现象：(1) `const SubList& sids = _bar_sub_map[key];` 对未订阅的 key 也会**插入空条目**，`_bar_sub_map` 随「见过的 (code,period,times) 组合」单调增长（`on_tick` 侧 L143 用的是 `find`，两处风格不一致，说明这里多半是无心之失）；(2) L102 日志在三元式里无条件读 `newBar->date` / `newBar->time`，`newBar==NULL` 时崩溃——而该日志与订阅分发无关，任何合约的 bar 回调都会走到。
- 期望：用 `find` 判断后再遍历；日志前判空。
- 状态: 待审核

---

## 复核前人结论（规则：引用前先自验）
- R3 修正过的「风控频率限流键空间不一致可达」结论，本 lane 未再触碰该面（不在本轮 6 个文件内）。
- 本 lane 对 `WtArbiExecuter::on_position` 的自动清理链路（L821-889）先自行验证过：`_auto_clear` 来自 `clear.active`（L101）、`_clear_includes/excludes` 支持 string 与 array 两种配置形态（L105-124）、`_stub->get_hot_mon()` 可经 `IExecCommand::setStub` 注入 ⇒ 判定矩阵 5 条全部可达且已启用，**不属于**不可达面。
- `p01_cta_env.h`/`p11_trader_fixture.h` 确认为可复用头文件；`p11_trader_fixture.h` 本轮未接入（`TraderAdapter::buy` 之后的回报推进需要连带读 TraderAdapter 语义，预算不足，登记为 BUG-R10-003 的测试缺口而非「不可达」）。
