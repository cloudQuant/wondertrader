# p07_btcore_replayer2 缺陷登记（迭代 01 · 第四波）

模块：`src/WtBtCore/HisDataReplayer.cpp`（tick 回放主循环 / 任务调度 / 扩展装载器 / 价格与费率）
状态均为「待审核」，本轮**未修改任何生产代码**。
既有编号 BUG-P04-001…011 不重复立案；与 p04 同源的条目只在「备注」里引用编号。

---

## BUG-P07-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `getNextTickTime`(L1591)、`getNextTransTime/OrdDtl/OrdQue` 同型判断
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp::tickOnlyReplayDrivesHftLoopPerDay`
- 输入/前置条件: tick 模式，扩展装载器对 20200102 给 5 笔、20200103 给 2 笔（时间戳均在 SD0930 交易时段内）
- 期望行为: 一个交易日的 tick 应全部回放（5 笔 + 2 笔 = 7 次 `handle_tick`）
- 实际行为: 每天**最后一笔永不回放**。游标语义是「`_items[_cursor-1]` 为下一笔待回放」，
  但 L1591 写的是 `if (tickList._cursor >= tickList._count) continue;`，
  `_cursor == _count` 时最后一笔（下标 `_count-1`）被跳过 → 实测 5 笔只回 4 笔、2 笔只回 1 笔
- 复现步骤: `ctest -R CoverageTests_btcore --output-on-failure -R P07TickLoaderTest.tickOnlyReplayDrivesHftLoopPerDay`
- 影响范围: 全部 tick 级回测（tick 模式撮合价、收盘价、涨跌停判断都会少一笔）；日线级别的同类问题见 BUG-P04-010
- 备注: 非阻塞覆盖率目标（当前断言按实际行为写死为「每天少一笔」，源码修正后需同步改回 7 笔）。
  修复方向：判定改为 `if (tickList._cursor > tickList._count) continue;`（未实施）

## BUG-P07-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `getNextTickTime`(L1610-L1614)
- 相关测试: 无（触发不了，纯静态审查 + 逻辑推演；本轮把它排除在启用用例之外）
- 输入/前置条件: tick 时间戳落在小节之间（如 1131）但仍早于收盘时间
- 期望行为: 按 L1598-L1605 的注释意图，非交易时间的 tick 应「指针一直步进」后跳过
- 实际行为: `while (!sInfo->isInTradingTime(nextMinTime) && tickList._cursor > tickList._items.size())`
  的第二个条件几乎恒为假（`_cursor` 是从不超过 `_items.size()` 的），循环体一次都不执行，
  跳过逻辑完全失效，非交易时间的 tick 照样被回放；
  反过来若条件写对（`<`），循环体里 `_items[_cursor - 1]` 又会在 `_cursor` 越界后 UB
- 复现步骤: 代码走查 L1610；如需运行时证据，构造 1131 的 tick 观察 `handle_tick` 次数
- 影响范围: Issue#104 声称修复的「小节中间 tick」场景实际未生效
- 备注: 建议改成 `while (cursor < size && !isInTradingTime(nextMinTime)) { cursor++; ... }` 并在越界后返回 UINT64_MAX（未实施）

## BUG-P07-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `run_by_tasks`(L990、L1010)
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_task_schedule_test.cpp::monthlyTaskNeedsYyMMddDayField`
- 输入/前置条件: `register_task(1, 2, 1500, "m", "CHINA", "SD0930")`，回测区间 20200102-20200103
- 期望行为: 「每月 2 号」在 20200102 触发一次调度
- 实际行为: 判定式是 `_cur_date % 1000000 == _task->_day`，只有把 `_day` 写成 YYMMDD（200102）才可能触发，
  传 1..31 时**永远不触发**且没有任何告警；周频同理退化成 `weekDay == _task->_day`（L1008-L1009 的区间判定被注释掉）
- 复现步骤: 见上述用例的两个分支（200102 触发 1 次 / 2 触发 0 次）
- 影响范围: 选股/调度型策略的月频、周频任务可能整轮静默不执行
- 备注: 需要产品口径确认 `_day` 的契约（YYMMDD 还是几号/星期几），确认后统一改代码或改文档（未实施）

## BUG-P07-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `get_kline_slice`(L2196-L2201) 与 `onMinuteEnd`(L2035-L2038)
- 相关测试: 无（本轮预算内未做半小时线用例，纯代码审查）
- 输入/前置条件: `get_kline_slice(code, "f", 10, 1, false)`（半小时线）
- 期望行为: `realTimes = PERIOD_TIMES_HALF`，闭盘时 `handle_bar_close` 回报周期串 `"f"`
- 实际行为: `"f"` 分支写的是 `realTimes = PERIOD_TIMES_HOUR;`（与 `"h"` 分支完全相同），
  于是半小时线按小时线重采样，`onMinuteEnd` 回报 `"h"`；
  L2037 的 `times == PERIOD_TIMES_HALF → "f"` 分支成为死代码
- 复现步骤: 订阅 `"f"` 后统计 `handle_bar_close` 的 period 参数
- 影响范围: 所有使用半小时线的回测/选股任务
- 备注: 修复方向是 L2200 改为 `PERIOD_TIMES_HALF`（未实施）

## BUG-P07-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `replayHftDatasByDay`(L1858-L1868) 对比 `replayHftDatas`(L1974-L1985)、`get_last_tick`(L2912)
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp::lastTickAndDayPriceHaveData`
- 输入/前置条件: tick 模式，20200102 五笔已知价格（10.2/10.8/11.5/11.6/12.0）
- 期望行为: 在 `on_tick` 回调里 `get_last_tick()`/`get_day_price()` 取到的是**当前正在回放的这一笔**
- 实际行为: `replayHftDatasByDay` 采用「先 `_cursor++` 再回调」（L1862-L1867），
  而 `get_last_tick` 返回 `_items[_cursor - 1]`（L2912），因此回调里取到的是**下一笔未回放的 tick**（未来数据）。
  实测第 1 次回调的 `get_day_price(code,3)=10.8`，而当前笔价格是 10.2。
  同一文件里 `replayHftDatas`（run_by_bars 的分钟级 tick 回放）却是「先回调再 `_cursor++`」（L1979-L1984），
  两条路径的游标语义互相矛盾
- 复现步骤: 运行上述用例，比较 `tickPrices` 与 `peekedPrices`/`dayLast` 的错位
- 影响范围: 逐 tick 回测里策略看到未来价（撮合价、涨跌停、成交量都会偏），结果不可信
- 备注: 不阻塞覆盖率（当前断言按实际行为写死并就地注明，修复后需同步收紧）

## BUG-P07-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `run_by_ticks`(L746)
- 相关测试: `p07_tick_loader_test.cpp::tickOnlyReplayDrivesHftLoopPerDay`（间接）
- 输入/前置条件: tick 模式回测，品种会话与 `DEFAULT_SESSIONID("TRADING")` 的偏移不一致（如纯白盘股票 vs offset=300 的 TRADING）
- 期望行为: 用订阅合约自己的会话（或至少股票用 SD0930）计算结束交易日
- 实际行为: 硬编码 `calcTradingDate(DEFAULT_SESSIONID, edt, etime, true)`，
  与 `replayHftDatasByDay` 内部按 `get_session_info(stdCode)`（品种会话）做的交易时间判定不是同一套口径，
  跨市场/夜盘品种时 `end_tdate` 会多/少一个交易日
- 复现步骤: 对夜盘品种（会话 offset=300）跑 21:00 起的 tick 区间，观察最后一个交易日是否被裁掉
- 影响范围: tick 回测的收尾交易日边界
- 备注: 建议按订阅品种的会话取交集（未实施）

## BUG-P07-007

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `replayHftDatas`(L1907-L2007)、`cacheRawTicksFromCSV`(L3505-L3584)
- 相关测试: `p07_tick_loader_test.cpp::DISABLED_mismatchedTickDateHangsReplayHftDatas`（禁用态，不进启用集）
- 输入/前置条件: tick 模式 + 主K线（走 `run_by_bars` → `replayHftDatas`），且订阅里存在与 `_cur_tdate` 串日的数据
- 期望行为: 无法推进时退出循环并告警
- 实际行为: 外层 `for(;;)` 每轮用**同一个固定 `stime`** 计算 `nextTime`，只有成功回放才推进游标；
  若某条订阅的 `lastTime > nextTime` 恒成立则游标永不推进 → 死循环（本波已有多条同类 HANG）。
  另外四个子循环的守卫不一致：orddtl/ticks/ordque 用 `_cursor > _count`，
  而成交明细用 `_cursor == _count`（L1950），未装载（`_cursor == UINT_MAX`）时 `_items[UINT_MAX-1]` 越界读
- 复现步骤: 见禁用用例；启用会挂死，故只作归档（遵守「崩溃/挂死类整类收进同一个 DISABLED_」纪律）
- 影响范围: 逐 tick 回测的健壮性；level2 数据在 bin 模式下的成交明细回放
- 备注: `cacheRawTicksFromCSV` 的 csv 分支已整体 `return false`，但下面还留了 ~70 行注释死代码（L3511-L3584），
  与 L3508 的告警文案（提示可以用 csv 生成 dsb）互相矛盾，属维护性问题
