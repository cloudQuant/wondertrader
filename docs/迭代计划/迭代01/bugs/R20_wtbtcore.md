# R20 缺陷与接缝台账 — WtBtCore（覆盖率 lane R20）

只记录，不改产品代码。每条 `- 状态: 待审核`。用例落盘 `src/TestUnits/coverage/p08_btcore_mockers3/r20_*.cpp`。

## A. 疑似缺陷

- 标题: EventNotifier 构造函数不初始化 4 个函数指针成员，动态实例 notify* 读野指针（UB）
  - 状态: 待审核
  - 位置: `src/WtBtCore/EventNotifier.h` ctor（仅 `: _mq_sid(0)`），`src/WtBtCore/EventNotifier.cpp` L127/L142/L183（`if (_publisher)`）
  - 现象: `_creator/_remover/_publisher/_register` 是裸函数指针，ctor 未置 NULL、init 的 inactive 早退
    也不写它们。`wtp::EventNotifier n; n.notifyEvent("x");` 在 heap/stack 实例上按野指针判定，
    可能直接调用随机地址；dtor 的 `_remover && _mq_sid != 0` 因 `_mq_sid` 已零初始化而侥幸短路。
  - 影响: 所有「先构造、后按需 init(false)」的调用方（回测主流程 CtaMocker 持有指针故未暴露）
    一旦本地值对象走 notify* 即 UB。
  - 复现: 测试侧只能改用 static 存储期实例（零初始化保证 `_publisher==NULL`），见
    `r20_event_notifier_test.cpp` 文件头说明；本 lane 禁 `#define private public`，无法在动态实例上固化断言。
  - 分级: S2

- 标题: EventNotifier::init 对 cfg==NULL 无守卫，第一行即解引用
  - 状态: 待审核
  - 位置: `src/WtBtCore/EventNotifier.cpp` L69
  - 现象: `if (!cfg->getBoolean("active"))` 直接解引用；对比同族 `init_uft_factory(NULL)` 有早退。
  - 复现: `r20_event_notifier_test.cpp#DISABLED_InitNullConfigDerefsAndCrashes`（崩溃路径整条 DISABLED）。
  - 分级: S3

- 标题: EventNotifier::init 只校验 _creator，_register/_publisher/_remover 缺符号时裸调用/裸赋
  - 状态: 待审核
  - 位置: `src/WtBtCore/EventNotifier.cpp` L102-110
  - 现象: create_server 符号存在但 `regiter_callbacks`（注意源码拼写即为 regiter）缺失时，
    L107 `_register(on_mq_log)` 调用 NULL；`publish_message` 缺失时 `_publisher` 置 NULL 反而安全，
    但 `destroy_server` 缺失 + `_mq_sid!=0` 时 dtor 调 NULL `_remover`。
  - 影响: 版本不匹配插件下的崩溃面；因需要真实动态库+MQ 服务才可达，本轮未实测（init active=true 整支留白）。
  - 分级: S3

- 标题: HisDataMgr::init 在全部失败臂仍无条件 `_reader->init(cfg, this)`，NULL 解引用必崩
  - 状态: 待审核
  - 位置: `src/WtBtCore/HisDataMgr.cpp` L46-71
  - 现象: load_library 失败（L64 else 分支只记 error）或 `createBtDtReader` 符号缺失（L51 只记 error）时，
    `_reader` 保持 NULL，但 L71 不判空直接调用；两条失败臂都应为 `return false` 却没有。
  - 复现: `r20_hisdatamgr_test.cpp#DISABLED_InitCrashesWhenReaderNullAfterLoadFailure`
    （触发条件：module 指向不存在的库；本 lane 禁真实插件加载，整条 DISABLED）。
  - 分级: S2

- 标题: ExecMocker::cancel(localid) 重复撤单二次归还，_undone 可被撤成负值
  - 状态: 待审核
  - 位置: `src/WtBtCore/MatchEngine.cpp`（`MatchEngine::cancel(uint32_t)`：置 `state=9` 后返回 `_left*(±1)`，
    不 erase、不判旧 state）+ `src/WtBtCore/ExecMocker.cpp` L253-265
  - 现象: 同一 localid 第二次 cancel 仍查到该单、仍返回全额 left → `_undone -= change` 二次扣减、
    `_cacl_cnt/_cacl_qty` 二次计数；对比 `stra_cancel` 族在 Hft/Uft 侧有 state 判定。
  - 影响: 撤单统计虚高、undone 记账漂移为负（handle_order 的 isCanceled 臂还会计第三次）。
  - 复现: `r20_exec_mocker_test.cpp#CancelByIdRestoresUndoneAndDoubleCancelQuirk`
    按现行为固化：第一次撤单 undone 3→0，第二次撤单 undone 0→-3。
  - 分级: S3

- 标题: ExecMocker 的事件面假定「已 init 且 _last_tick 非空」，未 init 时为 NULL 解引用链
  - 状态: 待审核
  - 位置: `src/WtBtCore/ExecMocker.cpp` L155/L159/L189（_exec_unit）、L168/L292-293/L311-316/L334（_exec_unit/_last_tick 双裸解引用）、
    `src/WtBtCore/MatchEngine.cpp` fire_orders（`_sink->handle_entrust` 无判空，且 `_sink` 只在 `ExecMocker::init` L57 经 regisSink 注入）
  - 现象: `handle_schedule` 在首个 tick 到达前被调度器触发即解引用 `_last_tick`；
    未调用 init 的 ExecMocker 收到「先 buy 后第二个 handle_tick」走 fire_orders 解引用 NULL `_sink`；
    `init()` 返回 false（模块缺失）时 `_exec_unit` 为 NULL，之后任何成交事件解引用崩溃。
  - 影响: 回放期任意执行器插件缺失/顺序异常 → 进程直接崩，无友好退出。
  - 复现: `r20_exec_mocker_test.cpp#DISABLED_ExecUnitNullDerefFamily`、
    `#DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick`（触发条件已写在用例注释）。
  - 分级: S3（生产主流程先 init 再挂 sink，故仅在异常配置下暴露）

- 标题: ExecMocker::handle_order 的取消臂对 _undone 的符号与 handle_trade 不一致（疑似重复回冲）
  - 状态: 待审核
  - 位置: `src/WtBtCore/ExecMocker.cpp` L307 vs L330
  - 现象: 取消回报走 `leftover`、成交回报走 `vol`，但同一订单先部分成交再撤单时，
    撤单回报的 leftover 与成交时已扣减的 `_undone` 是同一笔 left 的两套口径（fire→trade→cancel 序列下会双扣）；
    且 `_sig_time % 10000 * 100000` 把 HHMM 当 HHMMSS 的分钟域拼秒（`sigUnixTime` 与真实信号时刻差 100 倍量纲），
    trades csv 的 sigtimespan 列因此不可信。
  - 影响: 撤单统计与 sigtimespan 指标失真；因整条 _exec_unit 链无接缝可测（见上一条），本轮未能用启用用例固化。
  - 分级: S4（需业务口径裁定）

- 标题: MatchEngine 的 `_tick_cache` 懒创建后从不释放（每 Mocker 生命周期泄漏一个 WTSTickCache）
  - 状态: 待审核
  - 位置: `src/WtBtCore/MatchEngine.cpp` L425-426（create）；`MatchEngine.h` 无析构体、`clear()` 只清 `_orders`
  - 现象: `WTSTickCache::create()` 之后没有任何 release/delete 路径；ExecMocker/HftMocker 各持一个
    MatchEngine，回测进程内每个 mocker 至少泄漏一个缓存池及其持有的 tick 引用。
  - 影响: 资源泄漏（进程退出即回收，回测单次运行影响有限；长跑守护进程复用时会累积）。
  - 分级: S5（泄漏，非行为错误）

## B. 本 lane 采信的公开接缝（沿用 R2 实测清单）

- `WTSTickData::create(WTSTickStruct&)` 公开池工厂 + 同池 `release()`（ExecMocker::handle_tick 喂真实 tick）。
- `HisDataReplayer` 默认构造即安全：`WTSBaseDataMgr` ctor 创建全部 map（getCommodity 查无返 NULL）、
  `enable_tick(bool)` 公开、`get_date/get_raw_time/get_secs` 读默认零值。
- `MatchEngine::handle_tick` 对 `_tick_cache` 懒创建；`_orders` 为空时不触 `_sink`。
- 未新增线程/sleep/socket/shm/挂钟读取；`handle_replay_done` 只写
  `temp_directory_path()/r20_exec_mocker_<pid>/exec/`，TearDown `remove_all`。
