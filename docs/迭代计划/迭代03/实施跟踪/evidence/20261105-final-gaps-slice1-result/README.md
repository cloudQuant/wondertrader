# 迭代03 最终验收前第 1 段本地缺口修复（slice1-result，2026-11-05）

范围：第 1 段只读复核（`SCRATCH/review/slice1-result.json`）中标为 `C_LOCAL_GAP` 的基线条目与新增问题，以及主代理追加的 TSan 发现（临时编号 S1-NEW-1，主代理已定为 **I03-NEW-117**）；第 7 节为主代理第二轮指示的追加项。临时编号 S1-NEW-2 已由主代理定为 **I03-NEW-121**。按主代理 2026-11-05 指示执行。只用本地 fake/替身，不连接柜台、不调用真实厂商 API、不真实下单。未修改迭代跟踪文档。

> 全部是定向本地运行（普通 Debug+coverage 构建），**不是整体回归，也不是最终验收**；sanitizer 由主代理在 LLVM 22 树统一运行（第 4 节清单）。`SCRATCH` = `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`。

## 1. 环境、构建与方法

- macOS arm64，Apple clang（`/usr/bin/c++`），默认 `WT_WARNINGS_AS_ERRORS=ON`（`-Wall -Wextra -Werror`），所有新增/修改代码在该配置下零告警编译。
- 构建目录 `SCRATCH/slice1-result-build`：`cmake -S src -B … -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`，`-j 3`；日志在 `logs/build-*.log`、`logs/configure*.log`。
- 机器负载很高（load average 60–200，多个代理同时构建），`CoverageTests` 主目标与 `CoverageTests_btcore` 没有整体重编；这两个目标里的用例改用“单 TU 夹具”（`tools/tu_harness.py`：用目标自身的编译命令编译测试 TU 与所需产品 TU，链接目标的库部分）验证。
- 同一工作区里第 4 段的在制测试 `n01_wtcore_tradepaths/s4_parser_adapter_null_comm_test.cpp`、`q01_wtcore3/s4_k26_lifecycle_test.cpp`（以及前期一度出现的 s3 文件）在本次构建时编译失败（非本段文件），ninja 因此不能链接 `CoverageTests_wtcore`。本段用 `tools/relink.py` 按 ninja 原链接命令链接私有副本，仅去掉未生成的目标文件（`logs/*.dropped.txt`），未改他人文件；需要从 `bin/TestUnits/` 定位插件或自启子进程的用例（R3/R71 等），把同一副本放在本段构建树的 `bin/TestUnits/CoverageTests_wtcore` 下运行。
- 运行：`tools/run_filter.sh <bin> <label> <filter>`，每组在 `SCRATCH/slice1-result-run/<label>/` 独立目录、`perl alarm` 硬超时、产出 gtest XML（已复制到 `logs/runs/<label>/`）；逐例回归用 `tools/percase.py`（每例独立进程/目录，3 并发）。
- 红控：
  1. 产品修复：先写用例，在产品未改的构建上运行得到红，再改产品得到绿。
  2. 补测（产品此前已修，只缺用例）：把被测产品 TU 的“改前行为”写成 `variants/*.cpp`，用同一编译命令编进私有副本（`relink.py --variant` 或 `tu_harness.py SRC=VARIANT`）运行同一用例应失败；仓库源码不改。
- `before/`、`after/`：本段改动文件的改前/改后副本（路径 `/` 换成 `__`），哈希见 `sources.sha256`。

## 2. 逐项结果

| 条目 | 改动摘要（文件:函数） | 红控 | 绿控 |
|---|---|---|---|
| BUG-L03-002 | 新用例 `l02_wtstools_mq/s1_trademode_default_diag_test.cpp`（子进程初始化 WTSLogger） | 变体 `variants/WTSBaseDataMgr.red-no-trademode-warn.cpp`（去掉每文件一次告警）：1/1 失败，子进程返回码 15（无诊断） `runs/red-l03002` | 单 TU 夹具（当前 `WTSBaseDataMgr.cpp`）1/1 `runs/green-l03002-harness` |
| BUG-P01-004（K45 OOM） | `p01_wtcore_stractx/r42_cta_persist_test.cpp` 新用例 + 替身注入 `std::bad_alloc`（`R42ThrowingHotMgr`、`badAllocOnAg`） | 变体 `variants/CtaStraBaseCtx.red-swallow-bad-alloc.cpp`（`on_init` 吞 bad_alloc）：用例失败 `runs/red-k45` | `runs/pre-k45`、`runs/green2-*`（见第 3 节） |
| BUG-R10-004 | `WtCore/WtSelEngine.cpp`、`WtCtaEngine.cpp`、`WtHftEngine.cpp` `run()` 重复调用拒绝；`WtSelEngine::on_tick` 逐策略 try/catch + 复权 tick RAII | 改前：S1EngineRunGuard 3/3 失败（指针被替换）、S1K25SelIsolation tick 例 1/4 失败（异常穿出引擎）`runs/red-r10004-*`；调度异常例用变体 `variants/WtSelEngine.red-no-schedule-isolation.cpp` 见第 3 节 | `runs/green1-r10004` 10/10（含 S5MainK25Sel、S5SelScheduleThread） |
| BUG-R41-003（K24） | 迁入覆盖率 lane：`n02_wtcore_engines/s1_k24_notifier_bounded_stop_test.cpp`；`fixtures/r160_fake_wtmsgque.cpp` 增加可选阻塞发布（默认关闭） | 独立夹具链接 K24 改前 `EventNotifier`（`evidence/20261104-s5-main/k24-notifier/before`）：阻塞例失败（析构 3 s 内未返回）`runs/red-k24` | 同一夹具链接当前实现 2/2 `runs/green-k24-harness`；覆盖率二进制 2/2 `runs/pre-k24` |
| BUG-P02-005 / BUG-R202-003（K05） | `WtCore/AccountTradingGate.h` 账户级在途汇总与原子“复核+预占”（`adjustInflight/clearInflight/reserveEntry/releaseEntry`）；`WtCore/TraderAdapter.cpp` `doEntrust`/`updateUndone`/`bindAccount`/析构 | 改前：S1TraderEntryGate 的 K05 两例失败（兄弟适配器 B 照常下单；并发两笔都通过、undone=4）`runs/red-trader-gates` | `runs/green1-gates` 8/8（含 S5MainK05EntryLimit 2/2） |
| BUG-P02-005（卡8 整手） | `TraderCTPMini::orderInsert`、`TraderCTPOpt::orderInsert/orderInsertOpt`、`TraderFemas::orderInsert`、`TraderYD::orderInsert`：非有限/非正/超 INT_MAX/非整数量在 SDK 前拒绝（YD 同时拒绝 NULL 委托） | 四个插件各 1/1 失败（SDK 收到截断后的数量）`runs/red-card8-*` | 四个插件各 1/1 `runs/green-card8-*`；四插件全部用例逐例 52/55/76/54 全过 `runs/full-t_*` |
| I03-NEW-003（K16） | `fixtures/ctpmini_api_init/init_check.cpp` cross-instance 探针按 K16=A 改写；`run.py` 对当前源码自动运行该模式；README 更新 | 旧探针在当前产品上返回 40（旧期望 opens=1）`runs/new003-old-probe-cross-instance` | 新探针 0 `runs/new003-ctpmini-init/cross-instance.log` |
| I03-NEW-005（K03 宿主侧） | `TraderAdapter.h` 账本项 `not_sent/not_sent_evidence`；`TraderAdapter.cpp` `closeUnsent` 标记、`checkNotSentEvidence`，在 `onPushOrder`（存活或已成交）与 `onPushTrade` 调用 | 改前：K03 两例失败（账户未暂停）`runs/red-trader-gates` | 绿 `runs/green1-gates`；对照例（无回报 / 撤单终态不暂停）红绿均通过 |
| I03-NEW-006（WtCore；UFT 见第 7 节第 1 项） | `TraderAdapter.cpp` `makeLocalOrderID(date, epochMillis)` 改为由校验过的时钟快照播种、失败不分配并可重试；`doEntrust` 无 identity journal 时先取 `_order_clock` 快照，失败在 SDK 前拒绝；`TraderAdapter.h` 测试接缝 `setOrderClockForTest` | 改前（接缝已加、门控未加）：时钟失败仍下单 `runs/red-trader-gates` | `runs/green1-gates` |
| I03-NEW-009 | `t_ParserXeleSkt/r76_xele_boundary_test.cpp` 新增 3 例（未知帧类型、超长帧/截断心跳/短尾、正控），`ParserXeleSktTestAccess` 增加 `extractBuffer`/`ensureTickCache` | 变体 `variants/ParserXeleSkt.red-old-frame-loop.cpp`（未知类型不推进、无长度检查）：2/3 失败（2 s 有界等待超时；截断心跳被解析）`runs/red-new009` | 3/3 `runs/green-new009`；Xele 全部 37 例逐例全过 `runs/full-t_ParserXeleSkt` |
| I03-NEW-012 | 未改代码（11-04 已由 `LocalUdpReceiver` 修正测试缺陷，未登记）；本段复现与复核 | 改前测试（`evidence/20261104-s5-dt-parsers/before/...udp_handle_write_lock_test.cpp`）`--gtest_repeat=300`：125/300 失败 `runs/new012-repeat300-beforetest` | 当前测试 `--gtest_repeat=300` 0/300；独立进程 300 次 0 失败 `runs/new012-*-current` |
| R2_wtbtcore#2 | `q02_btcore3/r91_replayer_bin_bars_test.cpp`：竞速 loader 按实例返回不同因子，断言各自结果、按自身键缓存、重复查询不再进 loader | 变体 `variants/HisDataReplayer.red-static-key.cpp`（e453f048 的共享 `static char key[20]`）：失败（键、缓存与调用次数串扰）`runs/red-r2` | 单 TU 夹具 4/4 `runs/green-r2-harness` |
| S1-NEW-1（拟 I03-NEW-117，TSan） | `TraderAdapter.cpp`：`getOrders` 判空与遍历入锁（锁内 retain 快照、锁外过滤）；`cancelOne` 判空入锁、grab 判空；`reconcileOrders` 懒创建入锁、add/remove 判空。改前中间版本 `before/src__WtCore__TraderAdapter.cpp.pre-S1-NEW-1`，补丁 `patches/p_orders_lock.py` | 普通构建无法复现数据竞争，红控为主代理 TSan 预扫报告（R3TraderTest 三例）；本段未构建 sanitizer 树 | R3TraderTest 三例 3/3、wtcore 全量逐例见第 3 节；TSan 待主代理 |
| I03-NEW-025 | 见第 7 节第 3 项 | — | — |

### 各项说明与剩余

- **BUG-L03-002**：`S1TradeModeDefault.MissingTradeModeKeepsTmBothAndIsReportedOncePerFile`：同一文件两个品种缺 trademode、一个显式 TM_Long；断言缺省为 TM_Both 可做空、显式值保留、恰好一条 WARN 含数量 2、文件路径与示例品种、不含显式品种；全部显式的第二个文件不产生该 WARN。MSan 仍为外部条件。
- **BUG-P01-004**：`R42CtaPersistTest.K45AllocationFailureDuringRestorePropagatesWithoutPartialState`：在候选准备的两个阶段（第一个持仓的主力规则查询、第二个持仓的合约查询）由替身抛 `std::bad_alloc`，`on_init` 必须原样抛出且已发布状态/订阅保持基线，注入解除后同一候选完整恢复。没有替换全局 `operator new` 做真实堆耗尽（会影响同一二进制其他用例与 sanitizer 运行时），属于以替身模拟的分配失败。
- **BUG-R10-004**：A↔B 重入（`ReentrantTicksBetweenTwoStrategiesAreQueuedAndDeliveredOnce`，经未运行的 SEL ticker strand：A 的回调把 B 的 tick 送回 ticker、B 再把 A 的 tick 送回；断言不就地嵌套执行、FIFO、各恰好一次、无死锁）与回调内 stop（`StopFromInsideAStrategyCallbackDoesNotSelfJoin`，运行中的 ticker，策略回调内 stop 不自 join、随后析构有界完成）在改前已通过——这是卡9 9d strand 已实现的行为，属补测，**无改前红控**（9d 前的 ticker 快照不在仓库内）。TSan 需主代理运行（第 4 节）。“运行后再次 release 后能否再 run”不在合同内，未加用例。
- **BUG-R41-003**：迁入后不再依赖独立夹具；`S1K24NotifierStop.IdleNotifierDrainsAndDestroysServerOnce` 为正控。真实 WtMsgQue 内部线程排空仍为外部条件。
- **K05**：账户净持仓复核 = 本适配器持仓视图 + 账户门禁上所有参与方的在途 + 已通过复核尚未入账的预占；复核与预占在门禁的叶子锁 `_inflight_mutex` 内一次完成，预占在订单进入逐单账本（即计入在途）后释放，任何提前返回由 RAII 释放。`bindAccount` 迁移本适配器已有在途；`removeParticipant` 与析构清除其贡献。跨进程/外部客户端仍不受约束（外部条件）。
- **卡8**：只覆盖四个有厂商头且有 coverage lane 的整手通道；XTP/HuaX/OES 等股票通道的最小合法单位规则未改（不在本次指示内）。
- **I03-NEW-003**：仅按指示改跨实例探针；其余宿主的 `required` 由第 2 段在 R104-002 中实现，本段未做。**新发现 S1-NEW-2（已定为 I03-NEW-121，处理见第 7 节第 2 项）**：同一夹具的 missing-library/null-config（返回 14）与 missing-symbol/null-api（返回 12）四个模式在当前源码上失败，用本段改动前的 `init_check.cpp`/`run.py` 同样失败（`runs/new003-ctpmini-init-beforefixture`），与本段无关。原因是这些模式期望“同一实例失败后可重试/给出 initialization disabled 日志”，与卡9 9c=A 已实现的“重复 init 拒绝”（`src/ParserCTPMini/ParserCTPMini.cpp:191`）冲突，需按 9c 改写夹具期望（未改，超出本次范围）。
- **I03-NEW-005**：UFT 宿主（`WtUftCore/TraderAdapter.cpp:1426` 同型 closeUnsent）未改：K07a 决定暂不为 UFT 建账户门禁，UFT 无“暂停账户”机制，未擅定合同。Femas SDK 返回码语义与 ITraderApi 三态结果的逐 provider 迁移仍为外部条件。
- **I03-NEW-006**：仅 WtCore TraderAdapter；`WtUftCore/TraderAdapter.cpp` 的 `makeLocalOrderID`（约 189 行）与 `WtEngine/WtUftEngine` 构造期发布时钟未改（时间不足；UFT 头文件已加锁但未修改）。
- **I03-NEW-012**：11-04 已确认是测试缺陷（`sport=0` 使订阅包发往 `127.0.0.1:0`，macOS worker 以 EADDRNOTAVAIL 完成发送，按合同开启第二个 Close 周期，与注入的读错误断言竞争），修正为给订阅通道绑定本地接收端口；本段只做复现与 300 次复核，未改代码。
- **I03-NEW-025**：未做。标准配置（Homebrew 头为 `-isystem`）不受影响；静态检索有约 50 处 `EXPECT/ASSERT_xx(Nu, …->size())`，其中 `WTSKlineSlice::size()` 返回 `int32_t` 的可能在 `-I /opt/homebrew/include` 配置下触发，需在该配置下逐 TU `-fsyntax-only` 扫描。

### S1-NEW-1（主代理拟编号 I03-NEW-117）：TraderAdapter `_orders` 数据竞争（单列）

- 来源：主代理 TSan 预扫，`CoverageTests_wtcore` 中 `R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`、`R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache`、`R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code` 报 data race：TraderMocker 的 asio 线程在 `TraderAdapter::onPushOrder` 中懒创建/增删 `_orders`，主线程同时在 `TraderAdapter::getOrders` 锁外判空读取。
- 全部访问点核对（`src/WtCore/TraderAdapter.cpp`，行号为改后）：
  | 位置 | 改前 | 改后 |
  |---|---|---|
  | 析构（约 240 行） | 锁内取出并置空 | 不变 |
  | `getOrders`（约 1530 行） | 判空在锁外；锁内遍历且调用 `_bd_mgr->getContract`/`CodeHelper` | 判空、遍历在锁内，锁内只 retain 到快照；代码匹配与基础数据查询在锁外，逐项 release |
  | `cancelVerified`（约 2212 行） | 锁内判空 grab | 不变 |
  | `cancelOne`（约 3794、3811 行） | `_orders == NULL \|\| size()==0` 在锁外；grab 不判空 | 判空入锁；grab 前判空 |
  | `cancelByCode`（约 3864 行） | 锁内判空遍历、retain 后锁外撤单 | 不变 |
  | `reconcileOrders`（约 4436、4577、4625 行） | 懒创建在锁外；add/remove 不判空 | 懒创建入锁；add/remove 前判空 |
  | `onPushOrder`（约 5080 行） | 锁内判空、懒创建、add/remove | 不变 |
- `SpinMutex` 不可重入：锁内不回调外部、不调用会再次加 `_mtx_orders` 的函数；`getOrders` 返回值语义不变（调用时刻的订单快照，按代码过滤）。头文件未改。
- 验证：普通构建 R3TraderTest 三例 3/3、`CoverageTests_wtcore` 全量逐例无新增失败（第 3 节）；数据竞争是否消失只能由 TSan 判定，**待主代理运行**，本段不作结论。

## 3. 最终树复跑

- 构建：`logs/build-7-wtcore.log`（`ninja -j 3 -k 0 CoverageTests_wtcore`，exit=0；此时第 4 段在制文件已能编译，ninja 直接链接成功，无需 relink）。二进制 `SCRATCH/slice1-result-build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore`，sha256 `ada6fd332560b5f60c1136fcd1e703b8b7990efe9cae8954b1c3ccf19203c01a`（`logs/wtcore-green2.sha256`），包含本段全部产品改动（含 S1-NEW-1 与析构清除在途）。各副本哈希 `logs/binaries.sha256`。
- S1-NEW-1 复现入口（普通构建）：`run_filter.sh <bin> green2-s1-new-1-r3 'R3TraderTest.cancel_freq_window_denies_cancel_after_boundary:R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache:R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code'` → rc=0，3/3（`runs/green2-s1-new-1-r3`）。普通构建不能证明竞争消失，TSan 结论待主代理。
- 本段用例合集：`'S1TraderEntryGate.*:S5MainK05EntryLimit.*:S1EngineRunGuard.*:S1K25SelIsolation.*:S5MainK25Sel.*:S5SelScheduleThread.*:S1K24NotifierStop.*:R42CtaPersistTest.*'` → rc=0，37/37（`runs/green2-slice1`）。
- K25 调度异常隔离补测红控：`relink.py … CoverageTests_wtcore wtcore-red-k25sched --variant src/WtCore/WtSelEngine.cpp=variants/WtSelEngine.red-no-schedule-isolation.cpp`（`logs/link-wtcore-red-k25sched.log`），`S1K25SelIsolation.*` → rc=1：调度例失败（`std::runtime_error "s1 schedule failure"` 穿出 `on_minute_end`，健康策略 schedule_calls=0），tick 例同时失败（该变体也没有 tick 隔离），重入与 stop 两例通过（`runs/red-k25-schedule`）。说明：调度隔离在本段之前已存在，本段补测；tick 隔离是本段产品修复（`runs/red-r10004-k25` 为改前红）。
- `CoverageTests_wtcore` 逐例全量（每例独立进程、独立会话、独立目录，`tools/percase.py`，`runs/green2-wtcore-full/summary.json`）：1063 例，1059 通过、1 跳过、3 失败。
  - 跳过：`UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`（无符号链接策略，按设计跳过）。
  - 失败均为环境：`R59Exec.diff_stale_target_clears_with_raw_code_only_contract_lookup`、`R59Exec.diff_stale_target_uses_raw_code_lookup_and_clears_with_real_unit` 需要 `build-cov` 布局的插件目录（“cannot locate build-cov plugin bin”）；`S5MainK31DateBinding.StorageAndXeleModulesExportVersionedBinders` 需要本树未构建的 `libParserXeleSkt.dylib`（“missing module ParserXeleSkt”）。三者与本段改动无关，在 `build-cov` 树中应复跑。
  - 第一次逐例运行中断的归因见第 7 节第 4 项（结论：磁盘写满，不是用例向进程组发信号）。
- 交易插件与 Xele：`CoverageTests_t_TraderCTPMini` 52/52、`_t_TraderCTPOpt` 55/55、`_t_TraderFemas` 76/76、`_t_TraderYD` 54/54、`_t_ParserXeleSkt` 37/37 逐例全过（`runs/full-t_*`、`logs/full-t_*.log`；构建 `logs/build-5-small.log` exit=0）。
- 早先 green1（不含 S1-NEW-1 与析构清除）的 wtcore 相关复跑记录保留在 `runs/green1-*`，其中曾失败的 `S4OwnerRelease.*` 在改前红树上同样失败（`runs/check-s4-on-red`，第 4 段在制），在 green2 全量中已通过。

## 4. 需要主代理在 sanitizer 树运行的用例

| 二进制 | gtest filter | 期望 |
|---|---|---|
| CoverageTests_wtcore | `R3TraderTest.cancel_freq_window_denies_cancel_after_boundary:R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache:R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code` | TSan 0 报告（S1-NEW-1） |
| CoverageTests_wtcore | `S1TraderEntryGate.*:S5MainK05EntryLimit.*:P025AccountNetLimit.*` | TSan 0 报告（K05 并发复核/预占）；ASan/UBSan 无报告 |
| CoverageTests_wtcore | `S1K25SelIsolation.*:S5MainK25Sel.*:S5SelScheduleThread.*:R202SelTicker.*` | TSan 0 报告（R10-004 原验收要求）；ASan/LSan 无泄漏 |
| CoverageTests_wtcore | `S1EngineRunGuard.*` | ASan/LSan 无泄漏（拒绝重复 run 后不泄漏 ticker） |
| CoverageTests_wtcore | `S1K24NotifierStop.*:R160EventNotifier.*:R41EventNotifierTest.*` | ASan/TSan 无报告；阻塞例的分离 worker 在释放后完成，LSan 可能报告按设计常驻的 Core（K24：超时 Core 保留到进程退出） |
| CoverageTests_wtcore | `R42CtaPersistTest.*` | ASan/LSan 无报告（bad_alloc 传播路径无泄漏） |
| CoverageTests_btcore | `R91Fixture.R2AdjFactors_*` | ASan/UBSan/TSan 无报告 |
| CoverageTests_t_ParserXeleSkt | `R76ParserXeleSktUdpFrameTest.*` | ASan 无越读 |
| CoverageTests_t_TraderCTPMini / _t_TraderCTPOpt / _t_TraderFemas / _t_TraderYD | `I03S5TraderCTPMini.Card8*` / `R61_TraderCTPOpt.card8*` / `R93FemasTest.card8*` / `R93YdTest.card8*` | UBSan 无浮点→整数溢出报告 |
| CoverageTests | `S1TradeModeDefault.*` | 普通即可 |
| CoverageTests_uftcore | `S1New006UftOrderClock.*`；另单独进程运行 `R203UftLocalOrderId.*` | TSan 0 报告（发号播种互斥，第 7.1 节） |

## 5. 新增或修改的用例

- `CoverageTests::S1TradeModeDefault.MissingTradeModeKeepsTmBothAndIsReportedOncePerFile`（新）
- `CoverageTests_wtcore::R42CtaPersistTest.K45AllocationFailureDuringRestorePropagatesWithoutPartialState`（新）
- `CoverageTests_wtcore::S1EngineRunGuard.{SelSecondRunIsRejectedAndKeepsTheRunningTicker,CtaSecondRunIsRejectedAndKeepsTheRunningTicker,HftSecondRunIsRejectedAndKeepsTheRunningTicker}`（新）
- `CoverageTests_wtcore::S1K25SelIsolation.{ThrowingScheduleIsIsolatedAndOtherStrategiesStillRun,ThrowingTickHandlerIsIsolatedAndOtherStrategiesStillReceiveTicks,ReentrantTicksBetweenTwoStrategiesAreQueuedAndDeliveredOnce,StopFromInsideAStrategyCallbackDoesNotSelfJoin}`（新）
- `CoverageTests_wtcore::S1K24NotifierStop.{BlockedPublisherIsDetachedAfterBoundedWait,IdleNotifierDrainsAndDestroysServerOnce}`（新，迁入 lane）
- `CoverageTests_wtcore::S1TraderEntryGate.{K05SiblingAdaptersOfOneAccountShareInFlightQuantity,K05ConcurrentEntriesCannotBothPassTheAccountCheck,K03LiveOrderReportAfterNotSentClosurePausesAccount,K03TradeAfterNotSentClosurePausesAccount,K03NotSentWithoutLiveEvidenceKeepsTheAccountOpen,New006ClockFailureRejectsEntryBeforeTheProvider}`（新）
- `CoverageTests_btcore::R91Fixture.R2AdjFactors_IndependentReplayCallsKeepTheirOwnLoaderKey`（加强断言）
- `CoverageTests_t_ParserXeleSkt::R76ParserXeleSktUdpFrameTest.{UnknownFrameTypeEndsParsingOfTheDatagram,FrameLongerThanTheDatagramIsNotRead,ConsecutiveValidFramesAreAllParsed}`（新）
- `CoverageTests_t_TraderCTPMini::I03S5TraderCTPMini.Card8NonWholeLotQuantityIsRejectedBeforeTheSdk`、`CoverageTests_t_TraderCTPOpt::R61_TraderCTPOpt.card8_non_whole_lot_quantity_is_rejected_before_the_sdk`、`CoverageTests_t_TraderFemas::R93FemasTest.card8_non_whole_lot_quantity_is_rejected_before_the_sdk`、`CoverageTests_t_TraderYD::R93YdTest.card8_non_whole_lot_quantity_is_rejected_before_the_sdk`（新）
- 夹具 `src/TestUnits/fixtures/ctpmini_api_init` cross-instance 模式（改写期望，I03-NEW-003）；missing-library/missing-symbol/null-api/null-config 四个模式（改写期望，I03-NEW-121，第 7.2 节）。
- `CoverageTests_uftcore::S1New006UftOrderClock.{ClockFailureRejectsEntryBeforeTheProvider,EveryDirectionIsGatedAndTheDefaultClockIsRestored}`（新，第 7.1 节）
- I03-NEW-025：35 个测试文件 88 处比较字面量/常量的符号性（不改断言语义，第 7.3 节）。

## 6. 未完成与锁定说明

- 锁定但未修改：`src/WtUftCore/TraderAdapter.cpp`、`src/WtUftCore/TraderAdapter.h`、`src/TestUnits/coverage/q04_uftcore3/s1_new006_uft_order_clock_test.cpp`（未创建）、`src/TestUnits/coverage/p11_wtcore_traderadapter/s1_trader_entry_gate_test.cpp`（未创建，实际放在 n01 lane）。
- 第一轮未完成项（I03-NEW-006 UFT 部分、I03-NEW-121、I03-NEW-025）在第 7 节处理；剩余见第 7 节末尾。
- 主代理已将 `src/WtCore/WtCtaEngine.cpp`、`WtSelEngine.cpp`、`WtHftEngine.cpp` 的锁转给第 2 段；本段对这三个文件的改动（R10-004）在转锁前完成，`after/` 中三份副本是本段 14:27 的状态，之后的改动不属于本段。
- 同样，卡8 的四个交易插件源文件（`src/TraderCTPMini/TraderCTPMini.cpp`、`src/TraderCTPOpt/TraderCTPOpt.cpp`、`src/TraderFemas/TraderFemas.cpp`、`src/TraderYD/TraderYD.cpp`）及其测试 `t_TraderCTPOpt/r61_traderctpopt_api_test.cpp`、`t_TraderFemas/r93_traderFemas_api_test.cpp`、`t_TraderYD/r93_traderYD_api_test.cpp` 的锁已由主代理转给第 4 段；第 4 段在 15:07–15:09 又改动了其中部分文件（本段 `isWholeLotVolume` 与 card8 用例仍在）。`after/` 是本段转锁前状态，`sources.sha256` 第三部分记录写入时刻的仓库哈希。
- 主目标 `CoverageTests` 与 `CoverageTests_btcore` 未在本段构建树整体重编，相关用例只在单 TU 夹具中验证，最终冻结构建需包含这两个目标。

## 7. 第二轮追加（主代理 2026-11-05 指示）

### 7.1 I03-NEW-006 UFT 部分

- 差距：`src/WtUftCore/TraderAdapter.cpp` 的 `makeLocalOrderID()` 用 `TimeUtils::getCurDate()/getLocalTimeNow()` 在 `call_once` 内播种，时钟/时区失败时用失败值永久播种且不拒绝委托。
- 合同对齐：UFT 下单路径与 WtCore 同构（`doEntrust`：代码校验 → `ApiCallLease` → `makeEntrustID` → 分配本地号 → 风控 → 逐单账本 → provider），UFT 没有 identity journal，因此“无 journal 时”= 每笔委托。无需新合同即可对齐，未加账户门禁（K07a）。
- 改动（`patches/p_uft006.py`，CRLF+BOM 保持）：
  - `TraderAdapter.h`：公开 `OrderClock` 类型与 `setOrderClockForTest(OrderClock)`（NULL 恢复默认）；私有 `_order_clock`（默认 `defaultOrderClock` = `TimeUtils::tryGetDateTime`）。新增成员在类末尾，未改已有成员顺序，类不是跨模块 ABI。
  - `TraderAdapter.cpp`：`makeLocalOrderID(curDate, epochMillis)` 与 WtCore 同规则（首个成功快照播种，无效快照返回 0 不播种，可重试，回绕跳过 0）；`TraderAdapter::doEntrust` 先取一次快照，失败或得到 0 即记 ERROR 并返回 `UINT_MAX`，发生在风控、账本预占与 `orderInsert` 之前。
- 用例 `src/TestUnits/coverage/q04_uftcore3/s1_new006_uft_order_clock_test.cpp`：
  - `CoverageTests_uftcore::S1New006UftOrderClock.ClockFailureRejectsEntryBeforeTheProvider`：时钟失败时 openLong 返回 `UINT_MAX`、时钟被调用 1 次、provider 未收到请求、在途为 0；时钟恢复后下一笔成功，usertag 为 `wtp.<id>.<localid>`，在途 1。
  - `CoverageTests_uftcore::S1New006UftOrderClock.EveryDirectionIsGatedAndTheDefaultClockIsRestored`：开多/开空/平多/平空四个入口都经过时钟门（调用 4 次、无请求、无在途）；`setOrderClockForTest(nullptr)` 恢复生产时钟后可下单。
- 红控：变体 `variants/WtUftCore.TraderAdapter.red-no-clock-gate.cpp`（改前源码 + 仅加测试接缝定义，不改发号、不加门）经 `relink.py … CoverageTests_uftcore uftcore-red-new006 --variant src/WtUftCore/TraderAdapter.cpp=…` 链接（`logs/link-uftcore-red-new006.log`），`S1New006UftOrderClock.*` rc=1，2/2 失败（委托照发、在途 1、时钟未调用）`runs/red-new006-uft`。
- 绿控：构建 `logs/build-8-uftcore.log`（`ninja -j 3 -k 0 CoverageTests_uftcore` exit=0，二进制 sha256 见 `logs/uftcore-green.sha256`）；`'S1New006UftOrderClock.*:R203UftLocalOrderId.*:q04_trader_entrust.*:q04_trader_usertag_bounds*'` rc=0，25/25 `runs/green-new006-uft`；`CoverageTests_uftcore` 逐例全量 309/309 通过 `runs/full-uftcore`。
- 剩余：`WtEngine/WtUftEngine` 构造期发布时钟（`getCurDate/getCurMin` 等）不在本项改动内；真实时区故障（`getTZOffset()==99`）只通过注入失败快照模拟。
- sanitizer：`CoverageTests_uftcore` `S1New006UftOrderClock.*:R203UftLocalOrderId.*`（TSan：播种互斥；R203 需单独进程运行）。

### 7.2 I03-NEW-121（原 S1-NEW-2）：ctpmini_api_init 夹具按卡9 9c=A 改写

- 只改夹具：`src/TestUnits/fixtures/ctpmini_api_init/init_check.cpp`、`README.md`；产品 `src/ParserCTPMini/ParserCTPMini.cpp` 未改；`run.py` 未改（本轮）。改前副本 `before/…init_check.cpp.pre-I03-NEW-121`、`before/…README.md.pre-I03-NEW-121`。
- 新期望（missing-library / missing-symbol / null-api / null-config 四个失败模式）：
  1. 已保留模块的失败实例（missing-symbol、null-api）在 release 前再次 init（换另一模块）被拒绝，不产生新的装载，日志含 “initialization disabled”（返回码 12）。
  2. release（两次）后，四个模式下同一实例再次 init 一律被拒绝，`opens` 不变、未连接（返回码 14）。
  3. 新实例可以重试：`other.init(good)` 成功、已连接、`opens` 恰好 +1、装载路径不同于失败路径（返回码 13）；随后 API 恰好释放 1 次、无 dlclose、失败模块未卸载（返回码 15）。
  - 未断言：未装载任何模块就失败的实例（missing-library、null-config）在 **release 前** 能否原实例重试——当前产品允许（`_init_done` 只在成功时置位），卡9 9c 未覆盖“失败的首次 init”，**不确定，建议记用户指示忽略**；如需“失败即终止”，需用户裁定后再改产品。
- 运行（按夹具 README）：`conda run -n base python src/TestUnits/fixtures/ctpmini_api_init/run.py SCRATCH/slice1-result-build <repo>/src/ParserCTPMini/ParserCTPMini.cpp SCRATCH/slice1-result-run/new121-ctpmini-init` exit=0，十个步骤/模式全部 0（`logs/new121-run.log`、`runs/new121-ctpmini-init/`）。
- 红控（夹具对产品回退的检出能力；以 baseline 参数传入变体源码，只看各模式退出码）：
  - 改前夹具在当前产品上：missing-library/null-config 返回 14、missing-symbol/null-api 返回 12（`runs/new003-ctpmini-init-beforefixture`、`runs/new003-ctpmini-init`）。
  - 变体 `variants/ParserCTPMini.red-reinit-after-release.cpp`（init 门去掉 `_lc.shutDown()`）：missing-library、null-config 返回 14（`logs/new121-red-run.log`）。
  - 变体 `variants/ParserCTPMini.red-no-quarantine-reject.cpp`（去掉“模块已装载即禁用”分支）：missing-symbol、null-api 返回 12（`logs/new121-red2-run.log`）。
  - 两次红控时 `run.py` 按 baseline 期望码判定输出 “Unexpected test outcome …”，属预期（baseline 期望码是针对历史改前源码的）。`run.py` 中历史 baseline 期望码（崩溃 -11/-10、unload/connect baseline 的 10/16/25）未重新核对：这些历史源码不在仓库内；本次改写把 missing-symbol/null-api 的“禁用”检查移到 release 之前，若再用历史 crash baseline 运行，期望码可能需要更新。

### 7.3 I03-NEW-025：`-I /opt/homebrew/include` 配置下 `-Wsign-compare` 全量扫描

- 方法（`tools/signcmp_scan.py`，静态编译检查，非运行）：从本段构建树 `ninja -t commands` 取 44 个 `CoverageTests*` 目标的真实编译命令（`new025/cov_targets.txt`、`new025/cov_compile_cmds.txt`），对 `src/TestUnits/coverage/` 下每个测试 TU 取第一条命令，把 `-isystem /opt/homebrew/include` 换成 `-I/opt/homebrew/include`，去掉 `-o/-MD/-MF/-MT`，加 `-fsyntax-only -Wno-error -Wsign-compare`，3 并发。`-Wno-error` 只为一次收齐全部诊断；修后另以未改动的标准命令（`-isystem`、`-Werror`）`-fsyntax-only` 复核（`SCAN_MODE=std`）。
- 覆盖面：coverage 下 777 个 `.cpp` 中 775 个被扫描；未扫描 2 个为自带 `main` 的独立夹具 `p08_btcore_mockers3/r20_mq_fixture/{fake_mq,notifier_check}.cpp`（按设计不进 coverage 目标）。4 个插件 TU（`v2_exec_plugin/*`、`q04_uftcore3/fake_plugin/uft_lifecycle_trader.cpp`）命令中没有 Homebrew `-isystem`，按原命令扫描。
- 阳性对照：对 `evidence/20261104-s5-main/sign-compare/before/…/r71_cta_ctx_test.cpp`（已知改前副本）用同一变换编译，检出 1 处 `-Wsign-compare`、实例化位置 353 行（`new025/scan/new025-posctl/`）。
- 首轮全量结果（`new025/scan/new025-scan/summary.json`）：775 个 TU 全部 rc=0；只出现 `-Wsign-compare` 一类诊断（没有其他类别因头文件改为非系统头而出现）；43 个 TU、49 处可见实例化位置（`new025/new025-sites.txt`，含锁归属）。
- **注意计数口径**：gtest 比较模板按类型组合在每个 TU 内只实例化一次，clang 只报告第一个实例化位置，所以首轮 49 处是**下限**。修掉一处后同一 TU 会暴露下一处；本段对已加锁的文件迭代“扫描→修正→复扫”共 8 轮直到 0（`tools/new025_loop.sh`、`new025/scan/new025-round*/`）。
- 修正（只改测试字面量/常量的符号性，比较值均为非负常量，断言语义不变；`patches/p_new025.py` 首轮 38 处，`patches/p_new025_iter.py` 按编译器报告逐处 50 处，记录 `new025/new025-iter-record.jsonl`；行尾/编码逐字节保持）：
  - 返回 `int32_t` 的各类 slice `size()`（如 `WTSKlineSlice`）与 `Nu` 比较：去掉 `u`；返回无符号的 `size()`（如 `q02_btcore3/r103_replayer_tick_csv_test.cpp:59`）与有符号字面量比较：加 `u`；无符号值与有符号字面量比较：加 `u`（如 `tick.action_date`、`bdMgrQueryCalls`）。
  - `strcmp`/`std::string::compare`（返回 `int`）与 `0u`：改为 `0`；`FrontID`（`int`）与 `1u`：改为 `1`。
  - `std::strlen` 与 `MAX_INSTRUMENT_LENGTH`、`block->_capacity` 与 `MAX_KEY_CNT`、`getOrderTime()/getTradeTime()`（`uint64_t`）与 `TimeUtils::makeTime`（`int64_t`）：给常量一侧加 `static_cast`。
  - 共 35 个文件、88 处（列表 `new025/new025-locked-files.txt`；改前/改后副本 `new025/before/`、`new025/after/`，哈希 `new025/sources.sha256`）。
- 修后复核：35 个 TU 在 `-I` 配置下 0 处 `-Wsign-compare`（`new025/scan/new025-final-I/summary.json`）；在标准命令（`-isystem`、`-Werror`）下 35/35 `-fsyntax-only` rc=0（`new025/scan/new025-final-std/summary.json`）。
- **未修（文件被其他代理锁定，按规则跳过）**，首轮可见 11 处，实际数量可能更多（同上计数口径）：
  - 第 4 段：`p03_btcore_mockers2/r180_btcore_mockers2_test.cpp:232,303`、`q03_storage3/q03_rdm_kline_file_test.cpp:475`、`t_TraderCTPOpt/r61_traderctpopt_api_test.cpp:909,1485`（1485 为 `WTSDirectionType` 枚举与 `int`）、`t_TraderFemas/r93_traderFemas_api_test.cpp:652`（`std::string::find` 的 `size_t` 与 `0`）。
  - 第 3 段：`p05_datastorage2/r132_reader_ad_cache_test.cpp:61`、`q03_storage3/r203_storage_residual_test.cpp:265`、`r24_exefact2/r24_exec_runner_test.cpp:256`、`t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp:908,960`（960 为 `int` 与 `WTSErrorCode` 枚举）。
  - 处理方式相同（字面量符号性或常量侧 `static_cast`）；锁持有方完成后需在 `-I` 配置下迭代复扫到 0。
- 运行复核（普通构建 `logs/build-10-tstar.log` exit=0，二进制哈希 `logs/new025-binaries.sha256`）：受影响文件所在套件 `CoverageTests_t_TraderCTPMini` `R61_TraderCTPMini.*:T3_006_TraderCTPMini.*` 15/15、`_t_TraderFemas` `FemasContractFixture.*:TraderFemasDisabled.*:TraderFemasTest.*` 41/41、`_t_TraderYD` `TraderYDTest.*:yd_plugin_factory.*` 18/18、`_t_ParserFemas` `femas_parser.*` 18/18、`_t_ParserXTP` `ParserXTP.*` 12/12（`runs/new025-t_*`）。
- 未运行：`CoverageTests_wtcore`（`r71_hft_ctx_test.cpp`、`r73_cta_ctx_test.cpp`）、`CoverageTests_btcore`（p03/p04/p08/q02 共 10 个文件）、`CoverageTests_adstorage`（p05 共 6 个文件）、`CoverageTests_mqshare`（r19 1 个文件）与主目标 `CoverageTests`（l01/p09/q03/r22/r23 共 11 个文件）没有在修改后重编运行：机器负载 60–80，wtcore 依赖链重编需 480 余步，本轮放弃；这些文件只做了上述两种配置的编译复核。改动只涉及比较字面量后缀或常量侧 `static_cast`，需在冻结构建中随整体复跑确认。
- 补测说明：本项是测试代码的编译兼容修正，无产品行为变化；“红”为修改前 `-I` 配置的扫描诊断（`new025/scan/new025-scan/`，`-Werror` 下即为编译失败），“绿”为修改后同配置 0 诊断。

### 7.4 第一次全量逐例运行中断的归因（测试隔离排查）

- 现象：14:37 起的 `percase.py` 首轮（子进程与运行器同一进程组）在约 14:40:38 中断：运行器日志为空、外层 `echo exit=` 未写入、shell exit 1；在途用例中 3 个已写出 XML/profraw 但无 run.log，1 个（`R130EngineFee.MalformedKeysRejectInitializationWithoutApplyingValidPrefix`）没有任何输出。
- 排查：
  1. 源码检索 coverage 用例与 WtCore/Share/WTSUtils/WTSTools/WtPorter 中的 `kill(0`/`killpg`/`kill(-`：无；所有 `kill(child, …)` 都在 `posix_spawn`/`fork` 成功之后使用（例如 `q01_wtcore3/r3_trader_risklimit.cpp:203`）。
  2. 金丝雀复现（`tools/canary.py`）：对 gtest 顺序第 800–815 例（含中断时在途的 4 例）逐例在独立会话中与一个 `sleep 60` 金丝雀同组运行：16/16 用例通过，金丝雀全部存活，无进程组信号（`runs/pgkill/canary-1.log`）。
  3. 同一时刻（14:40:51 CST）另一代理记录 “Stop my background build due to full disk”；当时本段 `SCRATCH/slice1-result-run` 中逐例 `*.profraw` 共约 22 GB。
- 结论：中断由磁盘写满引起（`run.log`/日志写入失败使运行器异常退出，`echo` 追加也失败），**不是用例向进程组发信号，不是测试隔离问题**；收回第 3 节早先“疑似进程组信号”的说法。已删除本段 scratch 中的逐例 profraw（约 22 GB，均为本段自产、非证据文件），`percase.py` 保留“每例独立会话”与续跑功能。

### 7.5 I03-NEW-025 补充：转锁后的 8 个文件

- 主代理把第 3、4 段持有的 8 个文件的锁转给本段（`lock.py` 返回 ok，`new025b/new025b-files.txt`）。修改前按当前内容另存 `new025b/before/`（包括第 4 段刚改过的 r180、q03_rdm_kline_file，以及第 3 段启用改动后的 r132/r203/r24/cov_traderctpopt_spi）。编译命令按当前构建树重新导出（`new025b/cov_compile_cmds2.txt`，818 条）。
- 方法同 7.3：`-I/opt/homebrew/include` 配置下迭代扫描→修正 5 轮到 0（`tools/new025b_loop.sh`、`patches/p_new025b_iter.py`；规则在 7.3 基础上补充：枚举 `WDT_LONG` 常量侧 `static_cast<int>`；`int` 字面量与 `uint32_t` 底层枚举 `WTSErrorCode`、`size_t` 的 `std::string::find` 比较时加 `u`；`const int64_t timeBeforeQuote` 期望值一侧 `static_cast<uint64_t>`）。
- 计数：首轮可见 11 处，迭代后共修 **19 处 / 8 个文件**（r180 4、q03_rdm_kline_file 2、r61_traderctpopt_api 5、r93_traderFemas_api 1、r132_reader_ad_cache 1、r203_storage_residual 1、r24_exec_runner 2、cov_traderctpopt_spi 3；逐处记录 `new025b/new025b-iter-record.jsonl`）。只改字面量后缀或常量/期望值一侧 `static_cast`，断言语义不变；逐字节核对行数不变、LF 行尾与无 BOM 保持。
- 复核（编译检查，未运行）：`-I` 配置 8/8 TU 0 处 `-Wsign-compare`（`new025b/scan/new025b-final-I/summary.json`）；标准命令（`-isystem` + `-Werror`）8/8 `-fsyntax-only` rc=0（`new025b/scan/new025b-final-std/summary.json`）。运行按主代理指示留到最终冻结回归。
- 过程失误（已纠正）：第一次套用扩展规则时，规则文本经未加引号的 heredoc 写入 Python 普通字符串，反向引用 `\1` 被转成控制字符，11 行比较被改坏（记录 `new025b/new025b-iter-record.BROKEN-attempt.jsonl`）。随即用几秒前另存的本段 before 副本整文件恢复（逐个 `cmp` 一致，且差异只在这 11 行），修正脚本并对 8 条已知行自测后再执行。被改坏的状态没有参与任何编译或运行证据。
- 至此 I03-NEW-025：首轮全量扫描（约 15:00 的内容）中有诊断的 43 个文件全部扫到 0（35 + 8 个文件，88 + 19 = 107 处修正），其余 732 个 TU 首轮即为 0。其他代理在此之后新增或修改的测试 TU 未再全量复扫；冻结构建需在同配置下整体复核一次，并运行这些用例。

## 8. I03-NEW-138：TraderAdapter `_stat_map` 与交易统计计数的数据竞争（最终 TSan 回归发现）

- 现象（主代理 F3 TSan 全量回归）：`CoverageTests_wtcore` 的 `R3TraderTest` 有 5 例在隔离子进程中 abort（134）：`cancel_freq_window_denies_cancel_after_boundary`、`cancel_localid_frequency_cache_uses_standardized_contract_code`、`cancel_localid_risk_rejection_retries_do_not_grow_cache`、`order_freq_window_denies_submission_after_boundary`、`order_freq_window_precheck_uses_standard_code_after_raw_submission`（日志 `SCRATCH/final3/run-tsan/cwd/CoverageTests_wtcore/R3TraderTest.*/log.txt`）。TraderMocker 的 asio 线程在 `onPushOrder` 中（持 TraderMocker 自己的 M0/M1）向 `_stat_map`（`WTSHashMap`，底层 unordered_dense，插入可扩容）插入 `WTSTradeStateInfo` 并改计数；主线程在 `checkOrderLimits`/`checkCancelLimits` 中只持 `_risk_exclude_mutex`（M2）读 `_stat_map` 与 `total_orders()/total_cancels()`。两边不是同一把锁，扩容时还可能读到失效内存。
- 锁：新增 `TraderAdapter::_mtx_stat`（`mutable SpinMutex`，`TraderAdapter.h` `_stat_map` 声明之后）。它是叶子锁：持有期间只做 map 查找/插入/遍历、`WTSTradeStateInfo::create` 和计数读写，不加其他锁、不调用外部代码（日志、sink、provider、其他成员函数）。外层可能已持 `_risk_exclude_mutex` 或 SPI 线程上的锁，只存在“外层锁 → `_mtx_stat`”一个方向，不与 `_mtx_orders`、`_ledger_mutex` 等形成反向嵌套。查找/创建统一走新的文件内静态函数 `statInfoLocked(map, code)`（调用方持锁）。
- 访问点（`src/WtCore/TraderAdapter.cpp`）：
  | 位置 | 改前 | 改后 |
  |---|---|---|
  | 析构 | 无锁 release | 锁内取出置空，锁外 release |
  | `run()` 懒创建 | 无锁 | 锁内判空创建 |
  | `checkCancelLimits` / `checkOrderLimits` | 仅 `_risk_exclude_mutex` 下读 map 与 `total_cancels()/total_orders()` | 仍在 `_risk_exclude_mutex` 内，嵌套 `_mtx_stat` 取计数，判定与插入排除表不变 |
  | `buy` / `sell` | 取 `TradeStatInfo&` 引用，在下单循环中读 `l_openvol/s_openvol`（下单可同步触发回报改写） | 入口在锁内照旧创建条目；每个开仓规则检查前在锁内取 `TradeStatInfo` 快照，限额计算用快照，锁外下单 |
  | `reconcileOrders`（委托查询回报） | 无锁 get/create 与笔数/撤单/错单计数 | 计数块包在 `_mtx_stat` 内 |
  | `onRspTrades`（成交查询回报） | 无锁 get/create 与开平量计数（中间持 `_ledger_mutex`） | `_ledger_mutex` 去重块先完成并释放，再在 `_mtx_stat` 内累加 |
  | 成交查询后的统计日志遍历 | 无锁遍历 map | 锁内复制快照，锁外写日志 |
  | `onPushOrder` 撤单/错单计数 | 无锁 | `_mtx_stat` 内（该块只有计数） |
  | `onPushOrder` 首次推送笔数计数 | 无锁 | 显式作用域的 `_mtx_stat` 块，持仓可平更新在锁外 |
  | `onPushTrade` 开平量计数 | 无锁 get/create，计数与持仓更新交错 | 计数先累加到局部 `TradeStatInfo`，持仓更新完成后在 `_mtx_stat` 内一次性加到共享条目（`countStats` 为假时只创建条目、不计数，与原来一致） |
- `isTradeEnabled` 不访问 `_stat_map`（只看排除表与频控窗口），未改；全文件已无未加锁的 `_stat_map->` 访问（`grep` 核对）。对外语义不变：限额判定、计数口径、日志内容不变。
- 改前副本 `before/src__WtCore__TraderAdapter.cpp.pre-I03-NEW-138`、`before/src__WtCore__TraderAdapter.h.pre-I03-NEW-138`，补丁 `patches/p_new138.py`；行尾保持（cpp 全 CRLF+BOM；h 新增行沿用所在处的 LF）。
- 验证：见下方“8.1 运行”。普通构建无法证明竞争消失，TSan 由主代理运行：`CoverageTests_wtcore` `R3TraderTest.*`（重点为上述 5 例，隔离子进程模式）、`R170TraderRisk.*:R90TraderRisk.*:P11PreOrderRisk.*`。

### 8.1 运行（普通 Debug+coverage 构建）

- 构建：`logs/build-11-new138.log`（`ninja -j 2 -k 0 CoverageTests_wtcore` exit=0，零告警 `-Werror`）；二进制 sha256 `416b5bec592521fa0e777ae28b744335061c362390fb8533d04a5fd3b07c573a`（`logs/wtcore-new138.sha256`）；源码哈希 `logs/new138-sources.sha256`。
- 频控/风控相关套件逐例（每例独立进程、独立会话）：`R3TraderTest`（17，含上述 5 例，隔离子进程模式）、`R170TraderRisk`、`R90TraderRisk`、`R90TraderReport`、`P11*`（CrashClass/PreOrderRisk/PushNotify/PushPersist/SyncReport/TraderTest）、`R59Buy/R59Sell/R59Misc`、`R202TraderAdapter`、`N01TraderAdapter*`、`S1TraderEntryGate`、`S5MainK05EntryLimit`：124/124 通过（`runs/new138-related/`）。
- `CoverageTests_wtcore` 逐例全量：1082 例，1078 通过、1 跳过（`UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`，按无符号链接策略设计跳过）、3 失败；失败与第 3 节相同，均为环境原因：`R59Exec` 两例缺 build-cov 插件目录，`S5MainK31DateBinding` 缺 `libParserXeleSkt.dylib`（`runs/new138-wtcore-full/`）。
- 红控：数据竞争只能由 TSan 判定，普通构建没有改前红控；改前的 TSan 失败（5 例 abort 134）即主代理 F3 TSan 日志。TSan 复跑待主代理。
