# R21 缺陷登记 — WtDataStorageAD（WtRdmDtReaderAD.cpp / WtBtDtReaderAD.cpp / LMDBKeys.h）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
原登记中的 `DISABLED_` 用例需按实际运行结果解释；本轮复核结果区分已复现崩溃、异常穿出和仅静态推断，见文末。

分级口径：P0 = 静默丢数据/必崩；P1 = 特定输入崩溃或数据错误；P2 = 资源与健壮性；P3 = 语义可疑/死代码。

必读前账：`manifests/R4_adstorage.md`、`bugs/R4_adstorage.md`。
R4 的 `update_cache_from_lmdb`（`WtDataReaderAD.cpp:273`）空句柄崩溃那条**保持 DISABLED、未复活**；
本轮只把同形的 `std::move` 掏空在 **AD reader 侧**（rdm/bt 两个读取器）重新查了一遍，见 R21-BUG-05。

---

## R21-BUG-01 ｜P1｜tick 首尾时间把「偏移分钟」和「秒毫秒」直接相加，量纲少乘 100000
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:138`、`:141`、`:174`、`:198`
  （四处同形：`(uint64_t)trading_date * 1000000000 + sInfo->offsetTime(action_time / 100000, x) + action_time % 100000`）
- 现象: 键与区间时间用的是 `HHMM*100000 + SSsss`（见 `WtDataWriterAD.cpp:426` 的写入侧换算），
  而这里把 `offsetTime(HHMM)`（930）与 `SSsss`（15500）**当数字相加** → 得到 16430，
  正确值是 93015500。缓存的 `_first_tick_time/_last_tick_time` 因此恒为
  「交易日×1e9 + 一个 ≤2 万的碎数」。
- 后果: 这两个值同时是 `:110/:111` 的 `bNeedOlder/bNeedNewer` 判据和
  `:152/:184` 的查询边界 → 直接引出 R21-BUG-02。
- 证据边界（`r21_rdmtick_slice_test.cpp`）:
  - `FirstTickRangeReadLoadsThreeTicksInActionTimeOrder`
    日志显示 `3 ticks between [20240102000016430,20240102000016600] of XEX.rb.rb2409 loaded to cache`
    （期望语义应为 `[20240102093015500,20240102110015500]`）；该测试的价格/时间顺序断言因独立 R32-001 缓存首项被覆盖问题失败，不能把整条用例记作 PASS。
  - `SingleTickWindowYieldsOneTickSliceAndEqualBoundaryTimes` → `[20240102000000930,20240102000000930]`
  - `OlderBranchPrependsPreviousTradingDayTicksAndSlicesTwo`（`:174` 同一形态）
- 修复方向还需校正 `:174` / `:198` 的 `offsetTime` 左右边界参数：请求更旧数据应向左扩展首边界，请求更新数据应向右扩展尾边界；当前对应调用方向相反。

## R21-BUG-02 ｜P0｜复读同一区间会把已有 tick 整批重复追加，切片静默翻倍
- 状态: 本机定向契约回归 FAIL；首次读取另受 R32-001 干扰
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:111`（`bNeedNewer` 判据）、
  `:180-202`（更新分支），关键是 `:184` 用 R21-BUG-01 的碎数当区间下界
- 现象: `_last_tick_time % 1000000000` ≈ 16600，而库里真实键的时间位是 9 位 HHMMSSsss
  （≥ 93000000）→ `get_range` 的下边界**低于全部已有记录** → 已有的 3 笔被整批再读一遍，
  `:191` 用 `emplace_back` 追加（不判重）→ 缓存条数 3→6→9 单调膨胀，切片同样返回重复数据，
  全程没有任何 LL_ERROR/LL_WARN。
- 同时 `:111` 的判据本身不可靠：碎数远小于任何合法 etime，`bNeedNewer` 几乎恒真。
- 证据用例（`r21_rdmtick_slice_test.cpp`）: 原测试将 6/9 条和重复开库写成预期，现改为
  `RepeatReadSameWindowReturnsEachTickOnce` 和 `RepeatedWindowReadsPreserveTickData`，要求每次均为 3 条且内容一致。
  本机两项 FAIL：第二、三轮分别得到 6、9 条；首轮第一笔被覆写还触发 R32-001 的独立失败。
- 建议: 首尾时间与键时间统一为 `offsetTime(HHMM)*100000 + SSsss`，
  且追加分支改按时间做去重/截断。

## R21-BUG-05 ｜P0｜get_k_db 命中分支 `return std::move(it->second)` 掏空缓存，第三次取即空句柄
- 状态: 已合并（迭代02：主编号 BUG-P05-001）
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:417`（get_k_db）、`:441`（get_t_db）；
  同形缺陷还在 `WtBtDtReaderAD.cpp:148`（get_k_db）、`:174`（get_t_db）
- 现象: 与 R4-BUG-02 同一形态：命中缓存时把 map 里的 `shared_ptr` 移走并返回，
  缓存项变成空指针但 key 仍在。第 1 次取有效、第 2 次取「搬家后」的有效句柄、
  **第 3 次起返回空指针**。
- rdm 侧后果: `readKlineSliceByRange` 只在 `:293-294` 判 `db == NULL` 后**直接 return NULL**
  （不报错、不打日志）→ 交易所级共享一张表，**同一交易所同周期的第 3 次及以后所有合约读取全部失败**
  （第 3 个合约、甚至同一合约的第 3 次复读都拿不到数）。
- bt 侧后果: `read_raw_bars` 在 `:67-69` 返回 false → 回测取不到 K 线（p05 的
  `SecondLookupSucceedsThirdIsPoisonedByMoveFromCache` 已记录同形现象）。
- 为什么 get_t_db 没崩：`:439` 查 `"exchg.code"`、`:456` 存 `exchg`（R21-BUG-03），
  键永不相等 → 每次走新建分支 → 掏空缺陷在 tick 路径上被另一个缺陷意外掩盖，
  代价是每次读取重开一个 LMDB 环境。
- 证据用例:
  - `r21_rdmbars_kline_test.cpp` 的 `RepeatedKlineLookupOfSameExchangeReturnsCachedBars` 和 `ThirdCodeOfSameExchangeReturnsItsBars` 原先把第三次 NULL 当通过，现分别要求读回原有数据；本地两项均 FAIL，第三次读取返回 NULL。开库次数不作为公开正确性契约。
  - `r21_rdmbars_kline_test.cpp` → `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated`
    （第 2 次走的是被搬空的缓存项，句柄仍可用）
- 建议: 4 处命中分支改 `return it->second;`。


- 迭代02复核: DUPLICATE。map 中 shared_ptr 被 move 后仍保留 key、值为空，这是 BUG-P05-001 的同一缺陷族。R21 还指出该文件 get_t_db 有同样写法，应作为 canonical 修复范围的附加调用点，不另计根因。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## R21-BUG-03 ｜P2｜get_t_db 查键与存键不一致，tick 库永不命中缓存
- 状态: 已合并（主记录 BUG-P05-003；效率影响边界见主记录）
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:439`（查 `"exchg.code"`）vs `:456`（存 `_tick_dbs[exchg]`）；
  同形缺陷在 `src/WtDataStorageAD/WtBtDtReaderAD.cpp:171` vs `:191`
- 现象: 每次读取都 `new WtLMDB(true)` + `mdb_env_create/open`（`:443-446`），
  map 中同一 `_tick_dbs[exchg]` 槽位被覆盖，旧句柄在无其他引用時析构 → 重复开环、重复 mmap；不构成句柄数量随调用不断增长。
- 证据用例: 历史测试曾检查重复开库日志，现已改为数据内容回归，不再把重复开库当成正确行为；缓存键缺陷及性能影响以主记录的源码证据为准。

## R21-BUG-06 ｜P1｜区间内无记录时对空 vector 取 `&v[0]`，并把上界写成 `size()-1`
- 状态: 已合并（主记录 BUG-P05-002；本段保留运行证据）
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:216`、`:224`、`:232`、`:238`（tick 切片）；
  `:353`、`:361`、`:369`、`:376`（K 线切片）
- 现象: `get_range` 返回 0 条时 `tickList._ticks` / `barsList._bars` 为空，
  代码仍取 `&v[0]`（对空 vector 取下标，实际是对 NULL 做指针算术）并把右边界写成
  `&v[0] + (v.size() - 1)` —— `size()` 为 0 时 `size_t` 下溢成 `SIZE_MAX-1`，
  随后 `pTick->action_date` 解引用空指针 → 崩溃。
  读一个库里不存在的合约、或读一个刚被 `create_directories` 自建出来的空库，都会走到这里。
- 证据用例（两条整条 DISABLED，启用即崩）:
  - `r21_rdmtick_slice_test.cpp` → `DISABLED_TickWindowWithoutAnyMatchedRecordReadsBackOfEmptyVector`
    触发条件：`readTickSliceByRange` 的 `[stime,etime]` 在该合约的 tick 库里命中 0 条。
  - `r21_rdmbars_kline_test.cpp` → `DISABLED_KlineReadWithoutAnyBarCrashesOnEmptyVectorSubscript`
    触发条件：`day/{exchg}` 库存在但没有目标合约的任何键。
- 建议: 载入条数为 0 时直接返回空切片（`WTSTickSlice::create(stdCode, NULL, 0)`），
  并把上界写成 `&v[0] + v.size()`。

## R21-BUG-07 ｜P2｜K 线缓存的 `_last_bar_time` 从未赋值，「增量加载」实为全量重载且用 resize 覆盖
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:289`（`bNeedNewer` 判据）、
  `:317-338`（增量分支，`:323` 的下界取 `(uint32_t)barsList._last_bar_time`、`:330` 的
  `barsList._bars.resize(cnt)`），字段声明 `src/WtDataStorageAD/WtRdmDtReaderAD.h:155`、构造 `:163`
- 现象: 全函数没有任何地方给 `_last_bar_time` 赋值（载入回调只 resize+memcpy），
  它恒为 0 → `bNeedNewer` 恒真、增量分支的下界恒等于全量哨兵 0 →
  每次复读都把该合约的**全部** bar 重新读一遍并 `:330` 按新条数 `resize` 覆盖；
  每次复读都会重复读取完整范围；resize(cnt) 会按新长度覆盖，若新结果更短会截断尾部，不会保留多余旧尾项。已确认的是重复全量读取性能问题，不是尾部残留旧数据。
- 证据用例: `r21_rdmbars_kline_test.cpp` →
  `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated`
  （两次读取各打一条 `Reading back d1 bars of XEX.rb2409...`，开环只有 1 条）
- 对照: tick 侧 `:143/:176/:200` 会写 `_first/_last_tick_time`（虽然量纲错，见 R21-BUG-01），
  K 线侧连写都没写。

## R21-BUG-04 ｜P3｜请求上界那一笔：LMDB 区间含上界、切片采用排他上界（候选较可能是误报）
- 状态: 待验证（公开区间端点合同未定义，旧测试另受 R32-001 干扰）
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:124-125`（`get_range` 闭区间，见
  `src/WTSUtils/WtLMDB.hpp:219-258` 的 `MDB_SET_RANGE` + `memcmp > 0` 才 break）
  与 `:224`（切片用 `pTick->action_time >= eTick.action_time` 回退）
- 现象: LMDB `get_range` 是闭区间，tick 切片用 `>= etime` 回退，表现为排他上界；这符合常见 `[stime, etime)` 切片约定。把端点保留在内部缓存也可能是后续扩窗所需，不足以推出“越界记录”。
- 证据用例: 原等值上界测试受 R32-001 干扰且端点合同未明，现保留为禁用的 `DISABLED_EtimeOnExistingTickNeedsBoundaryContract`。新 `EtimeBetweenTicksExcludesLaterTickFromSlice` 不依赖等值规则，本机 FAIL：期望首笔 6001，实际为 6002，与 R32-001 的首项覆写一致。
-  结论边界: `IRdmDtReader` 未发现明确的闭/半开区间说明；结合其他 reader 的半开切片用法，现有证据更支持测试端点断言错误。需以公开接口约定确认后再决定是否立为产品缺陷。

## R21-BUG-11 ｜P1｜WtRdmDtReaderAD::init 先解引用 sink、后判 cfg
- 迭代02第二轮前状态（历史）: - 状态: 待审核
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:62-68`
  （`:64` `sink->get_basedata_mgr()`、`:65` `sink->get_hot_mgr()` 在 `:67` 的 `cfg == NULL` 之前，
  且整段没有对 `sink` 判空）
- 现象: `init(cfg_or_NULL, NULL)` 时空 sink 解引用 → 崩溃；但 sink 是否允许 NULL 尚无接口契约证据，故只能判为无效参数下的健壮性风险。
  `:85-86` 的 `getCommodity(...)->getSessionInfo()` 未判空，未知品种可疑但本轮未动态验证；`:269` 仅取得 `commInfo` 后未解引用，原登记将其并列为崩溃点不准确。
- 证据用例（整条 DISABLED，启用即崩）: `r21_lmdbkeys_btreader_test.cpp` →
  `DISABLED_RdmInitWithNullSinkDereferencesBeforeCfgGuard`
  触发条件：`new WtRdmDtReaderAD()` 或 `createRdmDtReader()` 后 `init(NULL, NULL)`。
- 对照: `WtBtDtReaderAD.cpp:51-56` 的 init 只 `_sink = sink` 再判 cfg，不解引用 → 未崩。

## R21-BUG-12 ｜P2｜rdm 侧 create_directories 未带 error_code，路径被同名普通文件占用时异常穿出
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:421`（get_k_db）、`:445`（get_t_db）
- 现象: 与 R4-BUG-05 同一形态（`boost::filesystem::create_directories(path)` 无 `error_code` 重载）：
  `{base}ticks/{exchg}/{code}` 或 `{base}{subdir}/{exchg}` 被同名普通文件占用时抛
  `filesystem_error`。本轮 disabled 用例的异常由 Google Test 捕获，进程以 rc=1 退出；宿主若不捕获会继续传播，只有异常逃出线程入口时才会 `std::terminate`。
  bt 侧不会抛（`WtBtDtReaderAD.cpp:152/:178` 先 `StdFile::exists` 再 open，走 LL_ERROR 分支），
  两个读取器的健壮性等级不一致。
- 证据用例（整条 DISABLED）: `r21_rdmtick_slice_test.cpp` →
  `DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfReadTickSlice`
  触发条件：预先把 `{base}ticks/XEX/rb2409` 建成普通文件，再 `readTickSliceByRange`。
- 安全对照（启用）: `r21_lmdbkeys_btreader_test.cpp` →
  `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures`（bt 不抛异常，但两支的日志命运不同，见 R21-BUG-13）

## R21-BUG-13 ｜P2｜bt 的 get_k_db 路径带结尾斜杠、get_t_db 不带，同类故障分岔到不同分支
- 状态: 已撤回（日志可观察性差异已观察，缺少要求一致日志的产品合同）
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:151-153`（`"{}{}/{}/"` + `StdFile::exists`）
  vs `:177-179`（`"{}ticks/{}/{}"` 无结尾斜杠）
- 现象: `{base}{subdir}/{exchg}/` 这个尾斜杠让 `std::filesystem::exists`
  （`src/Share/StdUtils.hpp:137-146`，底层是 `stat`）对「路径被同名普通文件占用」
  直接返回 false（POSIX 对普通文件加尾斜杠必 `ENOTDIR`）→ K 线库在 `:153` 就静默返回空句柄，
  **`:155-159` 的打开失败日志与错误文案永远不会出现**；
  而 tick 侧（无尾斜杠）走的是 `:181-185` 的 LL_ERROR。
  同一个「目录被文件占了」的运维故障，两条路径给出的可观测性完全不同（一条有 ERROR、一条什么都没有），
  排障时可能误判成「库里就是没数据」。这是 POSIX 下的日志可观测性差异，不直接证明数据读取错误。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` →
  `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures`
  （bars：false + LL_ERROR 0；ticks：false + LL_ERROR 1，原文 `Opening tick db of XEX.rb2409 failed: ...`）
- 建议: 两支统一去掉尾斜杠（或统一加），并让「路径存在但不是目录」单独给一条 LL_WARN。

## R21-BUG-08 ｜P3｜LMDBKeys.h 的 reverseEndian(uint16_t) 在 WtDataStorageAD 内无任何调用点
- 状态: 已撤回（覆盖率观察，不构成产品缺陷）
- 位置: `src/WtDataStorageAD/LMDBKeys.h:24-29`（`[[maybe_unused]] static uint16_t reverseEndian(uint16_t)`）
- 现象: 全模块（`WtDataWriterAD.cpp` / `WtDataReaderAD.cpp` / `WtRdmDtReaderAD.cpp` /
  `WtBtDtReaderAD.cpp`）只用 `LMDBHftKey`/`LMDBBarKey` 的 uint32 字段，
  没有任何地方传 16 位实参 → 该重载在产品 TU 里不生成代码，
  这 3 行**无法**由 AD 目标的生产路径带进覆盖（本轮报表把 LMDBKeys.h 记 0.00% 的 25 行里含它）。
  只有 `src/TestUnits/**` 里有直接调用（l04 与本轮 R21）。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` → `ReverseEndian16SwapsTheTwoBytes`（只能证语义，不产生 AD 目标增量）

## R21-BUG-09 ｜P2｜bt 读取器「读不到数据」与「读成功」都返回 true，空 buffer 无标记
- 状态: 已撤回（空范围返回成功不违反已见合同；复用 buffer 的语义未确认）
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:75-91`（read_raw_bars）、`:105-121`（read_raw_ticks）
  —— 回调里 `if (ayVals.empty()) return;`（`:77-78`、`:107-108`）后函数仍 `return true`
- 现象: 库存在但区间内 0 条时，返回 true 且 `buffer` 保持调用方传入的原值
  （既不清空也不置长度）→ 若调用方复用同一个 string，理论上会读到旧字节；本轮现有调用方均传入新建的局部 string，尚未发现实际复用路径。
  与 `get_k_db/get_t_db` 失败时的 false 无法区分「没有数据」和「路径没建好」。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` →
  `BtReadRawTicksOnDateWithoutRecordsReturnsTrueWithEmptyBuffer`
  （交易日 20240103 无键 → true + 空 buffer；`:107-108` 的空数组早退分支）
- 复核: `IBtDtReader` 将 true 表示为读取成功，未要求空范围返回 false；仓内已查到的调用者每次使用新建 string。旧测试只证明空范围成功和空输出，不证明错误或旧数据污染。若后续要支持复用 buffer，应先明确其输出所有权与清空合同，再补先读非空、后读空范围的用例；清空 buffer 可作为防御建议，不据此要求空范围返回 false。

## R21-BUG-14 按数量和交易日读取的三个 AD 接口未实现
- 状态: 源码确认实现缺口；三个本机正确性回归 FAIL
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp` 的 `readTickSliceByCount`、`readTickSliceByDate` 和 `readKlineSliceByCount`；均为 TODO 并直接返回 NULL。
- 合同: `WtRdmDtReaderAD.h` 分别说明按结束时间向前读取指定数量的 tick/K 线、按交易日读取全部 tick；`IRdmDtReader` 将它们列为接口方法。旧测试在已写入 tick 时把 NULL 当作正确结果，不能证明功能符合合同。
- 复现: `CoverageTests_adstorage` 的 `R21ReaderADTest.TickSliceByCountReturnsSeededTicks`、`TickSliceByDateReturnsSeededTicks` 和 `KlineSliceByCountReturnsSeededDayBars` 各自写入两条数据后调用对应接口，本机三项均 FAIL，均在非空切片断言处实际返回 NULL。真实数据源和已修复后的返回内容尚未验证。
- 影响范围: 选用此 AD reader 且调用这些方法时，即使数据存在也得不到切片；`WtDataManager` 有按数量、按日期转发调用，按日期构造秒线也依赖日期 tick 查询。真实部署是否选用 AD reader 及实际数据影响尚未验证。
- 修复方向: 实现按数量的逆向窗口及按交易日读取，复用一致的交易日、时间键和切片所有权规则；处理 count=0、无数据、跨日、截止边界与缓存生命周期。分别用写入有效 tick 和 K 线的独立回归验收，同时验证已记录的缓存覆写和时间游标问题不会污染结果；委托队列/明细/成交其他 stub 是否属于支持范围需另核合同。

## R21-BUG-10 ｜P3｜交易所/周期库打开失败的日志文案笔误（"db if"）
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:157`（`"Opening {} db if {} failed: {}"`）、
  `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:424` 和 `WtDataReaderAD.cpp:585`（同一笔误）；
  同文件 `:448` / `:183` 的 tick 文案用的是正确的 `"of"`
- 现象: 若运维检索依赖 `Opening .* db of`，会漏掉上述 K 线库失败记录；是否存在此类实际检索规则尚未验证。
- 证据用例: 原 R122/R162/R172 打开失败测试逐字匹配错误拼写；现改为检查错误级别、失败结果及稳定日志片段，三条定向 PASS，不再把错字固化为通过条件。bt 侧 K 线路径尾斜杠使“路径为普通文件”这一特定输入在 `:152` 先走不存在分支，见 R21-BUG-13；其它无法打开 LMDB 的输入仍可能到达打开失败分支。rdm 侧异常目录创建会阻断一部分错误输入，见 R21-BUG-12。
- 修复方向: 三处统一为 `Opening ... db of ... failed`，保留错误路径和诊断上下文；测试不要求错误拼写。

---

## 本轮留白（不注册为缺陷，但要说清为什么没测）

1. `WtRdmDtReaderAD.cpp:115-116`、`:293-294` 的「句柄为空即早退」：
   rdm 的 `get_t_db` 因 R21-BUG-03 永不返回缓存项（每次新建），而 `get_k_db` 的 NULL 分支
   已由 `RepeatedKlineLookupOfSameExchangeReturnsCachedBars` 的本地失败回归观察；
   `get_t_db` 返回 NULL 只能靠 `mdb_env_open` 失败制造，而 `:445` 的 `create_directories`
   会先把路径占掉（占用失败即 R21-BUG-12 的抛异常），本目标内没有安全的注入点 → 未覆盖。
2. `WtRdmDtReaderAD.cpp:424-425`、`:448-449` 两条 LL_ERROR 分支：同上，
   `WtLMDB::open`（`src/WTSUtils/WtLMDB.hpp:57-84`）走 `mdb_env_open(..., flags = 0, ...)`，
   目录可写时必成功；不可写/被占用时先在 `:421/:445` 抛异常 → 无法在不违反硬规则的前提下触达。
3. `WtRdmDtReaderAD.cpp:28`、`WtBtDtReaderAD.cpp:21`（`fmt::format_to` 模板体）：
   随任意一条日志路径执行，由本轮用例反复走到（不是遗留缺口，但模板实例的行号归属随 clang 版本漂移）。
4. `WtDataReaderAD.h` / `WtDataWriterAD.h`（简报里的另两个 .h）：
   简报列出的 `:281 cacheBarsFromStorage`、`:292 update_cache_from_lmdb`、`:302 read_bars_to_buffer`、
   `:312 get_rt_cache_bar`、`:351/:360 get_k_db/get_t_db`、`:353/:362`、`:369-426` 等
   全是**声明行**，定义体在 `WtDataReaderAD.cpp` / `WtDataWriterAD.cpp`（R4 已收口的文件），
   头文件里唯一可直接执行的 `L154/L190 inline empty()` 属 R4 的缓存块路径。
   本轮不改 R4 的用例，也不越界去补 R4 的文件 → 这两个 .h 的残余行仍留在 R4 名下，
   估计约 20~40 行需由 R4 的补测（或新 lane）认领。

---

## 本轮独立复核裁定（2026-10-29）

| 编号 | 裁定与证据边界 | 最小修复方向 |
|---|---|---|
| R21-BUG-01 | **产品缺陷，静态证据明确。** 时间构造把 `offsetTime(HHMM)` 与 `SSsss` 直接相加，写入侧则按 `offset*100000+SSsss` 编码。相关时间日志有异常数值；`FirstTickRangeReadLoadsThreeTicksInActionTimeOrder` 的排序断言实际失败，因 R32-001 首项覆盖而不能把整条测试记 PASS。另有 `:174/:198` 左右扩展参数方向错误。 | 统一时间编码与 writer；按旧/新方向分别使用正确 session offset，并用非零 offset、跨日窗口测试。 |
| R21-BUG-02 | **产品缺陷；与 R21-BUG-01 耦合。** 修正后两项重复读取回归均 FAIL：实际数量为 3/6/9 而非每次 3，首笔价格另受 R32-001 覆写影响。修复时间单位后仍须处理闭区间查询重读上次边界的情况。 | 用单一规范时间游标；增量查询排除已缓存端点或按 key 去重，并验证不同窗口扩展。 |
| R21-BUG-03 | **产品效率候选；与 R4-BUG-07、P05-003 重复并入主记录。** 查找 `exchg.code`、存储 `exchg`；旧测试曾通过重复开库日志断言，现已移除此现状式断言。map 槽位按交易所覆盖，不会无限增加 key/句柄。 | 统一 tick DB 缓存 key 的查询和存储口径；性能合同另行评估。 |
| R21-BUG-04 | **接口端点合同待书面确认。** 等值端点候选已禁用；两笔之间截止的独立测试 FAIL，显示首项覆写，与端点包含规则无关。 | 先明确 `IRdmDtReader` 的区间约定，再恢复等值端点测试；独立修复 R32-001 数据覆盖。 |
| R21-BUG-05 | **K 线缓存产品缺陷；与 P05-001 / R4-BUG-02 同根因。** 原测试把第三次空句柄当作通过，修正后的重复读取及第三合约回归均 FAIL。rdm tick `get_t_db` 的 move 命中被 R21-BUG-03 查存 key 不一致掩盖，不能宣称四个分支都动态复现。 | 所有缓存命中返回共享指针副本；空句柄统一记录错误并安全返回。 |
| R21-BUG-06 | **已确认空 vector 崩溃；与 P05-002 重复。** disabled tick/bar 两例实际 rc=139，复现空结果路径。当前命令仅证明该构建下崩溃，不代表所有空查询参数组合都已覆盖。 | 空结果立即返回合法空切片；用迭代器半开范围 `[begin,end)`，避免 `&v[0]` 与 `size()-1`。 |
| R21-BUG-07 | **产品性能缺陷。** `_last_bar_time` 只初始化为 0、从未更新，重复读取持续从全量哨兵查起；`RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated` PASS，日志显示重复 query。原“尾部残留旧数据”不成立：`resize(cnt)` 缩短会截断多余尾项。 | 每次加载后更新游标，增量请求只取新范围；保留全量 reload 的显式初始化路径。 |
| R21-BUG-08 | **非产品缺陷/覆盖率说明。** uint16 overload 在 AD 生产代码无调用点；单测只验证 helper 语义，不代表生产执行路径缺陷。 | 从产品缺陷计数移除；若覆盖率要求 helper 语义，可将其列为独立纯函数测试。 |
| R21-BUG-09 | **原产品缺陷主张撤回。** fresh empty buffer 测试只证明空结果返回成功，仓内已查调用方使用新 string；复用 buffer 语义尚无契约。 | 若要支持复用，先规定输出 buffer 的生命周期，再补顺序读取测试。 |
| R21-BUG-10 | **低影响日志文案问题，不是控制流不可达。** `"db if"` 拼写可静态确认；bt 普通文件+尾斜杠场景早退，但已有目录下损坏/打不开的 DB 或权限问题仍可能进入 open 失败日志分支。没有运行时断言。 | 将 `if` 更正为 `of`；错误日志可按同一格式回归。 |
| R21-BUG-11 | **无效/未知输入健壮性候选，接口契约待确认。** null sink disabled 用例实际 rc=139，但未确认 `init` 契约允许 NULL；未知商品 null dereference 静态可疑且未动态测试。`:269` 不解引用 `commInfo`，不能保留为崩溃点。 | 确认 sink 与 commodity 的有效性前置条件；若可为空则在解引用前返回错误/空结果，并覆盖 null/unknown 输入。 |
| R21-BUG-12 | **产品健壮性缺陷；与 R4-BUG-05 同模式。** disabled 文件冲突用例实际抛出 `filesystem_error`，由 Google Test 捕获、rc=1；异常传播已证实，线程 terminate 未证实。 | 使用 `error_code` 重载；日志记录创建目录失败并返回空句柄，避免异常穿出数据读取 API。 |
| R21-BUG-13 | **原产品缺陷主张撤回。** enabled 冲突测试观察到 POSIX 的日志差异，但未找到必须一致的日志合同，其他系统未验证。 | 可将路径非目录的诊断一致性列为改进项。 |

**命令证据：**`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_adstorage`。历史 R21 批次 10 项中 7 PASS、3 FAIL，但含现状式断言，不代表最终版本全部通过。修正后的 `RepeatReadSameWindowReturnsEachTickOnce`、`RepeatedWindowReadsPreserveTickData`、`EtimeBetweenTicksExcludesLaterTickFromSlice` 分别单跑均 FAIL，具体值见上；等值上界候选未运行。历史 disabled 空 vector 两项和 null sink 用例 rc=139；filesystem 冲突用例 rc=1（Google Test 捕获异常），不代表线程 terminate。
