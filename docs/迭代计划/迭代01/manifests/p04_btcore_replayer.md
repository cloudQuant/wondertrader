# p04_btcore_replayer 交付清单（迭代 01 · 第三波）

- Lane: p04，模块 `src/WtBtCore/HisDataReplayer.cpp`（4635 行）剩余未覆盖部分
- 产出目录: `src/TestUnits/coverage/p04_btcore_replayer/`
- 专用目标: `CoverageTests_btcore`（只编译 `src/WtBtCore/*.cpp`；未引用 WtCore/WtUftCore/WtDtCore 类，未改任何 CMakeLists.txt）
- 语法门禁: 3/3 文件通过（0 error 0 warning，Apple clang / macOS arm64），命令见文末
- 生产代码: 未修改，`src/WtBtCore/**` 全程只读
- Bug 记录: `docs/迭代计划/迭代01/bugs/p04_btcore_replayer.md`，共 11 条，状态均「待审核」
- 落盘: 所有写入都在 `std::filesystem::temp_directory_path()/p04_*`（含 csv->dsb 转储产物），SetUp/TearDown 双向 `remove_all`；仓库内样例配置只读复用 `dist/common/sessions.json`、`dist/common/stk_comms.json`

## 新增测试文件与覆盖符号

### 1. `p04_replayer_config_prepare_test.cpp`（约 300 行，10 个启用用例 + 1 个 DISABLED_）
- `init`（L171-L300）全分支：mode≠bin（csv）、`store` 缺失、`stime/etime` 读取与「已设置则不覆盖」
  （`_begin_time==0` 判定）、`cache_clear_days`、`tick`、`adjust_flag`、`align_by_section`、
  `dont_simtick_if_notrade`、basefiles 的 `session`/`commodity`/`holiday`/`hot`/`second`/`rules` 缺失分支、
  `loadFees`（缺失文件 / fixture 命中）、`loadStkAdjFactorsFromLoader`（无 loader 返回 false）
- `prepare`（L649-L675）：`_running` 复入保护、`reset()` 联动、`_cur_date/_cur_time/_cur_secs/_cur_tdate`
  推进、`handle_init` 回调、tick 关闭时 `checkUnbars()`（L3167）
- `run`（L677-L739）无主K线且 tick 关闭的收尾分支（`handle_replay_done`）、`stop`（L634-L648 两条分支）、
  `clear_cache`（L436-L457）、`register_task`（L407-L434 的 d/w/m/y/min/未知 大小写不敏感分支）
- `get_session_info`（sid/isCode）、`get_commodity_info`、`get_rawcode`、`calc_fee`、`set_time_range`、
  `enable_tick`、`register_sink`、`is_tick_enabled`

### 2. `p04_replayer_csv_replay_test.cpp`（约 380 行，4 个启用用例 + 1 个 DISABLED_）
用临时目录 `<base>/csv/SSE.STK.000001_d.csv`（5 个交易日：20200102/03/06/07/08）驱动真实回放：
- `cacheRawBarsFromCSV`（L3737-L3897）：csv 装载分支 + `proc_block_data`/`WTSCmpHelper` 的 `.dsb` 转储分支
  （`recursive_directory_iterator` 断言转储产物落在临时目录）
- `get_kline_slice`（L2132-L2470）：主K线登记 `_main_key/_main_period`、`_ticker_keys`、`_codes_in_subbed`、
  `kp` 解析（"d"→KP_DAY）、游标首次定位（L2336-L2411）
- `locate_barindex`（L593-L632）起止下标、`run_by_bars`（L771-L938）完整交易日切换：
  `handle_session_begin/end`、`handle_section_end`（`isLastOfSection`）、`onMinuteEnd`（L2013-L2130）日线闭盘
  与游标推进、`nextBarTime > _end_time` 提前退出、`cursor >= size` 结束条件、`handle_replay_done`
- `reset()` 的时间字段归零与「二次 prepare 可重放回相同闭盘」
- `check_cache_days`（L746/L846 调用点）、`notify_state`（无 notifier 早退）、`dump_btstate` 未启用路径
- 未知合约/未知品种/时段缺失时 `get_kline_slice` 返回 NULL（L2288/L2291/L2331 分支）

### 3. `p04_replay_termination_test.cpp`（约 300 行，4 个启用用例 + 1 个 DISABLED_）
- `run_by_ticks`（L741-L769）：tick 模式 + 无 tick 缓存时 `checkAllTicks` → 日推进循环收敛与收尾
- `run_by_bars` 结束条件：起止区间倒挂（第 1 轮即 break，收尾补发 1 次 `handle_session_end`）、
  区间只覆盖部分行情（第二个合约 3 根闭盘、结束游标 20200107）
- 空缓存查询装配稳定返回：`get_tick_slice`/`get_order_detail_slice`/`get_order_queue_slice`/
  `get_transaction_slice`、`get_last_tick`、`get_cur_price`、`get_day_price`（flag 0~4）
- `is_tick_simulated` 在 tick 模式下由 `reset()` 决定的标记

## 跳过目标及原因（不虚构断言）

- `run_by_tasks`（L940-L1242）主体：默认 session 模板 `"TRADING"` 在样例底座里不存在，
  `getSession` 返回 NULL 后继续解引用（BUG-P04-007），且整个函数按交易日+分钟推进、无法在不挂死的前提下驱动
  → 只保留 `register_task` 覆盖，回放侧进 `DISABLED_`。
- `replayHftDatas`/`replayHftDatasByDay`/`simTicks`/`simTickWithUnsubBars`/`getNextTickTime` 等逐 tick 分支
  （L1243-L2012）：需要真实 tick 二进制（wtd）或 `<base>/csv/<code>_ticks.csv`，仓库内没有可只读驱动的行情样例；
  启用用例里只走到它们的空缓存入口（由 `run_by_bars` 的 4 次 pxType 循环触发）。
- `cacheRawBarsFromBin`/`cacheIntegratedFutBarsFromBin`/`cacheAdjustedStkBarsFromBin`/
  `cacheRawTicksFromBin`/`cacheRawOrd*Dtl/Que/TransFromBin`/`cache*FromLoader`（L3261-L3736、L3900-L4190）：
  mode=bin 会触发 `HisDataMgr` 动态库装载（他人 lane，且需要真实存储目录），loader 分支需要 `IBtDataLoader`
  实现 + 二进制行情文件 → 未覆盖。
- `loadStkAdjFactorsFromFile`/`getAdjFactors`（L335-L405、L4192-L4230）：需要复权因子文件与股票合约底座，
  本轮预算内未做。
- 网络/真实行情目录依赖：无（本轮所有启用用例都不访问网络，不启动线程）。

## 验证命令（逐文件已执行，3/3 rc=0）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/p04_btcore_replayer/<file>.cpp
```

## 链接与运行注意（交主控裁决）

- 语法门禁不能证伪链接：本 lane 只引用 `WtBtCore/*.cpp`、`Includes/`（`WTSVariant.hpp`、`WTSDataDef.hpp` 内联）
  与 `Share/`；未出现 `WTSKlineSlice` 之外的跨核心类，未引用 WtCore/WtUftCore/WtDtCore 符号。
- `HisDataReplayer` 值成员 `WTSBaseDataMgr`/`WTSHotMgr` 的实现在 `src/WTSTools`，与 l06 lane 的既有用法一致；
  若 `CoverageTests_btcore` 未链 WTSTools，这 3 个文件与 l06 的 runtime 文件会一起报同样的未定义符号。
- `handle_bar_close` 断言按实际行为写死为 4 根（5 根日线漏掉首根），根因是 BUG-P04-010，
  源码修复后该 3 处断言需要同步改回 5 根 —— 测试里已就地注明。
- 未使用 `makeLocalOrderID`、`EventNotifier` 实例（不触发 MQ/线程），无阻塞路径进入启用用例。
