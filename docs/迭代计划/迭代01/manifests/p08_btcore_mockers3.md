# P08 WtBtCore 模拟交易第三轮 交付清单（迭代 01 第四波）

- Lane: p08，靶子 `src/WtBtCore/UftMocker.cpp`（983 行未覆盖）与 `src/WtBtCore/HftMocker.cpp`（808 行未覆盖）
- 产出目录: `src/TestUnits/coverage/p08_btcore_mockers3/`，映射目标 `CoverageTests_btcore`（只引用 `WtBtCore/`、`Includes/`、`Share/`、`WTSTools`，**未引用 WtCore/WtUftCore/WtDtCore 任何类**，未改 `CMakeLists.txt`）
- 语法门禁: **3/3 文件通过**，0 error 0 warning（Apple clang macOS arm64，命令见文末）
- 用例总数: 38（35 启用 + 3 `DISABLED_`，崩溃/阻塞类输入按同类整族收纳）
- 生产代码: 未修改；`src/**` 除 `src/TestUnits/coverage/p08_btcore_mockers3/` 外只读
- 落盘: 仅 `temp_directory_path()/p08_uft_order_position`、`p08_hft_order_cancel`、`p08_uft_dispatch_t1` 三个唯一目录，TearDown `remove_all`；`dist/**` 未读未写
- Bug 记录: `docs/迭代计划/迭代01/bugs/p08_btcore_mockers3.md`，**7 条**，状态均「待审核」，未修源码

## 关键前提修正（对 p03「UftMocker 需线程/hook」结论的推翻）

p03 认为 `UftMocker.cpp` 的成交回写依赖线程与 hook 而未做。实测源码：
`UftMocker::postTask()`（`UftMocker.cpp:91-128`）里**线程分支整段被注释掉**，只做 `_tasks.push()`；
`procTask()`（L67-89）同步排空队列。因此 `stra_buy/stra_sell/stra_enter_*/stra_exit_*/stra_cancel`
→ postTask → procTask → procOrder → on_trade → update_position 整条链**完全单线程可驱动**，
不需要行情流线程、不需要 `install_hook`、不触碰任何条件变量。本 lane 据此切入。

仍需回避的阻塞路径（`HftMocker`）：`step_tick()`（`HftMocker.cpp:328-343` 的 `while(!_resumed) notify_all()` 忙等）、
`on_tick()`/`handle_tick()` 在 `_has_hook && _hook_valid` 时的 `_cond_calc.wait()`（L361-368、L410-417、L424-429）。
全部收进 `DISABLED_BlockingHookAndNullDerefFamily`；`_has_hook` 默认 false，故未挂 hook 时 `on_tick` 本身不阻塞（但需真实 tick 指针，本 lane 未驱动）。

## 新增文件与覆盖符号

### 1. `p08_uft_order_position_test.cpp`（14 启用 + 1 DISABLED）
`UftMocker.cpp` 下单—委托—成交—持仓—资金主链（约 550 行主体逻辑）：
- 空状态与零值查询：`_orders/_pos_map/_tasks/_tick_subs/_user_datas` 全空、`stra_get_undone/position` 零值、`id()` 透传、`_use_newpx/_error_rate/_match_this_tick` 默认值
- 参数校验族：`stra_enter_long/short`(L597/L638) 与 `stra_buy/stra_sell`(L472/L534) 的未知合约、`qty<=0` 早退
- 订单装配：`OrderInfo` 字段（`_localid/_code/_isLong/_offset/_price/_total/_left/_proced_after_placed`）逐字段断言
- 任务队列：`postTask` 只入队、`procTask` 同步排空、空队列重入安全
- 撮合早退分支：`procOrder`(L889-970) 无最新 tick → false 且首轮回写置 `_proced_after_placed`；未知 localid → false
- 撤单：`stra_cancel(localid)`(L407) 的可用量归还（CM_CoverToday 与非 CoverToday 两分支结果一致）与订单摘除、重复撤单；`stra_cancel_all`(L456) 遍历过滤
- 先平后开：`stra_buy` 消耗空头 2 手 + 开多 3 手的订单数量/方向/offset 与净挂单量
- 成交回写 `on_trade → update_position`(L1155-1321)：offset==0（今仓+明细+开仓费+成交日志）、offset==1（盈亏按 volScale、当日平今费档、平仓日志）、offset==2（平今只动今仓、浮盈等比缩放、0 量明细残留）、空头盈亏取反
- 查询矩阵：`stra_get_position`(iFlag 1/2/3 × bOnlyValid)、`stra_get_local_position`、`stra_get_undone` 方向累加、`stra_get_comminfo`、`stra_get_date/time/secs`
- `update_dyn_profit`(L828) 的多头/空头 volume()==0 归零分支与未注册合约跳过（NULL tick 安全）
- `dump_outputs`(L1112) + `log_trade`(L1141) + `log_close`(L1147)：四份 csv 表头与内容断言
- `DISABLED_NullStrategyAndNullCommDerefFamily`：BUG-P08-001/002/006 全体

### 2. `p08_hft_order_cancel_test.cpp`（12 启用 + 1 DISABLED）
`HftMocker.cpp` 剩余缺口（订单装配、撤单、成交回写、未初始化查询）：
- 构造后空状态：`_strategy/_has_hook/_hook_valid/_resumed` 初值 + 8 个零值查询（`stra_get_position/avgpx/profit/undone/price/rawcode/comminfo/date/time/secs`，L841-960）
- 拒单守卫：`stra_buy/stra_sell`(L597/L789) 未知合约、`qty<=0`；`canShort()==false` 品种的无持仓卖出拒绝分支
- 订单装配：`OrderInfo` 的 `_code/_usertag/_isBuy/_price/_total/_left/_localid` 与 `strcpy` 落位（L614-626、L817-829）
- 延迟撤单：`stra_cancel(localid)`(L~500-572) 任务化 → procTask 后才摘单；未知/重复撤单
- 按合约+方向撤单与按量撤单：`stra_cancel(stdCode,isBuy,qty)`(L574-595) 的 `left<qty` 累减/`else break` 两条分支
- 成交回写：`on_trade`(L644) → `do_set_position` 的加仓/加权均价 3808/部分减仓/全平归零，`DetailInfo._price/_volume/_opentdate`
- 空头净持仓与 `PosInfo::valid()`(HftMocker.h:1131) 的 `volume-frozen` 语义（含负持仓下 frozen 使 valid 更负的可观测行为）
- `update_dyn_profit`(L665) 零持仓归零分支
- 回调注册与日志：`stra_log_info/debug/warn/error`(L989-1006)、`enable_hook`(L314) 开关回退
- 事件分发：`on_bar/handle_bar_close/on_init/handle_init/on_channel_ready/on_entrust/on_order/on_tick_updated/on_ordque_updated/on_orddtl_updated/on_trans_updated/handle_schedule`（L308-430、L432-470）
- `dump_outputs`(L~1020) 产物扫描断言（不假设具体文件名）
- `DISABLED_BlockingHookAndNullDerefFamily`：`init_hft_factory(NULL)`、`install_hook`+`step_tick`+带 hook 的 `on_tick`、有持仓时 NULL tick、四个 `stra_sub_*` 订阅（BUG-P08-007 的 qty=0 契约已放在启用用例中固化）

### 3. `p08_uft_dispatch_t1_test.cpp`（9 启用 + 1 DISABLED）
`UftMocker.cpp` 的配置解析、事件分发与 T+1 冻结分支（与文件 1 不重叠）：
- `init_uft_factory(NULL)`(L130-133) 早退 false 且不改动 `_use_newpx/_error_rate/_match_this_tick/_factory`
- IDataSink/IUftStraCtx 转发（无策略安全空转）：`handle_bar_close/on_bar/handle_init/on_init/on_channel_ready/handle_schedule/on_tick_updated/on_ord*_updated/on_trans_updated/on_order/on_entrust`(L189-341、L801-826)
- 逐笔/盘口分发：`on_order_queue/on_order_detail/on_transaction` 与三个 `handle_*` 转发（L299-330、L174-187）
- 订阅表门控：直接写入 `_tick_subs` 后 `on_tick_updated` 命中订阅但无策略（绕开 `stra_sub_ticks` 对 replayer 内部状态的依赖）
- 日志族与 `log_debug/log_info/log_error` 可变参模板（L1096-1109 与头文件 L33-52）
- **T+1 冻结分支**：股票开仓走 `commInfo->isT1()`(L1182) → `_newvol += qty` 但 `_newavail` 不增加、`frozen()==volume`、当日平多被拒；开仓费 0.5/手×100
- 隔夜昨仓结转后平昨：明细盈亏、`opentdate != curTDate` → 走 close 费档、冻结为 0，以及 `stra_exit_long` 受理后的订单结构与 undone 符号（BUG-P08-005 观测锚点）
- 撤单归还可用量：多头 6 手（4 昨 + 2 今）平 3 后撤单，两条 CoverMode 分支都归还到 `_preavail=4/_newavail=2`
- 析构安全：`_strategy==NULL` 且工厂未装载时 `~UftMocker`(L59-65) 不解引用 `_factory._fact`
- `DISABLED_TickPointerAndSessionDerefFamily`：需要真实 `WTSTickData*` 的 `on_tick/handle_tick`、`stra_sub_*` 四件套、`update_dyn_profit` 有持仓 + NULL tick、`on_session_begin/end`、`handle_replay_done`

## 有意未覆盖 / 剩余

- `UftMocker::on_tick`(L228-287) 与 `HftMocker::on_tick`(L345-430) 的真实分发：需要 `WTSTickData*` 实例（本仓库 `Includes/` 未导出可安全构造的入口，且 `procOrder` 会对 grab 到的 tick 调 `release()`，伪造对象有归还错池风险）。建议与 l06/n04 的 tick 缓存 fixture 合并处理。
- `procOrder` 的成交段（`UftMocker.cpp:917-969`、`HftMocker.cpp:725-786`）与 `update_dyn_profit` 的有持仓分支：同上依赖 `stra_get_last_tick` 返回非空。
- `UftMocker::on_session_begin/end`(L343-390)、`stra_enum_position`(L1030)：受 BUG-P08-001 阻塞，需要注入桩 `UftStrategy*`（其纯虚接口未在本 lane 简报内），建议下一波连同 `_strategy` 桩件一起做。
- `init_uft_factory`/`init_hft_factory` 读 `use_newpx/error_rate/match_this_tick/module/strategy` 的正路径：需要可用 `WTSVariant` 构造 fixture 与真实策略 DLL，本 lane 只覆盖 `cfg==NULL` 早退。
- `install_hook/step_tick/enable_hook(true)` 的 hook 同步：阻塞原语，永不进入启用用例。
- `_error_rate>0` 时 `genRand()` 决定撤单的随机分支（L901/L712）：非确定性，未断言（`genRand` 不变量已由 l06 覆盖）。
- `HftMocker::do_set_position` 的冻结/T+1 细分：本轮只锚定净持仓与均价的可验证区间，未逐字段断言（源码未精读段）。

## 验证命令（逐文件已执行，3/3 通过）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p08_btcore_mockers3/<file>.cpp
```

链接说明：测试引用 `HisDataReplayer`（`_bd_mgr._fullpid_map`、`_fee_map`、`get_commodity_info`、`calc_fee`、`get_date/get_min_time/get_secs/get_trading_date/get_cur_price/get_rawcode/get_last_tick`）、`WTSCommodityInfo`（Includes 内联）、`WtHelper`、`makeLocalOrderID`（库内 `HftMocker.cpp` 提供，测试未重复定义）。
需由主控在 `CoverageTests_btcore` 链接 `WtBtCore` + `WTSTools` + `WTSUtils`；coverage 子目录若非自动 glob，需把本目录 3 个文件加入该目标。
