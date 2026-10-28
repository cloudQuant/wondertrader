# R4 用例清单 — WtDataStorageAD（adstorage / CoverageTests_adstorage）

- lane: R4（路 4，`分派范围-20261027T113454Z.md`）
- 生产文件: `src/WtDataStorageAD/WtDataWriterAD.cpp`（1073 行）、`src/WtDataStorageAD/WtDataReaderAD.cpp`（619 行）
- 用例目录: `src/TestUnits/coverage/p05_datastorage2/`（前缀 `r4_`，只进 `CoverageTests_adstorage`）
- 规模: 44 条 `TEST_F` = **40 启用 + 4 `DISABLED_`**（3 个 .cpp + 1 个共用替身头）
- 缺陷登记: `docs/迭代计划/迭代01/bugs/R4_adstorage.md`（13 条，全部 `- 状态: 待审核`）
- 门禁（真实构建口径，逐条通过，3/3）:

```sh
cd build-cov
CMD=$(ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/p05_datastorage2/p05_minbar_key_overflow_test.cpp.o | tail -1)
for F in r4_writer_ad_cache_test r4_writer_ad_lmdb_test r4_reader_ad_slices_test; do
  P=$PWD/../src/TestUnits/coverage/p05_datastorage2/$F.cpp
  eval "$(printf '%s' "$CMD" | sed -E "s|-o [^ ]+ -c [^ ]+|-fsyntax-only $P|")"
done
```

## 文件与替身

| 文件 | 作用 |
|---|---|
| `p05_datastorage2/r4_ad_fixture.h` | `IDataWriterSink` / `IBaseDataMgr` 替身、tick 与块文件构造、`.dmb` 回读、零填充 `new`（规避 R4-BUG-01） |
| `p05_datastorage2/r4_writer_ad_cache_test.cpp` | 写入器块头构造/加载/扩容 + tick 缓存状态机 + K 线缓存合并（19 条） |
| `p05_datastorage2/r4_writer_ad_lmdb_test.cpp` | 写入器四条 LMDB pipe、HFT 键序、扩展 dumper、mapsize（11 条） |
| `p05_datastorage2/r4_reader_ad_slices_test.cpp` | 读取器 init/tick 切片/K 线切片/RT 缓存补线/onMinuteEnd（14 条） |

`WtDataWriterAD.cpp` / `WtDataReaderAD.cpp` 的行段均按当前工作区文件实测。

## 一、`r4_writer_ad_cache_test.cpp` → `src/WtDataStorageAD/WtDataWriterAD.cpp`

| 用例（启用除注明外） | 生产文件:行段 | 关键断言 |
|---|---|---|
| FreshInitBuildsFourBlockFilesWithValidADHeader | `:73-104`、`:116-157`、`:159-198`、`:200-239`、`:241-280` | 四个 `.dmb` 尺寸 = `sizeof(头)+400*sizeof(item)`；`_blk_flag`==BLK_FLAG、`_type`==BT_RT_Cache、`_version`==1、`_size`==0、`_capacity`==400；未建 ticks/min1/day 目录 |
| DisableFlagsKeepTickCacheButTouchNothingElse | `:90-93`、`:327-371`、`:352-353`、`:570/:659/:762` 的开关判定 | tick 缓存仍写入并广播 1 次；三段 bar 块 `_size` 仍为 0；无 `ticks/min1/min5/day` 目录；`canSessionReceive` 收到 `R4_DAY` |
| ReloadExistingCacheClampsSizeAndRebuildsIndex | `:122-157`、`:136`（`min(_size,_capacity)`）、`:150-155`、`:191-197` | size=5/capacity=2 被夹成 2；按落盘 item 的 `exchg.code` 重建索引 → `getCurTick` 取到 3811/3812；未入缓存的代码返回 NULL |
| TickCacheAtCapacityResizesFileAndKeepsOldItems | `:283-325`（resizeRTBlock）、`:873-882` | 文件从 2 项长到 402 项（`2 + CACHE_SIZE_STEP_AD`）；块头 `_capacity`==402、`_size`==3；旧下标条目仍在；LL_INFO 含 "Tick Cache resized to 402 items" |
| ProcFlagZeroOverwritesCacheVerbatim | `:950-981`（同日 else 分支 `procFlag==0`） | 第二笔原样覆盖：price 3910 / volume 20 / total_volume 120 / diff_interest 保持 1 |
| ProcFlagOneTurnsTotalsIntoDeltas | `:982-989` | volume=130-100=30、turn_over=350、diff_interest=60-50=10 |
| ProcFlagOneOnNewTradingDateRebaselinesCache | `:899-918`、`:946-948` | 跨交易日：volume 回填 total、diff_interest=oi-pre_interest；`item._date` 前进到 20230104；LL_INFO "First tick of new tradingday" |
| ProcFlagTwoAccumulatesTotals | `:990-1010`（含 `:1006-1007` 的 max 缺陷 R4-BUG-04） | total_volume 120 / total_turnover 1200 / open=high=3800 / low=3900（现实现抬价） |
| IdenticalActionTimeIsBumpedBy200Ms | `:972-975` | 同毫秒且总量不减 → 缓存 `action_time` 93000200，再同毫秒 → 93000400 |
| OlderTradingDateThanCacheIsRejected | `:891-895`、`:348-349`（break） | 交易日回退笔被丢：广播仍 1 次、缓存仍是 20230104/4000；三段 bar `_size` 仍 0；LL_INFO 含 "is less than cached tradingday" |
| OffsetDateAheadOfTradingDateIsAbandoned | `:954-960` | LL_ERROR "has an exception, abandoned"；广播不增加 |
| DecreasingTotalVolumeIsAbandoned | `:961-966`、`:961`（`&& procFlag != 2`） | procFlag=0 递减笔被丢并告警；procFlag=2 的同条件笔正常入库并广播 |
| BarCacheMergesTicksOfDayMinute5AndMinute1 | `:549-567`、`:570-656`（d1）、`:658-759`（m1）、`:761-839`（m5） | d1 `time`==交易日；m1 `time`==3301030931、m5==3301030935；同分钟第二笔 m1 vol=20/money=200 而 d1 vol=30/money=300（R4-BUG-03）；换分钟后 m1==3301030932 且建出 `min1/EXBC` |
| LastOfSectionTickRollsBackOneMinute | `:562-565`、`:691-698`、`:793-800` | 150000500 的 tick → m1 bar `time`==3301031500；disableday 下 d1 无该合约条目 |
| TickOutsideSessionUpdatesTickCacheOnly | `:555-557`（`timeToMinutes`==INVALID 早退） | 20:00 tick：广播 1 次、tick 缓存命中，三段 bar 块 `_size` 仍 0 |
| WriteTickGuardsNullTickNullContractAndClosedSession | `:329-330`、`:338-339`、`:344-345` | NULL tick→false；`setContractInfo(NULL)`→不广播不入缓存；`canSessionReceive=false`→不入缓存；放行后正常入 |
| GetCurTickCoversEmptyUnknownMissAndHit | `:842-860` | 空代码/未知合约/未命中均为 NULL；命中返回副本（price/oi/tradingdate 一致） |
| ReleaseWithoutTaskThreadIsInert | `:106-113`（`_task_thrd` 为空分支） | `release()` 后仍可同步写入并广播（不 join、不起线程） |
| LogGroupSizeEmitsCountEveryNthTick | `:88`、`:360-365` | groupsize=2：第 1 笔无分组日志，第 2 笔 LL_INFO 含 "ticks received from exchange" 与交易所名 |

（本文件 19 条全部启用；两条 `DISABLED_` 写在 `r4_writer_ad_lmdb_test.cpp`。）

## 二、`r4_writer_ad_lmdb_test.cpp` → `src/WtDataStorageAD/WtDataWriterAD.cpp`

| 用例 | 生产文件:行段 | 关键断言 |
|---|---|---|
| TickDbStoresTradingDateKeyedRecordsUntilHandleMoveOut | `:416-433`、`:425-429`、`:1055-1073` | 键 = `LMDBHftKey{exchg,code,大端交易日,大端偏移时间}`；值 = 整条 `WTSTickStruct`；4 笔只入库 2 笔且 LL_ERROR==0（静默丢失，R4-BUG-02） |
| HftKeysSortChronologicallyAcrossTradingDates | `:425-427` | 20230103/1459 的键字节序在 20230104/0930 之前；`_date` 等于 `reverseEndian(交易日)` 而非原值 |
| EachCodeGetsItsOwnTickDbDirectory | `:1055-1073`、`:1063-1065` | `ticks/EXT3/rb2301`、`ticks/EXT3/rb2302` 各自一库且有 `data.mdb`；两库分别 1 条、价 3901/3902 |
| MinuteBarRolloverPipesIntoMin1Db | `:483-499`、`:1016-1053`（KP_Minute1 分支） | 5 个分钟 → 4 次滚动 → 只入库 2 根；键 `LMDBBarKey{exchg,code,大端 3301030931/3301030932}`；bar 的 OHLC 与被滚动那根一致 |
| DayBarRolloverPipesIntoDayDbWithDuplicatedVolume | `:450-466`、`:1030-1033`（KP_DAY 分支） | 跨交易日才出管：`day/EXD5` 1 条、键 = 大端 20230103；bar vol=30、money=300（R4-BUG-03）、high/low/close 正确 |
| MinuteFiveBarPipesIntoMin5Db | `:516-532`、`:1025-1028`（KP_Minute5 分支） | `min5/EXM5` 1 条，键 = 大端 3301030935（(30/5)*5+5） |
| ExtendedDumperReceivesPipedTicksAndBars | `:435-447`、`:468-480`、`:501-513`、`:534-546`（含 `dumper==NULL` 的 continue） | 2 笔 tick + m1/m5 各 1 根转储；`period_count("d1")==0`；失败 dumper 产生 4 条 LL_ERROR 且含 "via extended dumper"；NULL dumper 不崩 |
| ExplicitMapSizesStillPipeData | `:95-99`、`:1042-1049`、`:1062-1069` | 带 `tickmapsize/klinemapsize` 仍能入库；bar 全禁用时不建 min1/day 目录 |
| BumpedActionTimeIsPersistedToTickDb | `:424-429` + `:972-975` | 同毫秒笔的键 time 与值 `action_time` 都是 +200 后的 93000200 |
| DISABLED_GroupSizeZeroDividesByZeroOnFirstTick | `:88`（无默认值）+ `:362` | 触发条件：`cfg` 不含 `groupsize` → `_log_group_size=0` → 首笔 tick 取模即 SIGFPE（R4-BUG-09） |
| DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick | `:1062-1069`（`:1064` create_directories）+ `:419-420` | 触发条件：`{base}ticks/{exchg}/{code}` 预置为普通文件后写入该合约 tick → `filesystem_error` 穿出任务体 → terminate（R4-BUG-05） |

## 三、`r4_reader_ad_slices_test.cpp` → `src/WtDataStorageAD/WtDataReaderAD.cpp`

| 用例 | 生产文件:行段 | 关键断言 |
|---|---|---|
| InitLogsRootOnlyWhenCfgProvided | `:60-78`（`:67-68` 早退 / `:70-77`） | `init(NULL,…)` 零日志；带 cfg 后 LL_INFO 含 "WtDataReaderAD initialized" 且路径带结尾斜杠 |
| FirstTickReadTakesIncrementalRangeAndKeepsLastCount | `:80-206`（`:122-140` reload_flag、`:147-163` get_range、`:188-205` 切片） | 请求 2 笔命中区间内 3 笔 → 只留后 2 笔，且因回绕拆成 2 个块（3910 / 3920）；DEBUG 出现 "Tick db of EXR1.rb2301 opened" |
| RepeatReadAtSameEtimeSkipsQueryButReopensDb | ``:122-140`、`:147-163`、`:597-619` | 同 etime 复读无 "append to cache" 日志但 DEBUG 计数仍增加（R4-BUG-07）；etime 前进 1 分钟只追加 932 那笔 → 切片 2 块各 1 条 |
| TickReadAgainstEmptyDbYieldsEmptySlice | `:143-145`、`:188-195` | 空库→切片非空但块大小 0；目录自建 `data.mdb` |
| KlineFirstReadLoadsLowerBarsAndChecksLatest | `:368-435`、`:230-267`（get_lowers + 左边界过滤）、`:269-279`（get_uppers 命中同时间不追加） | 3 根 bar 一块返回（时间 3301030931…933，close 3920.5）；DEBUG "bars of EXK1.rb2301 loaded to cache" |
| SecondCodeOnSameExchangeFailsAfterCacheHandleMovedOut | `:242-244`、`:576-578` | 同交易所第二个合约 `readKlineSlice` 返回 NULL（空句柄）；同合约同 etime 复读仍返回 2 根 |
| KlineAppendsRtCacheBarWhenLmdbLags | `:305-366`（懒加载 `cache_m1.dmb` + 建索引）、`:444-464` | 缓存 bar（3301030940）被追加 → 挤出 09:31，切片两块：[932,933] + [940]；DEBUG "updated from cache instead of lmdb" |
| KlineWithoutRtCacheEntryKeepsLmdbBarsOnly | `:346-362`（未命中返回 NULL） | 只有库里 2 根，无补线日志 |
| DayKlineUsesDateKeyAndDaySubdir | `:217-220`（date 语义）、`:425-432`（isDay 时 etime 取自然日） | `day/EXD9` 2 条按 `bar.date` 20230103/20230104 升序返回 |
| ZeroEtimeFallsBackToSinkClock | `:87-101` | etime=0 → 用 `get_date/get_min_time/get_secs` 组装；时钟前进后增量窗口 0 命中 → 仍是那笔 10:15:30 |
| OnMinuteEndGatesRepeatedAndOlderTimes | `:496-505`、`:505-546`（空缓存循环）、`:548-551` | 同/更旧时间不重复通知；`on_all_bar_updated` 收到 1000/1001；无 `on_bar` 回调 |
| UnknownPeriodHasNoDbAndReturnsNull | `:554-574`（`else return WtLMDBPtr()`）、`:242-244` | KP_Hour → 无库 → 回源 false → 切片 NULL |
| DISABLED_AdvancingEtimeAfterTwoHandlesCrashesOnNullDb | `:272-273` | 触发条件：同交易所同周期第 3 次取库且 etime 前进 → `WtLMDBQuery query(*db)` 解引用空句柄崩溃（R4-BUG-02） |
| DISABLED_OnMinuteEndOnBarlessCacheEntryReadsBackOfEmptyBuffer | `:511`、`:517-518`、`:530`、`:535-536` | 触发条件：某合约进了 `_bars_cache` 但 0 条 bar，再 `onMinuteEnd` → 空 `circular_buffer::back()`（UB）；`get_rt_cache_bar` 返回 NULL 也被直接解引用（R4-BUG-08） |

## 四、未覆盖与原因（诚实清单）

- `WtDataWriterAD.cpp:22-32` / `WtDataReaderAD.cpp:19-30`（日志模板）与 `:34-50` / `:32-45`
  （`createWriter/deleteWriter/createDataReader/deleteDataReader` 四个 `extern "C"` 入口）：
  与 `WtDataStorage` 同名，只能留在 adstorage 目标内；本轮未新增 C 入口用例，
  由既有 p05 用例与工厂调用面覆盖（报表口径为 8 个 C 入口各命中 2 处）。
- `WtDataWriterAD.cpp:307-311`（resizeRTBlock 的 catch 分支）、`:387-413`（工作线程体）：
  需要故障注入 / 起线程，违反"不起线程"硬规则 → 只登记 R4-BUG-01 / R4-BUG-11。
- `WtDataWriterAD.cpp:864-867`（`_tick_cache_block` 为空的错误分支）：
  `loadCache` 之后必非空，公开接口无法构造 → 未覆盖。
- `WtDataReaderAD.cpp:208-228`（`read_bars_to_buffer`）：私有且全仓库无调用点 → 死代码，只登记 R4-BUG-12。
- `WtDataWriterAD::writeOrderQueue/writeOrderDetail/writeTransaction`：AD 未覆写（基类直接 return false），
  不产生本文件覆盖增量。
