# R32 adstorage lane manifest — CoverageTests_adstorage（被测 src/WtDataStorageAD/）

门禁命令：`sh docs/迭代计划/迭代01/tools/lane_gate.sh CoverageTests_adstorage --build --jobs 3`
基线（同 id 对齐，非总数差）：枚举 90 / PASS 64 / FAIL 25 / CRASH 1。
终态：枚举 92（新增 2 条启用用例）/ **PASS 86 / FAIL 6 / CRASH 0**；连续 3 轮全量复跑逐 id 一致，
回归 0（基线 PASS 的 64 条全部仍 PASS；lane 交办清单里的 `R4WriterADTest.ExtendedDumperReceivesPipedTicksAndBars`
在接手实测时即已绿——交办清单为 fixture 命名空间修复前的快照，比实测多 1 条）。

缺陷登记：`docs/迭代计划/迭代01/bugs/R32_adstorage.md`（5 条，全部「待审核」，未改任何 `src/**` 产品代码）。

---

## 一、26 条非通过的逐条裁定

### ③ 夹具/观测面缺失 —— 修正后转绿（13 条，含 1 条 CRASH 的拆分处置）

共同根因 A（8 条）：**`WtLMDBQuery::get_all`（src/WTSUtils/WtLMDB.hpp:371-390）在游标
MDB_NOTFOUND 后仍多追加一条幻尾行**（NOTFOUND 不改动 bKey/mData ⇒ 末行逐字节重复；
BUG-R32-004）。`r4_writer_ad_lmdb_test.cpp` 的回读 helper `r4_read_db` 原先不剥这条，
所以所有 `rows.size()` 断言与真实入库条数恒差 1。只改 helper（剥「尾行==前行」的重复），
各用例的期望条数与内容断言一字未动：

1. `R4WriterADTest.TickDbStoresTradingDateKeyedRecordsUntilHandleMoveOut` → PASS（期望 2 继续钉 R4-BUG-02）
2. `R4WriterADTest.HftKeysSortChronologicallyAcrossTradingDates` → PASS
3. `R4WriterADTest.EachCodeGetsItsOwnTickDbDirectory` → PASS
4. `R4WriterADTest.MinuteBarRolloverPipesIntoMin1Db` → PASS（期望 2 继续钉 R4-BUG-02）
5. `R4WriterADTest.DayBarRolloverPipesIntoDayDbWithDuplicatedVolume` → PASS（vol=30 继续钉 R4-BUG-03）
6. `R4WriterADTest.MinuteFiveBarPipesIntoMin5Db` → PASS
7. `R4WriterADTest.ExplicitMapSizesStillPipeData` → PASS
8. `R4WriterADTest.BumpedActionTimeIsPersistedToTickDb` → PASS

9. `R4WriterADTest.TickCacheAtCapacityResizesFileAndKeepsOldItems` → PASS。两处 ③：
   - 夹具漏前置：块文件里的 rb2301/rb2302 没在 `R4BaseDataMgr` 登记，
     `getCurTick`（WtDataWriterAD.cpp:847-849）先查合约 → 恒 NULL，补 `add_contract` 两行；
   - 观测面：`last_log(LL_INFO)` 被紧随的 L946-948「First tick of new tradingday」覆盖，
     改用新增的 sink `count_containing` 断言 L881 扩容日志恰好 1 条（依据与行号写在用例注释）。
10. `R4ReaderADTest.FirstTickReadTakesIncrementalRangeAndKeepsLastCount` → PASS。③：
    开环日志（WtDataReaderAD.cpp:614）先落、增量分支日志（:163）后落，`last_log` 只剩后者；
    给 R4ReaderSink 补 `_all`/`count_containing` 后按原文断言 1 条，并加钉
    「loaded to cache for the first time」为 0 条（R4-BUG-06 增量分支的直接现象）。
11. `R4ReaderADTest.KlineFirstReadLoadsLowerBarsAndChecksLatest` → PASS。③(同 last_log 覆盖)
    + ②：回源日志（WtDataReaderAD.cpp:265）格式是 `"{} {} bars of {} loaded to cache"`，
    代码位传 **stdCode 原文** "EXK1.rb2301"、周期名 PERIOD_NAME[1]="min1"
    （WTSTypes.h:115-123）；期望子串从 `bars of EXK1.rb.rb2301 loaded to cache` 改为
    `min1 bars of EXK1.rb2301 loaded to cache`，改走 count_containing==1。
12. `P05RdmReaderADTest.NullCfgLeavesEmptyBaseDirButSinksAreWired`（基线唯一 CRASH）→ PASS。
    崩溃栈（macOS `~/Library/Logs/DiagnosticReports/CoverageTests_adstorage-2026-10-28-010541.ips`，
    EXC_BAD_ACCESS/KERN_INVALID_ADDRESS）落点：
    `wtp::WtRdmDtReaderAD::readKlineSliceByRange + 1788` → `std::lower_bound(...$_2)` →
    比较器 `$_2::operator()` 内解引用空数据指针 —— 即 :353 对空 `_bars` 取 `&v[0]`、
    上界写成 `size()-1` 下溢；前置链是 cfg==NULL ⇒ `_base_dir=""` ⇒ get_k_db(:420-421)
    拼相对路径 "day/SHFE/" 在 CWD 自建空库 ⇒ 0 根命中。**崩溃本体是既登记的
    R21-BUG-06（空结果集下标）+ R21-BUG-12（create_directories 自愈路径）**，不新计数；
    触发组合登记为 BUG-R32-005。处置：崩溃调用从启用用例里挪进新
    `DISABLED_NullCfgKlineSliceOpensCwdRelativeDbAndCrashesOnEmptyBars`（沿用 R4/R21 的
    「启用即崩一律 DISABLED 登记」规约），主用例保留 cfg==NULL 的桩断言并加钉
    「桩函数零 DEBUG 日志」。

### ② 用例前提算错 —— 改期望值（每条给出产品源码依据）

13. `R4WriterADTest.IdenticalActionTimeIsBumpedBy200Ms` → PASS。
    依据 WtDataWriterAD.cpp:972-975：+200 加在**本笔** `newTick.action_time`（每笔输入
    都是 93000000）上，不是在缓存值上累加；期望从 93000400 改为 93000200。
14. `R4WriterADTest.ProcFlagTwoAccumulatesTotals` → PASS（与③混合）。
    ③ 部分：夹具 `make_tick`（r4_ad_fixture.h:244-245）把 ≤0 的 total 字段回填成本笔量，
    L994-998「总量为 0 才累加」分支根本没进 —— 显式 `s2.total_volume=0; s2.total_turnover=0;`；
    ② 部分：high 期望依据 WtDataWriterAD.cpp:1003-1004
    `newTick.high = max(newTick.price, item._tick.high)` = max(3800,3900)，从 3800 改为 3900；
    low 维持 3900 的既钉现象（R4-BUG-04）。
15. `R21ReaderADTest.DayKlineFirstReadLoadsAllBarsAndClipsToRequestedWindow` → PASS。
    日志文案期望从 `d1` 改为 `day`：WtRdmDtReaderAD.cpp:298 用 PERIOD_NAME[period]，
    KP_DAY 位在 WTSTypes.h:115-123 里是 "day"。
16. `R21ReaderADTest.Min1KlineKeysByBarTimeAndSlicesTwoBars` → PASS。同上 `m1`→`min1`
    （WtRdmDtReaderAD.cpp:298 + WTSTypes.h:118）。
17. `R21ReaderADTest.AdBtFactoryBuildsUsableReaderAndToleratesNullDelete` → PASS。
    同上 `m1`→`min1`（WtBtDtReaderAD.cpp:71 + WTSTypes.h:118）。
18. `R21LmdbKeys.ReverseEndian32IsSelfInverseAndMovesLowByteToTop` → PASS。
    `reverseEndian(0x0000FF00u)` 的实参是 uint32_t 字面量，选定 4 字节重载
    （LMDBKeys.h:39-46，整字 4 字节换位）→ 期望从 0x000000FF 改为 0x00FF0000；
    与自反性检查（rev(0x00FF0000)=0x0000FF00）互洽。2 字节重载是 LMDBKeys.h:24-29，
    本 overload-set 里 uint32 实参不会退到它。
19. `P05KeyTailTest.BarsBeyond2033EscapeTheUpperSentinel` → PASS。
    前提修正依据 TimeUtils.hpp:337-340：`(uDate-19900000)*10000` 在 **uint32 域内**完成，
    20330101 的 helper 返回值已经回绕成 6,043,704，不可能 > 0xffffffff。改用测试自身的
    uint64 算式表达语义真值 4,301,011,000（>哨兵），并加钉 helper 的提前回绕
    `t_bad_2033 == (uint32_t)t_true_2033`（BUG-R32-003）；hits==3 不变。
20. `P05KeyTailTest.TruncatedKeyTailCollidesWithOlderBar` → PASS。
    同上：hi 用 uint64 算式直出（不能再借 timeToMinBar，它在 uint32 已先回绕，
    `hi-2^32` 因此下溢成 18446744069420628024 才是原红因）；wrapped=6,043,704<0xffffffff，
    覆盖写后 hits==1 不变。

### ① 产品缺陷 —— 保持红（6 条，断言的是应有行为）

新缺陷 BUG-R32-001（WtRdmDtReaderAD.cpp:137-141：`curTs` 是指向 `_ticks.front()` 的
**引用**，`curTs = tickList._ticks.back();` 把末笔整体覆写回 `_ticks[0]`，静默数据损坏；
用 lldb 直读 `get_block_addr(0)` 附近内存 + 新增探针实测：3 笔 [3901,3902,3903] 的切片
实测返回 [3903,3902,3903]，2 笔命中时 eIdx 判定被带偏成 size 0）：

21. `R21ReaderADTest.FirstTickRangeReadLoadsThreeTicksInActionTimeOrder`（保持红 :56/57）
22. `R21ReaderADTest.EachCodeHasItsOwnTickDbAndDoesNotLeakKeys`（保持红 :328/329）
23. `R21ReaderADTest.SliceLowerBoundTakesSecondLookupWhenFirstTickEqualsWindowStart`（保持红 :222）
24. `R21ReaderADTest.EtimeOnAnExistingTickIsLoadedButExcludedFromTheSlice`（保持红 :260）
25. `R21ReaderADTest.RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates`
    （保持红 :139：size==6 的部分钉既有 R21-BUG-02 且已通过；want[] 前段顺序被
    BUG-R32-001 破坏。双条目引用，未新登记重复缺陷）

新缺陷 BUG-R32-002（WtDataWriterAD.cpp:907-918：procFlag==1 跨交易日分支先 memcpy 覆盖
缓存、再用被覆盖成 0 的 `item._tick.pre_interest` 做差分，昨收持仓丢失；对照 procFlag==2
的 :921-922/:940-941 是「先存后拷」的正确形态）：

26. `R4WriterADTest.ProcFlagOneOnNewTradingDateRebaselinesCache`（保持红 :276：期望
    additional==10，缺陷实测 60）

以上 6 条一律**未改断言方向、未放宽匹配**，注释逐条写明依据行号与既有条目引用。

---

## 二、期望值修改登记表（规则 4 口径汇总）

| 用例 | 期望从 | 期望到 | 产品依据 |
|---|---|---|---|
| IdenticalActionTimeIsBumpedBy200Ms | 93000400 | 93000200 | WtDataWriterAD.cpp:972-975（+=200 基于本笔输入）|
| ProcFlagTwoAccumulatesTotals(high) | 3800 | 3900 | WtDataWriterAD.cpp:1003-1004 `max(price, cached high)` |
| ProcFlagTwoAccumulatesTotals(输入) | total=helper回填 | 显式 0 | WtDataWriterAD.cpp:994-998 仅总量为 0 才累加；r4_ad_fixture.h:244-245 |
| DayKlineFirstRead… | "Reading back d1 bars…" | "…day bars…" | WtRdmDtReaderAD.cpp:298 + WTSTypes.h:115-123 |
| Min1KlineKeys… | "…m1 bars…" | "…min1 bars…" | 同上 |
| AdBtFactory… | "…m1 bars…" | "…min1 bars…" | WtBtDtReaderAD.cpp:71 + WTSTypes.h:115-123 |
| ReverseEndian32… | rev(0x0000FF00)==0xFF | ==0x00FF0000 | LMDBKeys.h:39-46（uint32 重载整字换位）|
| BarsBeyond2033… | helper 返回 >0xffffffff | 真值用 uint64 算式；helper 已回绕钉死 | TimeUtils.hpp:337-340、LMDBKeys.h:120-136 |
| TruncatedKeyTail… | hi=helper 返回值 | hi=430101ull*10000+1000 | 同上 |
| KlineFirstReadLoadsLowerBars…（子串） | "bars of EXK1.rb.rb2301 loaded to cache" | "min1 bars of EXK1.rb2301 loaded to cache" | WtDataReaderAD.cpp:265（PERIOD_NAME 位 + stdCode 原文）|

`TickDbStores…UntilHandleMoveOut` 等 8 条 LMDB 用例的 `rows.size()` 期望**未动**，
动的是回读 helper（剥 get_all 幻尾行，WtLMDB.hpp:380-387 ⇒ BUG-R32-004）。

## 三、新增用例（src/TestUnits/coverage/p05_datastorage2/r32_residual_ad_test.cpp）

- `R32AdResidualTest.R32WriterFactoryInitReleaseAndNullTolerantDelete` → PASS。
  补 AD 族 `createWriter/deleteWriter`（WtDataWriterAD.cpp:34-50）+ 工厂产物完整
  init/loadCache/release 的无 `writeTick` 路径（不触 R4-BUG-01 的 `_async_task` 残值）
  + getCurTick「合约在、缓存空」分支（:854-855）。
- `R32AdResidualTest.R32ReaderFactoryAndZeroEtimeKlineSliceUsesSinkClock` → PASS。
  补 `createDataReader/deleteDataReader`（WtDataReaderAD.cpp:32-45）+ readKlineSlice
  的 etime==0 sink 时钟补齐分支（:374-382，此前 r4 只覆盖了 readTickSlice 的同一分支）。
  夹具与 sink 均在本文件匿名命名空间内（单 TU，共享头里的 fixture 保持具名 r4fx/r21fx，
  未引入 SUITECONF）。
- 另在 p05_rdmdtreader_ad_slices_test.cpp 增加 1 条 `DISABLED_NullCfgKlineSliceOpens…`
  （缺陷登记用，不计数）。

## 四、遗留与说明

- 保持红的 6 条全部对应 `src/**` 只读约束下的真实缺陷（1 条新 Blocker + 1 条新 Major +
  1 条与 R21-BUG-02 混合），修复后即可转绿，无需再改用例。
- BUG-R32-003（timeToMinBar 2033 uint32 回绕）在 `src/Share/TimeUtils.hpp`，超出被测目录，
  按关联模块登记并已在 P05KeyTail 用例里用现状断言钉住。
- 未动 `src/TestUnits/coverage/CMakeLists.txt`；新用例只进已登记目录 `p05_datastorage2`。
- LMDB 夹具全部走 `temp_directory_path()/r32_<pid>_<seq>`（新文件），TearDown remove_all；
  无自建线程、无 sleep。
