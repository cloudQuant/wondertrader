# 缺陷记录 — p05_datastorage2（车道 p05，WtDataStorage / WtDataStorageAD）

> 本迭代只记录缺陷，不修改生产代码；状态一律「待审核」。

## BUG-P05-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:146-148`（`get_k_db`）、`src/WtDataStorageAD/WtRdmDtReaderAD.cpp:415-417`（`get_k_db`）；同型写法见 `WtDataReaderAD.cpp:554+`
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.SecondLookupSucceedsThirdIsPoisonedByMoveFromCache`、`p05_rdmdtreader_ad_slices_test.cpp::P05RdmReaderADTest.WindowClippingUsesCachedBars`；理想行为用例 `DISABLED_KlineDbCacheShouldSurviveRepeatedLookups`、`DISABLED_RepeatedKlineSlicesShouldNotReturnNull`（修复后启用）
- 输入/前置条件: 同一 `(exchg, period)` 的第三次取数。`auto it = the_map->find(exchg); if (it != end()) return std::move(it->second);`
- 期望行为: 命中缓存并返回可用的只读 LMDB 环境，第 N 次取数与第 1 次结果一致。
- 实际行为: `std::move` 把 map 里的 `shared_ptr` 掏成空对象（键仍在、值为空）。第 2 次仍正常（把缓存搬空），第 3 次 `find` 命中空值 → `db == NULL` → `read_raw_bars` 返回 false / `readKlineSliceByRange` 返回 NULL，**静默丢数据**，且没有任何错误日志。
- 复现步骤: `src/TestUnits/coverage/p05_datastorage2/p05_btdtreader_ad_lmdb_test.cpp` 里对 `day/DCE` 建库后连续调用 `read_raw_bars("DCE","m2409",KP_DAY, buf)` 三次，第三次返回 false。
- 影响范围: 所有走 AD 存储的 K 线读取方（回测每根 bar 都会取数、`WtDataReaderAD` 的缓存刷新循环同样受影响），第二次之后随时可能拿到空指针。
- 备注: 阻塞“AD 读侧可用”这一契约；修复方向是 `return it->second;`（拷贝）或用 `const` 引用取回，不涉及行为变更。

## BUG-P05-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:353/367/376`（`readKlineSliceByRange` 切片段）、`:216/230/238`（`readTickSliceByRange` 切片段）
- 相关测试: `p05_rdmdtreader_ad_slices_test.cpp::P05RdmReaderADTest.DISABLED_EmptyResultSetsCrashOnVectorSubscriptZero`（5 类输入收在同一个 DISABLED_ 用例）
- 输入/前置条件: 请求区间内没有任何数据（db 存在但该合约 0 条；或只有别的合约；或请求的交易日落在库外）。此时 `barsList._bars` / `tickList._ticks` 为空。
- 期望行为: 返回 NULL 或 0 条切片。
- 实际行为: `std::lower_bound(&v[0], &v[0] + (v.size() - 1), ...)` 对空 `vector` 取 `&v[0]`（越界解引用，UB），`v.size() - 1` 下溢成 `SIZE_MAX`，搜索区间变成 `[0, SIZE_MAX)`；随后 `pBar->date` / `pTick->action_date` 无条件解引用 → 段错误。
- 复现步骤: 用 `--gtest_also_run_disabled_tests --gtest_filter='*EmptyResultSetsCrash*'` 运行；正常覆盖构建（ASAN）下这些输入会中止整轮用例，故未纳入常规执行。
- 影响范围: 新入库/空库/跨日查询都会命中；回测启动阶段（当日尚无数据）最容易触发。
- 备注: 修复方向是先 `if(v.empty()) return NULL;`，并统一用 `v.begin()/v.end()` 迭代器 + `std::distance`，参照 `WtRdmDtReader::readBarsFromCacheByRange` 已有的 `if (eit == begin()) return 0;` 保护写法。

## BUG-P05-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:171-192`（`get_t_db`：查表键 `"{exchg}.{code}"`，写表键 `exchg`）、`src/WtDataStorageAD/WtRdmDtReaderAD.cpp:438-457` 同型
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.TickDbCacheKeyNeverMatchesSoEveryCallReopens`（钉住“结果仍正确”的下界）
- 输入/前置条件: 连续两次 `read_raw_ticks(exchg, code, date)`。
- 期望行为: 第二次命中 `_tick_dbs` 缓存，复用同一个只读环境。
- 实际行为: `_tick_dbs[exchg] = dbPtr` 与查找键 `"exchg.code"` 永不相等 → 缓存永不命中，每次调用都 `new WtLMDB(true)` + `mdb_env_open` 重开同一个目录；同一交易所的 tick 库数量一多就是 fd/ mmap 抖动，且 `_tick_dbs[exchg]` 的覆盖写会随时销毁上一个环境（仅因返回的 `shared_ptr` 计数才没变成悬垂）。
- 复现步骤: 见上述用例，对 `DCE.i2409` / `DCE.j2409` 三个回合交替读取，结果正确但 `get_t_db` 每次都走开库分支（可用日志 `Tick db of {}.{} opened` 计数验证）。
- 影响范围: AD 存储的 tick 读取路径性能与资源占用；若日后把写表键改回 `exchg`，会立刻叠加 BUG-P05-001 的 `std::move` 掏空问题。
- 备注: 不阻塞覆盖率目标。修复方向：写表键与查表键统一为 `"{exchg}.{code}"`。

## BUG-P05-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.cpp:489/522`（`LMDBBarKey(exchg, code, (uint32_t)bar.time)`）、读侧 `src/WtDataStorageAD/WtBtDtReaderAD.cpp:73-74`、`WtRdmDtReaderAD.cpp:300/322`、`WtDataReaderAD.cpp:218/247/275` 的 `0xffffffff` 上界哨兵
- 相关测试: `p05_minbar_key_overflow_test.cpp`（`BarsBeyond2033EscapeTheUpperSentinel`、`TruncatedKeyTailCollidesWithOlderBar`、`MinBarTimeGrowsMonotonicallyUpTo2032`）
- 输入/前置条件: 分钟线 `bar.time = (date-19900000)*10000 + hhmm`（uint64）。20321231 → 4,212,311,000（仍在 uint32 内）；20330101 → 4,301,010,000 > 2^32-1。
- 期望行为: 键尾对全部合法日期单调、可寻址，且读侧 `[0, 0xffffffff]` 能覆盖合约的整个生命周期。
- 实际行为: 键尾被静默截断成 `4,301,010,000 - 2^32 = 6,042,704`，时间轴回绕到 1990 年附近；用例证明回绕条目仍会被 `0xffffffff` 区间扫到，但排序位置错误，并且与更早的键**同键覆盖**（`TruncatedKeyTailCollidesWithOlderBar` 里 2 条只读回 1 条）。
- 复现步骤: 直接运行 `p05_minbar_key_overflow_test.cpp`（纯 LMDB + 算术，无落盘依赖）。
- 影响范围: 2033 年以后的分钟线写入/读取（含 `update_cache_from_lmdb` 的 `lastBarTime` 去重比较，回绕后会误判为“更老的数据”）。日线键尾用 `bar.date`，不受影响。
- 备注: 属于日期回绕/整型溢出类。修复方向是键尾升为 64 位，或在入库前断言 `bar.time <= 0xffffffff`。

## BUG-P05-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.h:245`（`bool _async_task;` 无初始化）、`WtDataWriterAD.cpp:59-66`（构造列表未含）、`:73-104`（`init` 未读 `async` 参数）、`:373-414`（`pushTask`）、`:106-114`（`release`）
- 相关测试: 无（本车道因此**不驱动** `WtDataWriterAD::writeTick`，AD writer 主体 1073 行未覆盖，见清单“跳过目标”）
- 输入/前置条件: `new WtDataWriterAD()` 后调用 `writeTick`。`_async_task` 在整个 `src/WtDataStorageAD/` 里只出现两处（声明 + 读取），从未被写入。
- 期望行为: 同步/异步由配置决定；`release()` 有界返回。
- 实际行为: 读到未初始化内存（MSAN 会报 use-of-uninitialized-value；普通构建取决于堆上残留），随机走线程分支。线程分支里 `while(_tasks.empty()){ lock; _task_cond.wait(_task_mtx); }` 是**无谓词的 wait**，`release()` 只做 `notify_all + join`：若通知落在“判空之后、进入 wait 之前”的窗口，就会永久阻塞（丢唤醒死锁），且 `_tasks.empty()` 的判空本身在锁外（数据竞争）。
- 复现步骤: 静态检查：`grep -n "_async_task" src/WtDataStorageAD/*` → 只有 `WtDataWriterAD.h:245` 与 `WtDataWriterAD.cpp:375`。
- 影响范围: AD 数据落地引擎的启动与退出；一旦堆残留为真，用例/进程可能在 `release()` 处挂死（本波已出现 5 条 HANG，属于同一缺陷类）。
- 备注: 阻塞 AD writer 的覆盖率。修复方向：构造列表补 `_async_task(false)` 并从配置读取；`wait` 改成带 `_terminated || !_tasks.empty()` 谓词的 `wait_for`。

## BUG-P05-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:84-88`（`read_raw_bars`）、`:114-118`（`read_raw_ticks`）
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.DISABLED_ShortLmdbValuesOverreadStructBuffer`（4 类输入：value 长度 0、短于结构体、tick 短 value、长短混合，全部收在同一个 DISABLED_ 用例）
- 输入/前置条件: LMDB 里存在长度不等于 `sizeof(WTSBarStruct)` / `sizeof(WTSTickStruct)` 的 value（老版本结构体、半截写入、外部工具写入）。
- 期望行为: 逐条校验 `item.size()`，不匹配则跳过/报错。
- 实际行为: `buffer.resize(sizeof(结构体)*cnt)` 后 `memcpy(cursor, item.data(), sizeof(结构体))` → 短 value 越界读、长短混合时 `cursor` 越过 `buffer` 末尾（越界写）。
- 复现步骤: `--gtest_also_run_disabled_tests --gtest_filter='*ShortLmdbValuesOverread*'`。
- 影响范围: 与 BUG-L04-001/005 同一缺陷类（长度未校验），覆盖整个缺陷类的所有输入。
- 备注: ASAN 构建下会中止整轮，故不进常规执行。

## BUG-P05-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp:1666-1672`（`indexBarFromCacheByRange`）、`:1715-1721`（`indexBarFromCacheByCount`）
- 相关测试: 无（`WtRdmDtReader` 主体依赖 `IBaseDataMgr::calcTradingDate` + 真实 his 目录 + 实时共享内存块，本车道未覆盖，见清单）
- 输入/前置条件: `etime` 早于缓存里第一根 bar（`lower_bound` 返回 `begin()` 且 `begin()->date > eBar.date`）。
- 期望行为: 返回 0 条 / NULL。
- 实际行为: 缺 `if (eit == barsList._bars.begin()) return 0;` 保护（同文件 `readBarsFromCacheByRange:1762-1764` **有**这个保护），直接 `eit--` → 下界之前解引用，`eIdx` 变成 `(size_t)-1`，`curCnt = eIdx - sIdx + 1` 整型回绕。
- 复现步骤: 构造 `_bars_cache` 从 20240102 开始的数据，调用 `indexBarFromCacheByRange(key, stime, etime=201001010000, count, true)`。
- 影响范围: 非 AD 读侧的所有“查更早区间”调用（回放回看到库起点之前时很常见）。
- 备注: 属于同一缺陷类的两处实例（range / byCount），已合并记录；修复方向与 `readBarsFromCacheByRange` 对齐。
