# 08_ad_storage 第二轮复核

## 汇总

本组 16 项逐条核对原候选、实现、公开接口、正常调用者与测试：PRODUCT 10、TEST 4、NEEDS_DECISION 1、EXTERNAL_BLOCKED 1；LOCAL_REPRO 2、STATIC 14、PENDING 0。本轮只写复核产物，未改代码/测试/旧报告/构建文件。

刷新后的 CoverageTests_adstorage SHA-256：b906a816a4420f624e0f06cae44f3921557d96ba79cf8a87f52ad7b1d89a0d2c。6 个安全 singleton 独立进程/cwd/profile/XML、15 秒限制：5 PASS、1 FAIL。原始命令、目标 SHA、returncode、测试数和 stdout 在 evidence/08_ad_storage/safe-runs.json 与单例目录。

## 逐条裁定

### R21-BUG-07 — PRODUCT（LOCAL_REPRO）

本轮 singleton PASS 确认重复窗口每次都执行全范围 get_range；小 fixture 返回仍正确，本结论限有效查询读放大，不声称已有延迟 SLA。

**根因：** _last_bar_time 只初始化为0，始终未更新，增量下界恒为0。  
**修复/验收：** 成功加载后维护末 bar 游标，后续只查游标之后；保留必要的首次全量路径。 同窗口复读不扫全量；新数据只查游标后，返回内容/顺序不变。

### R21-BUG-04 — NEEDS_DECISION（STATIC）

LMDB 内部闭区间不等于输出边界。AD 与历史表现为 [stime,etime)，默认 RT 对 etime 等值保留，存在跨路径兼容歧义。between 单例本轮 FAIL 是 R32 首 tick 覆写，不能裁定端点。

**根因：** 公开端点合同缺失，RT/history 等值比较不一致。  
**修复/验收：** 建议统一左闭右开，和 AD/history 一致且避免相邻窗口重复；若选闭区间则改 AD/history 并评估调用者。 用 before/equal/after 与相邻窗口 fixture，在 AD/history/RT 验证同一所选合同。

### R21-BUG-11 — TEST（STATIC）

源码的空 sink 解引用成立，但用例人工违反初始化前置并混合 cfg/sink 为空；无生产空 sink 来源，撤回缺陷主张。

**根因：** 测试绕过 factory 传入未声明 nullable 的 sink。  
**修复/验收：** 不改产品行为；正常调用保持有效 sink，若未来允许 null 再定义错误返回。 保留 factory/有效 sink 生命周期测试，不以手工 NULL sink 为产品回归。

### R21-BUG-12 — PRODUCT（STATIC）

create_directories 使用抛异常重载，冲突时 open 错误处理不可达。保留异常传播缺口；原测试注释所称必然 std::terminate 没有证据。

**根因：** 创建目录失败未转成 reader API 错误。  
**修复/验收：** 用 error_code 创建目录，记录 path/error 并返回空 DB/slice，避免异常逃逸。 隔离普通文件冲突返回约定空结果和错误日志，后续有效查询可继续。

### BUG-R45-001 — TEST（STATIC）

二次直接调用确会使用已释放 API，但唯一触发方式是手工 double-release；正常 manager 只释放一次，撤回产品缺陷证据。

**根因：** 测试构造不在生产生命周期中的重复释放。  
**修复/验收：** 不改产品；验证 manager 正常一次释放。 API 恰好 release/delete 一次，manager map 清空。

### BUG-R45-002 — TEST（STATIC）

二次调用覆盖旧 api 的源码事实存在，但当前 case 人工重复私有/内部方法；没有正常重载调用者，撤回现产品主张。

**根因：** 测试假定未经承诺的重复扩展初始化合法。  
**修复/验收：** 不因本项改产品；未来支持 reload 时定义旧 API 清理和替换策略。 单次 init 与 manager close 正常；只有热重载合同确定后再测替换。

### R4-BUG-09 — PRODUCT（STATIC）

init 无条件以 getter 覆盖默认，缺键返回0，writeTick 后取模。disabled case 仅 GTEST_SKIP，不是除零复现。

**根因：** 缺少缺键默认与显式零校验。  
**修复/验收：** 缺键保留1000；显式0记录错误并让 init 失败。 缺键/0/正数三种 init-only 测试；不调用 writeTick，以避开独立未初始化 async flag。

### R4-BUG-06 — PRODUCT（LOCAL_REPRO）

容量不足先置2，紧接着时间条件覆盖成1。本轮单例 PASS 并观察到 append 全区间3笔，最终只回传2笔；PASS 确认 defect path，不是正确验收。

**根因：** 首载容量与增量时间逻辑共用 reload_flag，后者覆盖前者。  
**修复/验收：** 分离首次 count reload 与增量状态，首读用 get_lowers(count)。 count=2 首读最多扫2笔并返回末两笔；推进只扫新数据，空结果和排序正确。

### R4-BUG-05 — PRODUCT（STATIC）

create_directories 抛异常会离开 task。旧测试由异常直接声称必然 std::terminate，不成立；_async_task 未初始化，当前 writeTick probe 不安全且本轮不运行。

**根因：** 目录错误未转换，且 async flag 有独立初始化缺陷。  
**修复/验收：** error_code 转换并走 writer 错误通道，释放 tick；先初始化 async flag。 确定同步/异步模式分别冲突失败可观测、worker/进程存活，后续有效写成功。

### R4-BUG-10 — PRODUCT（STATIC）

loadCache 忽略 map 结果，addr 后直接 cast，仅 clamp size/capacity；正常 round-trip 不覆盖坏头/截断，存在越界/SIGBUS 风险。

**根因：** 文件长度、map 状态及 magic/type/version 没校验。  
**修复/验收：** 先校验 map/header/version/size/capacity与实际长度，损坏按明确策略拒绝或重建。 隔离测截断头、坏版本、size>capacity、文件短于容量声明；无越界/SIGBUS，按策略拒绝/重建。

### R4-BUG-13 — PRODUCT（STATIC）

AD 把日线 time 写为 trading date，和默认后端不一致。当前 disabled test 错误断言该现状；writeTick 又有 async flag 未初始化，未动态运行。

**根因：** 日线构造将 date 重复写入 time。  
**修复/验收：** AD day.time 写0，date 保留；分钟线语义不改。 相同 fixture 比较后端 date/time/OHLCV，day time=0，M1/M5 不变。

### BUG-R60-005 — EXTERNAL_BLOCKED（STATIC）

early trade 可能空指针；当前测试只测未登记合约并提前返回，stub 时序不能证明 vendor callback 可达。

**根因：** position map 初始化时点晚，trade 路径未判空。  
**修复/验收：** 需要 vendor order clause；若允许 early trade，则初始化/暂存并正确合并；若保证顺序按合同测试。 按真实 DLL/柜台序列覆盖 init 前后 trade，状态正确无崩溃，unknown contract 继续丢弃。

### BUG-R60-006 — PRODUCT（STATIC）

disconnect/release join matcher 却不清指针，isConnected 只检查指针，因此终态仍 true。现 test 仅断言 WTE_Connect 和持仓回报。

**根因：** 把 thread pointer 当连接状态。  
**修复/验收：** 用明确状态机；按统一 ready 定义更新，logout/disconnect/release 清 false。 覆盖初始、event、login success/failure、logout/disconnect/release；按状态合同断言。

### BUG-R60-007 — TEST（STATIC）

NULL 在 retain 处崩溃属源码事实，但未证是有效输入/合同违约；撤回人工 NULL 作为产品缺陷证据。

**根因：** 测试向未声明 nullable 的内部 action 参数传 NULL。  
**修复/验收：** 不按该候选改产品；如 API owner 规定 nullable，再定义失败码并加 guard。 当前只验证正常有效 action 路径；将来合同改变再测 NULL。

### BUG-P05-003 — PRODUCT（STATIC）

三处查 exchange.code 却只按 exchange 插入，命中又 move 出 handle。safe test 检数据但无 open count，故静态成立而未动态量开库次数。

**根因：** key 不一致且 move 清空 map handle。  
**修复/验收：** 统一 key 为 exchange.code；命中返回 shared_ptr 副本。 同 code 多次只 open 一次，不同 code 隔离，数据/空结果正确；增加 open counter/identity。

### BUG-P05-006 — PRODUCT（STATIC）

bar/tick 固定 memcpy struct 大小且不验长度，短值构成越界读。disabled 探针未运行、无 sanitizer；原输出 cursor overflow 主张错误。

**根因：** 反序列化未核验 value 长度/schema。  
**修复/验收：** memcpy 前校验支持长度，诊断 key/date/size；旧格式显式转换。 空/短/精确/长分别覆盖；坏值无越界且可诊断，合法值正常，旧版本按合同转换。

## 测试更正请求

- src/TestUnits/coverage/p05_datastorage2/r21_rdmtick_slice_test.cpp / DISABLED_EtimeOnExistingTickNeedsBoundaryContract：决策后加入 before/equal/after 和相邻窗口断言，跨存储路径统一 oracle。 预期：所有路径遵循选定端点合同。
- src/TestUnits/coverage/p05_datastorage2/r21_lmdbkeys_btreader_test.cpp / DISABLED_RdmInitWithNullSinkDereferencesBeforeCfgGuard：移除其产品崩溃主张；正常 init 通过 factory 和有效 sink，只有合同允许 null 后才另测。 预期：正常 reader 有效初始化与查询通过。
- src/TestUnits/coverage/p05_datastorage2/r21_rdmtick_slice_test.cpp / DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfReadTickSlice：修正文案；加防护后启用隔离冲突测试，断言失败结果/日志和后续有效读取。 预期：无异常逃出、返回空结果、进程继续。
- src/TestUnits/coverage/q05b_dtservo/r45_parseradapter_test.cpp / DISABLED_ReleaseTwiceContractPending：移除 double-release 作为缺陷证据；保留 manager 单次 close 的 API 释放计数。 预期：每个 API 恰好释放一次，map 清空。
- src/TestUnits/coverage/q05b_dtservo/r45_parseradapter_test.cpp / DISABLED_SecondInitExtContractUnproven：撤回该序列缺陷结论；保留单次 init/manager lifecycle。 预期：单次 init/release 正常，不声称重复 initExt 受支持。
- src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_lmdb_test.cpp / DISABLED_GroupSizeZeroWriteProbeBlockedByAsyncTaskInitialization：替换为缺键/0/正数 init-only；不调用 writeTick。 预期：缺键安全默认、0 拒绝、正数成功。
- src/TestUnits/coverage/p05_datastorage2/r4_reader_ad_slices_test.cpp / FirstTickReadTakesIncrementalRangeAndKeepsLastCount：修复后断言首载 get_lowers(count)，加扫描数观测且保留末 count 数据断言。 预期：count=2 最多处理2笔，不走全量 append。
- src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_lmdb_test.cpp / DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick：修正文案；async 与错误处理修好后再分同步/异步隔离测试，当前不启用。 预期：失败可观测且不 terminate，后续有效写成功。
- src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_lmdb_test.cpp / ReloadExistingCacheClampsSizeAndRebuildsIndex：保留合法 case；增加独立坏文件 fixture，待 loader guard 后运行/sanitizer。 预期：坏文件安全拒绝或重建，合法文件仍加载。
- src/TestUnits/coverage/p05_datastorage2/r4_writer_ad_cache_test.cpp / BarCacheMergesTicksOfDayMinute5AndMinute1BlockedByUninitializedAsyncTask：修正 day.time=0；async 生命周期可安全运行后启用，或测纯 bar 构造。 预期：day time=0/date正确，M1/M5不回归。
- src/TestUnits/coverage/t_TraderMocker/r60_tradermocker_async_test.cpp / connect_loads_positions_and_query_returns_long_short：保留持仓回归；新增无网络状态机单测，ready true，disconnect/logout/release false。 预期：ready 状态正确，断开后 false。
- src/TestUnits/coverage/p05_datastorage2/p05_btdtreader_ad_lmdb_test.cpp / TickDbCacheKeyNeverMatchesSoEveryCallReopens：修复后用 counter/identity seam 证明各 reader 的复用，并保留数据断言。 预期：同 code 只开一次，不同 code 隔离，读值正确。
- src/TestUnits/coverage/p05_datastorage2/p05_btdtreader_ad_lmdb_test.cpp / DISABLED_ShortLmdbValuesOverreadStructBuffer：修正文案，bar/tick 空短值拆开；加 guard 后才以隔离 fixture 和 sanitizer 运行。 预期：坏值安全失败无越界，精确长度成功，长值按 schema 拒绝/兼容。

## 定向运行与限制

R21 重复 K 线读取 PASS，证实重复全量读；R21 etime-between FAIL，是 R32 首 tick 覆写，不能作为端点证据；R4 首次 count tick PASS，但用例断言现状全量 append，不是正确性验收；P05 cache 读取 PASS 只证明数据结果；两条 writer cache PASS 仅控制合法文件。

本轮额外的 CoverageTests_dtservo 正常 manager 生命周期正控 PASS 1/1，SHA-256 为 679683540e58c77f78cc68d831d7797e2b099ab2b0fbe7494cd1a3d841da9604；这只验证一次正常释放，未运行 unsupported 重复 init/release。证据见 evidence/final-fix/r45.{json,log,xml}。

未运行危险 writeTick、坏 mmap、短 LMDB、真实 YD callback 时序、固定 UDP；ASan/UBSan NOT_RUN。各条尚缺动作见 JSON local_remaining。

