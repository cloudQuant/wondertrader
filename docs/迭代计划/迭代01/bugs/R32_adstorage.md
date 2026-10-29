# R32 缺陷登记 — WtDataStorageAD（WtRdmDtReaderAD / WtDataWriterAD / 关联的 Share 与 WTSUtils 头）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
`DISABLED_` 仅表示用例默认不运行，不代表现象已经复现；以下复核会分别标注源码推断、实际运行结果和未验证前提。

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
  "重新绑定"而是把**末笔整体覆写回 `_ticks[0]`**。日志用的
  `_last_tick_time`（`:141` 从 curTs 取值）碰巧与 back 一致所以看不出来，但缓存首笔已被
  永久替换：切片首元素=末笔，且当 `_ticks[1] >= eTick` 时 lower_bound 的判定也被带偏
  （实测 `EtimeOnExisting…`/`SliceLowerBound…` 因此返回 size 0）。
- 证据边界: 源码赋值明确将 `_ticks.back()` 整个覆盖到 `_ticks[0]`。运行升序测试观察到首项为末笔（price=3903、action_time=110015500，而期望为 price=3901、action_time=93015500）；不需要、也没有证据表明发生了“越界读相邻 ayVals 字符串”。改写后 vector 已不再有序，后续 `lower_bound` 的前置条件不成立，具体切片结果不得外推为所有输入的固定序列。
- 影响范围: 生产链路上凡是"读一段区间再取子窗"的调用方都会拿到错位的 tick（首笔被换成
  末笔）；`bNeedOlder`/`bNeedNewer` 分支（`:173`、`:197`）用的是 `const&`，没有此问题。
- 备注: 修复方向：分别从 front/back 取只读引用，避免把末项赋值给 front；`:140` 可改为 `const WTSTickStruct& lastTs = tickList._ticks.back();`
  （或先取值再计算），一行。本 lane 未改产品代码，相关 5 条断言"应有行为"的用例保持红。

## BUG-R32-002

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: 待契约确认（前提是输入 `pre_interest=0` 表示缺省/无昨持）
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.cpp`（`updateTickCache` procFlag==1 跨交易日分支 L907-918）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_cache_test.cpp` →
  `ProcFlagOneOnNewTradingDateRebaselinesCache`（`:265` 期望 additional==10，实测 60）
- 输入/前置条件: 缓存已有上一交易日持仓 50 的 tick；新交易日首笔 `procFlag=1`、
  `open_interest=60`、`pre_interest=0`。该输入只有在接口/上游约定 0 表示“字段缺省”时，才要求回退到缓存持仓。
- 期望行为（待确认）: 若 `pre_interest=0` 表示缺省，则 `diff_interest = 60 - 上一交易日持仓 50 = 10`；若它是有效的完整快照字段，则当前实现按输入计算 60 也与 `WTSStruct::pre_interest`（上日总持）相符。
- 实际行为: `:909` 先 `memcpy(&item._tick, &newTick, ...)` 把缓存整体覆盖成**本笔**，
  `:913` 才用 `item._tick.open_interest - item._tick.pre_interest` 做差分 —— 被减数已是
  本笔的 `pre_interest`（=0）→ `diff_interest = 60`；昨收持仓既没参与差分，也没回填进
  `item._tick.pre_interest`，跨日信息丢失。
- 复现步骤: 直接跑该用例（保持红即证据）。
- 影响范围: 仅在上游以 0 表示“昨持缺失”，而该路径应使用缓存值兜底时，`additional()/diff_interest` 才会把持仓增量报成整笔持仓；不能据当前接口注释断言 procFlag==1 每个交易日首笔必错。
- 备注: 修复方向待契约确认：若 0 是缺省值，先保存旧 `open_interest`，再回填为 `pre_interest` 并计算差分；否则应调整该用例输入/期望值，不改生产语义。


- 迭代02复核: TEST。公开字段注释说明 pre_interest 是上日总持；新交易日 fixture 没有给它设置，WTSStruct 默认值为 0。procFlag==1 计算差分时用这个输入值，因此 additional()==10 的 oracle 不成立，不能据此要求产品以缓存值覆盖输入。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R32-003

- 状态: 本地正确契约回归 FAIL；日期支持期限待确认
- 严重级别: Minor / 支持期限待确认（2033+ 时间编码与 32 位键尾均无法表示）
- 模块/文件: `src/Share/TimeUtils.hpp`（`timeToMinBar` L337-340；关联 AD 用法
  `src/WtDataStorageAD/WtDataWriterAD.cpp:698/:800`）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/p05_minbar_key_overflow_test.cpp` →
  `BarsBeyond2033EscapeTheUpperSentinel`、`TruncatedKeyTailCollidesWithOlderBar`。前者原把回绕值当通过条件，现按独立 uint64 真值断言，定向 FAIL；后者手工构造 32 位 LMDB 键并验证冲突，定向 PASS，不是 writer 端到端验证。
- 输入/前置条件: `TimeUtils::timeToMinBar(20330101, 1000)`。
- 期望行为: 返回 uint64 语义真值 (20330101-19900000)*10000+1000 = 4,301,011,000。
- 实际行为: `(uDate-19900000)*10000` 在 **32 位域内**完成后才提升 uint64 → 2033 起 helper 回绕，返回 6,043,704（= 真值 mod 2^32）。独立的 `LMDBBarKey` 尾字段也是 uint32；即使先修正 helper，key 仍会在转入 32 位时截断到同一值并与旧 bar 冲突。两处都需修，不能只改 helper。
- 复现步骤: 构建 `CoverageTests_adstorage` 后运行上述两项：helper 实际 6,043,704、预期 4,301,011,000；手工键尾碰撞对照通过。
- 影响范围: helper 返回值及用 uint32 尾键的分钟线存储/查询在 2033+ 均可能错序或覆盖。是否属支持范围内缺陷，需先确认产品对历史日期的承诺。
- 备注: 修复方向分两步：先用 `uint64_t` 中间乘法修 helper；再版本化/迁移 `LMDBBarKey` 尾字段并同步读写端及哨兵，不能静默改变现有 on-disk key 布局。

## BUG-R32-004

- 状态: 已合并（迭代02：主编号 BUG-R16-11）
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
- 复现步骤: `r16_lmdb_test.cpp` → `get_all_appends_a_phantom_duplicate_of_the_last_record`；R4 当前 `r4_read_db` helper 已剥除幻尾行，不能再用它的通过断言证明产品实现正确。
- 影响范围: `get_all` 对任何调用方都可能多返回一条重复项；空库会触发未初始化值。当前仓库搜索到的调用仅为测试与 R4 回读 helper，未发现产品源码调用点，因此不能声称现有生产路径已受影响。本项与 `R16_share_tools.md` BUG-R16-11 完全重复（后者还记录 cursor 未关闭）。
- 备注: 不另计缺陷；统一在 R16-11 修复并补非空/空库测试：仅在 `MDB_SUCCESS` 时 append，结束后关闭 cursor。


- 迭代02复核: DUPLICATE。get_all 未先判断 cursor 返回码即 append 末次 item，导致幻尾记录/空库未初始化数据；与 R16-BUG-11 是完全相同实现和缺陷，后者还包含 cursor 关闭问题。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-R32-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp`（`readTickSliceByRange` 首读路径与
  `init` 的 cfg==NULL 组合；崩溃面同既有条目）
- 相关测试: `src/TestUnits/coverage/p05_datastorage2/p05_rdmdtreader_ad_slices_test.cpp` →
  `NullCfgLeavesEmptyBaseDirButSinksAreWired`（当前启用版本只验证空 base dir 与未实现接口，不运行 range read；源码注释记录早期 range 调用曾 CRASH rc=139）、
  `DISABLED_NullCfgKlineSliceOpensCwdRelativeDbAndCrashesOnEmptyBars`（当前登记触发条件；本轮未运行）
- 输入/前置条件: `init(NULL, sink)` ⇒ `_base_dir` 为空串；随后任意一次
  `readKlineSliceByRange`/`readTickSliceByRange`。
- 期望行为: 要么拒绝空 cfg 后的读取并报错，要么把相对路径限制在可诊断的位置。
- 实际行为: `init(NULL, sink)` 保持 `_base_dir` 空串；`get_k_db`（`:420-421`）拼出 **相对路径** `"day/SHFE/"`，源码会在进程 CWD 下调用 `create_directories`（R21-BUG-12 的无 error_code 形态），0 根命中后走到
  `:353` 对空 vector 取 `&v[0]` + `size()-1` 下溢 → SIGSEGV。**崩溃本体是已登记的
  R21-BUG-06**（不重复登记计数），本条只登记新查明的触发组合：cfg==NULL 的 rdm 读取器
  + 任意区间读 = 在 CWD 里造目录再崩。
- 复现步骤: 打开 DISABLED_ 用例单独跑（case_runner 新建的 run 目录里会留下 `day/` 残骸）。
- 影响范围: 若 null cfg 在接口调用中可出现，range read 会尝试在 CWD 下创建数据目录；空 vector 崩溃本体是 R21-BUG-06 的重复项。本轮未运行此 DISABLED 用例，CWD 目录残留和崩溃仅由测试源码/产品路径推断。
- 备注: 关联前账：R21-BUG-06（空结果下标崩溃）、R21-BUG-12（create_directories 抛/自愈路径）。

---

## 本轮独立复核裁定（2026-10-29）

| 编号 | 裁定与证据边界 | 最小修复方向 |
|---|---|---|
| BUG-R32-001 | **已确认产品缺陷。** `curTs` 引用 `_ticks.front()` 后再赋 `_ticks.back()`，源码直接覆盖首项。本轮 5 个 R21 tick 测试均 FAIL：首项变为末笔；window-start 与 etime 用例切片实际 size=0。失序后 `lower_bound` 的区间已不满足其有序前置条件，具体切片形状不能外推；没有越界读相邻 ayVals 的证据。 | front/back 分别用只读引用取值，禁止将 back 赋给 front；回归多条记录及不同窗口。 |
| BUG-R32-002 | **契约待核实，不能仅凭现有红测判为产品缺陷。** `procFlag=1` 新日分支按输入 `open_interest - pre_interest` 得 60；测试期望 10 隐含 `pre_interest=0` 表示字段缺失并以前一交易日缓存 50 兜底。`IParserApi` 只定义完整快照，`WTSStruct` 将 pre_interest 注释为上日总持，未定义 0 为缺省。 | 若 0 是合法上日持仓，将测试输入设为 50、期望10；若 0 是缺省 sentinel，先补接口/上游契约，再实现缓存持仓 fallback 并验证 parser 输入。 |
| BUG-R32-003 | **2033+ 算术与键宽限制已由源码确认；支持范围待确认。** `timeToMinBar` 中间乘法 32 位回绕，`LMDBBarKey` 尾字段也是 uint32，单修 helper 不能消除落库截断。现有测试 PASS 锁定当前回绕/碰撞行为，没有实际未来生产数据迁移验收。 | 明确支持期限；若支持 2033+，修 helper 的 64 位运算，并版本化 64 位 key 布局、读写端及哨兵，提供旧库兼容/迁移策略。 |
| BUG-R32-004 | **完全重复，归并到 R16-BUG-11。** 非空末尾重复、空库访问未初始化 MDB_val 与 R16 记录相同；旧测试以重复记录为预期而 PASS，现已改为 `r16_lmdb.get_all_returns_each_record_once`，本机 FAIL：两条记录返回三条。仓库内搜索只找到测试与 R4 回读 helper 调用，未发现产品源码调用点。 | 不单独计数；在 R16-11 统一修：仅对 `MDB_SUCCESS` append、覆盖空库并关闭 cursor。 |
| BUG-R32-005 | **触发组合部分重复；CWD 相对写入是额外候选。** 空 vector 崩溃与 R21-BUG-06 重复，目录创建模式与 R21-BUG-12 重复。`init(NULL,sink)` 保持空 `_base_dir`，源码后续拼相对 `day/SHFE/` 并调用 `create_directories`；当前启用测试只测安全 stub，range-read DISABLED 本轮未运行，CWD 残留和崩溃未动态复现。 | 确认 cfg 是否可空；不可空则 init 失败并拒绝读取，可空则禁止空 base path 下创建相对库。崩溃修复归 R21-06，不重复计数。 |

**命令证据：**`CoverageTests_adstorage --gtest_filter=R21ReaderADTest.FirstTickRangeReadLoadsThreeTicksInActionTimeOrder:R21ReaderADTest.EachCodeHasItsOwnTickDbAndDoesNotLeakKeys:R21ReaderADTest.SliceLowerBoundTakesSecondLookupWhenFirstTickEqualsWindowStart:R21ReaderADTest.EtimeOnAnExistingTickIsLoadedButExcludedFromTheSlice:R21ReaderADTest.RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates`：5 项全部 FAIL，首项覆盖及其对切片的影响与源码一致。`R4WriterADTest.ProcFlagOneOnNewTradingDateRebaselinesCache`：FAIL，实测 additional=60、期望10（契约前提未证）。`P05KeyTailTest.*`：5 PASS。旧 `time_to_min_bar_overflows_after_2032` 和 `get_all_appends_a_phantom_duplicate_of_the_last_record` 曾以错误结果为预期而 PASS；现已改为正确的 `time_to_min_bar_roundtrips_historical_and_future_dates` 与 `get_all_returns_each_record_once`，定向运行均 FAIL。R32-005 range-read DISABLED 本轮未运行；未声称已动态复现。
