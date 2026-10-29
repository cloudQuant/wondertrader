# R4 缺陷登记 — WtDataStorageAD（WtDataWriterAD.cpp / WtDataReaderAD.cpp）

本轮 `src/**` 只读，以下缺陷**均未修改产品代码**，也没有放宽任何断言。
原登记中的 `DISABLED_` 用例需按实际运行结果解释；本轮独立复核结论和未验证边界见文末，不能仅凭用例名推定已复现。

分级口径：P0 = 静默丢数据/必崩；P1 = 特定输入崩溃或数据错误；P2 = 资源与健壮性；P3 = 语义可疑/死代码。

---

## R4-BUG-02 ｜P0｜LMDB 句柄缓存被 `std::move` 掏空，后续命中返回空指针
- 状态: 产品缺陷（`get_k_db` 移空与 BUG-P05-001 同源；P05-001 明确覆盖的仅是所列 `get_k_db` 位置。本条仍保留 writer `get_t_db` 命中路径及其独立 writer 证据，不标为整条已合并）
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1040`（get_k_db）、`:1060`（get_t_db）；
  `src/WtDataStorageAD/WtDataReaderAD.cpp:578`（get_k_db）、`:602`（get_t_db）
- 现象: 命中缓存时 `return std::move(it->second);` 把 map 里的 `shared_ptr` 移走并返回，
  缓存项随即变成空指针（但 key 仍在）。第 1 次取用返回有效句柄、第 2 次返回"搬家后"的有效句柄、
  后续取用返回空指针；一个公开读操作内部可能多次取库，因此 API 级别第几次请求失败取决于调用路径。
- 写入侧后果: `pipeToTicks`（`WtDataWriterAD.cpp:420`）与 `pipeToM1Bars/M5Bars/DayBars`
  （`:454/:487/:520`）都只在 `if (db)` 上做分支；命中已移空的缓存槽、返回空句柄时会**跳过写入且不报错、不打日志**。具体丢失条数取决于 open/命中顺序，旧用例观察的 2/4 不能当作稳定的产品结果；修正后的 writer 回归尚未运行。
- 读取侧后果: `update_cache_from_lmdb`（`WtDataReaderAD.cpp:273`）没有判空，直接
  `WtLMDBQuery query(*db);` → 空句柄时会解引用空 `shared_ptr`。`get_t_db` 的查存键不一致（R4-BUG-07）会遮蔽 tick 路径上的 move 命中；当前动态用例没有单独复现该崩溃。
- 证据用例:
  - writer tick 回归 `DISABLED_TickDbStoresAllTradingDateKeyedRecords` 覆盖 `WtDataWriterAD::get_t_db`（未列入 P05-001）；分钟线回归 `DISABLED_MinuteBarRolloverPipesAllCompletedBarsIntoMin1Db` 覆盖 writer `get_k_db`（与 P05-001 覆盖位置重叠）。两者都把旧的“4 笔只落 2 笔/4 次滚动只落 2 根”错误通过条件改为完整数据断言；因写入路径读取未初始化 `_async_task` 而禁用、NOT_RUN，旧结果还受堆块清零 fixture 的未定义行为影响。
  - `r4_reader_ad_slices_test.cpp` 的 `SecondCodeOnSameExchangeReadsSeededBars`：两个合约均有预置数据，第二个却返回 NULL，修正后的期望测试单跑 FAIL。
  - `r4_reader_ad_slices_test.cpp` → `DISABLED_AdvancingEtimeAfterTwoHandlesCrashesOnNullDb` 仅记录了预期场景；该 disabled 用例本轮未单独运行，空句柄崩溃尚无独立动态复现。它描述的是同一交易所/周期缓存命中移空后、etime 前进时再次取 `get_k_db` 的路径，API 调用次数取决于内部回源次数。
- 建议: 命中分支改 `return it->second;`（4 处）。

## R4-BUG-03 ｜P1｜日线 K 线成交量/成交额重复累加（翻倍）
- 状态: 源码确认的产品缺陷；正确 oracle 已写入但受 `_async_task` 未初始化阻塞，尚未运行
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:649-652`
- 现象: 同一交易日追加 tick 的 else 分支里 `vol`/`money` 各自被 `+=` 了两次
  （649-650 与 651-652 是重复语句）；m1（754-757）、m5（834-835）分支都只累加一次。
- 后果: 落地日线 `vol`/`money` 为真实值的约 2 倍（首条 tick 除外）。
- 证据用例: 旧 `BarCacheMergesTicksOfDayMinute5AndMinute1` 与 `DayBarRolloverPipesIntoDayDbWithDuplicatedVolume` 曾把日线 30/300 当作正确结果；现分别改为 `DISABLED_BarCacheMergesTicksOfDayMinute5AndMinute1BlockedByUninitializedAsyncTask` 与 `DISABLED_DayBarRolloverPipesIntoDayDbUsesCorrectVolumeMoneyBlockedByUninitializedAsyncTask`，按两笔各 10 手/各 100 成交额断言正确值 20/200。两条新 oracle 均因写入路径读取未初始化 `_async_task` 而禁用，NOT_RUN；安全初始化控制用例 PASS 不等于日线回归已验证。

## R4-BUG-01 ｜P1｜`_async_task` 全仓库无赋值，写线程是否启动取决于堆残值
- 状态: 已合并（主记录 BUG-P05-005 覆盖未初始化成员及 worker/release 竞态；R4 的同步控制证据保留在本条）
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
- 证据用例: `r4_writer_ad_cache_test.cpp` → `ReleaseWithoutTaskThreadIsInert` PASS（只覆盖无 worker 的同步分支）。异步分支、队列锁外检查及 `release()` 丢唤醒竞态均未动态验证；以清零堆块强迫同步分支的 fixture 不能证明成员初始化正确。

## R4-BUG-09 ｜P1｜`groupsize` 缺省为 0 时存在整数除零风险
- 状态: 静态条件风险；动态未复现
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:88`（`_log_group_size = params->getUInt32("groupsize")`，无默认值）
  → `:362`（`_tcnt_map[...] % _log_group_size == 0`）
- 静态风险: `WTSVariant::getUInt32` 缺少键时默认返回 0，且 `:362` 直接对 `_log_group_size` 取模；若该值确为 0 且执行到该语句，会触发整数除零。
- 证据边界: `DISABLED_GroupSizeZeroDividesByZeroOnFirstTick` 实际运行到测试显式 `FAIL()` 后以 rc=1 退出，没有观察到 SIGFPE；因此目前只是静态候选，不能写作已动态复现。
- 正常分支对照: `r4_writer_ad_cache_test.cpp` → `LogGroupSizeEmitsCountEveryNthTick`。

## R4-BUG-08 ｜P1｜`onMinuteEnd` 对空 K 线缓存取 `back()`、对 `get_rt_cache_bar` 的 NULL 不判空
- 状态: 已动态确认空缓存 `back()` 崩溃；RT bar NULL 解引用仅有源码证据、未单独验证
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:511`、`:530`（`_bars.back()`，非日线与日线分支）；
  `:517-518`、`:535-536`（`rtBar->time` / `rtBar->date` 未判空）
- 现象: 只要 `_bars_cache` 里存在一条"一条 bar 都没读到"的条目（回源失败即会产生），
  分钟结束时就可能在空 `boost::circular_buffer` 上取 `back()`（越界读，UB）；
  而实时缓存里没有该合约时 `get_rt_cache_bar` 返回 NULL，随后被直接解引用。
- 证据用例: `r4_reader_ad_slices_test.cpp` →
  `DISABLED_OnMinuteEndOnBarlessCacheEntryReadsBackOfEmptyBuffer`
  历史运行结果 rc=139，复现 `_bars.back()` 空访问；触发条件：先对某合约 `readKlineSlice`（库中无该合约 bar）→ 条目入缓存但为空 → 再调 `onMinuteEnd`。`get_rt_cache_bar` 返回 NULL 的另一分支未由此用例覆盖。

## R4-BUG-06 ｜P2｜`readTickSlice` 的 reload_flag 判定被后一个条件覆盖，首次读取退化为无上限区间扫描
- 状态: 产品性能缺陷；当前小数据用例通过，但未量化长历史成本
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:122-140`（`:127` 置 2，`:135` 立刻覆盖为 1）
- 现象: 首次读取时"容量不足"（应走 `get_lowers` 按 `count` 限量取数）与
  "请求时间前进"两个条件同时成立，`reload_flag` 最终是 1 → 走 `get_range`，
  把 `[上次时间+1, etime]` 区间内的**全部** tick 读进一个只有 `count` 容量的循环缓冲区
  （`count` 限制形同失效，历史越长放大越明显），且 `:171` 的全量分支基本不可达。
- 证据用例: `r4_reader_ad_slices_test.cpp` →
  `FirstTickReadTakesIncrementalRangeAndKeepsLastCount`
  PASS（库中 3 笔、容量/结果为 2 笔，实际查询 3 笔）；该小数据用例显示请求数量未限制扫描条数，但不能量化长历史数据的额外开销。

## R4-BUG-07 ｜P2｜reader `get_t_db` 存键与查键不一致，tick 库永不命中缓存
- 状态: 已合并（主记录 BUG-P05-003；R21-BUG-03 同源；效率影响仍是候选，重复 open 是否违反公开性能契约待评估）
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:599`（查 `"exchg.code"`）vs `:617`（存 `_tick_dbs[exchg]`）
- 现象: 查找用 `交易所.合约`、存储用交易所名做 key → 每次读取都重新
  `WtLMDB(true)::open` 一个环境；map 里同一交易所槽位会被新指针覆盖，旧句柄在无其他引用时析构（重复开环，不是句柄逐次堆积）。
- 证据用例: 旧 `RepeatReadAtSameEtimeSkipsQueryButReopensDb` 曾把重复 open 写成通过条件；现改为 `RepeatTickReadPreservesDataAndAdvancesWindow`，PASS 数据正确性和时间窗口对照。重复打开次数不属于该用例的公开契约断言；同 key map 槽位被覆盖，不随调用无限累积句柄。

## R4-BUG-05 ｜P2｜`create_directories` 未带 error_code，路径被同名普通文件占用时异常穿出任务体
- 状态: 条件健壮性缺陷；异常穿出同步写入已复现，异步 worker 终止未验证（与 R21-BUG-12 同模式、不同调用路径）
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1044`、`:1064`；
  `src/WtDataStorageAD/WtDataReaderAD.cpp:582`、`:606`
- 现象: `boost::filesystem::create_directories(path)`（无 `error_code` 重载）在路径上已有
  普通文件时抛 `filesystem_error`。本轮同步测试中异常穿出被 Google Test 捕获（rc=1）；若异常逃出工作线程才会触发 `std::terminate`，当前测试并未证明该线程场景。
- 证据用例: `r4_writer_ad_lmdb_test.cpp` →
  `DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick`
  用普通文件占位后调用写入，`filesystem_error` 被 Google Test 捕获、进程 rc=1；这证明异常穿出同步调用，不能据此宣称异步线程触发 `std::terminate`。

## R4-BUG-04 ｜P2｜procFlag==2 补最低价用的是 `max`
- 状态: 源码确认的产品缺陷；正确 oracle 已写入但受 `_async_task` 未初始化阻塞，NOT_RUN
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:1006-1007`
- 现象: `if (decimal::eq(newTick.low, 0)) newTick.low = max(newTick.price, item._tick.low);`
  —— 与 high 的补齐对称写法应为 `min`；现实现把最低价抬到不低于上一笔最低价。
- 证据用例: 旧 `ProcFlagTwoAccumulatesTotals` 固化 low=3900 的现状，现改为 `DISABLED_ProcFlagTwoAccumulatesTotals`，按上一笔 low=3900、本笔 price=3800 正确期待 low=3800。该公开写入路径读取未初始化的 `_async_task`，现用例暂时禁用、未运行；max 的错误来自源码，不能将 low=3900 写成本次动态复现。

## R4-BUG-10 ｜P2｜AD 缓存块加载不校验块头，也不校验文件大小
- 状态: 条件健壮性缺陷；损坏块头、截断文件及 SIGBUS 路径未验证
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:116-281`（`loadCache` 四段同构）
- 现象: 只做 `_size = min(_size, _capacity)`（`:136/:177/:218/:259`），
  既不检查 `_blk_flag`/`_type`/`_version`（块头里的 `BT_RT_Cache`、版本号 1 写进去后没人读），
  也不检查映射长度是否真的容得下 `capacity` 个 item；
  截断或跨版本（例如将来 `BarCacheItem` 加字段）的 `.dmb` 会被照单全收，
  重建索引时越界读映射尾部。
- 证据用例: `r4_writer_ad_cache_test.cpp` →
  `ReloadExistingCacheClampsSizeAndRebuildsIndex`（使用有效块头，将 `_size > _capacity` 后验证 size 被夹到 capacity；未验证垃圾块头或截断文件）、
  `FreshInitBuildsFourBlockFilesWithValidADHeader`（写入侧确实写了规范块头，只是没人校验）。
- 说明: 截断文件会触发映射尾部越界读（可能 SIGBUS），不宜作为启用用例，故只登记不落测。

## R4-BUG-11 ｜P2｜`resizeRTBlock` 异常分支返回旧块，调用方继续按新容量写
- 状态: 条件产品缺陷；正常扩容已覆盖，扩容失败分支未注入验证
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:299-311`（catch 后 `return mfPtr->addr();`），
  调用方 `:582/:671/:774/:880` 紧接着写 `_items[idx]`，而 `idx` 已是 `>= 旧 capacity`
- 现象: 扩文件失败（磁盘满等）时并不回退 `_size`，也不中止本笔写入 → 越界写映射区。
- 证据用例: 正常扩容分支由
  `r4_writer_ad_cache_test.cpp` → `TickCacheAtCapacityResizesFileAndKeepsOldItems` 覆盖
  （400+402 条、块头 capacity 回写、原条目仍在）；异常分支需要让 `BoostFile::write_file` 失败，
  本轮无安全的注入点，故只登记。

## R4-BUG-12 ｜P3｜`read_bars_to_buffer` 是未调用的死代码，存在待修风险
- 状态: 已撤回（活动产品缺陷判定撤回；保留为死代码风险，全仓库无调用点）
- 位置: `src/WtDataStorageAD/WtDataReaderAD.cpp:208-228`
- 现象: ①`:219-220` 把 `get_range` 的右边界也传成了 `lKey`（`rKey` 构造后未用）；
  ②`:225` `memcpy(buffer.data(), ayVals.data(), sizeof(WTSBarStruct)*ayVals.size())`
  把 `std::vector<std::string>` 的内部数组指针当成连续 bar 数据拷贝。
  全仓库无调用点（仅 `WtDataReaderAD.h:302` 声明）。
- 证据用例: 无（私有且无调用方，无法从公开接口到达）；仅登记。

## R4-BUG-13 ｜P3｜日线缓存项的 `time` 字段被写成交易日（语义待契约确认）
- 状态: 语义待定；当前证据不足以判为产品缺陷
- 位置: `src/WtDataStorageAD/WtDataWriterAD.cpp:601`（`barDate = curTick->tradingdate()`）
  与 `:620`（`newBar->time = barDate;`）
- 静态观察: 日线的 `time` 与 `date` 同值（都是交易日）；但当前未找到日线 `WTSBarStruct.time` 必须表示动作日期/时间的明确接口契约，不能据此断言数据丢失。
  m1/m5 的分钟语义本身不能证明日线字段必须采用相同编码。读取侧（`WtDataReaderAD.cpp:286`）日线按 `date` 取键，当前 AD 读取路径不依赖该 `time` 值。
- 证据用例: `r4_writer_ad_cache_test.cpp` → `BarCacheMergesTicksOfDayMinute5AndMinute1`
  （`d1._bar.time == 20230103`）。

---

## 本轮独立复核裁定（2026-10-29）

分类说明：**产品缺陷**表示源码与当前契约/运行结果相符；**条件缺陷**表示静态风险成立但故障注入或输入前提尚未验证；**误报/非产品缺陷**表示现有证据不支持登记中的结论；**重复**表示与既有候选同根因。下表中的 PASS 仅指所列本地测试，不代表更大范围验收。

| 编号 | 裁定与证据边界 | 最小修复方向 |
|---|---|---|
| R4-BUG-01 | **已合并至 P05-005。** `_async_task` 声明未初始化，唯一读取用于决定线程分支；构造/init 未赋值。worker 对队列的无锁检查及无谓词等待也有竞态。`ReleaseWithoutTaskThreadIsInert` PASS 只覆盖同步分支；fixture 释放零内存再 `new` 不能保证对象成员被初始化，异步及 release 竞态未验证。R4 保留该同步对照证据，不再重复计活动缺陷。 | 构造/init 显式读取有默认值的 async 配置；用互斥量保护队列并以谓词等待；测试同步/异步写入和并发 release。 |
| R4-BUG-02 | **产品缺陷；部分根因与 P05-001 重复，不整条合并。** P05-001 明列的 DataReaderAD/DataWriterAD `get_k_db` move 命中与本条重叠；R4 另有 `WtDataWriterAD::get_t_db` 命中路径和 tick writer 用例，未纳入 P05-001 的模块位置。writer tick 与分钟线正确断言已写入但因未初始化 `_async_task` 禁用、NOT_RUN；第二合约 reader 正确契约回归单跑 FAIL。reader tick move 路径会被 R4-BUG-07 查存键不一致遮蔽，空句柄崩溃尚未单独复现。 | 缓存命中返回 `it->second`；调用方仍对 open/缓存失败做显式错误处理。 |
| R4-BUG-03 | **源码确认产品缺陷。** `WtDataWriterAD.cpp:649-652` 对日线 vol/money 重复 `+=`。旧日线滚动和缓存聚合用例把错误值 30/300 当预期；现已改为正确的 20/200，并因写入路径读取未初始化 `_async_task` 明确禁用。新 oracle 尚未运行，安全初始化控制 PASS。 | 删除重复累加语句，先修复 R4-BUG-01 或提供安全接缝，再启用两条正确回归。 |
| R4-BUG-04 | **源码确认产品缺陷。** procFlag==2 补 low 使用 `max`；`ProcFlagTwoAccumulatesTotals` 现改为正确预期 3800，受未初始化 `_async_task` 阻塞而禁用，NOT_RUN。 | low 聚合改为 `min`，先修复 R4-BUG-01 的初始化问题/提供安全接缝，再覆盖新价格低于/高于既有 low 两种情形。 |
| R4-BUG-05 | **条件健壮性缺陷；与 R21-BUG-12 同模式、不同调用路径。** filesystem 冲突用例报告 `filesystem_error` 并由 Google Test 捕获（rc=1），证明异常穿出同步写入路径；未证明异步线程上的终止行为。 | 使用 `error_code` 重载创建目录；记录错误并中止写入，不让异常逃出 worker。 |
| R4-BUG-06 | **产品性能缺陷。** 首次读取“容量不足”标志被后续时间条件覆盖，导致走范围扫描。`FirstTickReadTakesIncrementalRangeAndKeepsLastCount` PASS：查询三条、容量/结果为两条；小数据用例不能量化长历史成本。 | 容量不足分支优先，按 `count` 限量读取；增加大历史数据的查询条数断言。 |
| R4-BUG-07 | **已并入 P05-003（与 R21-BUG-03 同源）；效率影响仍为候选。** 查 `exchg.code`、存 `exchg` 导致 tick DB 每次 miss；旧测试曾把重复 open 当作预期，现改为 `RepeatTickReadPreservesDataAndAdvancesWindow` 数据正确性对照并 PASS。重复打开是否违反公开性能契约需另行评估；map 槽位被同 key 覆盖，不会随调用无限累积句柄。 | 统一查找/存储 key，并按数据库实际共享粒度决定交易所级或合约级缓存。 |
| R4-BUG-08 | **已确认空缓存崩溃；第二个 NULL 分支为静态条件缺陷。** disabled 用例实际 rc=139，复现 `_bars.back()` 空访问；`get_rt_cache_bar` 返回 NULL 后解引用也存在，但本用例未单独验证该分支。 | 分别检查 `_bars.empty()` 与 RT bar 指针；为两个分支各加覆盖。 |
| R4-BUG-09 | **静态条件风险，动态未复现。** 缺键时 `getUInt32` 默认 0，随后对 `_log_group_size` 取模；若执行到该语句会除零。但 disabled 用例运行到显式 `FAIL()`，没有 SIGFPE，测试名不能作为复现证据。 | 使用非零默认值（如 1000）或拒绝 0；测试确认控制流确实执行到取模。 |
| R4-BUG-10 | **条件健壮性缺陷；原测试证据被过度表述。** loadCache 未校验映射长度/头字段；现有测试使用有效块头，仅验证 `_size > _capacity` 时夹断。损坏或截断文件及 SIGBUS 未测试。 | 映射后校验长度、flag/type/version、capacity 与 payload 大小，再遍历 item；增加截断文件失败关闭用例。 |
| R4-BUG-11 | **条件产品缺陷，故障分支待注入验证。** resize catch 返回旧映射、调用方继续按新索引写的源码路径成立；正常扩容用例 PASS，写入/map 失败未注入，因此越界写未动态复现。 | 扩容失败返回明确错误并使调用方中止写入，保证 size/index 不先提交；增加可注入 I/O 失败测试。 |
| R4-BUG-12 | **死代码，不构成当前可达产品缺陷。** `read_bars_to_buffer` 的边界参数和 memcpy 类型明显不匹配，但全仓库只有声明和定义、无调用点；没有公开路径可复现。 | 启用前修正为实际 bar 解码和半开区间；当前从活动缺陷计数移除或标成死代码风险。 |
| R4-BUG-13 | **语义待定，不足以判为产品缺陷。** 用例只证明日线 `.time == tradingdate`；当前读取键使用 `date`，未找到要求 `.time` 保存动作日期的接口契约。 | 核对对外日线字段约定及消费者，再决定规范值并补测试。 |

**命令证据：**本地可执行文件 `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_adstorage`。历史 R4 writer/reader PASS 受旧断言语义限制。后续将第二合约的“返回 NULL”现状断言改为读取已写入 K 线，`SecondCodeOnSameExchangeReadsSeededBars` 定向 FAIL：第二合约实际返回 NULL；重复 Tick 读取数据和时间窗口的 `RepeatTickReadPreservesDataAndAdvancesWindow` PASS，不再将是否重开数据库当作公开契约。disabled 空缓存用例旧记录 rc=139；groupsize 用例 rc=1（测试显式 FAIL）；filesystem 冲突用例 rc=1（Google Test 捕获异常）。这些结果不证明异步线程竞态、损坏缓存文件或 resize 失败分支。
