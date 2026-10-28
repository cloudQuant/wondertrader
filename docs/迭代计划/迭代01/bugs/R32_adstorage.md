# R32 缺陷登记 — WtDataStorageAD（WtRdmDtReaderAD / WtDataWriterAD / 关联的 Share 与 WTSUtils 头）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
凡"启用即崩"的现象，一律在用例里以 `DISABLED_` 登记并写清触发条件。

分级口径沿用 p01：Blocker = 静默数据损坏/必崩；Major = 特定输入下数据错误或崩溃；
Minor = 健壮性/远期隐患。（与 R4/R21 的 P0~P3 对照：Blocker≈P0/P1、Major≈P1/P2、Minor≈P2/P3。）

必读前账：`bugs/R4_adstorage.md`、`bugs/R21_adstorage.md`。
`RepeatReadSameWindowAppends…`、`EtimeOnAnExistingTick…` 等条与 **R21-BUG-02/04** 同源的红
不在本文档重复登记，只在 manifest 引用既有条目。

---

## BUG-R32-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp`（`readTickSliceByRange` L135-144）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/r21_rdmtick_slice_test.cpp` →
  `FirstTickRangeReadLoadsThreeTicksInActionTimeOrder`、`EachCodeHasItsOwnTickDbAndDoesNotLeakKeys`、
  `SliceLowerBoundTakesSecondLookupWhenFirstTickEqualsWindowStart`、
  `EtimeOnAnExistingTickIsLoadedButExcludedFromTheSlice`、
  `RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates`（首元素部分）
- 输入/前置条件: 首次 `readTickSliceByRange` 命中 ≥2 笔（isEmpty 分支 `:126` resize+memcpy 升序载入）。
- 期望行为: 缓存 `_ticks` 保持载入的升序，首笔不被任何后续步骤改写。
- 实际行为: `:137` `WTSTickStruct& curTs = tickList._ticks.front();` 之后，`:140`
  `curTs = tickList._ticks.back();` —— `curTs` 是指向 `_ticks[0]` 的**引用**，该赋值不是
  "重新绑定"而是把**末笔整体覆写回 `_ticks[0]`**（`memcpy` 语义，284 字节）。日志用的
  `_last_tick_time`（`:141` 从 curTs 取值）碰巧与 back 一致所以看不出来，但缓存首笔已被
  永久替换：切片首元素=末笔，且当 `_ticks[1] >= eTick` 时 lower_bound 的判定也被带偏
  （实测 `EtimeOnExisting…`/`SliceLowerBound…` 因此返回 size 0）。
- 复现步骤: 跑上述任一条用例；实测 `3 笔 [3901,3902,3903] → 切片 [3903,3902,3903]`
  （越界读到相邻堆块里的 ayVals 字符串），lldb 直读 `get_block_addr(0)` 附近内存可复核。
- 影响范围: 生产链路上凡是"读一段区间再取子窗"的调用方都会拿到错位的 tick（首笔被换成
  末笔）；`bNeedOlder`/`bNeedNewer` 分支（`:173`、`:197`）用的是 `const&`，没有此问题。
- 备注: 修复方向：`:140` 改为 `const WTSTickStruct& curTs = tickList._ticks.back();`
  （或先取值再计算），一行。本 lane 未改产品代码，相关 5 条断言"应有行为"的用例保持红。

## BUG-R32-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.cpp`（`updateTickCache` procFlag==1 跨交易日分支 L907-918）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_cache_test.cpp` →
  `ProcFlagOneOnNewTradingDateRebaselinesCache`（`:265` 期望 additional==10，实测 60）
- 输入/前置条件: 缓存已有上一交易日持仓 50 的 tick；新交易日首笔 `procFlag=1`、
  `open_interest=60`、`pre_interest=0`（多数行情源新交易日首笔不带昨持）。
- 期望行为: 与 `procFlag==2` 的跨日分支（`:921-922` 先存 `pre_close/pre_interest`、
  `:940-941` 回填）一致，`diff_interest = 本笔持仓 - 上一交易日持仓` = 10。
- 实际行为: `:909` 先 `memcpy(&item._tick, &newTick, ...)` 把缓存整体覆盖成**本笔**，
  `:913` 才用 `item._tick.open_interest - item._tick.pre_interest` 做差分 —— 被减数已是
  本笔的 `pre_interest`（=0）→ `diff_interest = 60`；昨收持仓既没参与差分，也没回填进
  `item._tick.pre_interest`，跨日信息丢失。
- 复现步骤: 直接跑该用例（保持红即证据）。
- 影响范围: 依赖 `additional()/diff_interest` 的策略（持仓增减）在新交易日首笔上看到的是
  "整笔持仓"而不是"当日增仓"，procFlag==1 的实时链路每交易日首笔必错。
- 备注: 修复方向：memcpy 前先取 `item._tick.open_interest` 作昨持（或同 procFlag==2 形态）。

## BUG-R32-003

- 状态: 待审核
- 严重级别: Minor（2033 年日期炸弹，潜伏）
- 模块/文件: `src/Share/TimeUtils.hpp`（`timeToMinBar` L337-340；关联 AD 用法
  `src/WtDataStorageAD/WtDataWriterAD.cpp:698/:800`）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/p05_minbar_key_overflow_test.cpp` →
  `BarsBeyond2033EscapeTheUpperSentinel`（`:124` 原断言 helper 返回值 > 0xffffffff，实测不成立）
- 输入/前置条件: `TimeUtils::timeToMinBar(20330101, 1000)`。
- 期望行为: 返回 uint64 语义真值 (20330101-19900000)*10000+1000 = 4,301,011,000。
- 实际行为: `(uDate-19900000)*10000` 两个操作数都是 uint32，乘法在 **32 位域内**完成后才
  提升 uint64 → 2033 起先在这里回绕：实测返回 6,043,704（= 真值 mod 2^32）。
  `WTSBarStruct.time`（uint64）自此与"分钟线键时间"的语义脱钩，且与 `LMDBBarKey(uint32)`
  的截断点**不是同一个位置**（helper 已经先错过一次）。
- 复现步骤: `EXPECT_EQ(TimeUtils::timeToMinBar(20330101, 1000), 430101ull*10000ull+1000ull)` 即可见。
- 影响范围: 所有用 `timeToMinBar` 落 `bar.time` 的模块（AD/非 AD writer、读侧时间折算）；
  本目标内先把"键尾截断撞旧 bar"的现象用测试自身的 uint64 算式钉住（见两条 P05KeyTail 用例
  的 R32 修正注释）。
- 备注: 修复方向：`(uint64_t)(uDate-19900000)*10000ull + uTime`。

## BUG-R32-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WTSUtils/WtLMDB.hpp`（`WtLMDBQuery::get_all` L371-390）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_lmdb_test.cpp` →
  `r4_read_db`（8 条 LMDB 落库用例的回读辅助，R32 起剥掉幻尾行后才与真实条数对齐）
- 输入/前置条件: 任意 `get_all` 调用：库里 N ≥ 1 条时。
- 期望行为: 回调收到 N 条真实记录。
- 实际行为: 循环体在 `mdb_cursor_get` 返回 `MDB_NOTFOUND` 后**没有先判错就再 append 一次**
  （NOTFOUND 不改动 `bKey/mData`，它们仍指向上一条）→ 永远多出一条与末条逐字节相同的
  幻尾行（实测 N=1 回读 2 行、N=2 回读 3 行）。N == 0 时更糟：`bKey/mData` 从未被赋值，
  `std::string((char*)bKey.mv_data, bKey.mv_size)` 用未初始化指针构造 → UB/崩溃。
- 复现步骤: `r4_writer_ad_lmdb_test.cpp` 任何一条（修 helper 前 `rows.size()` 恒 +1）。
- 影响范围: 全仓库 `get_all` 的调用方都会"多一条重复记录"；对幂等覆盖写的调用方是静默
  放大，对逐条派发的调用方是重复处理。本 lane 只改了测试侧 helper，产品未动。
- 备注: 修复方向：循环内先 `_errno != MDB_SUCCESS → break` 再 append。

## BUG-R32-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp`（`readTickSliceByRange` 首读路径与
  `init` 的 cfg==NULL 组合；崩溃面同既有条目）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/p05_rdmdtreader_ad_slices_test.cpp` →
  `NullCfgLeavesEmptyBaseDirButSinksAreWired`（原样启用时 CRASH rc=139）、
  `DISABLED_NullCfgKlineSliceOpensCwdRelativeDbAndCrashesOnEmptyBars`（登记触发条件）
- 输入/前置条件: `init(NULL, sink)` ⇒ `_base_dir` 为空串；随后任意一次
  `readKlineSliceByRange`/`readTickSliceByRange`。
- 期望行为: 要么拒绝空 cfg 后的读取并报错，要么把相对路径限制在可诊断的位置。
- 实际行为: `get_k_db`（`:420-421`）拼出 **相对路径** `"day/SHFE/"` 在进程 CWD 里
  `create_directories` 自建空库（R21-BUG-12 的无 error_code 形态），0 根命中后走到
  `:353` 对空 vector 取 `&v[0]` + `size()-1` 下溢 → SIGSEGV。**崩溃本体是已登记的
  R21-BUG-06**（不重复登记计数），本条只登记新查明的触发组合：cfg==NULL 的 rdm 读取器
  + 任意区间读 = 在 CWD 里造目录再崩。
- 复现步骤: 打开 DISABLED_ 用例单独跑（case_runner 新建的 run 目录里会留下 `day/` 残骸）。
- 影响范围: 宿主若忘配 path（或 cfg 解析失败退 NULL），读侧第一刀就崩并且污染 CWD。
- 备注: 关联前账：R21-BUG-06（空结果下标崩溃）、R21-BUG-12（create_directories 抛/自愈路径）。
