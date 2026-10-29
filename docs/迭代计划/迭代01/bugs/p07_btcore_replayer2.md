# p07_btcore_replayer2 缺陷登记（迭代 01 · 第四波）

模块：`src/WtBtCore/HisDataReplayer.cpp`（tick 回放主循环 / 任务调度 / 扩展装载器 / 价格与费率）。
最初只做源码与测试前提静态复核，未修改生产代码；后续定向结果见各条，不代表完整回放验收。
既有编号 BUG-P04-001…011 不重复立案；同源条目保留交叉引用。

---

## BUG-P07-001

- 状态: 源码边界缺陷及本机定向回放已复现；产品尚未修复
- 严重级别: Blocker
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `getNextTickTime`(L1591)、`getNextTransTime/OrdDtl/OrdQue` 同型判断
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp::tickOnlyReplayDrivesHftLoopPerDay`
- 输入/前置条件: tick 模式，扩展装载器对 20200102 给 5 笔、20200103 给 2 笔（时间戳均在 SD0930 交易时段内）
- 定向复核（2026-10-29）: 原夹具根目录上溯不足、且在 `prepare()` 前订阅会被 reset 清空；修复路径并在 `handle_init` 订阅后，断言已成功请求两日数据。重新构建单跑 FAIL：预期 7 笔，实际 5 笔；本机证据支持每天遗漏一笔的边界问题。
- 期望行为: 一个交易日的 tick 应全部回放（测试 fixture 合计 5 笔 + 2 笔 = 7 次 `handle_tick`）。
- 证据/审核结论: `HftDataList` 注释定义初始化后 cursor 为大于 0 的“下一条/已回放数”语义
  （`HisDataReplayer.h:271-284`）；loader 将 `_count` 设为 vector 项数（`HisDataReplayer.cpp:3443-3459`），
  定位用 `idx + 1`（`HisDataReplayer.cpp:1579-1588`），回放以 `_items[_cursor-1]` 取当前项
  （`HisDataReplayer.cpp:1855-1868`）。因此有效范围是 1..count；`getNextTickTime` 的
  `_cursor >= _count` 会在 cursor==count 时跳过尾项。相同边界判断也出现在
  `getNextTransTime/getNextOrdDtlTime/getNextOrdQueTime`（`HisDataReplayer.cpp:1661,1709,1757`），
  可能一并跳过对应 Level2 列表尾项。Level2 的同型问题尚未独立动态验证。
- 测试前提: 旧 `tickOnlyReplayDrivesHftLoopPerDay` 以缺尾笔作为通过期望；现改为 7 次并检查末笔。R80 旧用例还将 `Q/H` 当作字面复权后缀，loader 收到带 `Q` 的代码而找不到数据；现改用真正的 `SUFFIX_QFQ`/`SUFFIX_HFQ`（`-`/`+`），并核对交易日、会话、loader 请求代码及三笔数据。首轮期望 3 笔、实际 2 笔；调整为非致命数量断言后，第二轮亦实际 2 笔、累计 4 笔，均未达到每轮 3 笔的正确预期。P04-010 是日线首 bar 定位问题，根因并不相同，不作为重复项。
- 修复方向: 将所有 one-based cursor 的“已耗尽”条件统一为 cursor > count，并在读取前同时校验
  vector size 与 count；回归覆盖 1 项、N 项及空列表，确保每个 stream 尾项各回放一次。

## BUG-P07-002

- 状态: 小节间 tick 跳过路径缺陷已确认；修复前提需有边界保护
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `getNextTickTime`(L1610-L1614)
- 相关测试: 无（触发不了，纯静态审查 + 逻辑推演；本轮把它排除在启用用例之外）
- 输入/前置条件: tick 时间戳落在小节之间（如 1131）但仍早于收盘时间
- 期望行为: 按 L1598-L1605 的注释意图，非交易时间的 tick 应「指针一直步进」后跳过
- 证据/审核结论: 有效 one-based cursor 为 1..count（`HisDataReplayer.h:271-284`），且 loader 中
  `count == _items.size()`；因此 `cursor > _items.size()` 在访问有效当前项时不成立。
  当时间在 session 内的小节间隙，`isInTradingTime` 为 false，当前 while 不推进游标，随后
  `getNextTickTime` 仍把当前项作为 nextTime 返回（`HisDataReplayer.cpp:1591-1619`）。
  这使“跳过间隙 tick”的注释目标与实现不一致，静态缺陷成立；本轮未运行。
- 修复方向: 先用 `cursor > count` 判断耗尽，再在 `cursor <= count` 范围循环；每次递增后先检查
  是否越过 count，重新读取并计算 nextMinTime，然后再判 session。耗尽时返回 UINT64_MAX。
  用 SD0930 的 1131 tick 后接 1300 tick 做回归，断言 1131 不回调、1300 正常回调；仅把比较符改成
  小于号会有末尾越界风险，不能作为完整修复。

## BUG-P07-003

- 状态: 月频源码与公开契约冲突已确认；周频同类主张撤销
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `run_by_tasks`(L990、L1010)
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_task_schedule_test.cpp::monthlyTaskUsesDayOfMonthContract`
- 输入/前置条件: `register_task(1, 2, 1500, "m", "CHINA", "SD0930")`，回测区间 20200102-20200103
- 期望行为: 「每月 2 号」在 20200102 触发一次调度
- 定向复核（2026-10-29）: 先修正夹具资源根目录后，月频合法 day=2 的单测 FAIL（期望 1 次、实际 0 次）；同一夹具日频对照单测 PASS。此前未加载资源时的 0 次不能作为本条证据。
- 证据/审核结论: register_task 公共头文件及 TaskInfo 注释规定月频 date 为 1..31、周频为 0..6
  （`HisDataReplayer.h:361,810-820`）；月频实现以 `_cur_date % 1000000 == _task->_day` 比较完整
  YYMMDD（`HisDataReplayer.cpp:987-1005`），所以规范的 1..31 不会命中。属于源码与已公开契约
  冲突。周频主判据 `TimeUtils::getWeekDay(_cur_date) == _task->_day`（`:965,1007-1027`）
  与 0..6 契约一致；`TimeUtils::getWeekDay` 返回 tm_wday 0..6（`TimeUtils.hpp:201-220`），
  没有证据支持“周频同理失效”。
- 测试前提: `monthlyTaskNeedsYyMMddDayField` 用 `_day=200102` 证明的是单个完整日期的相等，
  不是月度重复；`_day=2` 不触发符合当前实现，但暴露契约违例。用例中的 weekday 范围断言
  1..7 与公开 0..6 契约不符，虽然该固定日期取值为 4（`p07_task_schedule_test.cpp:187-202`）。
- 修复方向: 月频提取 day-of-month（`_cur_date % 100`）与 1..31 目标比较，并明确短月无该日期时
  是否跳过或顺延；跨至少两个自然月及月末节假日做测试。将月频用例改为 date=2 跨月验证重复触发；
  周频现有主判据不按本条修改，weekday 测试的合法范围应使用 0..6。

## BUG-P07-004

- 状态: 与 Q02-005 重复；源码映射缺陷已确认
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `get_kline_slice`(L2196-L2201) 与 `onMinuteEnd`(L2035-L2038)
- 相关测试: 无；静态源码核查，未独立运行半小时线用例
- 输入/前置条件: `get_kline_slice(code, "f", 10, 1, false)`（半小时线）
- 期望行为: `realTimes = PERIOD_TIMES_HALF`，闭盘时 `handle_bar_close` 回报周期串 `"f"`
- 证据/审核结论: `get_kline_slice` 的 `"f"` 分支赋 `PERIOD_TIMES_HOUR`，与 `"h"` 分支相同
  （`HisDataReplayer.cpp:2190-2201`）；`onMinuteEnd` 只有收到 `PERIOD_TIMES_HALF` 才把回调周期设为
  `"f"`（`HisDataReplayer.cpp:2035-2038`）。源码证据支持映射不符，但本轮未运行实际重采样。
- 重复登记: 与 `q02_btcore3.md` BUG-Q02-005 同一根因，保留 P07 为主记录，不重复计数。
- 修复方向: `"f"` 映射为 `PERIOD_TIMES_HALF`；使用分钟底座校验半小时的首末分组边界和
  `handle_bar_close` period 参数 `"f"`，再把重复项 Q02-005 作为交叉引用。

## BUG-P07-005

- 状态: 真实缺陷（修正测试前提和断言后本机复现）
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `replayHftDatasByDay`(L1858-L1868) 对比 `replayHftDatas`(L1974-L1985)、`get_last_tick`(L2912)
- 相关测试: `src/TestUnits/coverage/p07_btcore_replayer2/p07_tick_loader_test.cpp::lastTickAndDayPriceMatchCurrentCallbackTick`
- 输入/前置条件: tick 模式，20200102 五笔已知价格（10.2/10.8/11.5/11.6/12.0）
- 期望行为: `get_last_tick` 文档称其返回最新 tick（`HisDataReplayer.h:875-880`）；在 tick 回调中，
  “最新”应不晚于当前回调 tick。`get_day_price` 在 tick 模式直接从 `get_last_tick` 取 open/high/low/price
  （`HisDataReplayer.cpp:3034-3064`），应和同一最新 tick 一致。
- 实际行为: `replayHftDatasByDay` 采用「先 `_cursor++` 再回调」（L1862-L1867），
  而 `get_last_tick` 返回 `_items[_cursor - 1]`（L2912），因此回调里取到的是**下一笔未回放的 tick**（未来数据）。
  修复测试订阅时序后，本机逐例运行观察到回调期间的 last-tick 及 day-price 字段前视一笔。
  同一文件里 `replayHftDatas`（run_by_bars 的分钟级 tick 回放）却是「先回调再 `_cursor++`」（L1979-L1984），
  两条路径的游标语义互相矛盾
- 证据边界: `replayHftDatasByDay` 在 callback 前递增游标、`get_last_tick` 以 cursor-1 取值是源码事实；
  最新 tick 的回调契约由接口注释支持。修正后的目标测试本机单独运行 FAIL；最后一笔的回调被 BUG-P07-001 遮蔽，尚未验证。
- 测试前提/误报边界: 原 `lastTickAndDayPriceHaveData` 将 `peekedPrices` 设为未来项
  （`p07_tick_loader_test.cpp:440-442`），但同一测试对 `get_day_price` 的 open/last 期望当前项、
  high/low 期望下一项（`p07_tick_loader_test.cpp:443-447`）；这些字段实现上调用同一个 `get_last_tick`，
  静态上应来自同一条 tick。原 oracle 内部不一致，已改为与实际回调 tick 的各字段逐项比较，且不依赖总回调数量。
- 修复方向: 明确回调期间“最新 tick”的契约，并维护不受预先推进 cursor 影响的已发布 tick 索引/
  当前事件快照；或调整回调与 cursor 更新顺序并评估重入影响。测试在每次回调中把 get_last_tick 的价格、
  get_day_price(0..3) 与回调参数的当前 WTSTickStruct 字段逐项比对，并覆盖最后一笔无后续项场景。

## BUG-P07-008

- 状态: 真实缺陷（本机单例复现）
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` 的年度任务日期判断（约 1029–1031 行）。
- 相关测试: `P07TaskScheduleTest.yearlyTaskFiresOnYearBoundaryCrossing`。
- 输入/前置条件: 年度任务设置 `day=0101`；CHINA 假日日历中 20200101 为假日，前一个交易日为 20191231，下一个为 20200102。
- 期望行为: 年度任务遇到假日顺延，在 20200102 触发一次，符合 `TaskPeriodType` 的注释。
- 实际行为: 比较 `preTDate % 10000 < day` 时，1231 不小于 0101，导致跨年后首个交易日未触发；修正旧测试的非法 day=2 后，单独运行观察到预期一次、实际零次。日频对照测试通过。
- 修复方向: 使用包含年份的目标日期与前后交易日区间判断跨年，避免仅比较 MMDD；增加年初假日顺延、普通年度触发及年末边界回归。未修改生产代码。

## BUG-P07-006

- 状态: 交易日边界缺陷已确认（特定 session/end-time 前提）
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `run_by_ticks`(L746)
- 相关测试: `p07_tick_loader_test.cpp::tickOnlyReplayDrivesHftLoopPerDay`（间接）
- 输入/前置条件: tick 模式回测，品种会话与 `DEFAULT_SESSIONID("TRADING")` 的偏移不一致（如纯白盘股票 vs offset=300 的 TRADING）
- 期望行为: 不得回放超过配置 `_end_time` 的 tick；日期边界也应使用明确的回放 session 契约。
- 证据/审核结论: `prepare()` 用 DEFAULT_SESSIONID 初始化 `_cur_tdate`（`HisDataReplayer.cpp:661-664`），
  `run_by_ticks()` 又用 DEFAULT_SESSIONID 计算 `end_tdate`（`:741-760`），但 `replayHftDatasByDay`
  中 `getNextTickTime` 按订阅合约的 session 判断时间（`:1541-1546`）。样例 `TRADING` offset=300，
  `SD0930` offset=0（`dist/common/sessions.json:2-18,164-183`）；BaseDataMgr 的交易日换算会因 offset
  改变自然日对应交易日（`WTSBaseDataMgr.cpp:702-733`）。
  更重要的是 `run_by_ticks` 只以交易日循环到 `end_tdate`，未在发 tick 前比较绝对时间 `_end_time`。
  因而股票 session=SD0930、区间结束为当日 21:00 时，DEFAULT_SESSIONID 可把 end_tdate 推到次日；
  若次日有股票 tick，按日回放可能将结束时间之后的 tick 送出。该边界从代码/样例可推导，未运行复现。
- 测试前提: 当前 P07 fixture 结束时间为 15:00（`p07_tick_loader_test.cpp:380`），此时两种 session
  换算未显示出该边界差异，不能验证本条。需要在 end-time 映射不同的 0-offset session 上放置次日数据，
  并断言 end_time 之后没有 callback。
- 修复方向: 回放每个事件前以完整时间戳检查 `nextTime <= _end_time * 100000`（按内部时间编码核实单位），
  绝不能只靠 end_tdate 截断；同时明确开始/结束日期用全局 session 还是按每个订阅合约计算，并在
  多合约会话情形下规定一致的边界策略。加入当日 21:00 截止、次日 09:31 有数据的股票测试。

## BUG-P07-007

- 状态: 成交明细空列表解引用源码风险已确认；原死循环主张未证且测试前提不成立
- 严重级别: Major（仅限 bin 模式注册 transaction 且当前日期没有明细、同时其它流提供事件的路径）
- 模块/文件: `src/WtBtCore/HisDataReplayer.cpp` · `getNextTransTime`(L1630-1668)、
  `checkTransactions`(L2786-2825)、`replayHftDatas`(L1904-2007)
- 相关测试: `p07_tick_loader_test.cpp::DISABLED_mismatchedTickDateHangsReplayHftDatas`；该用例不证明本文所列任一问题
- 输入/前置条件: bin 模式下注册 transaction 订阅；当前交易日 transaction 数据缺失/空，而 tick 或其它流有有效事件
- 期望行为: 空/未装载的 Level2 列表安全跳过；无法推进的循环须有明确退出/诊断。
- 证据/审核结论:
  1. 成交明细空列表的边界成立：`getNextTransTime` 调用 `checkTransactions`；装载失败时
     `checkTransactions` 建立空列表并设 `cursor=UINT_MAX,count=0` 后返回 false
     （`HisDataReplayer.cpp:2800-2825`）。同一轮若 tick 或其它流提供 `nextTime`，`replayHftDatas`
     仍遍历 `_trans_sub_map`，而 transaction 子循环以 `cursor == count` 判断耗尽
     （`:1945-1954`）；`UINT_MAX == 0` 为 false，随后读取 `_items[cursor-1]`，形成越界访问。
  2. `stime` 在各 getNext 函数中只于 `cursor == UINT_MAX` 时用于初次 lower_bound 定位
     （例如 `:1636-1658`）；后续事件按游标推进。仅“每轮传相同 stime”不足以推出死循环。
     当前禁用测试的 `StubBtLoader::loadRawHisTicks` 按请求日期筛选 fixture（`p07_tick_loader_test.cpp:192-221`），
     用例只调用 `sub_tick`、没有 transaction 订阅（`:610-623`），没有构造所称串日或 L2 输入；
     因而不是 hang 的复现证据。本轮未运行测试，也未独立复现 hang。
- 修复方向: transaction 子循环在索引前显式跳过 empty、cursor==UINT_MAX、cursor==0、
  cursor>count 或 cursor>items.size()；有效 cursor==count 仍是 one-based 最后一项，不得当成耗尽。
  新测试用 bin fixture 注册 tick+transaction、transaction 文件缺失而 tick 有数据，断言安全收尾；
  同时验证最后一条 transaction 恰好回调一次（one-based 尾项边界与 BUG-P07-001 同源）。
- 旧死循环主张: 需要先构造一个能证明每轮 nextTime 有限但所有订阅列表均不推进的具体输入；
  现有禁用用例没有建立该条件，暂标待验证，不列为已证缺陷。
- CSV 维护说明: `cacheRawTicksFromCSV` 明确提示 CSV tick 不受支持并在该分支 `return false`；
  后续被注释的旧 CSV loader 是不可达代码，转换 DSB 的告警与该策略一致。可清理死代码，但这不是
  与当前产品行为矛盾的缺陷证据。
