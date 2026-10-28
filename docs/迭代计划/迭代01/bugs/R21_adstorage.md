# R21 缺陷登记 — WtDataStorageAD（WtRdmDtReaderAD.cpp / WtBtDtReaderAD.cpp / LMDBKeys.h）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
凡"启用即崩/挂死/抛异常穿出"的现象，一律在用例里以 `DISABLED_` 登记并写清触发条件。

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
- 证据用例（`r21_rdmtick_slice_test.cpp`）:
  - `FirstTickRangeReadLoadsThreeTicksInActionTimeOrder`
    日志实测 `3 ticks between [20240102000016430,20240102000016600] of XEX.rb.rb2409 loaded to cache`
    （期望语义应为 `[20240102093015500,20240102110015500]`）
  - `SingleTickWindowYieldsOneTickSliceAndEqualBoundaryTimes` → `[20240102000000930,20240102000000930]`
  - `OlderBranchPrependsPreviousTradingDayTicksAndSlicesTwo`（`:174` 同一形态）

## R21-BUG-02 ｜P0｜复读同一区间会把已有 tick 整批重复追加，切片静默翻倍
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:111`（`bNeedNewer` 判据）、
  `:180-202`（更新分支），关键是 `:184` 用 R21-BUG-01 的碎数当区间下界
- 现象: `_last_tick_time % 1000000000` ≈ 16600，而库里真实键的时间位是 9 位 HHMMSSsss
  （≥ 93000000）→ `get_range` 的下边界**低于全部已有记录** → 已有的 3 笔被整批再读一遍，
  `:191` 用 `emplace_back` 追加（不判重）→ 缓存条数 3→6→9 单调膨胀，切片同样返回重复数据，
  全程没有任何 LL_ERROR/LL_WARN。
- 同时 `:111` 的判据本身不可靠：碎数远小于任何合法 etime，`bNeedNewer` 几乎恒真。
- 证据用例（`r21_rdmtick_slice_test.cpp`）:
  - `RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates`（3 笔→切片 6 条，价格序列
    3901,3902,3903,3901,3902,3903）
  - `ThreeSequentialReadsEachReopenTheSameTickDb`（三轮 size 依次为 3 / 6 / 9）
- 建议: 首尾时间与键时间统一为 `offsetTime(HHMM)*100000 + SSsss`，
  且追加分支改按时间做去重/截断。

## R21-BUG-05 ｜P0｜get_k_db 命中分支 `return std::move(it->second)` 掏空缓存，第三次取即空句柄
- 状态: 待审核
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
  - `r21_rdmbars_kline_test.cpp` → `ThirdKlineLookupOfSameExchangeReturnsNullAfterCacheHandleMovedOut`
    （第 1/2 次切片 3 根，第 3 次 NULL，LL_ERROR 计数为 0，开环日志只有 1 条）
  - `r21_rdmbars_kline_test.cpp` → `ThirdCodeOfSameExchangeFailsBecauseOneHandleServesTheWholeExchange`
    （rb2409 可用、rb2410 可用、**同交易所第 3 个合约 rb2411 直接 NULL**）
  - `r21_rdmbars_kline_test.cpp` → `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated`
    （第 2 次走的是被搬空的缓存项，句柄仍可用）
- 建议: 4 处命中分支改 `return it->second;`。

## R21-BUG-03 ｜P2｜get_t_db 查键与存键不一致，tick 库永不命中缓存
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:439`（查 `"exchg.code"`）vs `:456`（存 `_tick_dbs[exchg]`）；
  同形缺陷在 `src/WtDataStorageAD/WtBtDtReaderAD.cpp:171` vs `:191`
- 现象: 每次读取都 `new WtLMDB(true)` + `mdb_env_create/open`（`:443-446`），
  旧句柄被 `_tick_dbs[exchg] = dbPtr` 覆盖后关闭 → 重复开环、重复 mmap。
- 证据用例:
  - `r21_rdmtick_slice_test.cpp` → `ThreeSequentialReadsEachReopenTheSameTickDb`
    （`Tick db of XEX.rb2409 opened` 命中 3 次）
  - `r21_rdmtick_slice_test.cpp` → `RepeatReadSameWindowAppendsCachedTicksAgainAndSlicesDuplicates`（2 次）
  - `r21_rdmtick_slice_test.cpp` → `EachCodeHasItsOwnTickDbAndDoesNotLeakKeys`（两个合约各 1 次）

## R21-BUG-06 ｜P1｜区间内无记录时对空 vector 取 `&v[0]`，并把上界写成 `size()-1`
- 状态: 待审核
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
  一旦某次返回条数少于已有条数，前面的 bar 会被就地覆写、尾部残留旧数据（不是追加语义）。
- 证据用例: `r21_rdmbars_kline_test.cpp` →
  `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated`
  （两次读取各打一条 `Reading back d1 bars of XEX.rb2409...`，开环只有 1 条）
- 对照: tick 侧 `:143/:176/:200` 会写 `_first/_last_tick_time`（虽然量纲错，见 R21-BUG-01），
  K 线侧连写都没写。

## R21-BUG-04 ｜P3｜请求上界那一笔：LMDB 区间算「含」，切片算「不含」，缓存却永久留着它
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:124-125`（`get_range` 闭区间，见
  `src/WTSUtils/WtLMDB.hpp:219-258` 的 `MDB_SET_RANGE` + `memcmp > 0` 才 break）
  与 `:224`（切片用 `pTick->action_time >= eTick.action_time` 回退）
- 现象: `etime` 恰好等于库里某笔的 `action_time` 时，该笔被读进缓存（`:126` 的 resize 按命中条数算），
  但被 `:224` 从切片里剔除 → 调用方看不到、缓存里长期多留一条越界记录，
  并参与后续 `bNeedOlder/bNeedNewer` 与重复追加（R21-BUG-02）。
- 证据用例: `r21_rdmtick_slice_test.cpp` →
  `EtimeOnAnExistingTickIsLoadedButExcludedFromTheSlice`
  （日志 `2 ticks between [...]` 表示缓存收了 2 条，切片 `size()==1`）

## R21-BUG-11 ｜P1｜WtRdmDtReaderAD::init 先解引用 sink、后判 cfg
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:62-68`
  （`:64` `sink->get_basedata_mgr()`、`:65` `sink->get_hot_mgr()` 在 `:67` 的 `cfg == NULL` 之前，
  且整段没有对 `sink` 判空）
- 现象: `init(cfg_or_NULL, NULL)` 时空 this 解引用 → 崩溃；
  另外 `:85-86` 的 `getCommodity(...)->getSessionInfo()` 与 `:269` 同样不判空，
  未知品种（`cInfo._product` 不在基础数据里）直接崩在读取路径上。
- 证据用例（整条 DISABLED，启用即崩）: `r21_lmdbkeys_btreader_test.cpp` →
  `DISABLED_RdmInitWithNullSinkDereferencesBeforeCfgGuard`
  触发条件：`new WtRdmDtReaderAD()` 或 `createRdmDtReader()` 后 `init(NULL, NULL)`。
- 对照: `WtBtDtReaderAD.cpp:51-56` 的 init 只 `_sink = sink` 再判 cfg，不解引用 → 未崩。

## R21-BUG-12 ｜P2｜rdm 侧 create_directories 未带 error_code，路径被同名普通文件占用时异常穿出
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:421`（get_k_db）、`:445`（get_t_db）
- 现象: 与 R4-BUG-05 同一形态（`boost::filesystem::create_directories(path)` 无 `error_code` 重载）：
  `{base}ticks/{exchg}/{code}` 或 `{base}{subdir}/{exchg}` 被同名普通文件占用时抛
  `filesystem_error`，读取路径上没有 catch → 异常穿出 `readTickSliceByRange`，
  在无 try 的宿主（策略/服务线程）里就是 `std::terminate`。
  bt 侧不会抛（`WtBtDtReaderAD.cpp:152/:178` 先 `StdFile::exists` 再 open，走 LL_ERROR 分支），
  两个读取器的健壮性等级不一致。
- 证据用例（整条 DISABLED）: `r21_rdmtick_slice_test.cpp` →
  `DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfReadTickSlice`
  触发条件：预先把 `{base}ticks/XEX/rb2409` 建成普通文件，再 `readTickSliceByRange`。
- 安全对照（启用）: `r21_lmdbkeys_btreader_test.cpp` →
  `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures`（bt 不抛异常，但两支的日志命运不同，见 R21-BUG-13）

## R21-BUG-13 ｜P2｜bt 的 get_k_db 路径带结尾斜杠、get_t_db 不带，同类故障分岔到不同分支
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:151-153`（`"{}{}/{}/"` + `StdFile::exists`）
  vs `:177-179`（`"{}ticks/{}/{}"` 无结尾斜杠）
- 现象: `{base}{subdir}/{exchg}/` 这个尾斜杠让 `std::filesystem::exists`
  （`src/Share/StdUtils.hpp:137-146`，底层是 `stat`）对「路径被同名普通文件占用」
  直接返回 false（POSIX 对普通文件加尾斜杠必 `ENOTDIR`）→ K 线库在 `:153` 就静默返回空句柄，
  **`:155-159` 的打开失败日志与错误文案永远不会出现**；
  而 tick 侧（无尾斜杠）走的是 `:181-185` 的 LL_ERROR。
  同一个「目录被文件占了」的运维故障，两条路径给出的可观测性完全不同（一条有 ERROR、一条什么都没有），
  排障时容易误判成「库里就是没数据」。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` →
  `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures`
  （bars：false + LL_ERROR 0；ticks：false + LL_ERROR 1，原文 `Opening tick db of XEX.rb2409 failed: ...`）
- 建议: 两支统一去掉尾斜杠（或统一加），并让「路径存在但不是目录」单独给一条 LL_WARN。

## R21-BUG-08 ｜P3｜LMDBKeys.h 的 reverseEndian(uint16_t) 在 WtDataStorageAD 内无任何调用点
- 状态: 待审核
- 位置: `src/WtDataStorageAD/LMDBKeys.h:24-29`（`[[maybe_unused]] static uint16_t reverseEndian(uint16_t)`）
- 现象: 全模块（`WtDataWriterAD.cpp` / `WtDataReaderAD.cpp` / `WtRdmDtReaderAD.cpp` /
  `WtBtDtReaderAD.cpp`）只用 `LMDBHftKey`/`LMDBBarKey` 的 uint32 字段，
  没有任何地方传 16 位实参 → 该重载在产品 TU 里不生成代码，
  这 3 行**无法**由 AD 目标的生产路径带进覆盖（本轮报表把 LMDBKeys.h 记 0.00% 的 25 行里含它）。
  只有 `src/TestUnits/**` 里有直接调用（l04 与本轮 R21）。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` → `ReverseEndian16SwapsTheTwoBytes`（只能证语义，不产生 AD 目标增量）

## R21-BUG-09 ｜P2｜bt 读取器「读不到数据」与「读成功」都返回 true，空 buffer 无标记
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:75-91`（read_raw_bars）、`:105-121`（read_raw_ticks）
  —— 回调里 `if (ayVals.empty()) return;`（`:77-78`、`:107-108`）后函数仍 `return true`
- 现象: 库存在但区间内 0 条时，返回 true 且 `buffer` 保持调用方传入的原值
  （**既不清空也不置长度**）→ 若调用方复用同一个 string，会读到上一次的残留字节并当成有效数据。
  与 `get_k_db/get_t_db` 失败时的 false 无法区分「没有数据」和「路径没建好」。
- 证据用例: `r21_lmdbkeys_btreader_test.cpp` →
  `BtReadRawTicksOnDateWithoutRecordsReturnsTrueWithEmptyBuffer`
  （交易日 20240103 无键 → true + 空 buffer；`:107-108` 的空数组早退分支）
- 建议: 进函数先 `buffer.clear()`，并返回命中条数（或 0 条时返回 false）。

## R21-BUG-10 ｜P3｜交易所/周期库打开失败的日志文案笔误（"db if"）
- 状态: 待审核
- 位置: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:157`（`"Opening {} db if {} failed: {}"`）、
  `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:424`（同一笔误）；
  同文件 `:448` / `:183` 的 tick 文案用的是正确的 `"of"`
- 现象: 运维检索 `Opening .* db of` 会漏掉全部 K 线库失败记录（只有 tick 库那条能搜到）。
- 证据用例: 无（该分支运行时不可达 —— K 线库的尾斜杠让 `:152` 的存在性检查先把门关掉，
  见 R21-BUG-13；rdm 侧 `:424` 同理，且在它前面就抛异常，见 R21-BUG-12）→ 仅按源码文案登记。
- 说明: 用例按**现状原文**断言（tick 侧那条 LL_ERROR 文案已逐字断言，见
  `BtDbPathOccupiedByRegularFileTakesExistenceAndOpenFailures`），没有为了让断言通过而放宽匹配。

---

## 本轮留白（不注册为缺陷，但要说清为什么没测）

1. `WtRdmDtReaderAD.cpp:115-116`、`:293-294` 的「句柄为空即早退」：
   rdm 的 `get_t_db` 因 R21-BUG-03 永不返回缓存项（每次新建），而 `get_k_db` 的 NULL 分支
   已由 `ThirdKlineLookupOfSameExchangeReturnsNullAfterCacheHandleMovedOut` 覆盖；
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
