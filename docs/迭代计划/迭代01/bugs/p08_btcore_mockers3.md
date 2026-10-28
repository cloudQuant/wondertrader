# p08_btcore_mockers3 疑似缺陷登记（状态：待审核，未修改源码）

Lane: p08（第四波），靶子 `src/WtBtCore/UftMocker.cpp` / `src/WtBtCore/HftMocker.cpp`。
以下问题均由测试过程中直接读源码发现，**未对 `src/**` 做任何修改**。行号以当前 HEAD 为准。

---

## BUG-P08-001 UftMocker 会话回调无条件解引用 `_strategy`（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:361`（`on_session_begin` 末尾）、`src/WtBtCore/UftMocker.cpp:366`（`on_session_end` 开头）
- 现象：
  - `on_session_begin()` 结转冻结持仓后无条件执行 `_strategy->on_session_begin(this, curTDate);`
  - `on_session_end()` 第一句就是 `_strategy->on_session_end(this, curTDate);`，之后的资金/持仓日志反而永远走不到
- 对比：同文件 `on_init()`(L339)、`on_trade()`(L812)、`on_order()`(L803)、`on_entrust()`(L818)、`on_channel_ready()`(L824)、`on_bar()`(L224)、`on_tick_updated()`(L295) 全部有 `if (_strategy)` 守卫。
- 影响：`init_uft_factory()` 未配置 `strategy` 节点（或只加载工厂不加载策略）时，`handle_session_begin/handle_session_end` 直接空指针崩溃；回测的会话边界是最先被调用的入口之一。
- 测试处理：整族收入 `p08_uft_order_position_test.cpp` 的 `DISABLED_NullStrategyAndNullCommDerefFamily`，启用用例不触碰。

## BUG-P08-002 on_session_begin 把 PosInfo 强转成 PosItem，空头冻结永不释放（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:346-359`
- 代码：`PosItem& pInfo = (PosItem&)it.second;`（`it.second` 是 `PosInfo{ PosItem _long; PosItem _short; }`）
- 现象：C 风格强转后只指向 `_long`，结转逻辑（`_prevol += _newvol; _preavail = _prevol; _newvol = _newavail = 0;`）只对多头生效；空头持仓的今仓永远不结转为昨仓，`_newavail` 跨日累积。
- 该写法还依赖 `PosInfo`/`PosItem` 的内存布局，属未定义行为；且 `PosItem::_long` 成员本身未初始化（已由 p03 记录，见 `bugs/p03_btcore_mockers2.md`，本条不重造编号）。
- 影响：T+1 品种（股票）隔夜空头仓位的可用量计算失真。
- 测试处理：因 BUG-P08-001 的无条件解引用，本段结转逻辑在启用用例中不可安全到达（见 `DISABLED_NullStrategyAndNullCommDerefFamily`）。

## BUG-P08-003 update_position 明细清理区间 off-by-one，0 量明细永久残留（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:1251-1252`（offset==1）、`src/WtBtCore/UftMocker.cpp:1309-1310`（offset==2）
- 现象：
  - offset==1 分支只在 `dInfo._volume == 0` 时把 `eit = it`，最后 `erase(begin, eit)`；被完全平掉的那条明细自身**不在区间内**，单条全平时 `eit == begin` → 区间为空，什么都不删。
  - offset==2 分支 `erase(sit, eit)` 同理，`sit == eit` 时为空区间。
- 复现（已作为启用用例锚定当前行为）：`p08_uft_order_position_test.cpp:CloseTodayBranchScalesDynProfit` 全平 4 手后 `_details.size()==1 && _details[0]._volume==0`。
- 影响：`update_dyn_profit()`(L844/L870) 与 `update_position()` 尾部的浮盈汇总(L1316) 会持续遍历死明细；`stra_get_position_avgpx` 一类按明细求和的口径在长期运行的策略里会累积垃圾条目（内存与耗时随开平仓次数线性增长）。

## BUG-P08-004 stra_cancel_all 恒返回空容器（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:456-470`
- 现象：`OrderIDs ret;` 声明后从不 `emplace_back`，只 `stra_cancel(it->first)`，最后 `return ret;`。撤单实际发生（任务队列里），但调用方拿不到任何被撤的 localid。
- 对比：`HftMocker::stra_cancel(const char*, bool, double)`（`HftMocker.cpp:574-595`）会 `ret.emplace_back(it->first)`，两个 Mocker 的同类 API 返回契约不一致。
- 测试处理：`p08_uft_order_position_test.cpp:CancelAllScansMatchingCodeOnly` 断言"返回空但订单确实被撤"，把契约缺口固化在测试里。

## BUG-P08-005 平多订单方向标记与 stra_get_undone 语义不一致（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:719`（`stra_exit_long` 设 `ordInfo->_isLong = true`）vs `src/WtBtCore/UftMocker.cpp:780`（`stra_exit_short` 设 `false`）
- 现象：`stra_get_undone()`(L392-405) 用 `ordInfo->_left * (ordInfo->_isLong ? 1 : -1)` 汇总净挂单。
  - 平空（实际是**买入**动作）被记为负数；
  - 平多（实际是**卖出**动作）被记为正数。
- 即 `_isLong` 在这里表达的是"平哪一侧的仓"，但 `stra_get_undone` 把它当成"委托买卖方向"，净挂单量符号与实际相反。撮合逻辑 `procOrder()`(L918) 也据此读取对手盘量：`ordInfo->_isLong ? curTick->askqty(0) : curTick->bidqty(0)` —— 平多单（卖出）会去看卖一量而不是买一量，撮合量取错盘口。
- 测试处理：`p08_uft_dispatch_t1_test.cpp:T1OvernightPositionCloseable` 固化"平多 20 手 → undone = +20"的当前观测值。

## BUG-P08-006 平仓/撤单路径对品种信息缺 NULL 防护（状态：待审核）

- 位置：`src/WtBtCore/UftMocker.cpp:683-685`（`stra_exit_long`）、`src/WtBtCore/UftMocker.cpp:744-746`（`stra_exit_short`）、`src/WtBtCore/UftMocker.cpp:426-427`（`stra_cancel` 任务内）
- 现象：`WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);` 之后直接 `commInfo->getCoverMode()`。
- 对比：`stra_enter_long/short`(L599/L640)、`stra_buy/stra_sell`(L474/L536) 都有 `if (commInfo == NULL) return ...;`；`update_position()`(L1170-1172) 也有 NULL 早退。
- 影响：对未注册的合约调用平仓接口 → 空指针崩溃；`stra_cancel` 里的崩溃发生在任务队列中，会污染后续任务。
- 测试处理：收进 `DISABLED_NullStrategyAndNullCommDerefFamily`。

## BUG-P08-007 HftMocker 按量撤单在 qty=0 时只撤一笔（状态：待审核）

- 位置：`src/WtBtCore/HftMocker.cpp:574-595`
- 代码：`if (left < qty) qty -= left; else break;`，而 `qty` 的形参默认值是 0（`HftMocker.h:317`）。
- 现象：`stra_cancel(stdCode, isBuy)`（不传 qty）命中第一笔匹配订单即 `break`，返回的 `OrderIDs` 只有 1 个 id；"qty=0 表示按合约+方向全撤"的直觉语义不成立（`UftMocker` 的对应接口 `stra_cancel_all` 是全撤）。
- 次要：`HftMocker::on_trade()`(L646) 用 `_pos_map[stdCode]` 取 `const PosInfo&` 只为读 `_volume`，会给从未成交过的合约插入空持仓条目，使 `_pos_map` 随查询单调增长。
- 测试处理：`p08_hft_order_cancel_test.cpp:CancelByCodeAndDirection` 断言 qty=0 时 `canceled.size()==1`，把当前契约固化。

---

## 与既有缺陷的关系

- `UftPosItem::_long` 未初始化（`src/WtBtCore/UftMocker.h:307`）已由 p03 登记，本清单仅引用、不重造编号；BUG-P08-002 的强转写法是该问题的放大场景。
- `HftMocker::splitVolume` / `genRand` 不变量由 l06 覆盖；`ExecMocker`/`HisDataMgr`/`CtaMocker` 状态查询族由 n04 覆盖；`CtaMocker`/`SelMocker` 下单-成交-持仓-盈亏主链由 p03 覆盖，本 lane 未重复。
