# R21 用例清单 — WtDataStorageAD（adstorage / CoverageTests_adstorage）

- lane: R21（路 21，`分派范围` 简报 `manifests/api-brief-R21.md`）
- 范围（本轮要补的 missed 行，约 271 行）:
  - `src/WtDataStorageAD/WtRdmDtReaderAD.cpp` 222/410（45.85%）
  - `src/WtDataStorageAD/LMDBKeys.h` 25/25（0.00%）
  - `src/WtDataStorageAD/WtBtDtReaderAD.cpp` 17/162
  - 三个 .h：`WtDataReaderAD.h`、`WtDataWriterAD.h`、`WtRdmDtReaderAD.h`
- 用例目录: `src/TestUnits/coverage/p05_datastorage2/`（前缀 `r21_`，只进 `CoverageTests_adstorage`；
  不新建目录、不改 CMakeLists —— `dir:p05_datastorage2` 由 CONFIGURE_DEPENDS 的 GLOB 自动收）
- 前账: `manifests/R4_adstorage.md`、`bugs/R4_adstorage.md`（p05、R4 均已收口，本轮不动它们的文件）
- 规模: 3 个 .cpp + 1 个共用替身头 = 32 条 `TEST`/`TEST_F` = **28 启用 + 4 `DISABLED_`**
- 缺陷登记: `docs/迭代计划/迭代01/bugs/R21_adstorage.md`（13 条，每条独立一行的 `- 状态: 待审核`）
- 符号缝: 只引用 **AD 这一族** 的 `extern "C"` 工厂（`createRdmDtReader` / `deleteRdmDtReader` /
  `createBtDtReader` / `deleteBtDtReader`）；`WtDataStorage` 的同名入口不在本目标，绝不引用。
- 硬约束落实: 不起线程、不 sleep、不读挂钟、无真实 socket/shm；
  LMDB 全部开在自己 `temp_directory_path()` 下的 `r21_rdmad_<pid>_<seq>` 唯一目录并在 TearDown 删除；
  `WtRdmDtReaderAD` 会 `create_directories`，所以 `init` 一律带绝对临时路径（不用 `cfg==NULL` 那条，
  否则退化成相对 cwd 的建目录动作）。
- 门禁（真实构建口径，逐条通过，3/3）:

```sh
cd build-cov
CMD=$(ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_adstorage.dir/p05_datastorage2/p05_minbar_key_overflow_test.cpp.o | tail -1)
for F in r21_rdmtick_slice_test r21_rdmbars_kline_test r21_lmdbkeys_btreader_test; do
  P=$PWD/../src/TestUnits/coverage/p05_datastorage2/$F.cpp
  eval "$(printf '%s' "$CMD" | sed -E "s|-o [^ ]+ -c [^ ]+|-fsyntax-only $P|")"
done
```

## 文件与替身

| 文件 | 作用 |
|---|---|
| `p05_datastorage2/r21_ad_fixture.h` | `IBaseDataMgr`/`IHotMgr`/`IRdmDtReaderSink`/`IBtDtReaderSink` 替身、按写入器口径落 LMDB 的 tick/bar 夹具（含 `R21ReaderADTest`） |
| `p05_datastorage2/r21_rdmtick_slice_test.cpp` | `readTickSliceByRange` 全路径（首读/更旧/更新/切片/get_t_db）+ 2 条 DISABLED（9 启用） |
| `p05_datastorage2/r21_rdmbars_kline_test.cpp` | `readKlineSliceByRange` 与 `get_k_db`（哨兵键、窗口裁剪、三张周期表、move 掏空）+ 1 条 DISABLED（9 启用） |
| `p05_datastorage2/r21_lmdbkeys_btreader_test.cpp` | LMDBKeys.h 键语义 + `WtBtDtReaderAD` 缺口 + AD 族 C 入口 + 1 条 DISABLED（10 启用） |

所有行号按当前工作区文件实测。

## 一、`r21_rdmtick_slice_test.cpp` → `src/WtDataStorageAD/WtRdmDtReaderAD.cpp`

夹具的交易时段 0000-2359、偏移 0 分钟 → `offsetTime(t,left/right) == t`（`WTSSessionInfo.hpp:507-527`），
落库键一律复刻写入侧 `WtDataWriterAD.cpp:425-427` 的 `LMDBHftKey{exchg,code,交易日,HHMM*100000+SSsss}`。

| 用例 | 生产文件:行段 | 关键断言 |
|---|---|---|
| FirstTickRangeReadLoadsThreeTicksInActionTimeOrder | `:82-116`、`:118-145`、`:205-251`、`:436-458` | 区间 [93000000,150000000] 命中 3 笔 → 切片 `size()==3`、价格 3901/3902/3903、`action_time` 原样；DEBUG 原文 `3 ticks between [20240102000016430,20240102000016600] of XEX.rb.rb2409 loaded to cache`（R21-BUG-01 的实测值）；`XEX.rb` 传入 `calcTradingDate` |
| SingleTickWindowYieldsOneTickSliceAndEqualBoundaryTimes | `:124-133`、`:135-144`、`:216-235` | 单条命中：`lower_bound` 上界是空区间 → eIdx=0、切片 1 条（5001）；日志 `[20240102000000930,20240102000000930]` |
| RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates | `:109-111`、`:146-148`、`:180-202`、`:439` | 复读走更新分支 → 切片 6 条且价格序列 `3901,3902,3903,3901,3902,3903`（R21-BUG-02）；DEBUG `3 newer ticks of XEX.rb2409 loaded to cache`；开环日志 2 次（R21-BUG-03） |
| OlderBranchPrependsPreviousTradingDayTicksAndSlicesTwo | `:148-178`、`:152-153`、`:206-235` | 跨交易日往前取数：新 2 笔前插、老 3 笔后接（`_ticks` 变 5）→ 切片 2 条（3801/3802，`action_date==20240101`）；DEBUG `2 prev ticks of XEX.rb.rb2409 loaded to cache`；`bNeedNewer` 该轮为 false |
| SliceLowerBoundTakesSecondLookupWhenFirstTickEqualsWindowStart | `:223-228`、`:232-247` | 首条 `action_time` 恰等于请求下边界 → 走 `:238` 的第二次 `lower_bound`、`cnt = eIdx-sIdx+1 = 1`，切片只剩 4001；日志 `[20240102000000930,20240102000001000]` |
| EtimeOnAnExistingTickIsLoadedButExcludedFromTheSlice | `:91-93`、`:124-133`、`:224-228` | etime 落在已有笔上（93045500）：LMDB 闭区间收 2 条、切片 `>=` 剔除 → `size()==1`（只剩 6001），日志 `2 ticks between [...]`（R21-BUG-04） |
| ThreeSequentialReadsEachReopenTheSameTickDb | `:114`、`:118-145`、`:180-202`、`:439-457` | 三轮切片大小 3 / 6 / 9（重复追加可复现）；`Tick db of XEX.rb2409 opened` 计数 3；LL_ERROR 计数 0（全程静默） |
| EachCodeHasItsOwnTickDbAndDoesNotLeakKeys | `:444-445`、`:438-441` | rb2409 切片 2 条（7001/7002）、rb2410 切片 1 条（8001），键里的 code 不串味；`opened` 计数 2；两个临时目录都在 |
| ByCountAndByDateTickReadersStayUnimplementedStubs | `:76-80`、`:254-258`、`:260-264`；`WtRdmDtReaderAD.h:68/:78/:88` | `readTickSliceByCount`/`readTickSliceByDate`/`readKlineSliceByCount`/三个逐笔桩全 NULL；DEBUG 计数 0；INFO 原文 `WtRdmDtReaderAD initialized, root data folder is <root>/` |
| DISABLED_TickWindowWithoutAnyMatchedRecordReadsBackOfEmptyVector | `:216`、`:224` | 触发条件：区间内 0 条 tick（读库里不存在的合约）→ 对空 `std::vector` 取 `&v[0]` 且上界 `size()-1` 下溢 → 空指针解引用（R21-BUG-06，启用即崩） |
| DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfReadTickSlice | `:445`（无 error_code 的 create_directories）+ `:443` | 触发条件：`{base}ticks/XEX/rb2409` 预置为普通文件 → `filesystem_error` 穿出 `readTickSliceByRange`（R21-BUG-12，同 R4-BUG-05 形态） |

## 二、`r21_rdmbars_kline_test.cpp` → `src/WtDataStorageAD/WtRdmDtReaderAD.cpp`

| 用例 | 生产文件:行段 | 关键断言 |
|---|---|---|
| DayKlineFirstReadLoadsAllBarsAndClipsToRequestedWindow | `:266-295`、`:296-316`、`:340-391`、`:393-434` | 4 根日线（20240102…05）→ 窗口 [20240102,20240104] 切片 3 根（date/close=101/102/103，`1e-9` 容差只用于浮点等值比对）；DEBUG `Reading back d1 bars of XEX.rb2409...`；`calcTradingDate` 恰 2 次 |
| DayKlineWindowBeforeFirstBarReturnsWholeLoadedRange | `:367-373` | 请求下边界早于全部 bar → 走 `pHead = 首根, cnt = idx+1` → 切片 3 根（首 20240103、末 20240105） |
| ThirdKlineLookupOfSameExchangeReturnsNullAfterCacheHandleMovedOut | `:415-417`（`return std::move(it->second)`）、`:292-294` | 第 1/2 次切片 3 根、第 3 次返回 **NULL**；LL_ERROR 计数 0、`day db of XEX opened` 仅 1 条（R21-BUG-05，与 R4-BUG-02 同形的 reader 侧证据） |
| ThirdCodeOfSameExchangeFailsBecauseOneHandleServesTheWholeExchange | `:415-417`、`:286-294`、`:296-316` | 同交易所三个合约：rb2409 切片 2 根、rb2410 切片 1 根（单根时 `lower_bound` 空区间 → cnt=1）、**rb2411 直接 NULL**（交易所级放大）；开环仍只 1 条 |
| Min1KlineKeysByBarTimeAndSlicesTwoBars | `:281-283`、`:299-315`、`:353-358`、`:374-386`、`:397-401` | m1 键尾 = `3301030931/32/33`（`(日-19900000)*10000+HHMM`）；窗口 [0931,0932] → 切片 2 根、时间戳与 close（102/103）逐字段核；DEBUG `Reading back m1 bars of XEX.rb2409...`；`min1 db of XEX opened` 1 条 |
| ThreePeriodMapsAreIndependentSoEachFirstLookupOpensOnce | `:395-411`（三张表分派）、`:432-433` | m5 切片 2 根、m1 切片 3 根、d1 切片 4 根互不影响；`opened` 计数恰 3；LL_ERROR 0 |
| HourPeriodHasNoRoutedDbAndKlineReadReturnsNull | `:412-413`、`:292-294` | KP_Hour → `return WtLMDBPtr()` → 切片 NULL；DEBUG 计数 0；**没有建任何 `min1/` 目录**（早退在建库之前） |
| RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated | `:289`、`:317-338`（`:323` 下界恒 0、`:330` resize 覆盖） | 两次读取各打一条 `Reading back d1 bars ...`（`Reading back` 计数 2）、开环仍 1 条、两次切片都是同样的 3 根（R21-BUG-07：增量分支实为全量重载） |
| BarCacheIsKeyedByStdCodeAndPeriodSoCodesDoNotMix | `:286-287`、`:296-316`、`:340-391` | rb2409（close 701/702）与 rb2410（close 801/802）各自一条缓存键，第 2 次取库走被搬空的缓存项但句柄仍可用；切片分别 2 根 |
| DISABLED_KlineReadWithoutAnyBarCrashesOnEmptyVectorSubscript | `:353`、`:361`、`:367-376` | 触发条件：`day/XEX` 库存在但没有目标合约的任何键 → 载入 0 根后仍取 `&bars[0]`、上界 `size()-1` 下溢（R21-BUG-06，启用即崩；与 p05 的 `DISABLED_EmptyResultSetsCrashOnVectorSubscriptZero` 同一行段、不同入口） |

## 三、`r21_lmdbkeys_btreader_test.cpp` → `LMDBKeys.h` + `WtBtDtReaderAD.cpp` + AD 族 C 入口

| 用例 | 生产文件:行段 | 关键断言 |
|---|---|---|
| ReverseEndian32IsSelfInverseAndMovesLowByteToTop | `src/WtDataStorageAD/LMDBKeys.h:39-46` | 7 个字面量（1→0x01000000 等）+ 5 个探针的两次反转自反 |
| ReverseEndian16SwapsTheTwoBytes | `LMDBKeys.h:24-29` | 0x1234→0x3412 等；该重载在 AD 四个 .cpp 内**无调用点** → 这 3 行无法由本目标生产路径带进覆盖（R21-BUG-08） |
| HftAndBarKeyLayoutArePackedWithBigEndianTails | `LMDBKeys.h:51-97`（含 `:89-96` 构造）、`:104-138`（含 `:131-137`） | `sizeof(LMDBHftKey)==16+32+4+4`、`sizeof(LMDBBarKey)==16+32+4`（证明 pack(1)）；未用满的 `_exchg/_code` 逐字节为 0；尾部 4 字节 == 自算的大端字节；`_date/_time/_bartime == reverseEndian(原值)` |
| KeyByteOrderSortsByExchgCodeThenTimeAndSentinelsBoundEverything | `LMDBKeys.h:89-96`、`:131-137` | memcmp 顺序 == (exchg,code,date,time) 数值顺序；`0` 与 `0xffffffff` 哨兵框住同合约全部键；`rb2409 < rb2410`（第 5 字节 '0'<'1'）；短码 `"rb"` 的上界仍 < `"rb2409"` 的下界；跨交易日后时间不再可比 |
| BtRawBarsAndTicksWithNullSinkFillBuffersAndLogNothing | `WtBtDtReaderAD.cpp:13-24`（`:16-17` 的 sink 判空）、`:51-62`、`:64-92`、`:94-122`、`:124-167`、`:169-193` | `init(cfg, NULL)` 后 `read_raw_bars` 仍返回 true、`buffer.size()==2*sizeof(WTSBarStruct)`、date/close/vol/money 与喂入一致；`read_raw_ticks` 2 笔同理；rdm sink 计数 0（早退不解引用 NULL） |
| BtReadRawTicksOnDateWithoutRecordsReturnsTrueWithEmptyBuffer | `:105-108`（空数组早退）、`:121` | 交易日 20240103 无键 → 返回 **true** 且 buffer 为空（R21-BUG-09）；DEBUG 恰 2 条、末条原文 `Reading back ticks on 20240103 of XEX.rb2409...` |
| BtInitCfgGateControlsRootDirInfoLogAndReadReachability | `:51-56`（cfg==NULL 早退）、`:58-61`、`:152-153`、`:178-179` | 不带 cfg：LL_INFO 计数 0、tick/bar 都 false、buffer 始终空、不产生任何目录；带 cfg：INFO 原文 `WtBtDtReaderAD initialized, root data dir is <root>/`，缺 `day/XEX` 时仍 false 且 LL_ERROR 0（存在性检查在 open 之前） |
| BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures | `:151-153`（带尾斜杠的路径 + 存在性早退）、`:177-179`（无尾斜杠）、`:181-185`（get_t_db 的 LL_ERROR 分支） | 同名普通文件占位时两支分岔：bars → false 且 LL_ERROR 计数 0（尾斜杠使 `StdFile::exists` 先返回 false，`:155-159` 因此永不可达，R21-BUG-13）；ticks → false 且 LL_ERROR 1，原文含 `Opening tick db of XEX.rb2409 failed:`；K 线侧 `db if` 笔误见 R21-BUG-10 |
| AdRdmFactoryBuildsUsableReaderAndToleratesNullDelete | `WtRdmDtReaderAD.cpp:33-46`（`createRdmDtReader`/`deleteRdmDtReader` 含 `:43-44` 的 NULL 分支）、`:266-295`、`:296-316`、`:374-386` | `dynamic_cast<WtRdmDtReaderAD*>` 非空（证明确实是 AD 这一族）；经工厂读回 2 根日线（close 1102）；`deleteRdmDtReader(NULL)` 不崩；开环日志 1 条 |
| AdBtFactoryBuildsUsableReaderAndToleratesNullDelete | `WtBtDtReaderAD.cpp:26-39`、`:64-92`、`:128-133` | `createBtDtReader` 产物 `dynamic_cast<WtBtDtReaderAD*>` 非空；m1 全量哨兵读回 1 根（`buffer.size()==sizeof(WTSBarStruct)`、close 1201）；DEBUG 原文 `Reading back m1 bars of XEX.rb2409...`；`deleteBtDtReader(NULL)` 不崩 |
| DISABLED_RdmInitWithNullSinkDereferencesBeforeCfgGuard | `WtRdmDtReaderAD.cpp:62-68` | 触发条件：`init(cfg 或 NULL, NULL)` → `:64/:65` 在判 cfg 之前就解引用 sink（R21-BUG-11，启用即崩；对照 `WtBtDtReaderAD.cpp:51-56` 不崩） |

## 四、未覆盖与原因（诚实清单）

- `WtRdmDtReaderAD.cpp:115-116`（tick 的空句柄早退）与 `:424-425`、`:448-449`（两条 LL_ERROR 分支）、
  `WtBtDtReaderAD.cpp:155-159`（K 线库的 LL_ERROR 分支）：
  rdm 的 `get_t_db` 每次都走新建分支（R21-BUG-03），而 `WtLMDB::open`
  （`src/WTSUtils/WtLMDB.hpp:57-84`，`mdb_env_open(..., flags = 0, ...)`）在目录可写时必成功；
  不可写/被占用的构造路径先在 `:421/:445` 抛 `filesystem_error`（R21-BUG-12，只能 DISABLED）；
  bt 的 K 线侧则被 `:151` 拼出的**尾斜杠**在 `:152` 的存在性检查上先拦下（R21-BUG-13，
  「不可达」本身已由启用用例 `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures` 钉住，
  同用例的 tick 一支把 `:181-185` 真正触达）。
  估计残余 8~12 行。
- `WtRdmDtReaderAD.cpp:28`、`WtBtDtReaderAD.cpp:21` 的 `fmt::format_to` 模板体：
  每轮日志都会执行，行号归属随 clang 版本漂移，不作为缺口认领。
- `WtRdmDtReaderAD.cpp:393-434` 的 `create_directories` 自愈（`:421`）在**启用**用例里只能验证
  「目录存在时正常开库」；「目录不存在时自建」的那一条读取必以 R21-BUG-06 的空 vector 崩溃收尾，
  已整条登记为 `DISABLED_TickWindowWithoutAnyMatchedRecordReadsBackOfEmptyVector`。
- `WtBtDtReaderAD.cpp:94-122` 的 `read_raw_ticks` 里 `:110` 的 `cnt` 与 `:112` 的 `buffer.resize`
  在「 ayVals 非空但单个 value 短于 `sizeof(WTSTickStruct)`」时会越界读（p05 的
  `DISABLED_ShortLmdbValuesOverreadStructBuffer` 已登记），本轮不重复启用。
- `WtDataReaderAD.h`、`WtDataWriterAD.h`（简报里的另两个 .h）：简报列出的 `:281/:292/:302/:312/:351/:360`
  与 `:353/:362/:369-426` 全是**声明行**，定义体在 R4 已收口的 `WtDataReaderAD.cpp` / `WtDataWriterAD.cpp`；
  头文件里唯一可直接执行的 `L154/L190 inline empty()` 属 R4 的缓存块路径。
  本轮不改 R4 的用例、不越界补 R4 的文件 → 这两个 .h 的残余（估 20~40 行）仍留在 R4 名下。
- `WtRdmDtReaderAD.h` 本轮实际带进来的行：`:68`、`:78`、`:88` 三个 inline 桩、
  `:150-164` 的 `BarsList`（含 `:163` 构造）、`:170-184` 的 `TicksList`（含 `:183` 构造）
  —— 均由 `ByCountAndByDateTickReadersStayUnimplementedStubs` 与各条切片用例的缓存条目创建覆盖。
