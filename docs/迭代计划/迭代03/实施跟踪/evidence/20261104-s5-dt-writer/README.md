# 迭代03 s5 子任务：dt-writer（数据写端与 DataManager 生命周期，卡9 落地）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动）。开始时间 2026-11-04 13:11 +0800。本目录只记录本包定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实第三方 writer 插件、跨进程或最终整合树。未修改任何跟踪文件。

负责条目：BUG-R23-001、BUG-P10-001、BUG-P10-006、BUG-P10-007、BUG-P10-008、BUG-I02-NEW-038、BUG-R18-002、BUG-R18-011、BUG-L05-002、BUG-I02-NEW-056（Z）、BUG-I02-NEW-104（Z）；主线程追加 I03-NEW-007（写端复核）。

## 快照、构建与意外

- 13:1x 以 `rsync -a --no-links` 复制工作区 `src/`、`dist/common/` 到 `/tmp/wt-i03-s5-dt-writer-before`（`find -type l` 为 0），按 preamble 命令配置覆盖率构建（`WT_WARNINGS_AS_ERRORS` 默认 ON，`cov_boost_include` 为实体目录），13:39 `CoverageTests_dtcore`、`CoverageTests` 均 exit 0；同快照普通 Debug 构建 WTSTools/WTSUtils/WtDtCore/WtDataStorage exit 0。改前（红控/基线）覆盖率运行全部在该构建上完成，结果在 `runs/before_*`。
- **约 18:2x 该目录被外部整体删除**（磁盘占用由 96% 降至 82%，非本子任务操作），其全树哈希清单随之丢失；改前运行日志/XML 已先行复制入本目录。之后：
  - `/tmp/wt-i03-s5-dt-writer-after`：由原快照 rsync 复制，再用 `hashes/overlay.sh` 覆盖本包文件（`hashes/overlay-sha256.txt`），全树哈希 `hashes/after-snapshot-sha256.txt`。改后覆盖率构建（默认 `-Werror`）与普通 Debug WTSTools/WTSUtils 在此；与改前快照只差本包文件。
  - `/tmp/wt-i03-s5-dt-writer-base2`：复制 after 快照后用 `before/` 中改前原件还原本包文件，作夹具红控源码树（关键文件哈希与 `hashes/before-copies-sha256.txt` 一致）。
- 改前原件在 `before/`（相对路径保留，`hashes/before-copies-sha256.txt`），改后哈希 `hashes/after-sha256.txt`，逐文件 diff 在 `patches/`。
- 每例独立 cwd（`/tmp/wt-i03-s5-dt-writer-run/<label>`），`perl alarm` 硬超时，`LLVM_PROFILE_FILE` 指向该目录；脚本 `/tmp/wt-i03-s5-dt-writer-run/runcase.sh` 同 z15 版式。

## 产品修改

| 文件 | 改前 sha256 | 改后 sha256 | 内容 |
|---|---|---|---|
| `src/Includes/IDataWriter.h` | `943dca27…` | `3af18810…` | 追加版本化 C ABI「writer 生命周期 V1」（`getDataWriterLifecycleApiV1`：`request_stop`/`can_wait`/`wait_drained`，DRAINED 才授权 delete/卸载；未导出者为旧 ABI，宿主 release 后隔离）。类、vtable、成员布局、create/delete 导出不变。 |
| `src/WtDataStorage/WtDataWriter.h/.cpp` | `0c8a17ab…`/`228fbd8d…` | `98a5a77b…`/`05a8d818…` | 导出 V1 并登记本模块创建的 writer；`requestStop` 非阻塞可在回调/工作线程调用；`waitDrained` 有界等待已接纳操作、停队列、确认三类线程退出（started/exited 计数）后 join；`release()` 改为 requestStop + 无界 waitDrained（旧语义）。P10-008：委托队列/明细/成交在块锁内追加并入每块有序事件队列，解锁后由单一 drainer 广播；同块重入只追加入队后返回，在当前回调返回后的安全点按序投递；广播异常逐事件捕获记录。另将四个 `static` 计数表改为加锁共享并防 `groupsize=0` 除零。 |
| `src/WtDtCore/DataManager.h/.cpp` | `59e10208…`/`0b7504b5…` | `f9261193…`/`14c085a8…` | 堆上 `WriterHost`（卡9b）持 writer/remover/模块/借用对象，并作为交给 writer 的 sink；出入站帧按线程计数。回调帧内 release 只排队（V1 发 request_stop），由最后一个出站调用在安全点或下一次外部 release/析构投递；V1 等到 DRAINED 后 release→delete→卸载；旧 ABI writer 在安全上下文 release 后隔离到进程退出（9a）；`release_timeout_ms`（默认 0=不限）超时即隔离；重复 init 拒绝（9c）；广播逐 caster 捕获异常；sink 在隔离/析构后不再触达 caster/状态机/基础数据。加载失败时依赖仍只在模块与工厂就绪后登记（NEW-056 行为不变）。 |
| `src/WtDtHelper/WtDtHelper.h/.cpp` | `bcf66448…`/`88ffb7b6…` | `f785d224…`/`e0ea16a4…` | NEW-104：`resample_bars` 三条失败返回释放 `WTSSessionInfo`/`WTSKlineSlice`，空结果不再 `at(0)`。L05-002：`strFilter` 按原公开头注释“合约过滤器，支持通配符”实现为单一通配模式（`*`/`?`，整串匹配 .dsb 文件名主干，区分大小写）；无匹配输入新增状态 `WT_CSV_DUMP_NO_INPUT=6`（追加枚举），不再报告成功或“全部导出完成”。 |

测试/夹具：`src/TestUnits/fixtures/r18_writer_fixture.cpp`（`a1404d32…`→`dc7af4ee…`，假 writer 声明 V1，保持原 release/delete/destroy 断言不变；旧 ABI 变体由新夹具覆盖）；新增 `coverage/l05_dt_servo_porter/l05_dt_helper_v2_filter_test.cpp`、`coverage/q05_dtsrv/q05_resample_leak_paths_test.cpp`、`fixtures/i03_dt_writer_lifecycle_20261104s5/`（README、run.sh、run_cases.sh、假 writer、生命周期与 P10-008 用例）。未修改 coverage CMakeLists.txt。

（以下运行结果分节补全。）

## 定向运行结果

### P10-001（无链接重编重验）

- 改前快照覆盖率构建（实体快照、无符号链接、实体 `cov_boost_include`）原定向 `P10ProcBlockTest.*`：`runs/before_p10_procblock` **15/15 exit 0**。这是对迭代02 Boost bridge 时代证据的无链接重编重跑；本包未改 `proc_block_data`。
- 改后 after 快照覆盖率构建：`runs/after_p10_procblock` **15/15 exit 0**。
- 仍未插桩 GoogleTest/WTSTools/WTSUtils/Boost；sanitizer 版未在本轮重做。

### NEW-038 / R18-002 / R23-001 / 9c（夹具 `src/TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/`）

| 运行 | 结果 | 目录 |
|---|---|---|
| 改后 normal（after 快照源码，r1） | 15/15 exit 0 | `runs/fx-r1-after-normal` |
| **改前红控** normal（base2 源码，`-DS5_BASELINE_HOST`，r1） | 3/15 exit 0：两例 SIGSEGV(139)、四例 SIGABRT(134：`libc++abi: terminating`，含 `thread::join failed: Resource deadlock avoided` 自连接)、四例 P10-008 自旋死锁被 alarm 杀(142)、三例断言失败(1)；通过的 3 例为回归护栏（重复 init、并发生产者、析构恰好一次） | `runs/fx-r1-base-normal` |
| 改后 ASan+UBSan（LLVM 22，r2） | 15/15 exit 0，0 份 sanitizer 报告 | `runs/fx-r2-after-asan` |
| 改后 TSan（LLVM 22，r2） | 13/15：无数据竞争报告；1 例为隔离 writer 的检查线程未 join 的 thread leak（隔离语义：保留至进程退出，预期现象）；1 例为**测试**竞态（假 writer 的回调线程在 init 返回前触发 release，宿主按合同使 init 失败），已改为 init 返回后才武装 | `runs/fx-r2-after-tsan` |
| **最终 r3**（修正测试武装时序后全部重建，run.sh 增 `-I/usr/local/include`） | 改后 normal 15/15；改前红控 3/15（139×2、134×3、142×4、断言失败×3）；改后 ASan+UBSan 15/15、0 报告；改后 TSan 14/15、无数据竞争——唯一非 0 为 `RealWriterReleaseIsBoundedWhenCallbackBlocks` 的隔离线程 thread leak（用例本身通过），以 `report_thread_leaks=0` 单独重跑 1/1、0 报告 | `runs/fx-r3-*`、`runs/fx-r3-after-tsan-noleak` |

覆盖内容：回调内 `DataManager::release()`（旧 ABI 与 V1 假 writer、真实同步 WtDataWriter）只排队，活动写帧返回后才 release/delete/卸载（`write-exit` 先于 `release/delete/destroy`）；writer 自有线程回调内 release 不自连接，外部 release 时 V1 等到 DRAINED（`worker-exit`→`drained`→`delete`）；旧 ABI writer 外部 release 后隔离、模块仍驻留（`RTLD_NOLOAD` 可见）、计数 +1；重复 init 拒绝且原实例可写；回调阻塞时 `release_timeout_ms=300` 下 release <4 s 返回并隔离、被阻塞调用随后安全返回；4 个生产者并发写入期间 release，已接受写入数 == release 返回时的广播数，release 后写入被拒；析构恰好释放一次。

### P10-008（同一夹具 `P10Reentry.*`，真实 writer 经 createWriter）

三流同块同步重入各自返回、广播顺序 [首条, 重入条]、块文件 `_size==2`；回调期间另一线程追加同块 3 s 内返回且按存储序投递（证明回调不持块锁）；接收方抛异常时写入仍返回 true、错误日志、后续事件送达。改前：前四例 alarm 杀(142)，异常例断言失败。

### P10-007 / R23-001 回归

- 改后 after 构建 `P10RtBlockTest.*:P10TaskQueueTest.*:P10DayDsbTest.*`：41/41（`runs/after_p10_rt_task`；改前同组 41/41 `runs/before_p10_rt_task`）。
- P10-007 首插夹具（`src/TestUnits/fixtures/p10_first_insert/run.sh`，复制当前工作区源码、仅在副本插入屏障）对本包改后 writer：普通 6/6、TSan 6/6（`runs/p10_first_insert_current`）。六类过期关闭由 `P10RtBlockTest` 复合用例覆盖（同上 41/41）。
- R23 写端回归 17 组 `R23WriterTest/R100/R112/R122/R162/R172/R193/R212/WriterResizeAfterExtend/R52/R70/R74/R94/L03WriterSessionDate/R195/R22WriterStart/WriterTickRangeE2E`：82 例 0 失败、2 例既有 DISABLED（`runs/after_r23_writer_suites`）；改前快照被删除后未能补跑同组基线。
- R23 300 秒等待夹具（`src/TestUnits/fixtures/r23_retry_wait/build.sh`，当前 writer）：baseline 关闭红控 exit 5，normal/ASan/TSan 的关闭唤醒与正常路径各 0（`runs/r23_retry_wait_current`）。

### DataManager 既有用例（dtcore 目标）

| 组 | 改前 | 改后 |
|---|---|---|
| `R18WriterPluginTest.*` | 5/5 | 5/5（假 writer 改为声明 V1，原 release/delete/destroy 断言不变） |
| `R102DataManagerWriterTest.*`（AD writer=旧 ABI，现隔离） | 3/3 | 3/3 |
| `R68DataManagerFailureTest.*` | 3/4（`InitFailsWhenCustomModuleNameIsDirectory`，NEW-056） | 同 |
| `Q05DataManagerTest.*` | SEGV(139)：`InitFailsOnMissingModuleButKeepsSinkTargets` 断言失败后对空 `_bd_mgr` 解引用 | 3/4，同一 NEW-056 用例断言失败但不再崩溃（宿主对空基础数据返回空/0） |
| `R44DataManagerEdgesTest.*` | 1/7（6 例在夹具 SetUp 的 `StateMonitor::initialize` 失败，非本包） | 同 |
| `Q05ResampleTest.*` | 8/9（`DayResampleOfTenDailyBarsPassesThrough`，NEW-104 times=1 合同） | 同 |
| `Q05ResampleLeakPathsTest.*`（新增） | — | 4/4 |

### NEW-104（z15 发现 B，`leaks --atExit`）

- 改前：`DayRangeAfterLastBarFindsNothing` 3 leaks/224 B（WTSSessionInfo），`DayResampleOfTenDailyBarsPassesThrough` 10 leaks/832 B（含 WTSKlineSlice），成功正控 0（`runs/before_leaks_*`，复现 z15）。
- 改后：上述两例与正控、以及新增三条失败路径（仅头部的空块、尾后区间、times=1）和成功正控均 **0 leaks**（`runs/after_leaks_*`）。空块路径为新增用例，改前未单独运行（静态分析：该路径原先同样未释放 session）。

### L05-002

改后 `L05HelperBarTest.*:L05HelperTickTest.*:L05HelperV2FilterTest.*` **57/57**（改前 51/51 + 新增 6）：通配过滤（`rb*`、`rb22?0`、精确、整串与大小写、空过滤器全量）、空目录与仅非 .dsb 目录返回 `NO_INPUT` 且无“全部导出完成”、未知版本与未知文件标识各自独立拒绝（K 线与 tick 分别）并有合法版本正控（`runs/after_l05`，改前 `runs/before_l05`）。

### I03-NEW-007（AD writer 打包缓存对齐，写端复核）

当前 `WtDataWriterAD.cpp`（本轮修改前 `5b829a78…`，较 11-02 证据 `8d48d148…` 已有卡3/卡13 改动）静态复核：tick/m1/m5/day 缓存记录全部经对齐副本 + 字节复制访问，`static_assert` 固定布局；对外 dumper 接收栈上对齐副本，`getCurTick` 返回拷贝，无打包指针外泄。以 Homebrew LLVM 22 ASan+UBSan 隔离重编（after 快照源码）跑原证据同一过滤器 21 例 + 本包新增 3 例：**24/24，0 报告**（`runs/ad_green`）。另：Apple clang 版 ASan 二进制在 main 前挂起（300 s 无输出，exit 142，已弃用，与 z15 记录一致）；`R4WriterADTest` 全组在 UBSan 下于 `ReloadExistingCacheRebuildsIndex` 因测试夹具 `make_tick_item` 在栈上构造打包 `TickCacheItem` 报未对齐（测试代码问题，非产品），未纳入本过滤器。

### I03-NEW-038 与 L04-004 写端子项（AD writer）

- 修改：`pipe_writer_log` 改为 `fmt::format_to_n` 有界格式化 + 截断标记 + 异常兜底（与 AD 读端一致）；`writeTick` 身份不符合 LMDB 键时在任何缓存/库/队列副作用前拒绝并输出有界诊断；异步已接收但处理时复核（身份/交易日）失败的 tick 计入新增 `tickRejectedAfterAcceptance()` 并记错误日志（同步模式该失败使返回 false）。
- 红控（base2 改前 writer，`-DS5_AD_BASELINE`，LLVM 22 ASan+UBSan）：三例全部失败（`runs/ad_red_*`）——长路径诊断占满 512 字节无终止符（`512 vs 512`，越界写）、无准入拒绝日志、异步复核丢弃无日志。改后三例通过（含于 `runs/ad_green` 24/24）。ASan 未报告 thread_local 缓冲越界（TLS 无红区），越界由消息长度判定。

## 合同落地说明（卡9 → DataManager / writer）

- 9a=A+C：版本化“writer 生命周期 V1”作为排空能力证明；只有 `wait_drained` 返回 DRAINED 才 release→deleteWriter→卸载模块。未导出 V1 的旧 ABI writer 仍可加载与写入，但 release 后连同模块与 sink 隔离到进程退出（R102 的 AD writer 现属此类）；导出但表无效者在 createWriter 前拒绝。
- 9b=A：宿主对象 `WriterHost` 在堆上，出/入站帧引用计数，隔离时由进程级登记表持有；最后一个出站调用在安全点投递排队的 release；writer 线程退出由 started/exited 计数确认后才 join。
- 9c=A：DataManager 与 WtDataWriter 重复 init 均拒绝且原实例保持可用；隔离后不允许再 init。
- 9d=A：回调内（任一线程）的 release 只排队并对 V1 发非阻塞 `request_stop`，返回后在安全点投递；P10-008 同块同步重入只追加入队后返回，回调返回后按存储序投递。
- R23-001：停止期间写入——release 先关闭准入，已接受者全部处理（队列 drain，不 cancel）；三类线程统一以 started/exited 谓词等待后 join；300 秒清理重试等待对停止可中断（既有，复验通过）；release 时长默认不设上限（保持已接受数据不丢），配置 `release_timeout_ms` 后有界并隔离。

## 决策卡草稿（需用户裁定）

### 草稿 A：`release_timeout_ms` 默认值（关联 R23-001、NEW-038）

- 要决定：DataManager 默认是否给 writer 排空设置上限。
- 选项：A 默认 0=不限（现实现，保证已接受数据与收盘作业落盘，阻塞 sink 可使关闭无界）；B 默认有限值（如 60 s，超时隔离，进程退出时未完成的收盘作业可能丢失）；C 由 runner 在配置缺省时显式填写。
- 建议：A（可配置），并由 runner/运维按部署设定；超时即隔离而非强删。

### 草稿 B：外部 dumper 所有权与注册（BUG-P10-006，同时覆盖 AD writer）

- 现状证据（静态）：`IDataWriter::add_ext_dumper` 为非虚 inline，直接写 `_dumpers`，与收盘线程/AD 异步任务对 `_dumpers` 的遍历无同步；回调内注册可致 rehash 迭代器失效；不能给基类加成员（破坏旧 ABI）。`WtDtRunner` 中 `_dumpers` 声明在 `_data_mgr` 之后，析构时 ExpDumper 先于 DataManager 释放 writer（排空期间可能回调已销毁 dumper）；同 ID `createExtDumper` 直接替换 shared_ptr 即销毁旧对象。WtDataWriter 收盘线程持块租约调用 dumper，dumper 内同步写同一合约会在块自旋锁上自锁。
- 选项：A 借用：仅允许 init 前或首个收盘任务前注册，宿主保证存活至 writer DRAINED；B 版本化注册：在 V1 表追加 `add_dumper/remove_dumper`（writer 内加锁、快照并持引用），宿主持共享所有权至 DRAINED/隔离，旧 ABI writer 退回 A；C 维持现状、文档化后按卡15 忽略。
- 子项：重复 ID——建议拒绝并保留原 dumper（与 9c 一致）；关闭失败——dumper 保留至 writer DRAINED 或随隔离常驻；回调内注册——排队到安全点。
- 建议：B + 重复 ID 拒绝；并请 WtDtPorter 所属包在 `~WtDtRunner` 中先 `_data_mgr.release()`（或调整成员声明顺序）。合同未定前本包未改 dumper 路径。

### 草稿 C：ShmCaster 队列生产者合同（BUG-R18-011）

- 现状证据（静态）：`ShmCaster::broadcast` 对 volatile `_writable++` 非原子读改写，`_readable = wIdx` 无 release 语义；同步 writer 下多个 parser 线程可并发调用同一 caster，进程内即可能撕裂/丢槽；多个进程可映射同一路径；环形覆盖对慢消费者无检测。卡13b（多 writer 发布协议）只覆盖 RT 存储块，**不覆盖** ShmCaster/ParserShm 队列。
- 选项：A 单生产者：路径独占锁文件 + 进程内串行化（DataManager 对每个 caster 串行调用，或 ShmCaster 内锁）；B MPSC：原子 `fetch_add` 分槽 + 每槽序号 acquire/release 发布，消费者按序号识别覆盖；C 按卡15 忽略。
- 建议：至少 A 的进程内串行化（低成本），跨进程按 13b 精神取 B；实现属 ShmCaster/ParserShm 所属包。

## Z 类处置建议（卡15）

- BUG-I02-NEW-056：改前改后行为一致（依赖只在模块与工厂就绪后登记；失败时清空），生产调用方（QuoteFactory、WtDtRunner）忽略 init 返回值且失败后不存在 writer，sink 方法不会被产品调用，失败态取值无合同依据；本轮仅消除 Q05 用例中的空指针崩溃，不改断言、不改登记时序。建议记“用户指示忽略”，保持阻塞，解除条件：取得 init 失败后依赖保留/清空的书面合同。
- BUG-I02-NEW-104：z15 发现 B（三条失败返回泄漏）已修复并有 leaks 红绿；`times=1`（d→d）是否透传：`WTSDataFactory::extractKlineData` 明示“一倍不需要转换”返回 NULL，`resample_bars` 据此返回 0 并记“K线重采样失败”，无透传合同依据。建议透传语义记“用户指示忽略”，保持阻塞，解除条件：取得 times=1 返回合同；泄漏子项已闭合。

## 状态建议（供主线程整合，未改跟踪文件）

| 条目 | 建议 | 依据 / 剩余 |
|---|---|---|
| BUG-I02-NEW-038 | 本地验证通过 | 真实动态插件（假 V1/旧 ABI、真实 WtDataWriter 同步/异步）回调内 release 的红控 SEGV/abort → 改后延迟到安全点，normal/ASan/TSan 通过；第三方旧 ABI fail closed（隔离）。未验证 dlclose 物理卸载、Windows、真实第三方 writer。 |
| BUG-R18-002 | 本地验证通过（宿主侧） | 加载/符号/工厂/init(false) 既有 5/5 保持；V1 排空后才 delete/卸载，旧 ABI 隔离，sink 由堆宿主承载且隔离/析构后不再触达所有者对象。真实 provider 排空为外部条件。 |
| BUG-R23-001 | 本地验证通过 | 停止期间并发写入、drain（不 cancel）、重复 init 拒绝、三类线程谓词等待 join、300 s 等待可中断、`release_timeout_ms` 有界并隔离；硬超时子进程 + 屏障 + TSan。默认不限时为决策草稿 A。 |
| BUG-P10-008 | 本地验证通过 | 三流按块有序、同步重入返回语义（9d 排队）、异常隔离、回调不持块锁；改前四例死锁 142。多接收方“恰好一次”仅保证每接收方至多一次且异常可观察。 |
| BUG-P10-007 | 本地验证通过 | 六类首插锁争用普通/TSan 各 6/6（对本包改后 writer 重跑）、P10Rt/TaskQueue/DayDsb 41/41；外部依赖未插桩、其他平台未验。 |
| BUG-P10-001 | 本地验证通过 | 无链接实体快照重编重跑原定向 15/15，改后 15/15；依赖未插桩。 |
| BUG-L05-002 | 待验证 → 建议本地验证通过（本项剩余子项） | strFilter 按原公开头注释实现、空目录 NO_INPUT、未知版本/标识独立负控，57/57；旧 void 接口无返回、批次非事务、真实磁盘/Windows 仍未验。若主线程认为 NO_INPUT/过滤语义需用户确认，则保持待验证。 |
| BUG-P10-006 | 阻塞 | 决策草稿 B；同时需 WtDtRunner 关闭顺序修正（非本包文件）。 |
| BUG-R18-011 | 阻塞 | 卡13b 不覆盖 ShmCaster；决策草稿 C；实现非本包文件。 |
| BUG-I02-NEW-056（Z） | 阻塞，处置“用户指示忽略” | 见 Z 节。 |
| BUG-I02-NEW-104（Z） | 阻塞，处置“用户指示忽略”（times=1 语义）；泄漏子项已修复 | 见 Z 节。 |
| I03-NEW-007 | 写端子项本地验证通过；整项保持阻塞 | 读端 packed 指针、同分钟不重放/跨分钟完整性、跨进程属读端/外部。 |
| I03-NEW-038 | 本地验证通过 | 红控 512 字节无终止符 → 改后截断标记；ASan/UBSan 24/24。 |
| BUG-L04-004（写端子项） | 写端子项本地验证通过 | 准入拒绝诊断、异步“已接收/已持久化”区分计数、副作用前身份校验；直接构造与读端入口不在本包。 |

## 新发现（编号由主线程分配）

1. `WtDtRunner`（`src/WtDtPorter/WtDtRunner.h`）成员 `_dumpers` 声明在 `_data_mgr` 之后，析构时 ExpDumper 先销毁，DataManager 随后释放 writer，收盘排空期间可回调已销毁 dumper；建议析构函数先 `_data_mgr.release()`。关联 P10-006。
2. `WtDataWriter` 收盘线程持块租约调用外部 dumper；dumper 内同步写同一合约会在非递归块自旋锁上自锁（静态分析，未运行）。关联 P10-006/P10-008。
3. `WtDataWriter::init` 读 `groupsize` 缺省为 0 时，原 `cnt % _log_group_size` 会除零；本包在 `countReceived` 中已防护（行为修复，原四处计数改为加锁共享）。
4. `R44DataManagerEdgesTest` 6 例在夹具 SetUp 的 `StateMonitor::initialize` 失败（改前改后相同，非本包）。
5. p05 测试夹具 `r4_ad_fixture.h::make_tick_item` 在栈上构造打包 `TickCacheItem`，UBSan 报未对齐（`ReloadExistingCacheRebuildsIndex`），测试代码问题。
6. 行为变化提示：AD writer 未导出 V1，经 DataManager release 后被隔离（内存与模块保留至进程退出）；caster 广播异常改为逐 caster 捕获记录；writer 三流广播改为解锁后有序投递（并发同块写入的广播可能由另一线程投递）。AD writer 补 V1 需增加操作准入与线程归属跟踪，留作后续。
7. 环境：本包 `/tmp/wt-i03-s5-dt-writer-before` 在运行中被外部删除（磁盘清理），已按上文重建对照树。

## 增量（主线程追加，2026-11-04 晚）

### 1) WtDataWriterAD 导出生命周期 V1

- 修改 `src/WtDataStorageAD/WtDataWriterAD.h/.cpp`：导出 `getDataWriterLifecycleApiV1`（同一 C ABI，登记本模块创建的 writer，拒绝外来指针）；`AdOperationGuard` 统计 `writeTick`（停止后拒绝）与 `getCurTick`（停止后仍可读但计入）在途调用；`requestStop` 非阻塞关闭准入并让异步线程处理完已接收队列；`canWaitDrain` 在 worker 线程与本 writer 调用帧内为 false；`waitDrained` 有界等待在途调用归零、worker 退出确认后 join；`release()` 在回调/worker 内只请求停止（不再自 join），否则无界排空（旧语义）；停止后 `init` 拒绝（9c）；析构在安全上下文补 release，避免销毁可 join 线程。
- 隔离 ASan+UBSan（LLVM 22，after 快照）：原对齐 21 例 + 日志/身份 3 例 + V1 2 例 **26/26，0 报告**（`runs/ad_green2`）；TSan 同 26 例 **26/26，0 告警**（`runs/ad_tsan`）。
- 红控：改前 writer 上 `I03AdReleaseFromWorkerCallbackDoesNotSelfJoin` 失败，回调中 release 抛出 `thread::join failed: Resource deadlock avoided`（`runs/ad_red2`）。宿主侧：改后 DataManager + 改前 AD 模块 → `WLS_Quarantined`、隔离计数 +1（`runs/fx-r4-ad-red-oldmodule`）；改后 AD 模块 → `WLS_Released`、隔离计数不变（`runs/fx-r4-after-normal`）。物理卸载（dlclose 后映像是否真正移除）未验证。

### 2) I03-NEW-048：收盘线程持块锁调用 dumper、dumper 同步写同一合约自锁

- 运行时确认（红）：改前 writer，同步模式，dumper 在 `dumpHisTicks` 内对同一合约 `writeTick` → 收盘线程在块自旋锁上自锁，用例被硬超时杀（exit 142，`runs/fx-r4-base-normal/CloseDumpReentry_Dumper…`）。缺陷成立。
- 修复（卡9d）：`WtDataWriter` 收盘作业期间（`CloseJobScope`，线程局部标记）在该线程发起的同步写入被接收入队（返回 true），作业释放全部块租约后在收盘线程按序回放；异步模式本就经任务队列，不受影响。
- 绿：normal 通过（作业完成、dumper 收到 1 条、嵌套写返回 true、回放后第二次广播、历史文件生成、实时块仅余回放的 1 条，`runs/fx-r4-after-normal`）。

### 3) I03-NEW-050：groupsize 缺失除零

- 用例 `CloseDumpReentry.MissingGroupSizeDoesNotDivideByZero`（四类写入，配置不含 groupsize）。arm64 整数除零不陷入，普通构建改前改后都通过（`runs/fx-r4-base-normal`），红控须用 UBSan：见下。

### 4) R23 写端 17 组改前基线

- 由 base2 对照树（改前本包文件）全新覆盖率构建（默认 `-Werror`，exit 0）补跑：**82 例 0 失败 2 例 DISABLED**（`runs/base2_r23_writer_suites`），与改后（`runs/after_r23_writer_suites`，未含本增量 NEW-048 改动）用例集合逐一相同、结果相同。同构建 P10ProcBlock/Rt/TaskQueue/DayDsb 56/56（`runs/base2_p10_rt_task`）。

### 5) R44 / Q05 / R68 的 DataManager 失败态断言（NEW-056 归因）

- date-session 修好 R44 夹具 SetUp 后剩 2 例失败；与 Q05 `InitFailsOnMissingModuleButKeepsSinkTargets`、R68 `InitFailsWhenCustomModuleNameIsDirectory` 同源：旧断言要求模块加载失败后 `getBDMgr()` 仍返回传入对象、`canSessionReceive` 仍查询传入状态机（注释称“函数头部已赋值”，来自迭代01覆盖计划对应的旧实现）。
- 归因：与本包 DataManager 合同冲突——卡9 下 init 失败回滚为未初始化态（`WLS_None`，可重试），借用依赖只在 writer 存在时对其可见，失败态不对外发布；依据为 9c（拒绝/保留原实例的对偶：失败不留部分状态）、WP2“防止失败对象进入运行态”与 13a“先准备后发布”。产品不改；四例断言按此更新（失败后 `WLS_None`、`getBDMgr()==nullptr`、未登记状态机按全天候 `true`、无基础数据时 `getSessionComms==nullptr`/`getTradingDate==0`），用例名保留历史 ID。原文件 `before/src/TestUnits/coverage/q05_dtsrv/*.before`（R44 为含 date-session SetUp 修复的版本）。
- 这改变了此前“NEW-056 建议用户指示忽略”的处置：若主线程接受上述归因，NEW-056 可改记“按卡9 合同更新旧断言”；若认为仍需用户确认失败态合同，则回退四处断言（`.before` 可直接恢复）并保持 Z 处置。

### 增量运行汇总（最终）

| 运行 | 结果 | 目录 |
|---|---|---|
| 夹具 r4 改后 normal（含新增 CloseDump×2、AD×1） | 18/18 | `runs/fx-r4-after-normal` |
| 夹具 r4 改前红控 normal | 5/18：原 12 例同前（139×2、134×3、142×4、断言×3）+ `CloseDumpReentry.Dumper…` **142（自锁）**；`MissingGroupSize` 与 AD 用例在旧宿主下通过（见对应红控） | `runs/fx-r4-base-normal` |
| NEW-050 红控（改前 ASan+UBSan） | 用例通过但 **4 条 `runtime error: division by zero`**（tick/queue/order/trans 计数，WtDataWriter.cpp:1116/1163/1209/1255） | `runs/fx-r4-base-asan` |
| 夹具 r4 改后 ASan+UBSan | 18/18，0 报告（含 NEW-050 绿） | `runs/fx-r4-after-asan` |
| 夹具 r4 改后 TSan | 17/18，无数据竞争；唯一非 0 仍为隔离线程 thread leak，`report_thread_leaks=0` 重跑 1/1 | `runs/fx-r4-after-tsan`、`runs/fx-r4-after-tsan-noleak` |
| AD 宿主侧红控（改后宿主 + 改前 AD 模块） | `WLS_Quarantined`，计数 +1 → 失败 | `runs/fx-r4-ad-red-oldmodule` |
| after 覆盖率增量重编（含 NEW-048） R23 写端 17 组 | 82/0 失败/2 DISABLED（与 base2 改前完全一致） | `runs/after2_r23_writer_suites` |
| 同上 P10ProcBlock/Rt/TaskQueue/DayDsb | 56/56（base2 同组 56/56） | `runs/after2_p10` |
| 同上 L05 | 57/57 | `runs/after2_l05` |
| dtcore 增量重编后 R18 / Q05DM / R44 / R68 / R102 / Q05ResampleLeak | 5/5、4/4、6/6（+1 既有 DISABLED）、4/4、3/3（AD writer 现按 V1 排空后删除）、4/4 | `runs/after2_*` |

P10-007 首插夹具与 R23 300 s 等待夹具本增量未重跑（NEW-048 改动只在收盘作业的 `!_disable_his` 分支与四个同步写入口加延迟判断，不触及首插或清缓存重试路径）。

### 本子任务删除过的路径（如实列出）

只删除过本子任务自建的 `/tmp/wt-i03-s5-dt-writer-*` 下内容：重建前删除自己的夹具输出目录（`fx-after-normal`、各 `fx-r*-*` 输出目录在重建时 `rm -rf`）、每例运行目录（`runcase.sh` / `run_cases.sh` 重跑前清空）、改用 LLVM 后删除 Apple clang 版 `/tmp/wt-i03-s5-dt-writer-adalign`、从自建 base2 快照中移除两份不再需要的新测试文件副本。未删除任何他人目录或文件；`/tmp/wt-i03-s5-dt-writer-before` 的消失不是本子任务所为。
