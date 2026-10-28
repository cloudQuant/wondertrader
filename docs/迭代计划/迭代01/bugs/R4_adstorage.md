# R4 缺陷登记 — WtDataStorageAD（WtDataWriterAD.cpp / WtDataReaderAD.cpp）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
凡"启用即崩/挂死"的现象，一律在用例里以 `DISABLED_` 登记并写清触发条件。

分级口径：P0 = 静默丢数据/必崩；P1 = 特定输入崩溃或数据错误；P2 = 资源与健壮性；P3 = 语义可疑/死代码。

---

## R4-BUG-02 ｜P0｜LMDB 句柄缓存被 `std::move` 掏空，第三次取用即空指针
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1040`（get_k_db）、`:1060`（get_t_db）；
  `src/WtDataStorageAD/WtDataReaderAD.cpp:578`（get_k_db）、`:602`（get_t_db）
- 现象: 命中缓存时 `return std::move(it->second);` 把 map 里的 `shared_ptr` 移走并返回，
  缓存项随即变成空指针（但 key 仍在）。第 1 次取用返回有效句柄、第 2 次返回"搬家后"的有效句柄、
  **第 3 次起返回空指针**。
- 写入侧后果: `pipeToTicks`（`WtDataWriterAD.cpp:420`）与 `pipeToM1Bars/M5Bars/DayBars`
  （`:454/:487/:520`）都只在 `if (db)` 上做分支，db 为空时**不写、不报错、不打日志** →
  每合约 tick 只落 2 笔、每交易所每周期 K 线只落 2 根，之后全部静默丢失。
- 读取侧后果: `update_cache_from_lmdb`（`WtDataReaderAD.cpp:273`）没有判空，直接
  `WtLMDBQuery query(*db);` → 解引用空 `shared_ptr` → 崩溃。
- 证据用例:
  - `r4_writer_ad_lmdb_test.cpp` → `TickDbStoresTradingDateKeyedRecordsUntilHandleMoveOut`
    （4 笔 tick 只有 2 笔入库，LL_ERROR 计数为 0）、`MinuteBarRolloverPipesIntoMin1Db`
    （4 次滚动只入库 2 根）
  - `r4_reader_ad_slices_test.cpp` → `SecondCodeOnSameExchangeFailsAfterCacheHandleMovedOut`
    （同交易所第二个合约回源失败返回 NULL）
  - `r4_reader_ad_slices_test.cpp` → `DISABLED_AdvancingEtimeAfterTwoHandlesCrashesOnNullDb`
    触发条件：同一交易所/周期读到第 3 次 `get_k_db` 且 etime 仍在前进。
- 建议: 命中分支改 `return it->second;`（4 处）。

## R4-BUG-03 ｜P1｜日线 K 线成交量/成交额重复累加（翻倍）
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:649-652`
- 现象: 同一交易日追加 tick 的 else 分支里 `vol`/`money` 各自被 `+=` 了两次
  （649-650 与 651-652 是重复语句）；m1（754-757）、m5（834-835）分支都只累加一次。
- 后果: 落地日线 `vol`/`money` 为真实值的约 2 倍（首条 tick 除外）。
- 证据用例: `r4_writer_ad_cache_test.cpp` → `BarCacheMergesTicksOfDayMinute5AndMinute1`
  （两笔各 10 手：d1 缓存里是 30/300，m1 是 20/200）；
  `r4_writer_ad_lmdb_test.cpp` → `DayBarRolloverPipesIntoDayDbWithDuplicatedVolume`
  （入库日线 vol=30、money=300）。

## R4-BUG-01 ｜P1｜`_async_task` 全仓库无赋值，写线程是否启动取决于堆残值
- 状态: 待审核
- 位置: 声明 `src/WtDataStorageAD/WtDataWriterAD.h:245`；唯一读取点
  `src/WtDataStorageAD/WtDataWriterAD.cpp:375`；构造函数 `:55-66` 未初始化该成员，
  `init()`（`:73-104`）也未从 params 读取任何 async 开关。
- 现象: `new WtDataWriterAD()` 后 `_async_task` 是未定义的堆残值。若非 0，
  `pushTask` 会新建 `StdThread`（`:387-413`），落库时序从同步变异步，用例断言不可复现；
  同时 `release()`（`:106-114`）在未持锁的情况下 `notify_all()`，
  若工作线程尚未进入 `_task_cond.wait`（`:395`）则丢失唤醒 → `join()` 永久等待。
- 用例侧约束: `r4_ad_fixture.h::new_writer_zeroed()` 在 `new` 之前申请并释放一块
  `sizeof(WtDataWriterAD)` 的全 0 内存，令其复用同一 size class 把残值压成 0（同步分支），
  并以 `broadcastTick` 计数证伪；夹具刻意不调用 `release()` 的线程分支。
- 证据用例: `r4_writer_ad_cache_test.cpp` → `ReleaseWithoutTaskThreadIsInert`（仅覆盖无指针分支）。

## R4-BUG-09 ｜P1｜`groupsize` 缺省为 0 时首笔 tick 即整数除零
- 状态: 待审核
- 位置: `src/WtDataWriterAD.cpp:88`（`_log_group_size = params->getUInt32("groupsize")`，无默认值）
  → `:362`（`_tcnt_map[...] % _log_group_size == 0`）
- 现象: 配置对象不含 `groupsize` 时 `getUInt32` 返回 0，构造函数里的 1000 被覆盖，
  第一笔 tick 的取模即 SIGFPE。
- 证据用例: `r4_writer_ad_lmdb_test.cpp` → `DISABLED_GroupSizeZeroDividesByZeroOnFirstTick`
  触发条件：`cfg` 不带 `groupsize` 且写入任意一笔 tick（启用即崩）。
- 正常分支对照: `r4_writer_ad_cache_test.cpp` → `LogGroupSizeEmitsCountEveryNthTick`。

## R4-BUG-08 ｜P1｜`onMinuteEnd` 对空 K 线缓存取 `back()`、对 `get_rt_cache_bar` 的 NULL 不判空
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:511`、`:530`（`_bars.back()`，非日线与日线分支）；
  `:517-518`、`:535-536`（`rtBar->time` / `rtBar->date` 未判空）
- 现象: 只要 `_bars_cache` 里存在一条"一条 bar 都没读到"的条目（回源失败即会产生），
  分钟结束时就可能在空 `boost::circular_buffer` 上取 `back()`（越界读，UB）；
  而实时缓存里没有该合约时 `get_rt_cache_bar` 返回 NULL，随后被直接解引用。
- 证据用例: `r4_reader_ad_slices_test.cpp` →
  `DISABLED_OnMinuteEndOnBarlessCacheEntryReadsBackOfEmptyBuffer`
  触发条件：先对某合约 `readKlineSlice`（库中无该合约 bar）→ 条目入缓存但为空 →
  再调 `onMinuteEnd`。

## R4-BUG-06 ｜P2｜`readTickSlice` 的 reload_flag 判定被后一个条件覆盖，首次读取退化为无上限区间扫描
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:122-140`（`:127` 置 2，`:135` 立刻覆盖为 1）
- 现象: 首次读取时"容量不足"（应走 `get_lowers` 按 `count` 限量取数）与
  "请求时间前进"两个条件同时成立，`reload_flag` 最终是 1 → 走 `get_range`，
  把 `[上次时间+1, etime]` 区间内的**全部** tick 读进一个只有 `count` 容量的循环缓冲区
  （`count` 限制形同失效，历史越长放大越明显），且 `:171` 的全量分支基本不可达。
- 证据用例: `r4_reader_ad_slices_test.cpp` →
  `FirstTickReadTakesIncrementalRangeAndKeepsLastCount`
  （3 笔入库、请求 2 笔，日志走 "append to cache" 的增量分支，最终只剩后 2 笔）。

## R4-BUG-07 ｜P2｜reader `get_t_db` 存键与查键不一致，tick 库永不命中缓存
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:599`（查 `"exchg.code"`）vs `:617`（存 `_tick_dbs[exchg]`）
- 现象: 写入用交易所名做 key、查找用 `交易所.合约` 做 key → 每次读取都重新
  `WtLMDB(true)::open` 一个环境，旧句柄留在 map 里直到读取器析构（重复开环 + 句柄堆积）。
- 证据用例: `r4_reader_ad_slices_test.cpp` →
  `RepeatReadAtSameEtimeSkipsQueryButReopensDb`（同 etime 复读没有查询日志，
  但 `Tick db of EXR2.rb2301 opened` 仍逐次新增）。

## R4-BUG-05 ｜P2｜`create_directories` 未带 error_code，路径被同名普通文件占用时异常穿出任务体
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1044`、`:1064`；
  `src/WtDataStorageAD/WtDataReaderAD.cpp:582`、`:606`
- 现象: `boost::filesystem::create_directories(path)`（无 `error_code` 重载）在路径上已有
  普通文件时抛 `filesystem_error`；写入侧该调用在 `writeTick` 的任务 lambda 内，
  异常无人接住 → `std::terminate`。
- 证据用例: `r4_writer_ad_lmdb_test.cpp` →
  `DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick`
  触发条件：`{base}ticks/{exchg}/{code}` 预置为普通文件后写入该合约 tick（启用即 terminate）。

## R4-BUG-04 ｜P2｜procFlag==2 补最低价用的是 `max`
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1006-1007`
- 现象: `if (decimal::eq(newTick.low, 0)) newTick.low = max(newTick.price, item._tick.low);`
  —— 与 high 的补齐对称写法应为 `min`；现实现把最低价抬到不低于上一笔最低价。
- 证据用例: `r4_writer_ad_cache_test.cpp` → `ProcFlagTwoAccumulatesTotals`
  （上一笔 low=3900、本笔 price=3800 → 落缓存的 low 是 3900 而不是 3800）。

## R4-BUG-10 ｜P2｜AD 缓存块加载不校验块头，也不校验文件大小
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:116-281`（`loadCache` 四段同构）
- 现象: 只做 `_size = min(_size, _capacity)`（`:136/:177/:218/:259`），
  既不检查 `_blk_flag`/`_type`/`_version`（块头里的 `BT_RT_Cache`、版本号 1 写进去后没人读），
  也不检查映射长度是否真的容得下 `capacity` 个 item；
  截断或跨版本（例如将来 `BarCacheItem` 加字段）的 `.dmb` 会被照单全收，
  重建索引时越界读映射尾部。
- 证据用例: `r4_writer_ad_cache_test.cpp` →
  `ReloadExistingCacheClampsSizeAndRebuildsIndex`（垃圾块头字段被接受、size 夹到 capacity）、
  `FreshInitBuildsFourBlockFilesWithValidADHeader`（写入侧确实写了规范块头，只是没人校验）。
- 说明: 截断文件会触发映射尾部越界读（可能 SIGBUS），不宜作为启用用例，故只登记不落测。

## R4-BUG-11 ｜P2｜`resizeRTBlock` 异常分支返回旧块，调用方继续按新容量写
- 状态: 待审核
- 位置: `src/WtDataWriterAD.cpp:299-311`（catch 后 `return mfPtr->addr();`），
  调用方 `:582/:671/:774/:880` 紧接着写 `_items[idx]`，而 `idx` 已是 `>= 旧 capacity`
- 现象: 扩文件失败（磁盘满等）时并不回退 `_size`，也不中止本笔写入 → 越界写映射区。
- 证据用例: 正常扩容分支由
  `r4_writer_ad_cache_test.cpp` → `TickCacheAtCapacityResizesFileAndKeepsOldItems` 覆盖
  （400+402 条、块头 capacity 回写、原条目仍在）；异常分支需要让 `BoostFile::write_file` 失败，
  本轮无安全的注入点，故只登记。

## R4-BUG-12 ｜P3｜`read_bars_to_buffer` 是死代码且两处必错
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:208-228`
- 现象: ①`:219-220` 把 `get_range` 的右边界也传成了 `lKey`（`rKey` 构造后未用）；
  ②`:225` `memcpy(buffer.data(), ayVals.data(), sizeof(WTSBarStruct)*ayVals.size())`
  把 `std::vector<std::string>` 的内部数组指针当成连续 bar 数据拷贝。
  全仓库无调用点（仅 `WtDataReaderAD.h:302` 声明）。
- 证据用例: 无（私有且无调用方，无法从公开接口到达）；仅登记。

## R4-BUG-13 ｜P3｜日线缓存项的 `time` 字段被写成交易日
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:601`（`barDate = curTick->tradingdate()`）
  与 `:620`（`newBar->time = barDate;`）
- 现象: 日线的 `time` 与 `date` 同值（都是交易日），丢失了动作日期信息；
  m1/m5 用的是 `TimeUtils::timeToMinBar` 的分钟语义，两者不可混用。
  读取侧（`WtDataReaderAD.cpp:286`）只按 `isDay ? date : time` 取 `date`，暂未放大，
  但落库的 `WTSBarStruct.time` 对外部消费者是误导。
- 证据用例: `r4_writer_ad_cache_test.cpp` → `BarCacheMergesTicksOfDayMinute5AndMinute1`
  （`d1._bar.time == 20230103`）。
