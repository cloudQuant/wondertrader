# R23_storage（r23_storage4）交付清单 — WtDataStorage 读/写「公开入口剩余分支」

- 车道: R23（主目标二进制 `CoverageTests`；`src/TestUnits/coverage/CMakeLists.txt` 的 GLOB_RECURSE(CONFIGURE_DEPENDS) 与 `MAIN_PLUGIN_TUNS` 自动收新目录，**本车道未改任何 CMakeLists**）
- 靶子: `src/WtDataStorage/WtRdmDtReader.cpp`、`src/WtDataStorage/WtDataReader.cpp`、`src/WtDataStorage/WtDataWriter.cpp`
- 生产代码改动: **无**（`src/**` 只读；只新增 `src/TestUnits/coverage/r23_storage4/` 与两份文档）
- 用例数: **49 个 TEST_F（启用 46 + `DISABLED_` 3）**，断言宏 ≈ 290 条
- 简报 `api-brief-R23.md` 混进来的 `t_TraderFemas/t_TraderXTP/t_ParserXTP/t_CTPOptLoader/t_ParserXeleSkt` 目标不在本轮范围，按派工忽略

## 一、动手前做的「已有覆盖普查」（决定本车道写什么）

`grep -rn 'WtRdmDtReader\|WtDataReader\|WtDataWriter' src/TestUnits/coverage --include=*.cpp`（排除 r23_storage4）命中 24 个 TU，逐个读注释头 + manifest 后的分工表：

| 已有 lane / 目录 | 已经覆盖到的函数（不重做） | 它自己写明的留白原因 |
|---|---|---|
| `l04_datastorage` | 全局版 `proc_block_data`、块头/版本矩阵、LMDB key 布局、`WtBtDtReader` | — |
| `p05_datastorage2`（→ `CoverageTests_adstorage`） | `WtRdmDtReaderAD`/`WtBtDtReaderAD` 全套 | 明确「`WtDataStorage` 本轮未新增覆盖」；排除 writer 线程 + 命名共享内存 |
| `p09_storage_readers` | rdm：`init(NULL)`/`indexBarFromCacheByRange`/`ByCount`/`readBarsFromCacheByRange`/`clearCache`/`loadStkAdjFactorsFromFile`/`getAdjFactorByDate`；rd：`init`/复权因子全链 | 用 `#define private public`；跳过 RT 共享内存块、5s 轮询线程、`cacheHisBarsFromFile` 族 |
| `q03_storage3` | rdm：`readTickSliceByDate`/`readTickSliceByRange`/`cacheHisBarsFromFile` 普通分支/`readKlineSliceByRange`/`getRTTickBlock`/`getRTKilneBlock`；rd：`readTickSlice`/`readOrd*Slice`/`getRTTickBlock` | 禁宏（chdir 沙箱）；**点名跳过 `readTickSliceByCount`(:2395)、`readKlineSliceByCount`(:2219)**、HOT/复权分支、writer 私有全链 |
| `r1_storage` | rdm：`readOrdQueSliceByRange`/`readOrdDtlSliceByRange`/`readTransSliceByRange` 的 his 首读与校验；rd：`readKlineSlice`(day/min5/min1+rt 拼接) | OrdDtl/Trans 的「二次读缓存」踩缺陷不固化；`WtDataWriter` 全文件 0 新增 |
| `p10_storage_writer` | writer：`dump_day_data`/成员版 `proc_block_data`/`getTickBlock`/`getKlineBlock`/`getTrans|OrdQue|OrdDtlBlock`/`pipeToTicks`/`releaseBlock`/`release`/`isSessionProceeded` | **不调 `init()`**（起线程 + 300s 分支撞 HANG 门禁）→ `loadCache`/`updateCache`/`getCurTick`/`resizeRTBlock`/`procTick`/`pipeToKlines`/`procQueue|Order|Trans`/`proc_loop`/`dump_bars_to_file`/`dump_bars_via_dumper` 全部留白；靠 `#define private public` 才拿到私有成员 |

结论：写侧的「`init()` 之后才能到达的公开入口」（`writeTick/writeOrderQueue/writeOrderDetail/writeTransaction/transHisData/getCurTick`）四波都没碰；读侧 `readTickSliceByCount`/`readKlineSliceByCount` 是 q03 点名的最大单块留白。本车道只做这两块，不重做已覆盖分支。

## 二、新增文件

| 文件 | 用例 | 主题 |
|---|---|---|
| `r23_writer_fixture.h` | — | `IBaseDataMgr`/`IDataWriterSink`/`IHisDataDumper` 替身 + json 参数落盘 + `.dmb/.dsb` 读回辅助 |
| `r23_writer_tick_cache_test.cpp` | 15 启用 + 1 DISABLED | `init`/`loadCache`/`updateCache`/`getCurTick`/`writeTick→procTick`/`resizeRTBlock`/`pipeToTicks(savelog)` |
| `r23_writer_kline_hft_test.cpp` | 14 启用 | `pipeToKlines` min1/min5 合成全分支 + `procQueue/procOrder/procTrans` + disable/canSessionReceive 拒绝面 |
| `r23_writer_closing_test.cpp` | 8 启用 + 1 DISABLED | `transHisData` → `proc_loop` → tick/trans 迁移、`dump_bars_to_file`、`dump_bars_via_dumper`、marker 往返 |
| `r23_rdm_count_slice_test.cpp` | 12 启用 + 1 DISABLED | `readTickSliceByCount`/`readKlineSliceByCount`/`indexBarFromCacheByCount`（复用 q03 夹具，只读不改） |

白盒手法：**没有任何访问权限宏**。写侧全部走 `init`+公开虚函数；`dump_bars_via_dumper` 需要的 `_dumpers` 是 `IDataWriter` 的 **protected** 成员，用一个薄派生 `class R23Writer : public WtDataWriter { void add_dumper(...) }` 开放（派生类访问 protected 是语言规则，不是绕权限）。读侧沿用 q03 的「`_base_dir` 留空串 + chdir 沙箱」。

## 三、逐用例 → 生产文件:行段

### `r23_writer_tick_cache_test.cpp` → `src/WtDataStorage/WtDataWriter.cpp`

| 用例 | 行段 |
|---|---|
| InitCreatesCacheDmbSizedByContractCount | `init` L117-175（参数解析、`fs::create_directories` L126、`cache` 默认名 L128-129）+ `loadCache` L267-300 新建分支 + 收尾日志 L170-173 |
| InitReadsMarkerSectionAndReportsProceededByDate | `init` L154-164（`IniHelper::readSecKeyValArray("markers")`）+ `isSessionProceeded` L107-114 |
| LoadCacheReindexesItemsFromExistingFile | `loadCache` L275-289 + L301-309（既存在文件的 `_tick_cache_idx` 重建）+ `updateCache` L1452-1454 续槽 |
| GetCurTickRejectsEmptyCodeAndUnknownAndUncached | `getCurTick` L1420 / L1424 / L1430-1431 / L1433-1435 |
| WriteTickNullAndUnknownContract | `writeTick` L368-369 + `procTick` L383-385（`ct==NULL` break） |
| FirstTickOfTradingdaySeedsCacheAndRaisesNewHighLow | `procTick` L383-404 + `updateCache` L1446-1454、L1467-1471、L1476-1505（新交易日 + `setLimitFlag(1\|2)`）+ `pipeToTicks` L626-649 |
| ProcFlagOneDerivesIncrementalVolumeTurnoverInterest | `updateCache` L1481-1490（procFlag==1 首笔取总量）、L1528-1561（增量派生 + 新高新低判定 L1542-1547） |
| TickWithOlderTradingdayIsDroppedBeforeAnyWrite | `updateCache` L1468-1472（交易日倒退 return false）+ `procTick` L394-395（break 掉后续 broadcast） |
| NightSessionOffsetDateAheadOfTradingdayAbandonsTick | `updateCache` L1510-1516（`getOffsetDate` 超前 → LL_WARN abandoned）；夹具用 offset=180 的夜盘时段（`WTSSessionInfo::getOffsetDate` L117-134） |
| TotalVolumeRollbackAbandonsTick | `updateCache` L1517-1522（总量回退 → LL_WARN）|
| SameOrEarlierTimestampGetsCentisecondBumpChain | `updateCache` L1528-1536（`left_msecs` 公式，5 连击覆盖夹 200 / +50 / +0 三种取值）|
| CacheGrowsByStepWhenCapacityExhausted | `updateCache` L1455-1459 + `resizeRTBlock` L313-363（追加 + 重映射 + 回写 capacity，CACHE_SIZE_STEP=200）|
| SaveTickLogWritesCommaSeparatedRows | `getTickBlock` L972-978（`_save_tick_log` 开 csv 流）+ `pipeToTicks` L651-665（12 列 csv）|
| DisableTickStillFeedsCacheAndKlines | `procTick` L398-399（`_disable_tick`）+ L402（`pipeToKlines` 不受该开关影响）|
| SessionNotReceivingStopsEverythingBeforeCache | `procTick` L390-391（`canSessionReceive` 过滤早于 updateCache）|
| LogGroupSizeControlsReceiptCounterLogging | `procTick` L406-412（`cnt % _log_group_size`）|
| DISABLED_SecondInitOnSameWriterTerminatesProcess | `init` L168 + `check_loop` L1619-1621 + `release` L177-215（不 join `_proc_chk`）→ `~std::thread` joinable ⇒ `std::terminate`（BUG-R23-001）|

### `r23_writer_kline_hft_test.cpp` → `src/WtDataStorage/WtDataWriter.cpp`

| 用例 | 行段 |
|---|---|
| Min1BarIsLabelledWithMinuteEnd | `pipeToKlines` L1058-1114（时间→分钟→分末标签）+ L1123-1150（新建 bar 全字段）+ `getKlineBlock` L1320、L1327-1331、L1405（capacity=getTradingMins） |
| SecondTickSameMinuteMergesOhlcAndAmounts | `pipeToKlines` L1152-1188（合并分支：open/low 非零、high/low 取极值、vol/money 累加、hold 覆盖 add 累加）|
| Min5BucketsEveryFiveMinutes | `pipeToKlines` L1193-1256（min5 桶 `(minutes/5)*5+5`）+ `getKlineBlock` L1332-1336（`totalMins /= 5`）|
| LastMinuteOfSectionFallsIntoSectionCloseLabel | `pipeToKlines` L1080-1083（`isLastOfSection` → `minutes--`，`WTSSessionInfo::isLastOfSection` L454-466）|
| ZeroPriceOpeningTickIsBackfilledByNextTick | `pipeToKlines` L1161-1167（`open==0` / `low==0` 补写）+ `updateCache` L1542-1547（新高新低组合）|
| InvalidActionTimeOnlyWarnsAndKeepsBars | `pipeToKlines` L1070-1075（`timeToMinutes`==INVALID → LL_WARN 早退）+ `updateCache` L1528-1535（同链上 action_time 变 200）|
| SkipNotradeBarSuppressesBarGeneration | `pipeToKlines` L1058-1064（`_skip_notrade_bar`）|
| SkipNotradeTickMergesOnlyTradingTicks | `pipeToKlines` L1152、L1258（`_skip_notrade_tick && tickNoTrade` 不合并）|
| MinPriceModeOneWritesBidAskIntoHoldAddUnion | `pipeToKlines` L1141-1145（`_min_price_mode==1` 写 bid/ask）；联合体见 `Includes/WTSStruct.h` L114-124 |
| OrderQueueWritesRtBlockAndCountsReceipts | `writeOrderQueue` L416-427 + `procQueue` L429-467 + `getOrdQueBlock` L668-734 |
| OrderDetailAndTransactionWriteTheirOwnRtBlocks | `writeOrderDetail` L470-481 + `procOrder` L483-522 + `writeTransaction` L524-535 + `procTrans` L537-576 + `getOrdDtlBlock` L762 / `getTransBlock` L857 |
| NullAndDisabledHftWritersAreRejected | `writeOrderQueue/Detail/Transaction` 的 `NULL` 与 `_disable_ordque` 拒绝（L418/L472/L526）|
| DisableOrdDtlAlsoBlocksTransaction | `writeTransaction` L526 判错变量（BUG-R23-002）；对照 `_disable_trans` 只在 `proc_loop` L2492 被读 |
| SessionNotReceivingStopsAllHftWriters | `procQueue` L437 / `procOrder` L491 / `procTrans` L545（三处 `canSessionReceive` break）|

### `r23_writer_closing_test.cpp` → `src/WtDataStorage/WtDataWriter.cpp`

| 用例 | 行段 |
|---|---|
| TransHisDataMovesRtTicksToCompressedDsb | `transHisData` L1571-1600（`getSessionComms`→split→`getCommodity`→`getCodes`→`_proc_que`→首次起 `proc_loop`）+ `proc_loop` L2227-2238、L2408-2489（tick 迁移：`compress_data` + `BlockHeaderV2/BT_HIS_Ticks/CMP_V2`、`_size=0`）|
| TransHisDataWritesMarkerThenNextInitSeesSessionProceeded | `proc_loop` L2392-2403（MARK 分支 `IniHelper::writeInt/save`）+ `init` L154-164 读回 + `isSessionProceeded` L107-114 |
| TransHisDataWithUnknownSessionStaysSilent | `transHisData` L1574-1576（`pCommSet == NULL` 早退，不起线程）+ 对照组 L1578-1613 |
| SecondClosingRunAppendsMinBarsAndReplacesDayBar | `dump_bars_to_file` L2020-2055（日线快照来自缓存 tick）+ L2057-2134（min1 读旧文件→`proc_block_data` 去头解压→追加→重压 CMP_V2）+ `dump_day_data` L1955-1999（同日期不同值 → 替换末条）+ `getTickBlock` L1009-1049 / `getKlineBlock` L1351-1411（releaseBlock 之后重新映射的「非新建」路径）|
| ExtendedDumperReceivesDayMin1Min5BarsAndTicks | `proc_loop` L2432-2441（dumpHisTicks）+ `dump_bars_via_dumper` L1698-1816 三段（d1/m1/m5，含 `_tick_cache_idx` 派生日线 L1716-1727）|
| ExtendedDumperFailureOnlyLogsError | `proc_loop` L2437-2440、`dump_bars_via_dumper` L1737-1740 / L1763-1766 的 LL_ERROR 分支；文件落地不受影响 |
| DisabledHistorySkipsClosingWorkButStillMarks | `proc_loop` L2413、L2696-2699（`_disable_his` 跳过整段收盘但 MARK 仍写）+ `init` L141 参数回显 |
| GetCurTickSeesCacheWrittenBeforeClosing | `updateCache`/`loadCache` 与 `transHisData` 的隔离面：`_tick_cache_idx`/`cache.dmb` 不参与 `CMD_CLEAR_CACHE` 之外的清理（`proc_loop` L2419-2489 只动 rt/ticks）|
| DISABLED_ClearCacheCommandMaySleepThreeHundredSeconds | `proc_loop` L2240-2390（清理分支：`_tick_cache_file->close()`→`_tick_cache_block=NULL`→重 map L2323-2335、`fs::remove_all` 六个目录、异常时 `sleep_for(300s)` L2385）→ 撞 HANG 门禁，收进 DISABLED_ |

### `r23_rdm_count_slice_test.cpp` → `src/WtDataStorage/WtRdmDtReader.cpp`

| 用例 | 行段 |
|---|---|
| CountWalksBackAcrossDaysUntilSatisfied | `readTickSliceByCount` L2397-2416、L2475-2504（`nowTDate` 回退 + `_his_tick_map` key）、L2522-2548（读文件 + 长度校验通过）、L2552-2599（`lower_bound` + `insertBlock(0,...)` 前补）|
| CountStopsAfterThirtyMissingDays | L2507-2531（缺文件 `missingCnt++`）+ L2481-2482（缺满 30 天 break）|
| CountTakesRealtimeTailFirstThenOneHisTick | L2417-2473（RT 块当日尾窗）+ L2475-2478（`nowTDate==curTDate → getNextDate(-1)`）+ L2594-2597（his 只补缺口 1 条）|
| CountZeroReturnsEmptySliceWithoutTouchingFiles | L2468-2471（`thisCnt=min(eIdx+1,0)`）+ `Includes/WTSDataDef.hpp` L1438-1444（`insertBlock` count==0 不挂块）+ L2479（`left>0` 不进回退循环）|
| HisWindowBoundaryDropsTickExactlyAtEtime | L2405-2412（etime 拆 rDate/rTime/rSecs）+ L2585-2592（his 分支 `>=` 把边界条退掉）|
| RtWindowBoundaryKeepsTickExactlyAtEtime | L2438-2470（RT 分支 `>` 保留边界条；与 his 分支的差别 ⇒ BUG-R23-005）|
| CountSizeCheckFailureLeavesEmptyEntryForever | L2536-2543（先插空条目再判长度）+ L2567-2569（`_block==NULL` 直接 break）+ L2503-2504（缓存命中不再读文件）⇒ BUG-R23-004 |
| CountWithNoDataAtAllReturnsEmptySlice | L2431-2433（`getRTTickBlock` NULL → break）+ L2481-2482 + L2604（恒返回切片对象）|
| KlineCountIsTailWindowOfHisDayBars | `readKlineSliceByCount` L2219-2236（`_bars_cache` 未命中→建库）、L2254-2264（pname switch / isDay）、L2285-2315（KP_DAY 无 RT 块）、L2376-2392；`indexBarFromCacheByCount` L1688-1727（`eit==end`、`curCnt=min`、尾窗 sIdx）；`getRTKilneBlock` L2164（非 min1/min5 直接 NULL）|
| KlineCountWithZeroEtimeUsesYear2030Sentinel | L2238-2239（`etime==0 → 203012312359`）+ L2278-2281（eBar 时间标签换算）|
| KlineCountSplicesHisTailWithRtMin1Block | L2291-2314（RT 尾窗 + `bNeedHisData`）+ L2376-2389（his 补 1 条 + `appendBlock` 双块）；`getRTKilneBlock` L2162-2216（min1 只读映射、`_last_cap`）|
| KlineCountWithNoBarsAnywhereReturnsNull | L2384-2392（`hisCnt+rtCnt==0 → NULL`）+ `cacheHisBarsFromFile` L1568-1607（文件不存在 / barcnt==0）|
| DISABLED_CountByCodeShapedPidDereferencesNullCommodity | L2398-2401（用 `_code` 查品种 + 无条件解引用）⇒ BUG-R23-003 |

## 四、留白面（含原因与行数量级）

| 留白块 | 行数量级 | 为什么本车道没做 |
|---|---|---|
| `WtRdmDtReader::cacheHisBarsFromFile` 的 HOT 主力连续分支 L1162-1363 与股票复权分支 L1364-1567 | ~400 行 | 需要 `IHotMgr::splitCustomSections` 返回真 + `HotSections` + `getRuleFactor`，且 q03 已证 `barcnt==0` 时 `hotAy->at(barcnt-1)` 下溢抛异常（BUG-Q03-001），未修源码前只能 DISABLED_，主体待裁定 |
| `WtDataReader::cacheIntegratedBars` L969-1286 / `cacheAdjustedStkBars` L1287-1563 / `cacheFinalBarsFromLoader` L930-968 | ~660 行 | 同上 HOT/复权依赖，外加 `IHisDataLoader` 桩 + `_adjust_flag` 组合；且与本车道的 `CodeInfo` 夹具（`CodeHelper::CodeInfo` 由 `extractStdCode` 产出，不能手工构造 `_ruletag`）叠加，预算不够 |
| `WtDataWriter::proc_loop` 的 `CMD_CLEAR_CACHE` 清理分支 L2240-2390 | ~150 行 | 异常路径里 `sleep_for(300s)`（L2385）+ 最多 5 次重试直接撞 60s HANG 门禁；且 `fs::remove_all` 与用例 TearDown 争抢目录。收进 `DISABLED_` |
| `WtDataWriter::check_loop` L1616-1696 的 6 张过期块表回收 | ~80 行 | 需要「跑满 10s sleep + `_lasttime` 超 600s」的墙钟推进，派工禁止断言墙钟；`_proc_chk` 又不可 join（BUG-R23-001） |
| `WtDataWriter::pushTask`/异步任务线程 L578-622 + `writeTick` 的 `_async_proc==true` 分支 | ~50 行 | `_task_thrd` + 无谓词 `_task_cond.wait()` 的丢唤醒死锁同型风险（BUG-P05-005/BUG-P10-005 未复现），本车道一律 `async=false` |
| `pipeToTicks`/`pipeToKlines`/`procQueue|Order|Trans` 的 RT 块扩容分支 | ~30 行 | 需要把 capacity 压到 2500/270/54 以下（`resizeRTBlock` 只在 `cache.dmb` 路径被测到；已覆盖模板本体一份，其余 4 个实例化各占一份符号）|
| `WtDataReader::onMinuteEnd` L2210-2312、`WtRdmDtReader` 的 `_thrd_check` 轮询 L87-165 | ~180 行 | 依赖墙钟/5s 轮询线程 + 析构 join，沿用 p09/p10/q03 结论整块排除 |
| `WtDataReader::getRTOrdDtlBlock` L1970-2023 / `getRTOrdQueBlock` L2024-2077 / `getRTTransBlock` L2078-2131 | ~160 行 | `readOrd*Slice` 的 isToday 分支才走它们，需要逐笔类 RT `.dmb` + 当日判定链；本车道预算收在 tick/kline 两条 ByCount 入口上 |

## 五、线程与落盘纪律（评审要点）

1. **调过 `init()` 的 writer 一律「只 `release()`、不 `delete`」**：`~std::thread` 对 joinable 线程会 `std::terminate`（BUG-R23-001），`release()` 又不 join `_proc_chk`。`case_runner` 一用例一进程 ⇒ 泄漏有界、不跨用例累积。夹具里 `abandon_writer()` 用于走过 `transHisData` 的对象（`release()` 对 `_proc_thrd` 的 notify+join 有丢唤醒窗口，见 BUG-P10-005），TearDown 不再碰它。
2. **不用墙钟做期望值**：唯一例外是 `marker.ini` 的「今日/未来日期 ≥ 今日」判定（`isSessionProceeded`），本道用例改用 `20991231` 未来日期避开跨午夜抖动。
3. 落盘全部在 `temp_directory_path()/r23_w_<pid>_<seq>`，读侧沿用 q03 `Sandbox`（chdir 进 temp 目录，TearDown 还原 + `remove_all`）；**未读写仓库 `dist/`**，不依赖上次运行残留。
4. `proc_loop` 是后台线程，用例用「等 `marker.ini` 出现该 sid」作为队列排空信号（`transHisData` 把 MARK 排在最后），带上限 10s 轮询；等待结束后才读文件/断言，避免与线程并发读写同一目录。

## 六、疑似缺陷

`docs/迭代计划/迭代01/bugs/R23_storage.md`，6 条（全部 `- 状态: 待审核`）：

- BUG-R23-001（Blocker）重复 `init()` ⇒ `~std::thread(joinable)` ⇒ `std::terminate`；连带「调过 init 的对象不能 delete」
- BUG-R23-002（Major）`writeTransaction` 判 `_disable_orddtl`，`_disable_trans` 在写入路径无人读
- BUG-R23-003（Blocker）`readTickSliceByCount` 用**合约代码**查品种且无条件解引用返回值 → 段错误（两文件 20+ 处同类调用里唯一用 `_code` 的一处）
- BUG-R23-004（Major）历史 tick 文件长度校验失败后残留空缓存条目 ⇒ 该交易日此后**静默永久跳过**（连错误日志都不再打）
- BUG-R23-005（Minor）同一条 etime，RT 分支用 `>`（保留边界条）、his 分支用 `>=`（退掉边界条）⇒ 跨当日/历史边界取数少一条且行为不一致
- BUG-R23-006（Major，未复现）`updateCache` L1457-1458 对 `resizeRTBlock` 的 NULL 返回值立即解引用（旧映射在返回前已被 `reset()`），只登记代码事实

## 七、门禁状态

- `cmake --build build-cov --target CoverageTests`：本车道 4 个 TU 的 .o 全部编译通过（`-Wall -Wextra -Werror` 零告警）。
- **链接被兄弟车道挡住，本车道暂无法自证「跑绿」**：`src/TestUnits/coverage/r24_exefact2/r24_exec_runner_test.cpp` 引用 `WtExecRunner::get_comm_info/get_real_time/get_sess_info`，这三个成员的定义在 **`src/WtExecMon/WtExecRunner.cpp:429-445`**，而 `WtExecMon/*.cpp` 只被编进 `CoverageTests_exefact` / `CoverageTests_riskmon` 两个拆分目标，主目标 `CoverageTests` 既没编它也没链 `libWtExecMon` → `ld: Undefined symbols for architecture arm64` → `build_x64/Debug/bin/TestUnits/CoverageTests` 不产出。
  根因是「新目录被 GLOB_RECURSE 自动收进主目标，但它需要的生产 TU 属于别的拆分目标」的路由问题，只有主控能改 `WT_COV_SPLIT_TARGETS`（把 `dir:r24_exefact2` 挂到 exefact 那条）；按派工纪律本车道不改别人的目录、不改 `src/TestUnits/coverage/CMakeLists.txt`，在此点名交给主控。
  同一目标在本轮还曾两次因 `src/Share/ModuleHelper.hpp:48` 的 `void inst_hlp()`（头文件里的非 inline 定义）与 `WtDtPorter.cpp` 撞 duplicate symbol 而链接失败，主控随后以 `-Dinst_hlp=cov_inst_hlp_<hash>` 绕过 —— 该缺陷仍未在产品侧修（BUG-L00 系），记录于此供归因。
