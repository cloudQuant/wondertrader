# 迭代03 最终缺口修复（slice2，2026-11-05）

范围：第 2 段复核（`scratchpad/review/slice2-result.json`）中标为 C_LOCAL_GAP 的基线条目 BUG-R100-003/BUG-R32-003、BUG-R100-004、BUG-R132-001、BUG-P10-001、BUG-P10-006、BUG-P10-007、BUG-R18-011、BUG-R104-002（并入 I03-NEW-003 的 K13/K14 部分），新增问题 I03-NEW-038、039、045、046；另按主代理追加完成 I03-NEW-111 的 WtDataWriter 子项。

- 工作区：HEAD `ccaad010`（大量未提交迭代改动，其他代理同时在改）。本目录只记录本段改动；**不是整体回归**，不改变 213 分母，未修改任何迭代跟踪文档。
- 构建：`cmake -S src -B scratchpad/slice2-result-build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`，Apple clang 17，默认 `WT_WARNINGS_AS_ERRORS=ON`，`ninja -j 3`。构建日志 `logs/build-*.log`。
- 运行：每个用例在 `scratchpad/slice2-result-run/` 下独立 cwd、独立进程（`work2/runcases.py`，120–180 s 硬超时）；摘要 `logs/*-summary.json`，单例日志 `logs/*.log`、XML `logs/*.xml`。
- 红控方法：
  - 本段新改的产品（WtDataWriter、WtBtDtReader、HisDataReplayer、ParserShm、各 runner 的 required、K14）：用 `before/` 中的改前副本按同一编译命令重编该 TU，仅替换目标里的这一个对象重新链接（`work2/redlink.py`，产物在 `slice2-result-build/red/<名字>/`），其余对象与绿构建相同。
  - 产品已在本段之前修好、只缺用例的（I03-NEW-038 的两个读端、I03-NEW-045/046）：在 `scratchpad/slice2-red-src/` 的源码副本里临时还原对应改动，同样单对象重链。仓库源码未被还原。
- 文件锁：按协调规则经 `scratchpad/review/lock.py` 登记（owner `slice2-result`）。
- 行尾：每个被改文件改前后用 python 二进制读写保持原行尾与 BOM（混合行尾的 `ParserShm.cpp`、`WtDtServo/WtDtRunner.cpp` 逐行保持）；改前改后副本在 `before/`、`after/`，哈希 `sources.sha256`。
- 新发现问题用临时编号 S2-NEW-n，最终编号由主代理分配。

## 逐条结果

下表“红”指改前（或临时还原）对象重链后的结果，“绿”指当前工作区构建的结果；均为每例独立进程。日志与 XML 路径都在 `logs/` 下，`cwd/*/out.log` 为单例原始输出（`scratchpad/slice2-result-run/` 下保留同名目录）。

### BUG-R100-003 / BUG-R32-003：文档化 2032 上限（K33=A）

- 修改：`docs/data_engine.md` 新增 `data_lmdb_minute_limit` 小节（V1 分钟键编码、2032-12-31 为最后可表示交易日、写端拒写并记 ERROR、读端 64 位计算并拒绝、日线不受影响、非 AD 存储不受限、V2 未接入）；`src/WtDataStorageAD/LMDBKeys.h` `_bartime` 注释写明上限并指向文档。
- 文档项，无红绿；行为用例沿用复核时列出的 `S5ReaderADTest` 2033 用例、`P05KeyTailTest.*` 与 mw 夹具 `MwAd.MinuteBarBeyondUint32KeyIsRefusedNotWrapped`（不在本段重跑）。

### BUG-R100-004：WtRdmDtReader RT 读端跨进程发布证明

- 新增用例 `CoverageTests::I03S2R100RdmCrossProcess.ParkedWriterSlotStaysInvisibleUntilPublishedAcrossProcesses`（`q03_storage3/i03_s2_r100_004_rdm_cross_process_test.cpp`）：fork 子进程运行真实 `WtDataWriter`，在 `before_rt_publish` 钩子处以管道确定性停住；本进程用真实 `WtRdmDtReader` 读取：
  - 停点 a：成交槽 101 已写未发布，与已发布槽 100 同一时间戳 → 区间读恰好 101 条，逐条序号与校验和正确；
  - 停点 b：扩容 2500→5000 后槽 2550 未发布 → 读端重映射并读到 2550 条；
  - 停点 c：放行后同一 reader 读到全部 2600 条成交；委托队列槽 5 未发布 → 5 条；
  - 停点 d：委托队列放行后 10 条；tick 槽 3 未发布 → count 与 range 读都为 3 条；结束后 tick 10 条。
- 产品无改动（K36 已在本段之前实施），属补测。红控：在源码副本里把 K36 的 `visibleRecords` 还原为改前 `[0,_size-1)`／未钳映射（`slice2-red-src/WtRdmDtReader.redK36.cpp`）→ `fail`（停点 a 期望 101 实际 100，`logs/red-runs-2.txt`）；绿 `pass`（`logs/green9-new-tests.txt`）。
- 剩余：Windows `LockFileEx`、Linux/NFS 未验；ASan/TSan 由主代理在冻结树运行（见文末清单）。

### BUG-R132-001：K37 解压预算可由宿主配置

- 修改：
  - `src/WTSUtils/WTSCmpHelper.hpp`：新增 fail-closed 状态 `refuse_uncompress()/uncompress_refused()`，置位后 `uncompress_data` 一律抛异常；`set_uncompress_budget` 应用合法预算时解除。
  - 新文件 `src/WTSUtils/WTSCmpBudgetConfig.hpp`：`wt_cmp_budget::apply(cfg, error)` 读取与 `path` 同级的 `uncompress_max_bytes`、`uncompress_max_ratio`（JSON 整数、YAML/JSON 十进制数字串、整数值有限实数；两键都缺省时不变，默认 1 GiB / 1000:1；0 关闭该项）；非法值（负数、小数、布尔、对象、数组、空串、带符号、溢出）返回 Invalid 并置 fail-closed。
  - 接入：`WtBtDtReader::init`（store 配置，非法时记 ERROR，本模块压缩块全部拒绝）、`HisDataReplayer::init`（有 store 节点时取 store，否则取根配置，与 store 路径相同；非法时 `init` 返回 false）、`WtDataWriter::init`（非法时 `init` 返回 false）。预算按模块生效，因此在各自执行解压的模块内应用。
- 用例：`CoverageTests::I03R132CmpBudgetConfig.*`（4，新接口，红控不适用）、`CoverageTests::I03R132BtReaderBudget.*`（4）、`CoverageTests::I03R132WriterBudgetTest.*`（3）、`CoverageTests_btcore::I03R132ReplayerBudget.*`（4）。
- 红控：改前 `WtBtDtReader.cpp`、`WtDataWriter.cpp` 重链（redA）→ BtReader 3 例失败（配置被忽略，压缩块照常读出）、Writer 2 例失败；改前 `HisDataReplayer.cpp` 重链（redBt）→ 3 例失败（非法配置仍 init 成功、预算未生效）。缺省正控在红绿两侧都通过。绿：16/16（main）＋4/4（btcore）。
- **未完成（锁冲突）**：`WtDataReader.cpp`、`WtRdmDtReader.cpp` 被 slice4-result（I03-NEW-083）锁定，未接入；补丁与用例方案见 `pending-patch-R132-locked-readers.md`。在该补丁落地前，实盘读端单独使用时仍不能由宿主配置预算。

### BUG-P10-001：只缺 sanitizer 运行

- 本段不做；需主代理在 ASan+UBSan（alignment 开）树运行 `CoverageTests --gtest_filter='P10ProcBlockTest.*'`，期望 15/15、0 报告。

### BUG-P10-006：普通写端 dumper 注册与遍历并发（K20=B）

- 新增 `CoverageTests::I03S2P10006DumperConcurrency.*`（3 例，`r23_storage4/i03_s2_p10_006_dumper_concurrency_test.cpp`），驱动收盘作业 `transHisData` 的全部 7 类 dumper 调用（d1/m1/m5 K 线、tick、成交、委托明细、委托队列）：
  - dumper A 在第一次回调中阻塞时另一线程 `addDumperV1("B")`、重复 ID、注销未知 ID 都在 5 s 内返回；A 每类恰好一次；B 收不到 A 阻塞那一处（旧快照），其后每类恰好一次；被拒的重复实例从不被调用；
  - 回调期间注销 A：进行中的调用完成，之后不再调用 A，其余 dumper 每类一次；
  - 回调内重入登记不死锁，被登记者自下一类起收到。
- 新接口（改前无 `addDumperV1`），红控不适用；绿 3/3。TSan 重复运行列入主代理清单。

### BUG-P10-007：只缺 TSan 运行

- 用例已存在，本段不改；需主代理在 TSan 树运行 `CoverageTests --gtest_filter='P10RtBlockTest.ExpiredCheckerCloseWaitsForActiveBlockLease:P10RtBlockTest.RealCheckerClosesAndReopensAllSixExpiredMappings:P10RtBlockTest.SixBlockMapsSnapshotCloseReopenAndShutdownAreSynchronized:P10RtBlockTest.AsyncWorkerFirstCreatesAllSixBlocksBeforeRealCheckerSnapshot'`（建议 ×5），期望 0 数据竞争报告。

### BUG-R18-011：消费端 acquire + 跨进程消费者（K21）

- 产品修复：`src/ParserShm/ParserShm.cpp` 接收循环每轮只读一次 `_readable`，随后 `std::atomic_thread_fence(std::memory_order_acquire)`，再按该值判断与读取槽位，与 `ShmCaster::broadcast` 发布前的 release 栅栏成对（原先 volatile 读后直接读槽，arm64 上可能读到旧载荷）。只用标准栅栏，不依赖 GCC 内建，Windows 同样可编译（未验）。
- 用例：
  - `CoverageTests_t_ParserShm::I03S2ShmCrossProcess.ForkedProducerItemsArriveCompleteContiguousAndAcrossWrap`：真实 ParserShm 消费，fork 子进程按 ShmCaster 协议写 20000 条（> 8192，两次回绕）带序号与全结构校验和的 tick，按消费进度限速；断言 0..19999 连续、不丢不重、校验全对。
  - `CoverageTests_dtcore::I03S2ShmCasterCrossProcess.ConcurrentProducersReachForkedConsumerCompleteAndInOrderAcrossWrap`（4 线程 × 5000，真实 ShmCaster 串行化）与 `SingleProducerControl`（单生产者 20000 对照）：fork 子进程按 ParserShm 规则跨进程消费，逐槽校验类型、校验和与各生产线程的序号连续。
- 红绿：绿 1/1、2/2。改前 `ParserShm.cpp` 重链（redShm）同一用例仍通过——内存序缺陷在本机（同核心缓存一致性下）不保证可复现，**没有确定性红控**，修复依据为静态分析。原验收要求的 TSan 红绿（K21 进程内串行化）列入主代理清单。
- 子项“慢消费者落后超过队列容量时的溢出处置”：K21 未裁定，本段未定合同，**不确定，建议按卡15 记用户指示忽略**（依据：仓内无合同；现行为生产者覆盖写、消费者可能读到被覆盖的槽；解除条件：用户裁定溢出策略）。用例以限速避开该场景。
- 外部剩余：跨进程多生产者（K21 B，后续）、Windows。

### BUG-R104-002（并入 I03-NEW-003 的 K13/K14 部分）

- 修改（K13=B，`required` 缺省 true）：
  - `WtPorter/WtRtRunner.cpp initParsers`：初始化抛异常/失败/登记失败时候选照旧隔离；required 时配置失败，optional 时 WARN 并继续。
  - `WtUftRunner/WtUftRunner.cpp initParsers`：optional 失败不计入 failed，WARN 并继续；重复 id 仍是配置错误。
  - `WtDtPorter/WtDtRunner.cpp initParsers`：required 失败照旧 `_startup_blocked`；optional 失败 WARN，候选隔离，启动不被阻止。
  - `WtDtServo/WtDtRunner.cpp initParsers`：required 失败照旧整批失败；optional 失败候选隔离、其余 parser 照常登记。
  - K14：`WtRunner/WtRunner.cpp run()` 逐个提交连接命令并记录未提交的 id；经 `initParsers` 登记的 required parser（新成员 `WtRunner.h::_required_parsers`）未提交时回滚（`startupFailed` → `shutdownRuntime(true)`，traders/engine 不再启动，`runSucceeded()` 为 false）；optional 或直接注入的 parser 仍只报告。未改 WtCore `ParserAdapterMgr::runWithResult`（该文件被 slice4 锁定），在 runner 内逐个调用。
- 用例（每个宿主 required 正控 + optional 反控）：
  - `CoverageTests_m_WtRunner::I03S2K13WtRunnerRequired.*`（2）、`I03S2K14WtRunnerRollback.*`（2）
  - `CoverageTests_porter::I03S2K13RtRunnerRequired.*`（2）
  - `CoverageTests_m_WtUftRunner::I03S2K13UftRunnerRequired.*`（2）
  - `CoverageTests::I03S2K13DtPorterRequired.*`（2）
  - `CoverageTests_dtservo::I03S2K13DtServoRequired.*`（2）
- 红控（改前对象重链）：WtRtRunner、WtUftRunner、WtDtPorter、WtDtServo 的 optional 用例各 1 例失败；WtRunner 的 K14 required 回滚用例失败（改前仍启动 engine、`runSucceeded` 为 true）。WtRunner 的 K13 optional 在本段之前已实现，两侧都通过（补测）。绿：全部通过。
- QuoteFactory 部分在本段之前已完成（复核时已列用例），未改。

### I03-NEW-038：存储读写端日志有界

- 修改 `src/WtDataStorage/WtDataWriter.cpp`：新增 `format_writer_log`（`fmt::format_to_n` 写 512 字节 thread_local 缓冲，超长截断并加 `...[truncated]`），`pipe_writer_log` 与 `defer_writer_log` 都改用它；`getKlineBlock` 的 256 字节 thread_local 路径缓冲改为 `std::string`；文件中其余 19 处 `fmtutil::format(...)`（历史文件名、RT 路径、MARK 任务名、缓存键等，均以配置根目录或代码拼接）改为返回 `std::string` 的 `fmt::format`。
- 用例：`CoverageTests::I03New038WriterLogTest.*`（2：约 620 字节根目录下 init 与块创建日志都 < 512 且带截断标记，RT tick/min1/min5 块在完整长路径下创建；短根目录正控不截断）、`CoverageTests::I03New038ReaderLogTest.*`（3：WtRdmDtReader 长 adjfactor 路径、WtBtDtReader 长根目录、短根目录正控）。
- 红绿：WtDataWriter 改前对象（redA）长根目录用例 exit 134——`libc++abi: terminating due to uncaught exception of type fmt::v11::format_error: output is truncated`；WtRdmDtReader/WtBtDtReader 在副本中还原无界格式化（redB）两例同样 exit 134；正控两侧通过。绿 5/5。
- 新发现 I03-NEW-118（原临时编号 S2-NEW-1，主代理已分配）：fmt 11 对 `char[N]` 数组的 `format_to` 越界即抛 `format_error`，而 `fmtutil::format<N>` 声明为 `noexcept`，因此仓内任何以无界输入调用 `fmtutil::format`/`format<N>` 的位置，超长时不是“写穿缓冲”而是**进程 terminate**；`fmtutil::format_to(char*)` 走指针重载则仍是无界写。本段只处理了 NEW-038/039 范围内的调用点，其余模块未普查。

### I03-NEW-039：其余模块 64 字节缓存键

- 修改 21 处（`std::string key = fmt::format(...)`、`strcat(key,"1")` 改 `key += "1"`、复用键改赋值）：`WtBtCore/SelMocker.cpp`（2）、`WtBtCore/CtaMocker.cpp`（3）、`WtBtCore/HisDataReplayer.cpp`（3）、`WtExecMon/WtSimpDataMgr.cpp`（2）、`WtDtServo/WtDataManager.cpp`（3）、`WtCore/WtDtMgr.cpp`（2）、`WtCore/SelStraBaseCtx.cpp`（2）、`WtCore/CtaStraBaseCtx.cpp`（3）、`WtCore/WtEngine.cpp`（1）。
- 第二阶段时 `WtCore/WtHftEngine.cpp`、`WtCore/WtCtaEngine.cpp`、`WtCore/WtSelEngine.cpp` 各 1 处被 slice1-result 锁定未改；slice1 完成后锁已转给本段，第三阶段已落地（见下文“第三阶段”），NEW-039 现共 24 处全部完成。I03-NEW-083（切片 create 复制代码）按指示未改，`S5StorageReaderTest.DISABLED_LongestLegalStdCodeHistoryTickKeyStaysExact` 仍 DISABLED。
- 用例：`CoverageTests_dtservo::I03S2New039ServoCacheKey.LongestLegalStdCodeKeepsExactCacheKeys`（63 字节合法 stdCode，fake reader 不返回数据、不创建切片；秒级与重采样两个缓存键与完整代码一致）。
- 红绿：改前 `WtDataManager.cpp` 重链（redServo）→ `signal6`（同上 fmt 11 `format_error` → terminate）；绿 pass。其余 20 处为静态修改，仅编译与相关二进制回归覆盖（`thread_local char key[64]` + `fmtutil::format_to(char*)` 的越界写不被 ASan 插桩，无运行红控）。

### I03-NEW-045：WtRtRunner / WtExecRunner 析构保留未排空 owner

- 产品在本段之前已改，属补测。新增 `CoverageTests_porter::I03S2RtRunnerTeardown.DestructorRetainsUndrainedParserCandidateInsteadOfAborting`、`CoverageTests_exefact::I03S2ExecRunnerTeardown.*`（2，含无候选正控）：子进程内注入持有 API、无排空证明的失败候选后析构 runner，期望正常退出且候选被进程级持有者保留。
- 红控：源码副本删除保留逻辑（`slice2-red-src/WtRtRunner.red045046.cpp`、`WtExecRunner.red045.cpp`）→ 子进程 SIGABRT 134；绿 pass。

### I03-NEW-046：porter 单例析构不回调宿主

- 产品在本段之前已改，属补测。新增 `CoverageTests_porter::I03S2PorterHostCallbacks.ProcessExitDestructionDoesNotCallBackIntoTheHost`（显式调用全局 runner 析构模拟进程退出期，ExpParser::release 经 `getRunner()` 回到同一对象：EVENT_PARSER_RELEASE 不再投递）与 `ExplicitReleaseStillNotifiesTheHostOnce`（显式 release 仍投递一次）。
- 红控：副本删除 `_host_callbacks_closed` 置位 → 析构期回调 1 次（期望 0）失败；正控两侧通过。绿 2/2。

### I03-NEW-111（主代理追加）：WtDataWriter `_bd_mgr` 未初始化

- `WtDataWriter::WtDataWriter()` 初始化列表首项（按声明顺序）补 `_bd_mgr(nullptr)`，注释注明 I03-NEW-111，CRLF 保持。
- 现成未 init 用例：`CoverageTests::R122WriterBaseInitTest.R122WriterTickCacheUninitializedIsLogged`、`P10TaskQueueTest.PublicOperationsAreClosedBeforeInitialization` 在绿构建中通过（见回归摘要）；改前的垃圾指针只在 sanitizer 下可见，交主代理 ASan 复跑。

## 相关既有用例复跑（绿构建，每例独立进程）

| 范围 | 结果 | 证据 |
|---|---|---|
| CoverageTests 存储相关组（`I03New038*:I03R132*:R23*:P10*:L04*:L05*:Q03*:R100*:R52*:R112*:R122*:R132*:R142*:R162*:R172*:R182*:R193*:R212*:R70*:R74*:R94*:S5Main*:S5StorageReaderTest.*:WriterResizeAfterExtendTest.*:R55_*`，NEW-038/R132 修改后） | 536/536 | `logs/regress-main-1.txt`、`logs/regress-main-1-summary.json` |
| CoverageTests 全部（含 NEW-039、K13 改动后，官方 `run_regression.py`，4 路） | 1352 例：1348 过、1 跳过（既有子进程用例 `R22PorterCapiTest.ConfiguredStartChildChecksStateMonitor`）、3 例信号终止均为 slice4 进行中的 `S4SliceLongCode.*`（I03-NEW-083，非本段改动）；复修基线 1323 例无一回退 | `logs/regress4-summary.json`、`logs/regress4-results.jsonl.gz` |
| CoverageTests_porter 全部 | 269/269 | 同上 |
| CoverageTests_exefact 全部 | 418/418 | 同上 |
| CoverageTests_btcore 全部 | 691 例：686 过；5 例 `R111ExecMocker.*` 失败原因为“missing fake plugin fixture”——本构建目录未构建 `exec_v2_plugins/*.dylib` 夹具目标（环境），补建后复跑见下行 | 同上 |
| CoverageTests_btcore `R111ExecMocker.*`（补建 exec_v2 夹具后） | 16/16（`R111ExecMocker.*`） | `logs/regress5-btcore-r111.txt` |
| CoverageTests_dtcore / dtservo / t_ParserShm / m_WtUftRunner 全部 | 228 过+1 既有跳过（R44 合同未定）/ 125 / 21 / 56 | `logs/regress2-a.txt`、`logs/regress2-*-summary.json` |
| CoverageTests_m_WtRunner 全部（官方工具预置模块；`real_cta_factory_null_self…` 按 round1 方式不预置单跑） | 108/108 | `logs/regress3-m_WtRunner.txt`、`logs/regress3b-m_WtRunner-unprovisioned.txt` |
| CoverageTests_wtcore 全部（含 NEW-039 的 WtCore 改动） | 1063 例：1050 过、1 既有跳过（卡15 无符号链接用例）；12 例非通过全部是 slice4 进行中的新用例（`S4OwnerRelease.*` 4、`S4ParserAdapterNullComm.*` 4 信号、`R66Fixture.S4EnginePushQuoteWithoutContractSkipsHotFanOut`），以及首轮缺环境的 `R59Exec.diff_stale_*` 2 例与 `S5MainK31DateBinding.StorageAndXeleModulesExportVersionedBinders`——补建 ParserXeleSkt、设 `WT_COV_PLUGIN_BIN` 后 4/4 通过；复修基线 1016 例无回退 | `logs/regress7-wtcore-summary.json`、`logs/regress7b-wtcore-env.txt` |
| CoverageTests_btporter + adstorage 全部 | 447/447 | `logs/regress6-summary.json` |

## 需主代理在 sanitizer 树运行的用例

| 条目 | 二进制 | gtest filter | 期望 |
|---|---|---|---|
| BUG-P10-001 | CoverageTests（ASan+UBSan，alignment 开） | `P10ProcBlockTest.*` | 15/15，0 报告 |
| BUG-P10-007 | CoverageTests（TSan，×5） | `P10RtBlockTest.ExpiredCheckerCloseWaitsForActiveBlockLease:P10RtBlockTest.RealCheckerClosesAndReopensAllSixExpiredMappings:P10RtBlockTest.SixBlockMapsSnapshotCloseReopenAndShutdownAreSynchronized:P10RtBlockTest.AsyncWorkerFirstCreatesAllSixBlocksBeforeRealCheckerSnapshot` | 0 数据竞争 |
| BUG-P10-006 | CoverageTests（TSan，×10） | `I03S2P10006DumperConcurrency.*` | 0 数据竞争（注册锁与快照调用） |
| BUG-R18-011 | CoverageTests_dtcore（TSan） | `S5MainK21ShmCaster.*:I03S2ShmCasterCrossProcess.*` | 0 报告；红控：以 `evidence/20261104-s5-main/k21-shmcaster/before/src/WtDtCore/ShmCaster.cpp` 重链后 `S5MainK21ShmCaster.*` 应报 `_writable`/`_items` 竞争（TSan 看不到跨进程部分） |
| BUG-R18-011 | CoverageTests_t_ParserShm（ASan+UBSan） | `I03S2ShmCrossProcess.*` | 0 报告 |
| BUG-R100-004 | CoverageTests（ASan+UBSan 与 TSan） | `I03S2R100RdmCrossProcess.*:S5MainK36RdmRtVisible.*` | 0 报告 |
| BUG-R132-001 | CoverageTests（ASan+UBSan） | `I03R132*:R132ReaderHisTest.*:S5StorageReaderTest.*:S5MainK37CmpBudget*` | 0 报告 |
| BUG-R132-001 | CoverageTests_btcore（ASan+UBSan） | `I03R132ReplayerBudget.*` | 0 报告 |
| I03-NEW-038 | CoverageTests（ASan+UBSan） | `I03New038*` | 0 报告（thread_local 缓冲不插桩，越界以长度断言观测） |
| I03-NEW-039 | CoverageTests_dtservo（ASan+UBSan） | `I03S2New039ServoCacheKey.*` | 0 报告 |
| I03-NEW-045/046 | CoverageTests_porter、CoverageTests_exefact（ASan+UBSan，LSan 关：保留到进程结束为设计） | `I03S2RtRunnerTeardown.*:I03S2PorterHostCallbacks.*`、`I03S2ExecRunnerTeardown.*` | 0 报告 |
| I03-NEW-111 | CoverageTests（ASan+UBSan） | `R122WriterBaseInitTest.*:P10TaskQueueTest.PublicOperationsAreClosedBeforeInitialization` | 无未初始化读 |
| K13/K14 | 各宿主二进制（ASan+UBSan） | `I03S2K13*:I03S2K14*` | 0 报告 |

## 第三阶段（主代理追加）：I03-NEW-115 与 4 处测试侧 TSan 竞争

- 改前副本 `before-phase3/`（本阶段开始修改时的工作区；`WtEngine.cpp` 已含第二阶段 NEW-039 改动；三个引擎文件取自 slice1 完成 BUG-R10-004 后的内容，未覆盖其 run() 防重入与 SEL on_tick 隔离）；改后 `after-phase3/`；哈希（含红控源码、红/绿二进制）`sources-phase3.sha256`；日志、摘要、XML 在 `logs/phase3/`。第二阶段的 `after/` 与 `sources.sha256` 中 `WtEngine.cpp`、`WtRtRunner.cpp` 两项已被本阶段取代。
- 锁：`WtEngine.cpp/.h` 与新用例（I03-NEW-115）；`WtCtaEngine.cpp`、`WtSelEngine.cpp`、`WtHftEngine.cpp`（slice1 转入）；`r66_tickers_test.cpp`、`udp_handle_write_lock_test.cpp`、`t6_ctploader_yaml_factory_stub.hpp/.cpp`；`WtRtRunner.cpp`（本段已持有，按 I03-NEW-122 追加登记）。`CTPLoader.cpp` 被 slice4-result 锁定，未改。
- 构建：同一构建目录增量构建，`-Werror` 零告警（`logs/phase3/build-*.log`）。期间磁盘写满一次（他方与本段 scratch 合计），已删除本段 `slice2-result-run/` 下自产的 8.7 GB `*.profraw` 后重建；本阶段运行均设 `LLVM_PROFILE_FILE=/dev/null` 或用官方工具（自动删除 profraw）。

### I03-NEW-115：WtEngine 持仓 / 资金 / 时钟数据竞争

加锁顺序固定为 `_pos_mtx → PosInfo::_mtx → _fund_mtx`，无反向嵌套，任何路径都不在持锁时调用 `save_datas()`（SpinMutex 不可重入）。

1. 持仓表（问题 1、2）：新增受保护的 `get_or_create_position()`（`_pos_mtx` 内查找/插入并拷贝 `PosInfoPtr`）与 `snapshot_positions()`（`_pos_mtx` 内按表序拷贝 `(code, PosInfoPtr)`）。`on_tick` 任务线程 lambda 在 `_pos_mtx` 内查找并拷贝 shared_ptr，释放后再锁 `pInfo->_mtx`；`do_set_position` 改用 `get_or_create_position`，不再持有指向表内存的引用。`WtCtaEngine::handle_pos_change`、`WtSelEngine::handle_pos_change` 同样拷贝 owner，并在 `pInfo->_mtx` 内读取 `_volume`；`WtCtaEngine::on_schedule`（原约 398 行）遍历 `snapshot_positions()` 的快照，每项在其锁内读 `_volume`，循环体内的 `append_signal` 不再与表锁嵌套。
2. `save_datas`（问题 3）：资金部分在 `_fund_mtx` 内拷贝 `WTSFundStruct` 后序列化；持仓部分遍历快照，每项在 `pInfo->_mtx` 内读取（明细同锁）。已核对调用方：`on_tick`、`handle_pos_change`、`on_session_end`、`setVolScale`、`getFundInfo` 均在 `do_set_position` 返回、锁全部释放后才调用。
3. 资金（问题 4）：`update_fund_dynprofit` 先在 `_fund_mtx` 内做结算日与更新间隔判断，再对快照逐项持 `pInfo->_mtx` 汇总 `_dynprofit`，最后在 `_fund_mtx` 内**复核结算日**后更新；`do_set_position` 中每处 `_fees/_balance/_profit` 累加都在短 `_fund_mtx` 作用域内（平仓分支把 `calc_fee` 提到锁外，四个字段同一作用域更新，`_balance` 先加盈亏后减手续费的顺序不变）；`on_session_end` 在 `_fund_mtx` 内拷贝待结算值并完成重置，锁外用该副本写 `funds.csv`，`save_datas()` 在锁外。`load_datas`（init 单线程）未改。附带：`_port_fund == NULL` 时 `update_fund_dynprofit`/`on_session_end` 跳过而非解引用空指针。
4. 时钟（问题 5）：`_cur_date/_cur_time/_cur_raw_time/_cur_secs/_cur_tdate` 改为 `WtEngineClockField`（`WtEngine.h` 内的小包装：`std::atomic<uint32_t>`，relaxed 读写，隐式转换为 `uint32_t`，禁止拷贝），并为它特化 `fmt::formatter`。未采用裸 `std::atomic<uint32_t>` 的原因（静态分析，未实际试编）：派生引擎直接 `WTSLogger::info("{}", _cur_tdate)`，fmt 11 在未包含 `<fmt/std.h>` 时不能格式化 `std::atomic`，`rapidjson::AddMember` 模板也不接受原子类型；包装类在不改派生引擎表达式的情况下编译通过。各字段独立原子，读者仍可能组合相邻两次发布的值（与改前语义相同），但不再有数据竞争。`WtHelper::setTime` 的静态字段未改（见剩余）。
5. 用例 `CoverageTests_wtcore`（`n02_wtcore_engines/i03_new115_engine_pos_race_test.cpp`，独立的基础数据替身预建 rb1000–rb8999 合约，手续费按量开 2 / 平今 3）：
   - `I03New115EngineRace.TaskWorkerDynprofitWhileEngineThreadInsertsNewContracts`：4 轮、每轮新引擎；引擎线程插入 8000 个新合约持仓（表存储多次搬迁）、每 10 个平掉一个、每 1000 次落盘，每次插入后两笔 tick 让任务线程查表算浮盈（默认 5 s 资金更新间隔，任务线程与插入全程重叠：插入约 0.7 s，排空晚 0–9 ms）；排空后逐个持仓的量/浮盈/明细、资金手续费/平仓盈亏/静态权益、最终浮盈与 `datas.json` 全部精确相等（全为整数倍，double 精确）。
   - `I03New115EngineRace.SettlementWhileWorkerUpdatesFundKeepsTheSettledState`：持仓与价格不变时一边持续投递资金更新一边结算；结算记录与重置后状态精确，`_max_dyn_bal/_min_dyn_bal` 必须保持 `DBL_MAX`（不得有更新落在重置之后）。
   - `I03New115EngineRace.ClockPublishedByTickerIsReadWithoutTornFields`：发布线程 20 万次 `set_date_time/set_trading_date`，读线程经 getter 与 `update_fund_dynprofit` 读取；普通构建是冒烟，竞争本身由 TSan 判定。
6. 红绿：红控 `red115` 以新头文件 + 改前加锁逻辑的 `WtEngine.cpp` 单对象重链（`scratchpad/slice2-red-src/new115/WtEngine.cpp`，仅为新时钟类型做了构造函数与 `AddMember` 两处编译适配；该红二进制的其余对象来自三个引擎文件改动之前的构建）。红 5 轮：主用例 **SIGSEGV 5/5**；结算用例 2 次失败（一次开仓阶段浮盈合计 19900≠20000——任务线程在搬迁中的表上查找丢失；一次 `_max_dyn_bal/_min_dyn_bal` 不为 `DBL_MAX`——更新落在结算重置之后）+1 次 SIGSEGV；时钟用例 5/5 通过（只在 TSan 下可判）。绿（最终构建）：上述 3 例 + `R66Fixture.R66HftTicker*` 2 例 + `R41EngineFixture.R41OnScheduleWithPoolCommitsLeftoverPositionToZero`（I03-NEW-100 的 SIGSEGV 用例）连续 5 轮 30/30。`logs/phase3/red115-run*-*.log`、`green115-run*-summary.json`、`green115-run1-*.xml`。
7. CTA/SEL `handle_pos_change` 与 `on_schedule` 快照：当前用例下只有引擎线程插入，普通构建无确定性红控，属静态加固 + 回归覆盖；条件单（`bFireAtOnce`）在 CTA 线程池上回调 `handle_pos_change` 时才有多线程插入。

### I03-NEW-039 剩余 3 处引擎缓存键

- `WtCtaEngine::on_bar`、`WtSelEngine::on_bar`、`WtHftEngine::on_bar` 的 `thread_local char key[64]` + `fmtutil::format_to` 改为 `const std::string key = fmt::format(...)`（与第二阶段 `WtEngine.cpp` 同法）。指针重载越界写在普通/ASan 构建下不可观测，无运行红控；由 wtcore 全量回归覆盖。

### 4 处测试侧 TSan 竞争（未改任何断言）

| 用例 | 处理 |
|---|---|
| `CoverageTests_wtcore::R66Fixture.R66HftTickerThreadedMinuteCloseReplays`、`R66HftTickerSectionEndAndStaleTick` | 删除 worker 运行中的第二次 `ticker.init()`。HFT `run()` 不覆盖 `_store`（CTA/SEL 的 `run()` 才会用 `data_mgr->reader()` 覆盖，且那些用例先让 worker 退出再 init），第二次 init 只把同一 reader/session 和墙钟时间重写一遍，却与 `worker_pass` 并发写 `_store/_s_info/_date/_time`。 |
| `CoverageTests_t_ParserUDP::ParserUDPShutdownTest.SelfReleaseAndDeleteRetainOwnerForExternalRetry` | 测试 sink 的 `_on_event` 读写经互斥量，回调在锁外调用（回调内会释放 parser）。 |
| `CoverageTests_porter::ExecUnitStatusApiV2Fixture.SynchronousRunFailsFastOnSynchronousSegmentationFault` | **竞争在产品代码**：`g_runner_signal_*` 三个标志定义在 `src/WtPorter/WtRtRunner.cpp`（不是测试）。信号处理函数运行在收到信号的线程上（同步故障即故障线程），同步 run 线程轮询这些标志；`volatile sig_atomic_t` 只在同一线程内安全。按新发现 **I03-NEW-122** 在产品中改为 `std::atomic<int>`（`static_assert` 无锁，无锁原子在信号处理函数中可用）。普通构建无红控（静态分析），以 TSan 判定。 |
| `CoverageTests_t_CTPLoaderYamlPositive::t6_ctploader_yaml_smoke.valid_file_applies_config_and_initializes_fake_without_network` | 假 API 的 `Init()` 在登录回调后先（有界 5 s）等待 SPI 查询 worker 把已入列的合约查询投递到替身，再报告前置断开；费率批次仍在进行中，终态（EXIT_FAILURE）与全部断言不变。**根因在产品**，见 I03-NEW-123。 |

绿：`CoverageTests_t_ParserUDP` 45/45、`CoverageTests_t_CTPLoaderYamlPositive` 1/1 且目标用例连续 5 次通过（`logs/phase3/green-*.json`）。测试侧修复的“红”只在 TSan 下可见，改前副本在 `before-phase3/`。

### 新发现（主代理已分配编号：S2-NEW-2→I03-NEW-122、S2-NEW-3→I03-NEW-123、S2-NEW-4→I03-NEW-124）

- **I03-NEW-122**（原 S2-NEW-2，已修；主代理确认保留产品修改）：见上表，`WtRtRunner.cpp` 信号标志跨线程。
- **I03-NEW-123**（原 S2-NEW-3，未修，`CTPLoader.cpp` 被 slice4 锁定；主代理转 slice4 修复）：`CTPLoader.cpp` 的 `run()` 先 `RegisterSpi(NULL)`、`Release()`、`pUserApi = NULL`，之后才 `WaitWorkersStopped()`；而 `CTraderSpi::RunQueryWorker` 在“锁内未停止”检查后提交的查询任务仍会调用 `pUserApi->ReqQryInstrument/...`。真实 SDK 下这是对已释放 API 的调用或空指针解引用。修法建议：先停止并有界等待 worker，确认停下后再注销/释放 API；超时则 API 与 SPI 一并保留到进程结束（卡 9a）。`CTPOptLoader` 无查询 worker 线程，不受影响。（静态分析，TSan 报告为佐证。）
- **I03-NEW-124**（原 S2-NEW-4；(a)(b) 已在第四阶段修复，(c) 只登记，见下文“第四阶段”）：当 `handle_pos_change` 在 CTA 线程池线程上被并发调用（条件单 `bFireAtOnce`）时，`append_signal` 写 `_sig_map`、`get_cur_price` 在缓存未命中时写 `_price_map`，二者都无锁，`on_tick` 也读写它们；另 `WtHelper::setTime` 的静态时钟字段为普通 `uint32_t`；`getFundInfo()` 把 `_port_fund` 裸指针借给风控，风控读字段不经 `_fund_mtx`（WtPortContext 接口约束）。均为静态分析，未运行验证。

### 第三阶段回归（绿构建，官方 `run_regression.py`，4 路，每例独立进程）

| 范围 | 结果 | 证据 |
|---|---|---|
| CoverageTests_wtcore 全部（含三引擎改动；`WT_COV_PLUGIN_BIN` 已设） | 1066 例：1065 过、1 既有跳过，无失败 | `logs/phase3/reg-wtcore-summary.json`、`reg-wtcore-results.jsonl.gz` |
| CoverageTests_porter / m_WtRunner（预置模块，`real_cta_factory_null_self…` 排除预置）/ exefact / execv2 / ctafact / riskmon / selfthft / m_WtLatencyHFT / t_ParserUDP / t_CTPLoaderYamlPositive 全部 | 974/974（269 / 108 / 418 / 55 / 29 / 6 / 22 / 21 / 45 / 1） | `logs/phase3/reg-deps-summary.json`、`reg-deps-results.jsonl.gz` |

说明：首次 wtcore 全量运行在三引擎改动落地前被本段中止并删除（二进制即将重建）；中止时用 `pkill -f "build_x64/Debug/bin/TestUnits/CoverageTests_wtcore --gtest"` 清理残留进程，该模式不含本段目录前缀，若 2026-11-05 14:47 前后其他代理恰有同名二进制用例在跑，可能被一并终止，需要时请重跑。

### 需主代理在 sanitizer 树运行（第三阶段）

| 条目 | 二进制 | gtest filter | 期望 |
|---|---|---|---|
| I03-NEW-115 | CoverageTests_wtcore（TSan，建议 ×3） | `I03New115EngineRace.*:R41EngineFixture.*:R66Fixture.*:R101CtaEngineHot.*:R170TickerRun.*:R181TickerRun.*:R192CtaTicker.*:R202CtaTicker.*:S5LiveFeeSchedule.*` | 不再有 `WtEngine::on_tick` 任务 lambda / `do_set_position` / `save_datas` / `update_fund_dynprofit` / `on_session_end` / `set_date_time` / `set_trading_date` / `get_min_time` 与 `WtHftRtTicker::init`×`worker_pass` 的竞争；boost 线程池 thread leak、`_price_map`/`_sig_map`（I03-NEW-124）若出现属剩余 |
| I03-NEW-115 | CoverageTests_wtcore（ASan+UBSan） | `I03New115EngineRace.*:R41EngineFixture.R41OnScheduleWithPoolCommitsLeftoverPositionToZero` | 0 报告；红控可用 `slice2-result-build/red/red115/` 同法在 ASan 树重链 |
| 测试侧 | CoverageTests_t_ParserUDP（TSan） | `ParserUDPShutdownTest.SelfReleaseAndDeleteRetainOwnerForExternalRetry` | 0 竞争 |
| I03-NEW-122 | CoverageTests_porter（TSan） | `ExecUnitStatusApiV2Fixture.SynchronousRunFailsFastOnSynchronousSegmentationFault` | 子进程内 0 竞争 |
| 测试侧 / I03-NEW-123 | CoverageTests_t_CTPLoaderYamlPositive（TSan） | `t6_ctploader_yaml_smoke.valid_file_applies_config_and_initializes_fake_without_network` | 0 竞争（产品根因 I03-NEW-123 仍在，换时序的用例仍可暴露） |

## 第四阶段（主代理追加）：I03-NEW-124 (a)(b)，(c) 只登记

- 锁：`lock.py slice2-result I03-NEW-124` 登记 `WtEngine.h/.cpp`、`WtCtaEngine.cpp`、`WtSelEngine.cpp`、`WtHftEngine.cpp`（已持有）、`WtCore/WtHelper.h`、`WtCore/WtExecMgr.h`（新登记）与新用例。`src/WtCore/WtHelper.cpp` 的锁在 main 名下（I03-NEW-112），**经主代理授权**直接修改，未在 locks.txt 登记；只改了 setTime 相关的 4 个静态成员定义，`getCWD` 未动（改前改后 `getCWD()` 函数体逐字节一致）。`WtRtRunner.cpp` 只把注释里的临时编号 S2-NEW-2 改为 I03-NEW-122。
- 改前 `before-phase4/`（即第三阶段 `after-phase3/` 的状态，开始修改前已核对哈希未变）；改后 `after-phase4/`；哈希（含红控源码、红/绿二进制）`sources-phase4.sha256`；日志、摘要在 `logs/phase4/`。

### (a) 线程池上的 handle_pos_change

1. 新增 `_sig_mtx`（SpinMutex）：保护 `_sig_map`，并把一次组合目标变更做成一步：读基数（待触发目标或持仓）、加增量、写信号或经 `do_set_position` 直接落地。`append_signal` 自己加锁后调用新增的 `append_signal_locked`；`WtCtaEngine::handle_pos_change`、`WtSelEngine::handle_pos_change` 在 `_sig_mtx` 内读持仓量（再嵌 `pInfo->_mtx`）、`pending_target_or`、`append_signal_locked`；`WtEngine::on_tick` 在 `_sig_mtx` 内查找、触发并删除待触发信号。只保护 map 内存不够：两个策略同时对同一合约 +1 时会读到同一基数而丢一次增量（红控已复现）。
2. 新增 `_price_mtx`（SpinMutex，叶子锁）：`get_cur_price` 在锁内查缓存；未命中时在锁外向 `_data_mgr` 取最新 tick，再在锁内 `emplace`（期间若别的线程已写入则保留已写入的值；单线程下与改前相同）。`WtEngine::on_tick` 以及 CTA/SEL/HFT 复权 tick 的写入改走新增的 `set_cur_price()`。
3. 加锁顺序扩展为 `_sig_mtx → _pos_mtx → PosInfo::_mtx → _fund_mtx → _price_mtx`，无反向嵌套，任何锁都不跨 `save_datas()`。`_sig_mtx` 持有期间会进入 `do_set_position`（与改前持 `PosInfo::_mtx` 写 trades.csv/closes.csv 的做法相同）；已核对 `do_set_position` 只调用 `get_or_create_position/get_cur_price/calc_fee/log_trade/log_close`，不回到信号路径。附带效果：产品里的两个 `do_set_position` 调用点都在 `_sig_mtx` 内，`_trade_logs/_close_logs` 的并发写也随之串行。
4. 同一路径上另外两处（不在协调消息列出的两项里，但都在线程池上的 `handle_pos_change` 路径上，一并修复）：
   - `save_datas()`：线程池线程在 `handle_pos_change` 末尾并发调用，每次都截断重写 datas.json，可交错成不完整文件。新增 `_save_mtx`（`std::mutex`：持锁期间做文件 I/O，所以不用自旋锁）串行化；它先于其余各锁获取，`save_datas()` 只在 `_sig_mtx` 释放后调用，两者不嵌套。
   - `WtExecuterMgr::get_route_by_strategy()`（`WtExecMgr.h`）：函数内静态集合在首次调用时才 `insert("ALL")`，线程池线程首次同时调用即是写竞争。改为由（线程安全的）静态初始化一次构造好。普通构建无红控，属静态分析，由 TSan 判定。
5. 未改：`_exec_mgr.handle_pos_change` 仍在锁外调用——它只向执行器传增量（`on_position_changed(diff)`），各次增量可交换，顺序不影响结果；执行器内部的线程安全不在本条范围。`_risk_volscale/_risk_date` 与 `WtFilterMgr` 过滤表在这条路径上只读（过滤表热加载线程未查）。

### (b) WtHelper 静态时钟

- `WtCore/WtHelper.h`：`_cur_date/_cur_time/_cur_secs/_cur_tdate` 改为 `std::atomic<uint32_t>`，`setTime/setTDate/getDate/getTime/getSecs/getTradingDate` 用 relaxed 读写。字段是 private，所有访问都经这些内联函数，调用方（`WtEngine::set_date_time/set_trading_date`、`WtExecRunner`、`TraderAdapter` 下单时间判断、测试）无需改动。`WtCore/WtHelper.cpp`：4 个定义改为原子，并 `static_assert(std::atomic<uint32_t>::is_always_lock_free)`。语义与 `WtEngineClockField` 相同：各字段独立发布，读者仍可能组合相邻两次发布的值。
- 静态数据成员的符号名不含类型（`__ZN8WtHelper9_cur_dateE`），头文件与定义类型不一致会静默链接成功。已核对：只有 `WtCore/WtHelper.cpp` 与 `WtUftCore/WtHelper.cpp` 定义这些成员，后者用自己的 `WtUftCore/WtHelper.h`；UFT 用例目录与 WtUftCore/WtUftRunner/WtLatencyUFT 产品源码都不包含 `WtCore/WtHelper.h`。

### (c) 风控借用资金指针（只登记，不改）

- `WtEngine::getFundInfo()` 把 `_port_fund`（`WTSPortFundInfo*`）借给风控模块（WtPortContext 接口），风控线程读 `fundInfo()` 字段不经 `_fund_mtx`；引擎侧的写入都已在 `_fund_mtx` 内。接口返回的是裸指针，要改成加锁副本就得改 API 合同（风控插件 ABI），按主代理指示只登记。

### 用例（CoverageTests_wtcore，`n02_wtcore_engines/i03_new124_engine_sig_price_race_test.cpp`）

| 用例 | 内容 | 红控 red124（5 轮） |
|---|---|---|
| `I03New124EngineRace.StrategyThreadsReadPricesWhileEngineThreadCachesNewCodes` | 4 轮、每轮新引擎：引擎线程缓存 8000 个新代码的价格（量为 0 的 tick，只走价格缓存与信号检查），3 个策略线程同时随机读已发布代码的价格，每次读取都必须等于该代码价格 | **SIGSEGV 5/5** |
| `I03New124EngineRace.PoolThreadsIncrementSharedTargetsWithoutLosingUpdates` | 已缓存价格；4 个线程（4 个策略）各 256 次对同 16 个合约 `handle_pos_change(+1)`；每个合约恰为 64 手、64 条明细，手续费 2048、成交 1024 行，datas.json 完整且与内存一致 | **失败 4/5**（每次都有一个合约 63≠64，手续费 2046≠2048），1 次通过 |
| `I03New124EngineRace.PoolThreadsQueueSignalsForNewCodesThenQuotesTriggerThem` | 无价格（走待触发信号）；4 个线程对互不相交的 800 个新代码各 +1 两次（第二次累加到待触发目标上，K26=B1），信号表持续增长；之后逐个报价触发，每个合约恰为 2 手、1 条明细 | **失败 3/5**（2 次待触发目标 1≠2，即丢增量；1 次 SIGABRT 堆损坏），2 次通过 |
| `I03New124EngineRace.HelperClockPublishedByTickerIsReadByTraderThreads` | 发布线程 20 万次 `set_date_time/set_trading_date`（经 `WtHelper::setTime/setTDate`），读线程读 `WtHelper::get*`；普通构建只是冒烟，竞争由 TSan 判定 | 未跑（只能 TSan 判） |

- 红控：用现头文件，把去掉第四阶段新增锁的 `WtEngine.cpp`、`WtCtaEngine.cpp` 两个对象重链（`scratchpad/slice2-red-src/new124/`；与绿源码只差加锁语句，以及 `get_cur_price` 末尾恢复为改前的覆盖写）。`WtExecMgr.h` 与 `WtHelper` 的改动在头文件里，不在红控中（静态分析 + TSan）。
- 绿：上述 4 例 + `I03New115EngineRace.*` 3 例 + `R41EngineFixture.R41OnScheduleWithPoolCommitsLeftoverPositionToZero`，连续 5 轮 40/40。日志：`logs/phase4/red124-run*-*.log`、`green124-run*-summary.json`。

### 第四阶段回归（绿构建，官方 `run_regression.py`，4 路，每例独立进程）

| 范围 | 结果 | 证据 |
|---|---|---|
| CoverageTests_wtcore 全部（`WT_COV_PLUGIN_BIN` 已设） | 1075 例：1074 过、1 例既有跳过（`UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`），无失败 | `logs/phase4/reg-wtcore-summary.json`、`reg-wtcore-results.jsonl.gz` |
| CoverageTests_porter / m_WtRunner（预置模块，同第三阶段排除）/ exefact / execv2 / ctafact / riskmon / selfthft / m_WtLatencyHFT / t_ParserUDP / t_CTPLoaderYamlPositive 全部 | 979/979（269 / 110 / 420 / 55 / 29 / 7 / 22 / 21 / 45 / 1） | `logs/phase4/reg-deps-summary.json`、`reg-deps-results.jsonl.gz` |

- 构建：`-Werror` 零告警（`logs/phase4/build-wtcore.log`、`build-dependents.log`、`build-porter-comment.log`）。主目标 `CoverageTests` 编译失败：`l04_datastorage/s4_r132_live_reader_budget_test.cpp` 调用了已变为 private 的 `WTSVariant::create`，该文件由 slice4-result 锁定（BUG-R132-001），与本段改动无关；主目标本阶段未运行。

### 需主代理在 sanitizer 树运行（第四阶段）

| 条目 | 二进制 | gtest filter | 期望 |
|---|---|---|---|
| I03-NEW-124 (a)(b) | CoverageTests_wtcore（TSan，建议 ×3） | `I03New124EngineRace.*:I03New115EngineRace.*:R41EngineFixture.*:P01CtaQueryTest.*` | 不再有 `_sig_map`（`append_signal`/`pending_target_or`/`on_tick`）、`_price_map`（`get_cur_price`/`on_tick`/`set_cur_price`）、`save_datas`、`get_route_by_strategy` 的 `ALL_EXECUTERS`、`WtHelper::_cur_*` 的竞争 |
| I03-NEW-124 (a) | CoverageTests_wtcore（ASan+UBSan） | `I03New124EngineRace.StrategyThreads*:I03New124EngineRace.Pool*` | 0 报告；红控可用 `slice2-red-src/new124/` 两个源文件在 ASan 树同法重链 |

## 第五阶段（主代理追加）：I03-NEW-128、I03-NEW-129（slice3 禁用用例处置中复现，不在 213 项内）

- 锁：`r13_porter_sel_hft_export_test.cpp` 由 slice3 转入；`lock.py slice2-result I03-NEW-128/I03-NEW-129` 登记 `r6_porter_lifecycle_test.cpp`、`WtPorter/WtPorter.cpp`（`WtRtRunner.cpp/.h` 本段已持有）。
- 改前 `before-phase5/`（`WtRtRunner.cpp` 的改前哈希与第四阶段改后一致，期间无他人改动）；改后 `after-phase5/`；哈希 `sources-phase5.sha256`；日志、摘要、XML 在 `logs/phase5/`。
- 红控做法：先只改用例、产品保持改前内容，在同一构建目录增量构建 `CoverageTests_porter` 并复制为 `scratchpad/slice2-result-build/red/red128/CoverageTests_porter`；再改产品、重建为绿。红绿二进制只差两个产品 TU。

### I03-NEW-128（BUG-Q06-002 类，与 R6-006 相关）：引擎未就绪时解引用空引擎

- `WtPorter.cpp`：新增 `ready_engine(entry)`，引擎为空时 `WTSLogger::error` 记一条“未就绪”诊断并返回 NULL；cta/sel/hft 三族全部 14 个引擎直取入口（`cta_get_price/day_price/tdate/date/time`、`sel_get_price/day_price/tdate/date/time`、`hft_get_price/date/time/secs`）改为引擎为空时返回 0（与各入口“无数据返回 0”的既有约定一致）。改后 `WtPorter.cpp` 中不再有 `getRunner().getEngine()->` 直取。
- `WtRtRunner::get_raw_stdcode`：`_engine` 为空时记诊断并返回空串（线程局部缓冲清空后返回，不返回 NULL）；`stdCode` 为 NULL 时诊断中按空串输出。已核对 `WtRtRunner.cpp` 其余 `_engine->` 解引用（扩展 parser、`initParsers`、`run`、推送回调）都已有判空或只在 config 成功后可达。
- 诊断：`WTSLogger` 在 `init_porter` 之前默认级别为 `LL_NONE`，不输出任何日志（既有行为，与 `feedRawBars` 的告警相同）；用例断言的是失败返回值，未断言日志文本。
- 用例（`CoverageTests_porter`，原 DISABLED_ 用例改写为启用的合同用例）：
  - `R13PorterEngineSeam.engine_direct_getters_fail_closed_without_engine`（原 `DISABLED_engine_direct_getters_deref_null_engine`）：在 threadsafe 死亡测试子进程里（同二进制有用例会在进程内 config 进程级 runner，子进程重新执行二进制可保证未 config），先断言前置条件“引擎为空”，再逐个断言 14 个入口返回 0、C 入口 `get_raw_stdcode` 返回空串、调用后仍未 config；子进程以 `exit()` 退出（LLVM profile 落盘），期望退出码 0。
  - `R6PorterRawCode.get_raw_stdcode_without_engine_fails_closed`（原 `DISABLED_get_raw_stdcode_without_engine_crashes`）：本地 runner 未 config，`get_raw_stdcode("CFFEX.IF.2108")` 与 `get_raw_stdcode(NULL)` 都返回非空指针的空串。原用例断言的 "IF2108" 是有引擎时的成功路径，不属于本合同，已删去并在注释中说明。
- 红：R13 子进程被信号 11 终止（3/3），R6 RawCode SIGSEGV（3/3）。绿：5 轮全过。

### I03-NEW-129（BUG-Q06-004/R46-001）：feedAdjFactors 调用空函数指针

- `WtRtRunner::feedAdjFactors`：与 `feedRawBars` 同样的两道检查——未注册扩展复权因子加载器、或尚未经 `loadAdjFactors/loadAllAdjFactors` 装入 feeder 时，`WTSLogger::error` 告警并返回，不调用 `_feeder_fcts`。不加 `_feed_mtx`：`loadAdjFactors` 持该锁同步调用宿主加载器，加载器在锁内回调 `feed_adj_factors`（`std::mutex` 不可重入）。
- 用例 `R6PorterFeed.feed_adj_factors_without_feeder_fails_closed`（原 `DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage`）：未注册加载器时调用返回；注册加载器但未请求加载时调用返回，探针 feeder 调用次数为 0；正向对照：`loadAdjFactors(&owner, code, probe)` 后，加载器经 `feedAdjFactors` 回灌的 2 条因子原样转交给 feeder（obj、代码、条数、日期、因子逐项精确相等）。
- 红：SIGSEGV（3/3，第一次调用即调用空函数指针）。绿：5 轮全过。

### 第五阶段回归

| 范围 | 结果 | 证据 |
|---|---|---|
| CoverageTests_porter 全部，逐例独立进程（官方 `run_regression.py`，4 路，`WT_COV_PLUGIN_BIN` 已设） | 276/276 通过；含上述 3 个新启用用例；DISABLED 数 6（3 个改为启用） | `logs/phase5/reg-porter-summary.json`、`reg-porter-results.jsonl.gz` |

- 构建：`-Werror` 零告警（`logs/phase5/build-porter-red.log`、`build-porter-green.log`）。编译 `WtPorter.cpp/WtRtRunner.cpp` 的只有 `CoverageTests_porter` 与 `libWtPorter.dylib`，均已重建。

### 需主代理在 sanitizer 树运行（第五阶段）

| 条目 | 二进制 | gtest filter | 期望 |
|---|---|---|---|
| I03-NEW-128 / I03-NEW-129 | CoverageTests_porter（ASan+UBSan） | `R13PorterEngineSeam.*:R6PorterRawCode.*:R6PorterFeed.*` | 0 报告（改前 UBSan 报 WtPorter.cpp:1022 空指针成员调用）；R13 用例的检查在死亡测试子进程中，子进程内的 sanitizer 报告会以非 0 退出码体现 |

## 未完成与外部剩余

- R132-001：`WtDataReader.cpp`、`WtRdmDtReader.cpp` 被 slice4-result 锁定，读端预算入口未接入（补丁见 `pending-patch-R132-locked-readers.md`）。
- I03-NEW-039：三处引擎缓存键已在第三阶段补齐；`thread_local char[64]` + 指针重载的越界写在普通/ASan 构建下都不可观测，三处引擎键为静态修改；端到端长代码用例依赖 I03-NEW-083（slice4）。
- R18-011：慢消费者溢出处置不确定，建议按卡15 记用户指示忽略；跨进程多生产者为 K21 B 后续；ParserShm acquire 修复无确定性红控（静态分析）。
- 外部：Windows/MSVC（LockFileEx、栅栏行为）、Linux、真实行情/厂商、断电均未验。
- 全部为本机定向运行，不是整体回归；sanitizer 由主代理在冻结树执行。
- 第三阶段：I03-NEW-123（CTPLoader 释放顺序，slice4 锁，主代理转 slice4）未修；I03-NEW-124 的 (a)(b) 已在第四阶段修复，(c) 风控借用资金指针只登记；I03-NEW-115 的 TSan/ASan 判定与测试侧 4 处的 TSan 复验由主代理执行。`WtEngine.h` 中 `template <> struct fmt::formatter<wtp::WtEngineClockField>` 的限定名特化与 spdlog 内置 fmt 的组合只在本机（Homebrew fmt 11.2、Apple clang）编译过，MSVC/Windows 未验。
- 第四阶段：I03-NEW-124 (c)（风控经 `getFundInfo()` 借用 `_port_fund` 裸指针，读字段不经 `_fund_mtx`）属 API 合同限制，只登记未改；执行器内部线程安全、`WtFilterMgr` 热加载与线程池读取的关系未查；(a)(b) 的 TSan/ASan 判定由主代理执行。
- 第五阶段：I03-NEW-128/129 只修了未就绪/未注册的空指针路径；`getEngine()` 与 `_engine`、`_feeder_fcts` 在 config/load 线程与调用线程之间的并发可见性未处理（宿主约定先 config 再调用）；sanitizer 由主代理执行。
