# 缺陷记录 — p05_datastorage2（车道 p05，WtDataStorage / WtDataStorageAD）

> 本迭代只记录缺陷，不修改生产代码；状态一律「待审核」。

> 复核注记（2026-10-29）：除 BUG-P05-001 下明确记录的重复读取定向测试外，其余条目的测试运行情况以各条证据为准；测试源存在不等于已运行。

## BUG-P05-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:146-148`（`get_k_db`）、`src/WtDataStorageAD/WtRdmDtReaderAD.cpp:414-418`、`src/WtDataStorageAD/WtDataReaderAD.cpp:576-578`、`src/WtDataStorageAD/WtDataWriterAD.cpp:1038-1040`
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.RepeatedDayBarReadsReturnStableBytes`；另有 `p05_rdmdtreader_ad_slices_test.cpp::P05RdmReaderADTest.WindowClippingUsesCachedBars` 和禁用的 `DISABLED_RepeatedKlineSlicesShouldNotReturnNull`。原 `SecondLookupSucceedsThirdIsPoisonedByMoveFromCache` 未断言第三次读取；现将其与重复的禁用理想行为用例合并为启用的正确契约测试。
- 输入/前置条件: 同一 `(exchg, period)` 的第三次取数。`auto it = the_map->find(exchg); if (it != end()) return std::move(it->second);`
- 期望行为: 命中缓存并返回可用的只读 LMDB 环境，第 N 次取数与第 1 次结果一致。
- 实际行为: `std::move` 把 map 里的 `shared_ptr` 移到本地（键仍在、值为空）。当前一次调用使用本地句柄可成功；下次 lookup 命中空值，`db == NULL` 后读取失败或 writer 无库可写。准确失败次数取决于首次开库/首次缓存命中路径。
- 复现步骤: 构建 `CoverageTests_adstorage` 后设置 `LLVM_PROFILE_FILE=/tmp/wt-review-p05-%p.profraw`，运行 `--gtest_filter=P05BtReaderADTest.RepeatedDayBarReadsReturnStableBytes`。独立 LMDB 中首读和第二次读取成功且内容一致，第三次读取返回 false，结果长度 0 而首读为 440 字节，正确契约测试 FAIL。源码的 shared_ptr 移动语义解释缓存槽位被清空。
- 影响范围: 命中该缓存的 AD 读写类：`WtBtDtReaderAD.cpp:146-148`、`WtRdmDtReaderAD.cpp:414-418`、`WtDataReaderAD.cpp:576-578`、`WtDataWriterAD.cpp:1038-1040` 都移动 map 中的 `shared_ptr`。map key 保留、值变空；下次命中返回空指针，读侧返回失败、写侧无 db 可写，存在静默漏读/漏写风险。
- 复核裁定: 原测试遗漏第三次断言；改正后本机已复现 `WtBtDtReaderAD` 同一日线库第三次读取失败。其它 reader/writer 的相同代码模式为源码证据，尚未分别动态验证。
- 另一本地读取对照：`WtDataReaderAD` 的 `R4ReaderADTest.SecondCodeOnSameExchangeReadsSeededBars` 首合约读取后请求另一个已写入同交易所 LMDB 的合约，期望 1 根、实际 NULL，定向 FAIL；相关 `RepeatTickReadPreservesDataAndAdvancesWindow` PASS。首次操作内部可能多次取句柄，不能将失败笼统表述为 API 第三次调用。
- 修复方向: 所有 `get_k_db`/`get_t_db` 缓存命中都返回 shared_ptr 拷贝（`return it->second;`），不要 move map 元素；保留现有连续多次读的正确契约回归，另增加写侧直接断言，并在安全前提满足时启用 RDM 的理想行为用例。

## BUG-P05-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtRdmDtReaderAD.cpp:353/367/376`（`readKlineSliceByRange` 切片段）、`:216/230/238`（`readTickSliceByRange` 切片段）
- 相关测试: `p05_rdmdtreader_ad_slices_test.cpp::P05RdmReaderADTest.DISABLED_EmptyResultSetsCrashOnVectorSubscriptZero`（5 类输入收在同一个 DISABLED_ 用例；本轮未运行）
- 输入/前置条件: 请求区间内没有任何数据（db 存在但该合约 0 条；或只有别的合约；或请求的交易日落在库外）。此时 `barsList._bars` / `tickList._ticks` 为空。
- 期望行为: 返回 NULL 或 0 条切片。
- 实际行为: `std::lower_bound(&v[0], &v[0] + (v.size() - 1), ...)` 对空 `vector` 取 `&v[0]`（UB），`v.size()-1` 下溢；随后无条件访问 `pBar/pTick`。常见结果是崩溃，但 C++ 未定义行为不保证具体表现为段错误。
- 测试前提复核: 五种场景串在同一禁用用例中，不能分别证明五条路径：首次读取 SHFE 日线后，第二次同交易所读可能被 BUG-P05-001 的空数据库句柄提前截断；后续空分钟线一旦发生未定义行为，tick 场景也无法继续验证。本轮未运行该用例。
- 复现步骤: 需要拆成独立 reader、独立数据库及隔离进程的单场景用例，并分别记录到达空向量路径的证据；在修复前使用 sanitizer 诊断，修复后验证安全空结果。原复合测试不能作为五条独立动态复现。
- 影响范围: 数据库目录已存在而查询区间无匹配数据时可触发；缺少数据库目录则 `get_k_db/get_t_db` 会提前返回，不经过空 vector 切片尾段。
- 修复方向: bars/ticks 生成切片前先判断空向量并返回 NULL/空切片；改用 `begin()/end()`，明确 `[start,end]` 区间语义并处理单元素、首元素前、尾元素后等边界。不要只检查空向量后继续用会越界的 `p--`。

## BUG-P05-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:171-192`、`src/WtDataStorageAD/WtRdmDtReaderAD.cpp:436-457`、`src/WtDataStorageAD/WtDataReaderAD.cpp:597-618`（三个读侧 `get_t_db`）
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.TickDbCacheKeyNeverMatchesSoEveryCallReopens`（只反复断言结果正确；没有 open 次数/句柄复用断言，本轮未运行）
- 输入/前置条件: 对同一 `exchg, code` 连续走 tick 读取路径。
- 期望行为: 第二次命中 `_tick_dbs` 缓存，复用同一个只读环境。
- 实际行为: 三个读侧都用 `"exchg.code"` 查询，却只以 `exchg` 插入，因此 map 不命中、每次重新打开环境。`WtDataWriterAD` 的写读键都是复合 `key`，不属于此问题。重复开库由代码分支可证，但并非已运行日志观测。
- 复现步骤: 测试做了多轮结果正确性检查，没有 open counter / handle-identity 断言；本轮未运行。需增加缓存观测后才可声称有运行复现。
- 影响范围: 三个读侧 `get_t_db`：Bt、Rdm、DataReaderAD 的查询键为 `"{exchg}.{code}"`，写入键分别为 `exchg`、`exchg`、`exchg`，导致每次读侧查找 miss 并重开环境。`WtDataWriterAD.cpp:1057-1072` 则读写都用 `key`，不属于本缺陷。当前测试只证明结果可读，没有证明重新打开的次数。
- 修复方向: 三个读侧统一以 `key` 写入 `_tick_dbs[key]`；增加直接观测 open 次数或缓存实例复用的断言。该项是性能/资源缺陷，当前证据不支持“结果错误”。

## BUG-P05-004

- 状态: 已合并（迭代02：主编号 BUG-R32-003）
- 严重级别: 待定（与 BUG-R32-003 有重叠，暂不独立计数）
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.cpp:489/522`（`LMDBBarKey(exchg, code, (uint32_t)bar.time)`）、读侧 `src/WtDataStorageAD/WtBtDtReaderAD.cpp:73-74`、`WtRdmDtReaderAD.cpp:300/322`、`WtDataReaderAD.cpp:218/247/275` 的 `0xffffffff` 上界哨兵
- 相关测试: `p05_minbar_key_overflow_test.cpp`（`BarsBeyond2033EscapeTheUpperSentinel`、`TruncatedKeyTailCollidesWithOlderBar`、`MinBarTimeGrowsMonotonicallyUpTo2032`）
- 输入/前置条件: 若一个语义正确、超过 uint32 的 `bar.time` 能到达 `pipeToM1Bars/pipeToM5Bars`，键构造处的 `(uint32_t)bar.time` 会截断。当前 writer 的 `updateBarCache` 先在 `WtDataWriterAD.cpp:698/:800` 调用 `TimeUtils::timeToMinBar`；该 helper 在 `src/Share/TimeUtils.hpp:337-340` 先进行 uint32 乘法并已回绕（BUG-R32-003）。
- 期望行为: 键尾对全部合法日期单调、可寻址，且读侧 `[0, 0xffffffff]` 能覆盖合约的整个生命周期。
- 实际行为: 测试文件用独立 uint64 算式构造真值，再手工将真值传入 `LMDBBarKey(uint32)`；它证明了 32 位键尾本身会截断/碰撞，但没有证明当前 writer 能把该正确宽值传到 pipe 函数。当前 writer 的上游 `TimeUtils::timeToMinBar` 已先回绕，因此此项与 BUG-R32-003 重叠，不能把测试夹具描述成真实 writer 路径复现。
- 复现步骤: `p05_minbar_key_overflow_test.cpp` 中有纯 LMDB 夹具代码，但本轮未运行；测试文件不是运行证据。
- 影响范围: 当前已证实的产品影响是 BUG-R32-003 的时间生成回绕。额外的键截断影响仅在正确宽值可到达 writer 时成立；该数据流待验证。日线键尾用 `bar.date` 不受此条影响。
- 修复方向: 先修复并验证 `timeToMinBar` 的宽位乘法及读侧时间转换；然后确认 AD writer 是否可能接收外部正确宽值。若可以，须一并设计兼容性迁移（LMDB 键尾升宽或限定日期并拒绝），不能只把键尾改宽而遗漏所有读侧哨兵。


- 迭代02复核: DUPLICATE。测试手工将独立正确 uint64 时间压入 uint32 key 可展示截断，但当前真实 writer 上游先在 timeToMinBar 回绕；当前可证的生产错误与 BUG-R32-003 重叠，额外扩展 key 格式要在迁移政策下处理。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-P05-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtDataWriterAD.h:245`（`bool _async_task;` 无初始化）、`WtDataWriterAD.cpp:59-66`（构造列表未含）、`:73-104`（`init` 未读 `async` 参数）、`:373-414`（`pushTask`）、`:106-114`（`release`）
- 相关测试: 无（本车道因此**不驱动** `WtDataWriterAD::writeTick`，AD writer 主体 1073 行未覆盖，见清单“跳过目标”）
- 输入/前置条件: `new WtDataWriterAD()` 后调用 `writeTick`。`_async_task` 在整个 `src/WtDataStorageAD/` 里只出现两处（声明 + 读取），从未被写入。
- 期望行为: 同步/异步由配置决定；`release()` 有界返回。
- 实际行为: 读到未初始化内存（MSAN 会报 use-of-uninitialized-value；普通构建取决于堆上残留），随机走线程分支。线程分支里 `while(_tasks.empty()){ lock; _task_cond.wait(_task_mtx); }` 是**无谓词的 wait**，`release()` 只做 `notify_all + join`：若通知落在“判空之后、进入 wait 之前”的窗口，就会永久阻塞（丢唤醒死锁），且 `_tasks.empty()` 的判空本身在锁外（数据竞争）。
- 复现步骤: 静态检查：`grep -n "_async_task" src/WtDataStorageAD/*` → 只有 `WtDataWriterAD.h:245` 与 `WtDataWriterAD.cpp:375`。
- 影响范围: `writeTick` 等触发 `pushTask` 的 AD writer 路径。async 标志值未定义；若走线程分支，队列锁外 `empty()` 和无谓词 `wait` 与 release 的 notify 顺序存在数据竞争/丢唤醒死锁窗口。本轮无运行挂死证据，撤回“已出现 5 条 HANG”的说法。
- 修复方向: 构造时初始化 `_async_task` 并明确配置来源；用同一 mutex 保护队列检查与取出，条件变量使用包含“已终止或队列非空”的谓词，release 在受保护状态下通知并 join；用可控并发测试覆盖退出竞态。

## BUG-P05-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/WtBtDtReaderAD.cpp:84-88`（`read_raw_bars`）、`:114-118`（`read_raw_ticks`）
- 相关测试: `p05_btdtreader_ad_lmdb_test.cpp::P05BtReaderADTest.DISABLED_ShortLmdbValuesOverreadStructBuffer`（构造空/短 bar value 与短 tick value；本轮未运行）
- 输入/前置条件: LMDB 里存在长度不等于 `sizeof(WTSBarStruct)` / `sizeof(WTSTickStruct)` 的 value（老版本结构体、半截写入、外部工具写入）。
- 期望行为: 逐条校验 `item.size()`，不匹配则跳过/报错。
- 实际行为: `buffer.resize(sizeof(结构体)*cnt)` 后逐项 `memcpy(cursor, item.data(), sizeof(结构体))` → 短 value 会越界读；长 value 被截断。长短混合时 N 个固定大小写入恰好占满 N 个结构体缓冲区，不会导致 cursor 越界写。
- 复现步骤: DISABLED 测试含空/短值构造；可尝试显式启用运行，但本轮未执行。
- 复核裁定: 短 value 路径确会从 `item.data()` 读取结构体大小，属于损坏/非规范 LMDB value 的条件性越界读。测试注释中“同一区间混合长短 value 会使 cursor 溢出 buffer”不成立：buffer 是 `N*sizeof(struct)`，每项固定拷 `sizeof(struct)`，N 项写满但不越界；长 value 会被截断。当前没有运行证据。
- 修复方向: 对 bars/ticks 每个 value 在 memcpy 前验证 `item.size() == sizeof(struct)`，坏条目明确失败或跳过并记录；修订测试注释，保留空/短值输入，增加长值策略断言。与 L04-005 是类似防护模式但不同调用点，不代表该项已实测。

## BUG-P05-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtRdmDtReader.cpp:1666-1672`（`indexBarFromCacheByRange`）、`:1715-1721`（`indexBarFromCacheByCount`）
- 相关测试: `src/TestUnits/coverage/q03_storage3/q03_rdm_kline_file_test.cpp::day_window_out_of_range_returns_null` 覆盖公开入口的早窗口；本轮未运行，且返回 NULL 不能证明内部 UB 安全。P09 还有直接私有方法的 DISABLED 复现定义。
- 输入/前置条件: 公开 `readKlineSliceByRange` 收到非零且早于首根缓存 bar 的 `etime`；仅 `etime==0` 会在 `WtRdmDtReader.cpp:1807-1808` 归一化，不能据此排除明确早区间输入。
- 期望行为: 返回 0 条 / NULL。
- 实际行为: 缺 `if (eit == barsList._bars.begin()) return 0;` 保护（同文件 `readBarsFromCacheByRange:1762-1764` **有**这个保护），直接 `eit--` → 下界之前解引用，`eIdx` 变成 `(size_t)-1`，`curCnt = eIdx - sIdx + 1` 整型回绕。
- 复现步骤: 源码显示 `indexBarFromCacheByRange/ByCount` 中 `eit==begin()` 时仍 decrement（`WtRdmDtReader.cpp:1654-1669,1703-1718`）；已有公开入口 `readKlineSliceByRange` 可传明确早区间。本轮未运行复现。q03 的早窗口用例即使返回 NULL，也不能证明 UB 路径安全。
- 影响范围: 非 AD `WtRdmDtReader` 的公开 K 线区间读取，用户请求整体早于缓存区间时可能出现越界迭代器和计数回绕。
- 复核裁定: 与已有 `BUG-P09-001` 同源，应保留一个 canonical 条目并把 P05 作为交叉引用，避免重复计数。修复方向是在 `eit==begin()` 且首 bar 晚于结束时间时返回 NULL/0，并覆盖公开输入及单元素缓存。
