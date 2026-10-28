# p04_btcore_replayer 缺陷记录（迭代 01 · 第三波）

模块：`src/WtBtCore/HisDataReplayer.cpp` / `.h`（回放主循环与数据装配）。
生产代码未改动，状态一律「待审核」。测试文件位于
`src/TestUnits/coverage/p04_btcore_replayer/`，崩溃/挂死类入参统一收进
`DISABLED_crashClassInitAndPrepareAndZeroEndRun`（config 文件）与
`DISABLED_emptyCsvCrashes`、`DISABLED_taskFrequencyAndOutOfRangeReplayCrash`（另两个文件）。

## BUG-P04-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `init` L231-L232
- 相关测试: p04_replayer_config_prepare_test.cpp `DISABLED_crashClassInitAndPrepareAndZeroEndRun`（崩溃点 1）
- 输入/前置条件: `WTSVariant` 配置里没有 `basefiles` 子项
- 期望行为: 返回 false 或按“无基础数据”降级运行
- 实际行为: `WTSVariant* cfgBF = cfg->get("basefiles")` 对缺失键返回 NULL，紧接 `cfgBF->get("session")` 空指针解引用
- 复现步骤: 构造 `mode/path/stime/...` 齐备但不含 basefiles 的配置调用 `init()`
- 影响范围: 任何上游漏配 basefiles 的场景直接崩溃，且 init 无返回值可用的错误通道
- 备注: 不阻塞覆盖率目标（禁用用例已归档）；修复方向是先判空并 `WTSLogger::error` 后 return false

## BUG-P04-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `prepare` L669
- 相关测试: 同上（崩溃点 2）
- 输入/前置条件: 调用方未 `register_sink` 就 `prepare()`
- 期望行为: 返回 false 并记录“未注册数据接收器”
- 实际行为: `_listener->handle_init()` 无条件解引用 NULL；`onMinuteEnd` L2128 反而做了 `if(_listener)` 判空，前后不一致
- 复现步骤: `init()` -> `prepare()`
- 影响范围: 回放前置阶段崩溃
- 备注: 建议 prepare 入口判空并 return false

## BUG-P04-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `run_by_ticks` L744-L760、`prepare` L661-L664
- 相关测试: 同上（挂死点 3）；p04_replay_termination_test.cpp `DISABLED_taskFrequencyAndOutOfRangeReplayCrash`（挂死点 3）
- 输入/前置条件: `_end_time` 为 0（配置缺 etime 或未调用 set_time_range）
- 期望行为: 明确报“回测区间未设置”并拒绝运行
- 实际行为: `edt=_end_time/10000`、`etime=_end_time%10000` 双双退化为 0，`calcTradingDate` 在 uDate==0 时改用系统当前时间，`while(_cur_tdate <= end_tdate)` 的推进上界取决于墙钟，循环次数不可控
- 复现步骤: stime/etime 都填 0，开 tick，prepare + run
- 影响范围: 测试与生产都可能挂死；同时使“回放结束条件”无法静态验证
- 备注: 覆盖率阻塞项（该分支只能禁用覆盖）；修复方向是 prepare 校验 `begin/end != 0`

## BUG-P04-004

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `cacheRawBarsFromCSV` L3863-L3866
- 相关测试: p04_replayer_csv_replay_test.cpp `DISABLED_emptyCsvCrashes`
- 输入/前置条件: `csv/<stdCode>_d.csv` 只有表头、0 行数据
- 期望行为: `barsList->_count == 0` 时返回 false 并告警
- 实际行为: 仍然读取 `barsList->_bars[0].date` 与 `_bars[_count-1].date`，对空 vector 取 `operator[]`（未定义行为）后返回 true
- 复现步骤: 写一个空表头 csv，`prepare()` 后调用 `get_kline_slice(..., isMain=true)`
- 影响范围: 空行情文件会污染缓存并让后续切片/回放越界
- 备注: 修复方向是 count==0 早退

## BUG-P04-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `locate_barindex` L616-L617
- 相关测试: p04_replayer_csv_replay_test.cpp `DISABLED_emptyCsvCrashes`
- 输入/前置条件: `_bars` 为空（配合 BUG-P04-004）
- 期望行为: 返回 UINT32_MAX 之类的“未找到”标记
- 实际行为: `idx = barsList->_bars.size() - 1`，size()==0 时无符号下溢为 4294967295 并被当作合法下标返回
- 复现步骤: 同上
- 影响范围: 上层 `total_barcnt = eIdx - sIdx + 1` 计算出巨大的进度分母
- 备注: 修复方向是空容器直接返回 UINT32_MAX

## BUG-P04-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `locate_barindex` L620-L628
- 相关测试: p04_replay_termination_test.cpp `invertedTimeRangeStopsBeforeFirstBar`（起止倒挂路径，断言只取可观察回调数，不下标）
- 输入/前置条件: `bUpperBound == true` 且 `lower_bound` 命中容器首元素（结束时间早于首根K线）
- 期望行为: 不越过首元素，返回 0 或“未找到”
- 实际行为: 判定 `it->date > bar.date` 成立后无条件 `it--`，对 `begin()` 的迭代器自减属未定义行为，随后 `idx = it - begin()` 得到 -1（无符号化为 4294967295）
- 复现步骤: `set_time_range(202001020930, 202001010930)` 后 `run()`
- 影响范围: 起始/结束区间错配时定位失效
- 备注: 修复方向是 `it != begin()` 才回退

## BUG-P04-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/HisDataReplayer.h L820（默认参数）+ .cpp `run_by_tasks` L943-L945
- 相关测试: p04_replay_termination_test.cpp `DISABLED_taskFrequencyAndOutOfRangeReplayCrash`（崩溃点 1）
- 输入/前置条件: `register_task(id, day, time, "d")` 使用默认 `session="TRADING"`，而 `dist/common/sessions.json` 只有 SD0930/FD0915/FN0100 等
- 期望行为: 模板不存在时报错并拒绝该任务
- 实际行为: `_bd_mgr.getSession("TRADING")` 返回 NULL 后继续用于日推进；`DEFAULT_SESSIONID` 本身也是 `"TRADING"`（src/Includes/WTSSessionInfo.hpp L18），在样例底座里同样取不到
- 复现步骤: 注册任务后 `prepare()`+`run()`
- 影响范围: 调度频率回放（run_by_tasks）不可安全启用，本 lane 只能禁用覆盖
- 备注: 修复方向是判空后 `WTSLogger::error` 并回退到品种自带时段

## BUG-P04-008

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `run_by_bars` L796-L805
- 相关测试: 同上（崩溃点 2）
- 输入/前置条件: 回测区间整体晚于行情最后一根K线，`get_kline_slice` 把 `_cursor` 定位到等于 `_bars.size()`
- 期望行为: 先判 `_cursor < _bars.size()`，否则按“区间内无数据”结束
- 实际行为: 只判 `_cursor != UINT_MAX` 就直接读 `_bars[_cursor]`，越界读一根记录
- 复现步骤: 用 20200120~20200128 区间回放 20200102~20200108 的日线
- 影响范围: 区间配置写错时读到相邻堆内存并继续回放，产生错误价格
- 备注: 覆盖率阻塞项；L906 的 `cursor >= size` 收尾判断发生在循环体之后，来不及兜底

## BUG-P04-009

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `get_kline_slice` L2137-L2141 与 `run_by_bars` L775-L777
- 相关测试: p04_replayer_config_prepare_test.cpp `runWithoutAnyDataEndsReplay`（安全侧锚点）、p04_replayer_csv_replay_test.cpp `missingCsvFileYieldsNullSlice`（只用 isMain=false 规避）
- 输入/前置条件: `get_kline_slice(code, period, cnt, times, true)` 的装载后续步骤失败（csv 缺失 / 时段缺失 / 重采样失败）
- 期望行为: 主K线登记应只在数据装配成功后生效
- 实际行为: 函数入口处已把 `_main_key`/`_main_period` 写好，失败时 `_bars_cache[key]` 可能仍是空的 `BarsListPtr`，`run()` 于是进入 `run_by_bars` 并解引用 `barsList->_code`
- 复现步骤: 对不存在的合约用 isMain=true 调 `get_kline_slice`，随后 `run()`
- 影响范围: 装载失败叠加主K线登记会崩溃；本 lane 未把该入参放进启用用例
- 备注: 修复方向是把 `_main_key` 赋值移到 `bHasHisData` 判定之后

## BUG-P04-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `get_kline_slice` L2384-L2391（游标初始定位）
- 相关测试: p04_replayer_csv_replay_test.cpp `dailyBarsReplayFullTradingDayCycle`、`csvBarsTransferedToDsbAndReRead`；p04_replay_termination_test.cpp `secondCodeReplaysItsOwnBars`
- 输入/前置条件: 回测起始交易日恰好等于行情首日（`_cur_tdate == bars[0].date`，且当前时刻在收盘前 → `isClosed == false`）
- 期望行为: 游标定位到 0，起始日收盘时闭盘第一根日线
- 实际行为: `lower_bound` 返回 `begin()`、`eIdx == 0` 时不回退，兜底条件又只在 `curBar.date > _cur_tdate` 才把游标压回 0，于是 `_cursor = eIdx + 1 = 1`，首根日线在整轮回放里永不触发 `handle_bar_close`（5 根日线只闭盘 4 根，`handle_session_begin/end/section_end/schedule` 同步少一次）
- 复现步骤: 5 根日线 csv，`stime=202001020930`，`get_kline_slice("SSE.STK.000001","d",1,1,true)` 后 `run()`
- 影响范围: 以行情首日为回测起点的策略少一根日线，指标/信号起点整体前移；Issue#122 的补丁只覆盖了“标的尚未上市”分支
- 备注: 不阻塞覆盖率（断言按实际行为写死并在测试里注明），但阻塞“回放结果正确性”结论

## BUG-P04-011

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtBtCore/HisDataReplayer.h L777-L786（`set_time_range` 注释）与 .cpp L661-L662、L744-L745、L840-L841、L852、L2414
- 相关测试: p04_replayer_config_prepare_test.cpp `cfgTimeRangeIsUsedOnlyWhenUnset`、`prepareAdvancesDateAndTradingDate`
- 输入/前置条件: 按头文件注释传入 14 位 `YYYYMMDDHHMMSS`
- 期望行为: 与文档一致地解析日期与时分秒
- 实际行为: 实现一律用 `/10000` 取日、`%10000` 取时分（12 位 `YYYYMMDDHHMM` 约定），14 位入参会得到 `_cur_date = 2020010209`、`_cur_time = 3000` 这类错位值；同时 `get_min_time()` 与 `get_raw_time()` 返回同一个 `_cur_time`，无法区分 HHMM 与 HHMMSS
- 复现步骤: `set_time_range(20200102093000, 20200108150000)` 后 `prepare()`
- 影响范围: 文档与实现的时间格式不统一，跨语言（wtpy）侧容易配错
- 备注: 修复方向是统一格式并在 init/set_time_range 做位数校验
