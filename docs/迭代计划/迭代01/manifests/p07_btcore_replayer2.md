# p07_btcore_replayer2 交付清单（迭代 01 · 第四波）

- Lane: p07，模块 `src/WtBtCore/HisDataReplayer.cpp`（4635 行，p04 之后仍 2941 行未覆盖 / 现 19.78%）
- 产出目录: `src/TestUnits/coverage/p07_btcore_replayer2/`
- 专用目标: `CoverageTests_btcore`（只编译 `src/WtBtCore/*.cpp` + 链接 WTSTools/WTSUtils）
- 链接纪律: **未引用** WtCore / WtUftCore / WtDtCore 的任何类；只用 `Includes/`（`WTSVariant.hpp`、
  `WTSDataDef.hpp` 内联）、`Share/`（`TimeUtils.hpp`）与 `WtBtCore` 自己的 `IBtDataLoader`/`IDataSink` 接口。
  未改任何 `CMakeLists.txt`，未写仓库 `dist/`（只读复用 `sessions.json`/`stk_comms.json`/`holidays.json`）
- 语法门禁: 3/3 文件通过（0 error 0 warning，Apple clang / macOS arm64），命令见文末
- 生产代码: 未修改，`src/WtBtCore/**` 全程只读
- Bug 记录: `docs/迭代计划/迭代01/bugs/p07_btcore_replayer2.md`，新增 BUG-P07-001…007，状态均「待审核」，
  与 p04 同源的（空CSV越界 BUG-P04-004/005、漏首根 BUG-P04-010、未知会话 NULL 解引用 BUG-P04-007）只引用不重复立案
- 落盘: 全部在 `std::filesystem::temp_directory_path()/p07_*`（含 loader 自动转储的 `.dsb`、自建 `fees_p07.json`），
  SetUp/TearDown 双向 `remove_all`，目录名带 pid 前缀 `p07_min_` / `p07_task_` / `p07_ld_`，互不重叠

## 新增测试文件与覆盖符号

### 1. `p07_minute_replay_test.cpp`（约 350 行，4 个启用用例 + 1 个 DISABLED_）
- `cacheRawBarsFromCSV`(L3737) 的 **KP_Minute1 / KP_Minute5** 装载分支（L3745-3751、L3818-3868）与 min1 目录转储
- `run_by_bars`(L771) 的**非日线分支**：L795-814（分钟 `nextBarTime = time + 199000000000`）、
  L820-850（逐分钟 `calcTradingDate` / 交易日切换 / `_day_cache.clear()`）、L869 `isEndTDate`
- `simTicks`(L1243) 的**分钟线分支** L1248-1323：`_day_cache` 开高低累积、`update_price`、
  每根K线按 pxType 0/1/2/3 四笔 `handle_tick`（逐笔价格断言）
- `simTickWithUnsubBars`(L1389) + `sub_tick`(L3089) + `checkUnbars`(L3167) 联动：
  未订阅合约被自动装进 `_unbars_cache` 并模拟开高低收，但不产生 `handle_bar_close`
- `onMinuteEnd`(L2013) 分钟闭盘分支 L2023-2057 与 `_unbars_cache` 游标推进分支 L2085-2126
- `get_kline_slice`(L2132) 的 **m15 重采样/合并**分支（L2213-2221 取原始 m5 线、L2298-2326
  `WTSDataFactory::extractKlineData` 合并，断言合并后 OHLC/vol 等于三根 m5 之和）+
  「已定位过」的分钟游标推进分支 L2412-2455（回放中反复取切片，长度 1..8 递增）
- `get_cur_price`(L3021) / `get_day_price`(L3034) 的**有数据**路径（逐 bar 断言 open/high/low/price 序列）
- DISABLED_：分钟 CSV 只有表头（0 行）触发 `_bars[0]` / `locate_barindex` 越界（引用 BUG-P04-004/005，不重复立案）

### 2. `p07_task_schedule_test.cpp`（约 300 行，6 个启用用例 + 1 个 DISABLED_）
`run_by_tasks`(L940-L1241) 主体全部首次覆盖（p04 因 BUG-P04-007 把整个函数放进 DISABLED_；
这里显式传 `session="SD0930"`（`dist/common/sessions.json` 里真实存在）后主循环可安全收敛）：
- 日频及以上分支 L949-L1103：`endtime = getNextMinute(task_time, -1)` 对齐、L1042-1046 时间补齐、
  L1048-1058 未触发日的换交易日判定、L1060-1082 触发分支（simTicks×4 + onMinuteEnd + session begin/end）、
  L1084-1101 日期推进与 `nextTime > _end_time` 收尾（补发 session_end + replay_done）
- `TPT_Daily` / `TPT_Weekly` / `TPT_Monthly` / `TPT_Yearly` / `TPT_None` 五个 switch 分支（L978-1033）
- 分钟分支 L1104-L1241：`timeToMinutes`/`minuteToTime` 步进、小节跳跃（1130 → 1305）、
  L1141-1164 `mins >= getTradingMins()` 闭盘、L1167-1202 `getOffsetMins()==0` 的整体换日、L1203-1224 步进与换小节判定
- 断言全部是确定的调度时刻序列（49 次调度、930/1130/1305/1455/1500 等），无随机数、不断言墙钟
- DISABLED_：`session` 传不存在的模板 → L1065 `sInfo->offsetTime` NULL 解引用（BUG-P04-007），
  以及「TPT_Minute 配 offset≠0 会话导致 `nextTime` 永不前进」的死循环形态只在注释里归档，不进启用集

### 3. `p07_tick_loader_test.cpp`（约 400 行，5 个启用用例 + 1 个 DISABLED_）
自建 `StubBtLoader : public IBtDataLoader`（纯内存给数，不依赖存储引擎/动态库/线程）：
- `run_by_ticks`(L741) + `replayHftDatasByDay`(L1769) + `getNextTickTime`(L1536) 的**逐 tick 回放主循环**
  （按日推进、`_cursor/_count` 游标推进、`update_price`、session begin/end、`handle_replay_done`）
- `checkTicks`(L2828) 的 loader 分支（L2852-2855）+ `cacheRawTicksFromLoader`(L3443)（含装载失败清空缓存 L3461-3462）
- `get_last_tick`(L2881) 的**有数据**路径（首次 `UINT_MAX` 定位 L2890-2908、已定位后直接取 L2912）
- `get_day_price`(L3034) 的 **tick 模式分支** L3036-3066（flag 0/1/2/3 + default）与 `get_cur_price` 联动
- `cacheFinalBarsFromLoader`(L3589)：extloader 装载分支 L3670-3702、`isAutoTrans` 的 dsb 转储 L3704-3736、
  dsb 回读分支 L3634-3667（第二次运行断言 `loadFinalHisBars` 调用次数为 0 且闭盘序列完全一致）
- `get_kline_slice`(L2229-2243) 的「先走 extloader」分支，以及装载器无该周期数据时返回 NULL
- `init` 带 loader 时的 `loadStkAdjFactorsFromLoader`(L302) 复权因子回调（L307-329，含 19900101 兜底与排序）
- `loadFees`(L2946) + `calc_fee`(L2983) 的**有数据**路径：按金额（`volscale`）/按手数、offset 0/1/2/非法、
  `apply_fee_minimum`(header L929) 最低收费生效与不生效、未登记品种返回 0
- DISABLED_：`replayHftDatas`(L1907) 固定 `stime` + 串日数据导致游标不推进的死循环，
  以及成交明细守卫 `_cursor == _count` 与其余三个 `_cursor > _count` 不一致的越界读（BUG-P07-007）

## 跳过目标及原因（不虚构断言）

- `cacheRawTicksFromCSV`(L3470-L3588) 的 csv 装载分支：源码 L3509 已经无条件 `return false`，
  下面 ~70 行是注释掉的死代码（L3511-L3584），**启用路径只有 dsb 回读**；构造 `bin/ticks/*.dsb`
  需要 `HisTickBlockV2` + `WTSCmpHelper::compress_data` 的真实压缩产物，属 p05/l04 存储 lane 的夹具，
  本 lane 用 `IBtDataLoader` 覆盖了同一段回放主循环，故未重复造二进制夹具 → 记入 BUG-P07-007。
- `cacheRawBarsFromBin`/`cacheIntegratedFutBarsFromBin`/`cacheAdjustedStkBarsFromBin`/
  `cacheRawTicksFromBin`/`cacheRawOrd*Dtl/Que/TransFromBin`（L3299-L3442、L3900-L4190、L4236-L4483、L4484-L4606）：
  mode=bin 触发 `HisDataMgr` 动态库装载（他人 lane）+ 需要真实 wtd 存储目录，仍未覆盖。
  其中 `cacheAdjustedStkBarsFromBin`(L4236-L4483，~250 行，含复权因子换算)是**下一波最大单体缺口**，
  前置条件是可只读的股票 dsb 夹具。
- `replayHftDatasByDay`/`replayHftDatas` 的 level2（委托明细/委托队列/成交明细）子循环：
  csv 模式下 `checkOrderDetails/Queues/Transactions`(L2702/L2744/L2786) 直接 `return false`（L2721/L2763/L2805），
  只有 bin 模式才可能喂进数据 → 未覆盖。
- `get_tick_slice`(L2470) 等有数据切片：`etime` 参数与游标语义需要真实 tick 时间戳口径，
  为避免写出无据可依的断言，本轮只在 `DISABLED_` 之外的 tick 用例里覆盖了 `get_last_tick`/`get_day_price`；
  `get_tick_slice` 的数据路径留给下一波（本轮只保留 p04 的空缓存断言）。
- 线程/阻塞路径：本轮 0 条启用用例进入 `run()` 之外的线程、join、条件变量或真实行情流；
  `run()` 本身是同步循环（p04 已验证可用），全部启用用例都在毫秒级收敛。

## 验证命令（逐文件已执行，3/3 rc=0）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p07_btcore_replayer2/<file>.cpp
```

## 运行/评审注意（交主控裁决）

- 三条用例依赖仓库只读底座 `dist/common/{sessions.json,stk_comms.json,holidays.json}`，
  且 `run_by_tasks` 用例显式使用 `SD0930` 会话（不是 `DEFAULT_SESSIONID`）；
  若主控把 `dist/` 换成没有 `SD0930` 的底座，`p07_task_schedule_test` 会命中 BUG-P04-007 崩溃，不是测试逻辑问题。
- `p07_task_schedule_test.minuteTaskStepsThroughBothSections` 是最长的一条（49 次调度循环），
  仍是同步收敛，若观测到超时优先怀疑 BUG-P07-003/会话偏移口径，而不是加超时。
- BUG-P07-001/005 的断言按**当前实际行为**写死（每天少一笔、`get_last_tick` 前瞻一笔），
  源码修复后需要同步收紧，测试里已就地注明。
- 未使用 `EventNotifier`、`makeLocalOrderID`、`HisDataMgr`，不触发 MQ/动态库/线程。
- 剩余可继续榨取的单体缺口（按行数从大到小）：`cacheAdjustedStkBarsFromBin`(250)、
  `cacheIntegratedFutBarsFromBin`(290)、`get_kline_slice` 的 bin 侧装载、`simTickWithUnsubBars` 的分钟分支(L1397-L1440)。
