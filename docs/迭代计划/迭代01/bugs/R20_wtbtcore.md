# R20 缺陷与接缝台账 — WtBtCore（覆盖率 lane R20）

本文件记录 R20 lane 的候选项。本次只读源码、接口与现有测试前提；没有修改生产代码/测试，也没有运行测试或 sanitizer。用例落盘 `src/TestUnits/coverage/p08_btcore_mockers3/r20_*.cpp`，其中 DISABLED/GTEST_SKIP 不能视为动态复现。

## A. 逐项复核

- 标题: EventNotifier 构造函数不初始化 4 个函数指针成员，动态实例 notify* 读野指针（UB）
  - 状态: 源码缺陷已确认；与 `bugs/R131.md` 的引用相同问题，R20 保留主证据，本项仅计一次
  - 位置: `src/WtBtCore/EventNotifier.cpp:41-56,124-129,139-144,181-185`；成员 `src/WtBtCore/EventNotifier.h:113-116`
  - 证据/审核结论: 构造函数只初始化 `_mq_sid`，四个函数指针没有默认成员初始化。自动存储期对象上的 notify* 会读取不确定的 `_publisher`；
    析构函数同样先读取 `_remover`，`_mq_sid==0` 位于 `&&` 第二操作数，不能避免对 `_remover` 的读取。原记录称析构会被 `_mq_sid` 安全短路不成立。
    R20 测试使用 static 存储期对象，静态零初始化使测试指针为 NULL，只覆盖安全空转，不覆盖普通 stack/heap 对象。`bugs/R131.md` 也将该现象列为既有 R20 项，不是独立根因或复现。
  - 修复方向: 在成员声明或构造函数中将四个指针初始化为 `nullptr`；修复后用自动存储期实例覆盖 inactive init、notify* 与析构安全路径。
    当前未运行 sanitizer，不能声称已观察随机调用或崩溃。
  - 分级: S2（不确定函数指针读取/潜在间接调用）

- 标题: EventNotifier::init 对 cfg==NULL 无守卫，第一行即解引用
  - 状态: 已合并（迭代02：主编号 BUG-L06-006）
  - 位置: `src/WtBtCore/EventNotifier.cpp` L69
  - 证据/审核结论: `init(WTSVariant* cfg)` 在 L69 直接 `cfg->getBoolean("active")`，NULL 会被解引用；接口没有非空前置条件说明。
    `WtBtRunner::initEvtNotifier()` 在 `src/WtBtPorter/WtBtRunner.cpp:626-632` 调用前拒绝 NULL 或非 object 配置，正常 runner 路径不会传入 NULL。
    `DISABLED_InitNullConfigDerefsAndCrashes` 是 GTEST_SKIP 占位，未动态运行。原记录只证明直接传 NULL 的行为，不能证明受支持调用路径可达。
  - 修复方向: 维持 runner 入口校验；若 `EventNotifier::init` 对外承诺容忍缺配置，再增加入口检查并测试其返回 false。沿用 BUG-L06-006，不纳入产品缺陷汇总。
  - 分级: 防御性增强候选（契约待确认）


- 迭代02复核: DUPLICATE。同 BUG-L06-006：init(NULL) 源码会解引用，但正常 Runner 先过滤空配置；未找到 init 接受 NULL 的公开契约。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）:   - 状态: 未确认产品缺陷；与 `bugs/l06_wtbtcore.md` BUG-L06-006 同一候选，NULL 支持契约未找到
- 标题: EventNotifier::init 只校验 _creator，_register/_publisher/_remover 缺符号时裸调用/裸赋
  - 状态: `_register` 缺失时的空函数调用源码成立；对 `_remover` 的原崩溃主张撤销
  - 位置: `src/WtBtCore/EventNotifier.cpp` L102-110
  - 证据/审核结论: `create_server` 在 L92-99 有 NULL 检查，之后 `regiter_callbacks`、`publish_message`、`destroy_server` 分别赋给 `_register/_publisher/_remover`；
    L107 不检查 `_register` 就调用，故一个缺少注册导出符号的已加载模块会走空函数指针调用。相反，`destroy_server` 缺失时 `_remover==NULL`，
    析构条件 `_remover && _mq_sid!=0` 会短路，不会调用 NULL；若 `_mq_sid` 有效，则服务器缺少销毁路径而可能泄漏。`_publisher==NULL` 时各 notify 函数按注释空转。
    active=true 整条路径目前只在 DISABLED/GTEST_SKIP 占位，未验证实际模块 ABI 或 socket 行为。
  - 修复方向: 在创建 server 前校验契约规定的必需符号；任何必需符号缺失都释放模块并返回 false。明确 publisher 是否可选；若 remover 必需，避免创建后才发现无法释放，
    并为失败清理定义一致策略。为缺符号模块构造无 socket 的装载替身后再验证。
  - 分级: S3（仅当外部模块缺失契约必需导出时）

- 标题: HisDataMgr::init 在全部失败臂仍无条件 `_reader->init(cfg, this)`，NULL 解引用必崩
  - 状态: 已合并（主记录 BUG-N04-002；同一 HisDataMgr reader 初始化失败路径）
  - 位置: `src/WtBtCore/HisDataMgr.cpp` L46-71
  - 证据/审核结论: `HisDataMgr` 构造函数将 `_reader` 置 NULL（`HisDataMgr.h` ctor）；L46 load 失败时不赋值，或者库已加载但 L50-60 找不到/未创建
    reader 时仍保持 NULL，L71 无守卫解引用。布尔接口没有在失败臂返回 false，而是先进入未定义行为。`DISABLED_InitCrashesWhenReaderNullAfterLoadFailure` 是跳过占位，未动态运行。
  - 修复方向: 每个库加载/导出符号/reader 工厂失败臂都清理已取得资源并返回 false；创建器返回 NULL 时也要拒绝初始化；仅 `_reader` 有效后调用 init。
    `IBtDtReader::init` 当前返回 `void`（`src/Includes/IBtDtReader.h:43`），不能从该接口传递 reader 初始化失败结果；如需传播失败，需先调整接口契约和所有实现，再由 `HisDataMgr::init` 返回该状态。
  - 分级: S2（配置/插件加载失败路径；R20 不重复计数）

- 标题: ExecMocker::cancel(localid) 重复撤单二次归还，_undone 可被撤成负值
  - 状态: 重复调用的源码行为已确认；撤单 API 是否承诺幂等/拒绝终态订单需明确
  - 位置: `src/WtBtCore/MatchEngine.cpp`（`MatchEngine::cancel(uint32_t)`：置 `state=9` 后返回 `_left*(±1)`，
    不 erase、不判旧 state）+ `src/WtBtCore/ExecMocker.cpp` L253-265
  - 证据/审核结论: `MatchEngine::cancel(localid)` 只检查订单是否存在，随后写 state=9 并返回 `_left`；在订单被 `match_orders` 收尾前再次调用，仍会返回同一 left。
    `ExecMocker::cancel` 每次非零都再次扣 `_undone` 并累计撤单数/量，故旧用例中 3→0→-3 的预期有源码支持，但旧预期不是本轮实际运行证据。原“handle_order 再扣第三次”不成立：
    取消订单下一 tick 经 `match_orders` 派发 `handle_order(... leftover=0, isCanceled=true)`（`MatchEngine.cpp:84-91`），取消臂减去的也是 0。
    旧 `CancelByIdRestoresUndoneAndDoubleCancelQuirk` 固化现状，现改为 `DISABLED_CancelByIdDoesNotRestoreUndoneTwice`：首撤后再次撤同一单不得再次减少未完成量，不强加第二次 bool 返回值。重建后测试在首次 `handle_tick` 就 signal 11，未到 buy/cancel；同夹具 tick 控制也 signal 11，无 tick 控制 PASS，暂保持禁用。诊断回溯映射至 `ExecMocker::handle_tick` 调 matcher 的调用点，缺少更深因果栈；此外 `_exec_unit` 构造未初始化。不能将此次崩溃归因重复撤单，也不能将该正确断言作为动态复现。公共接口未见明确重复撤单返回值约定。
    另独立运行仓内采用真实 `WtExeFact` 初始化的 `R111ExecMocker.InitWithRealExecuterAndInitSchedule` 亦 signal 11；尚无因果栈，不能用它证明初始化或定位 R20 崩溃。后续应先诊断该夹具与真实插件 ABI/生命周期，再验证撤单回归。
  - 修复方向: 明确终态语义；若重复撤单应失败，在 MatchEngine 中拒绝 state=9/终态订单并返回 0，使 ExecMocker 不重复更改账本/统计。先建立已正确初始化且安全的 tick/执行单元夹具，再验证第二次 `_undone` 保持 0；如接口明确拒绝重复撤单，再验证返回 false，
    并覆盖“部分成交后撤剩余量”使成交量与剩余撤单量合计恰好清零。
  - 分级: S3（账本负值可由当前 API 重复调用产生；最终产品裁定需确认契约）

- 标题: ExecMocker 的事件面假定「已 init 且 _last_tick 非空」，未 init 时为 NULL 解引用链
  - 状态: 已合并（迭代02：主编号 BUG-N04-001）
  - 位置: `ExecMocker.cpp:28-85,122-190,285-335`、`ExecMocker.h:301`；调用链 `WtBtRunner.cpp:503-505`、`HisDataReplayer.cpp:649-670`；sink `MatchEngine.cpp:51-62`
  - 证据/审核结论: 构造函数把 `_last_tick` 置 NULL，但没有初始化 `_exec_unit`（头文件 L301 也无默认值）。`init()` 仅在 `cfgExec` 存在时才写 `_exec_unit`（L77-83），
    无 `executer` 子项时仍返回 true；模块加载/符号失败时返回 false。`WtBtRunner` 忽略该返回值并仍注册 sink，随后 `prepare()` 调用 `handle_init()`，其 L155 解引用 `_exec_unit`。
    因此正常 runner 在无效 module 配置下有源码支持的失败链，但 `_exec_unit` 是不确定值，不可断言“必为 NULL”或保证固定崩溃；无 `executer` 子项还会以 true 返回保留同一未初始化风险。
    `handle_tick` 也会读取 `_exec_unit`；`handle_schedule` 在 L168 先读 `_last_tick`，调用早于首 tick 时有 NULL 解引用，但调度是否允许早于 tick 尚无契约证据。
    `_sink==NULL` 的测试情形要求从未调用 `init()` 却先提交订单再喂 tick；这不是 `WtBtRunner` 的常规路径。`init()` 自身在加载 module 前就执行 `regisSink(this)`，所以失败时 sink 实际已设置，随后危险点是 `_exec_unit` 而非空 `_sink`。
  - 测试边界: `DISABLED_ExecUnitNullDerefFamily` 与 `DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick` 均 GTEST_SKIP；前者把未初始化指针写成 NULL，不能作为确定的运行时复现；后者以未 init 使用上下文为前提，未证明受支持生命周期内可达。另有 `TickSwapReleasesPreviousTick`、`BuyAfterTickAccumulatesUndone`、`SellDecrementsUndone`、`CancelByCodeSkipsUnfiredOrders`、`SessionEndClearsMatcherAndUndone` 五条也在未成功初始化下调用 `handle_tick`，现保留原测试体并改为 DISABLED；先修复产品初始化并提供安全执行单元夹具才能验收。未运行这些禁用路径。
  - 修复方向: 沿用 BUG-N04-001：在构造函数把 `_exec_unit` 明确置 NULL；要求 `executer` 配置及执行器实例有效后才令 init 成功；`WtBtRunner` 必须检查 init 结果，失败时不得注册 sink/进入 `prepare()`。
    对 `handle_schedule` 明确 tick 尚未到达时的行为并作守卫；所有插件回调仅在执行器 ready 后派发。测试优先验证失败配置可被上层拒绝，而非依赖未初始化指针必崩。
    `createExeUnit()` 返回 NULL 后在 `init()` 内解引用是邻近的 BUG-R121-002，需独立处理，不能并入本项的生产失败链。
  - 分级: S2（失败配置继续进入回放的生产调用链；R20 不重复计数）


- 迭代02复核: DUPLICATE。ExecMocker 初始化失败仍登记/进入回调，与 BUG-N04-001 同一调用链。R20 disabled 测试也用未初始化对象写 NULL 前置，不能单独证明。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）:   - 状态: 产品缺陷；与 `bugs/n04_btcore_mockers.md` BUG-N04-001、`bugs/R36_rerun_20261028.md` BUG-R36-002 同根重复，N04 保留主记录
- 标题: ExecMocker::handle_order 的取消臂对 _undone 的符号与 handle_trade 不一致（疑似重复回冲）
  - 状态: 已撤回（当前源码不支持符号不一致或重复回冲结论；原缺陷主张不成立）
  - 位置: `src/WtBtCore/ExecMocker.cpp` L307 vs L330
  - 证据/审核结论: 取消臂 `_undone -= leftover * sign` 与成交臂 `_undone -= vol * sign` 使用相同符号。一个买单先成交 `vol` 后撤销剩余 `leftover` 时，
    两项分别释放已成交量和剩余量；`match_orders` 的终态回调给 leftover=0，不形成第三次扣减。`_sig_time` 在 `handle_init/handle_schedule` 以 `date*10000+HHMM` 保存；
    `TimeUtils::makeTime` 的时间参数契约是 HHMMSSsss（`src/Share/TimeUtils.hpp:240-260`），`HHMM*100000` 正好表示该分钟的 `SSmmm=00000`，并非 100 倍错误。
    schedule 输入本身为分钟精度；没有证据要求 `sigtimespan` 精确到秒/毫秒。
  - 修复/验证方向: 撤回本候选，不修改两处符号或时间换算。若接口将来要求亚分钟 schedule 精度，应先扩展 schedule 时间契约，再调整 `_sig_time` 编码和对应测试。

- 标题: MatchEngine 的 `_tick_cache` 懒创建后从不释放（每 Mocker 生命周期泄漏一个 WTSTickCache）
  - 状态: 资源生命周期缺陷已由源码确认；累计规模未运行测量
  - 位置: `src/WtBtCore/MatchEngine.cpp:39-42,425-428`；成员/构造 `src/WtBtCore/MatchEngine.h:96-106,301`；容器 `clear/release` `src/Includes/WTSCollection.hpp:718-744`
  - 证据/审核结论: `MatchEngine` 构造时 `_tick_cache=NULL`，首个非空 tick 创建 `WTSTickCache::create()`；`add(..., true)` 保留 tick 引用。
    `clear()` 只清 `_orders`，类没有析构函数或其他 `_tick_cache->release()` 调用。`WTSTickCache` 是 `WTSHashMap<std::string>`，其 `release()` 会清 map、释放其中对象并删除容器；
    默认析构 `MatchEngine` 只丢弃裸指针，因此每个至少接收过 tick 的实例泄漏 cache 以及当前按合约缓存的 tick 引用。没有做进程内存增长测量。
  - 修复方向: 为 `MatchEngine` 提供析构/RAII 所有权，在析构时对非空 `_tick_cache` 调用 `release()`；同时明确复制/移动语义，避免多个实例重复释放同一 raw pointer。回归应覆盖多合约缓存清理及引用计数归还。
  - 分级: S5（对象生命周期泄漏）

## B. 本 lane 采信的公开接缝（沿用 R2 源码与现有用例核对清单）

- `WTSTickData::create(WTSTickStruct&)` 公开池工厂 + 同池 `release()` 可构造 tick；`ExecMocker::handle_tick` 还需要成功初始化的执行单元，不能仅凭 tick 工厂判定夹具安全。
- `HisDataReplayer` 默认构造即安全：`WTSBaseDataMgr` ctor 创建全部 map（getCommodity 查无返 NULL）、
  `enable_tick(bool)` 公开、`get_date/get_raw_time/get_secs` 读默认零值。
- `MatchEngine::handle_tick` 对 `_tick_cache` 懒创建；`_orders` 为空时不触 `_sink`。
- 未新增线程/sleep/socket/shm/挂钟读取；`handle_replay_done` 只写
  `temp_directory_path()/r20_exec_mocker_<pid>/exec/`，TearDown `remove_all`。
