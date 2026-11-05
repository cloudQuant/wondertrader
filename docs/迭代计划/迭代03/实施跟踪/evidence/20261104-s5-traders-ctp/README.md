# s5 traders-ctp：CTP 系交易适配器生命周期与身份（卡9 / 卡6d / 卡15）

2026-11-04，macOS arm64（Apple clang 17），HEAD `ccaad010`（工作区含大量他人未提交改动）。本目录只记录本包的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、Linux、真实 CTP/CTPMini/CTPOpt SDK 或柜台、最终整合树。未修改任何跟踪文件；未修改共享 `src/TestUnits/coverage/CMakeLists.txt`；未修改 `src/CTPLoader/*`。

会话中途因额度中断约 4 小时，恢复后先对已改文件做 `-fsyntax-only -Werror` 复核（exit 0）再继续；拥有文件在本包开始后未被他人改动（逐文件对比 `before/` 哈希，仅本包修改的文件不同）。

## 构建与运行方式

- 覆盖率构建目录 `/tmp/wt-i03-s5-traders-ctp-cov`（`-DWT_ENABLE_COVERAGE=ON`，默认 `-Werror`，`-j 3`）。13:10 起的整目标构建 exit 0（`/tmp/wt-i03-s5-traders-ctp-cov.baseline-build.log`）。之后其他包正在编辑 `WtDtCore/DataManager.cpp`（未声明 `_writer`），整目标重建在该无关 TU 失败；因此本包改为只用 ninja 重编本 lane 的对象（测试 TU 与 Trader TU），再用 ninja 记录的原链接命令（`ninja -t commands`）与 13:xx 已构建的依赖库重链。与他人在建依赖的混合属本机局部诊断，不是干净整合树。
- 红控：`scripts/overlay_build.py` 把 `before/` 中的改前产品与改前旧测试覆盖到实体快照 `/tmp/wt-i03-s5-traders-ctp-before/src`（`rsync --no-links`，`find -type l` 为 0），新测试以 `-DWT_I03_S5_TRADERS_BASELINE` 编译（改前不存在的观察接口以哨兵值代替，使断言失败而非编译失败），链接同一批依赖库。实际编译/链接命令见 `scripts/overlay-*-red-commands.sh`。
- 每例/每 lane 在 `/tmp/wt-i03-s5-traders-ctp-run/<label>` 独立 cwd 运行，`perl alarm` 硬限时，`LLVM_PROFILE_FILE` 指向该目录。可能挂死的新用例自带 Watchdog：超时 `_Exit(98)`（红控 exit 98 即“未在界限内完成”）。
- 所有用例只用本地 stub/fake SDK，不加载厂商库、不连接柜台、不真实下单/撤单/结算。

## 产品修改（合同依据）

新增 `src/TraderCTP/VendorTdLifecycle.hpp`（三家 CTP 交易适配器共用的头文件，仅本包拥有的目录）：

- 无锁准入闸门（API 闸门 + 回调闸门）与线程局部帧栈：所有可能触达 SDK 的宿主调用（含 worker 发出的查询）持 API lease，所有 SPI 回调持回调 lease；不在 SDK 调用期间持锁，故同步回调重入（Req* 内回调再撤单）不死锁（T1-CTPOpt-02 旧候选死锁原因）。
- 终止停止 `release()`/`disconnect()`（卡9a/9b/9d）：先关闭两道闸门与 worker；若当前线程处于本实例回调/接口调用/worker 内 → 只记录延后（`StopDeferred`），由下一次安全线程的生命周期调用完成；否则在 2 秒界限内等待 worker 退出、所有已准入 lease 离开，再在调用线程上 **恰好一次** `SDK Release()`，随后清容器、状态 `Released`。超过界限（worker 卡在 SDK 请求、宿主回调阻塞）→ 不 Release、不释放任何对象，状态 `DrainUncertain` 并记错误日志（sink 已注销时写 stderr），之后的 `release()` 可重试。
- 工厂 remover `deleteTrader`：曾创建 SDK 对象的实例（无版本匹配 SDK 头文件证明 `Release()` 会 join SDK 线程或禁止迟到回调）一律不删除：先发起停止，登记进程级隔离表并 `dlopen(RTLD_NOLOAD|RTLD_NODELETE)`/`GetModuleHandleEx(PIN)` 钉住本模块，指针保留（不置空）表示隔离到进程退出；迟到回调被已关闭的闸门拒绝。从未创建 SDK 的实例照常删除并置空。Windows 钉住分支未在本机编译验证。
- 实例单次使用（卡9c；T2-CTP-03 原修复方向“单次对象或可重连状态机”取前者）：重复 `connect()` 保留原 API 并记错误；停止后 `connect()` 被拒绝。
- void `connect()` 失败（无 creator、creator 返回空、流目录不可建、已停止）统一以 `handleEvent(WTE_Connect, -1)` + 错误日志可观察（T2-CTP-02、T3-006；宿主 `TraderAdapter::handleEvent` 现有约定即 `ec != 0` 为连接失败）。
- 同步拒绝的查询（无终帧）立即解除查询 gate，后续查询不被饿死（T25-005 README 已指出的 queryAccount 等缺口）。
- 析构：自身回调/worker 内析构 → `abort`（工厂路径会隔离而不是删除）；否则关闭闸门、无界等待 worker/在途帧后 **归还查询数组容器**（T1-CTPOpt-01；析构从不调用 SDK Release，沿用“先 release 后 delete”的旧合同）。
- `m_sink`（CTPOpt 另含 `m_optSink`）、`m_wrapperState`、`m_bInQuery`、`m_bStopped` 改为原子量，回调读取本地快照后判空；原 OnRspError/OnRspQrySettlementInfo 无条件解引用 sink 的路径顺带判空（BUG-T25-003 的 sink 子项，未主张关闭该条目；`pSettlementInfo` 空指针仍在）。

逐文件：

- `TraderCTP.{h,cpp}`：上述生命周期；`disconnect()` 不再把 `release()` 排到被查询 gate 卡住的队列（BUG-T25-005 根因）；保留 11-03 已实施的 CTP 身份分配（T2-CTP-08 CTP 部分）未改。
- `TraderCTPOpt.{h,cpp}`：上述生命周期 + T1-CTPOpt-02 lease；T2-CTP-08：`orderInsert`/`orderInsertOpt` 缺委托 ID 时与 `makeEntrustID` 共用原子计数器分配唯一 OrderRef 并写回 ID；已有合法预分配 ID（严格十进制、前置/会话与当前一致）原样保留并把计数器抬到其之上；同会话同一引用不重复提交（含已发送失败）；空 tag 补 `ctpopt-auto:<eid>` 并写入 entrusts 关联；`m_iniHelper` 读写加互斥（下单线程与 SDK 回调线程并发）。
- `TraderCTPMini.{h,cpp}`：上述生命周期；T2-CTP-08 同 CTPOpt（自动 tag `ctpmini-auto:`，eid/oid 缓存加互斥）；T3-015：`GetTradingDay()` 必须恰为 8 位数字且为合法公历日期（含闰年），否则登录 fail-closed（`WS_LOGINFAILED`、`onLoginResult(false, 含原始串)`、`m_lDate=0`、`invalidTradingDayRaw()` 保留原始串）；`m_lDate` 为 0 时订单/成交回报不再以宿主当前日期合成日期而是丢弃并记错误日志；构造函数补初始化 `m_lDate/m_frontID/m_sessionID/m_orderRef`（原未初始化）。
- `TraderDumper/{Dumper.h,Dumper.cpp,TraderDumper.cpp}`（T3-012）：状态机 `IDLE/RUNNING/STOP_DEFERRED/RELEASED/DRAIN_UNCERTAIN`（`lifecycle_state()` + 日志可观察）；重复 `run` 拒绝（不再替换可 join 线程）；`release` 幂等；worker 线程或用户回调内 `release` 不自 join/自锁而延后；用户回调经准入闸门，`release` 完成后不再回调用户；`run(true)` 在 `release` 后及时返回；有界等待超时记 `DRAIN_UNCERTAIN`；导出单例改为进程内常驻（不在静态析构期销毁）；`release` 完成后用户显式重新 `register_callbacks` 才重新开放（使既有单例测试互不污染，也是显式的重启入口）。测试钩子调用点移到持锁之外（钩子可回调 Dumper）。

修改前后 sha256 见 `hashes/`，增量 diff 见 `patches/`（相对 `before/`，即本包开始时的工作区内容，不是相对 HEAD）。原文件混合 CRLF/LF 行尾，编辑后已按 `before/` 恢复未改行的原行尾，patch 不含整文件行尾噪声。

## 测试修改（旧断言改写，均保留 before/ 原文与理由）

| 文件 | 用例 | 改写理由（卡号） |
|---|---|---|
| `t_TraderCTP/t25_traderctp_gap_test.cpp` | `disconnect_without_worker_only_queues_release` | 旧断言要求无 worker 时只把 release 压队列不释放，正是 T2-CTP-03 原修复方向“无 worker 时不留旧释放任务”指出的缺陷；现为有界终止停止、释放一次、不留任务（卡9）。 |
| 同上 | `connect_with_stub_creator_wires_spi_fronts_and_worker_sends_query` 第二次 connect | 旧断言要求再次 RegisterFront/Init（泄漏旧 SDK 对象并让其继续回调）；卡9c 拒绝重复初始化、保留原实例。 |
| `t_TraderCTPOpt/r61_traderctpopt_api_test.cpp` | `connect_subscribes_topics_and_disconnect_releases_api` | 旧断言要求同实例 disconnect 后可重连新 API；迟到回调无法区分新旧 API（T2-CTP-03/卡9c），改为拒绝，QUICK 分支改用新实例。 |
| 同上 | `order_insert_and_cancel_fill_ctp_requests`、`option_exec_order_paths_fill_ctp_requests` | 旧断言把 BUG-R61-004（无 tag 单复用 OrderRef "0"、同一委托重复提交）当 oracle；与卡6d（缺 ID 原子分配唯一 OrderRef、不重复分配）冲突。 |
| `t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp` | `exec_order_query_uses_constructor_initialized_array_and_starts_fresh_batches` 末段 | 旧断言要求 release() 后的回调仍推送宿主；卡9 停止后回调闸门关闭。 |
| `t_TraderCTPMini/r61_traderctpmini_api_test.cpp` | `connect_subscribes_topics_and_disconnect_releases_api`、`order_flags_action_and_queries_are_driven_by_stub` | 同上两类（卡9/T2-CTP-03；卡6d，BUG-R61-005）。 |
| `t_TraderCTPMini/ctpmini_trader_callbacks_test.cpp` | `rsp_qry_orders_and_trades_forward_counts` | 只补设合法交易日：T3-015 fail-closed 后未经校验的交易日不再合成成交日期；该用例仅验证转发计数。 |

新增测试：`t_TraderCTP/i03s5_traderctp_lifecycle_test.cpp`、`t_TraderCTPOpt/i03s5_traderctpopt_lifecycle_test.cpp`、`t_TraderCTPMini/i03s5_traderctpmini_lifecycle_test.cpp`、`t_TraderDumper/i03s5_dumper_lifecycle_test.cpp`（均落在既有 lane 目录，自动纳入目标）。

## 结果（见 `runs/`）

最终运行（全部目标对象在最终源码上重编、按原链接命令重链；`runs/final-*`，`scripts/run_lane.sh`，300 s 硬限时）：

| lane 二进制 | 用例 | 通过 | 失败 | 退出码 | XML |
|---|---:|---:|---:|---:|---|
| CoverageTests_t_TraderCTP | 80 | 55 | 25（与改前基线失败集合逐名一致，`runs/*/failed-set.txt` diff 为空；新增 11 例全过） | 1 | `runs/final-TraderCTP/result.xml` |
| CoverageTests_t_TraderCTPOpt | 52 | 52 | 0 | 0 | `runs/final-TraderCTPOpt/result.xml` |
| CoverageTests_t_TraderCTPMini | 49 | 49 | 0 | 0 | `runs/final-TraderCTPMini/result.xml` |
| CoverageTests_t_TraderDumper | 33 | 33 | 0 | 0 | `runs/final-TraderDumper/result.xml` |

- DISABLED 红控（`--gtest_also_run_disabled_tests`，均预期失败，不计通过）：T2-CTP-07 Touched → 实际 99（`'c'`=WOS_Nottouched，`runs/final-disabled-ctp`）；新发现 N-1 CTPMini/CTPOpt NotTouched → 实际 98（`'b'`=WOS_Cancelling，`runs/final-disabled-mini`、`runs/final-disabled-opt`）。
- 产品模块：`TraderCTP/TraderCTPMini/TraderCTPOpt/TraderDumper` 产品对象在默认 `-Werror` 下重编 exit 0（`runs/product-objects-werror.log`），四个 dylib 重链 exit 0，导出仅 `createTrader/deleteTrader`（Dumper 为原 5 个 C 入口）。
- 模块钉住探针（`runs/pin-probe`，独立小 dylib 包含 `VendorTdLifecycle.hpp`）：未隔离时 `dlclose` 后 `dlopen(RTLD_NOLOAD)` 为 0（已卸载，对照）；调用 `quarantine()` 后为 1（仍驻留）。只证明 macOS dyld 行为。
- T2-CTP-08 CTP 既有身份 fixture：21/21（`runs/ctp-identity-green`）。

修改文件 sha256（前 12 位，改前 → 改后）：

| 文件 | 改前 | 改后 |
|---|---|---|
| src/TraderCTP/VendorTdLifecycle.hpp（新） | — | e92011919ceb |
| src/TraderCTP/TraderCTP.cpp | 1e90110f8129 | e898c7ea1b75 |
| src/TraderCTP/TraderCTP.h | 0b27e5caf873 | af0013de19af |
| src/TraderCTPOpt/TraderCTPOpt.cpp | 80acbb8bfd59 | 108e6fb70f5d |
| src/TraderCTPOpt/TraderCTPOpt.h | 122643ed5401 | 6d3dfd75f986 |
| src/TraderCTPMini/TraderCTPMini.cpp | 931e49af17a1 | e1767f761610 |
| src/TraderCTPMini/TraderCTPMini.h | 8d61fbf8757c | 002678d5b578 |
| src/TraderDumper/Dumper.cpp | 8cfc2d9235f0 | 983823e186f7 |
| src/TraderDumper/Dumper.h | 153a794c4264 | 9671bc2fe921 |
| src/TraderDumper/TraderDumper.cpp | b02dc5056d5e | 428eda4db01d |
| t_TraderCTP/t25_traderctp_gap_test.cpp | 198ed93ce407 | a23a50b2c78d |
| t_TraderCTP/i03s5_traderctp_lifecycle_test.cpp（新） | — | 2726dae70fc5 |
| t_TraderCTPOpt/r61_traderctpopt_api_test.cpp | d0cb0ac9ab23 | b0b72823e7a5 |
| t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp | 7a855bd467af | 374f33836836 |
| t_TraderCTPOpt/i03s5_traderctpopt_lifecycle_test.cpp（新） | — | 65f27b8abe66 |
| t_TraderCTPMini/r61_traderctpmini_api_test.cpp | 9300a0575222 | 0408fa8bb9c4 |
| t_TraderCTPMini/ctpmini_trader_callbacks_test.cpp | 49ec3b8c3e49 | f53a14f30e58 |
| t_TraderCTPMini/i03s5_traderctpmini_lifecycle_test.cpp（新） | — | 10cf286bfef7 |
| t_TraderDumper/i03s5_dumper_lifecycle_test.cpp（新） | — | a4f839c12dd8 |

完整哈希：`hashes/before-sha256.txt`、`hashes/after-sha256.txt`、`hashes/binaries-sha256.txt`（含红控二进制）。

## 红控（改前产品 + 新测试，`runs/*-red*`）

| lane | 用例 | 改前退出码 | 现象 |
|---|---|---|---|
| TraderCTP | NoFinalFrameDisconnectIsBoundedJoinsAndReleasesOnce | 98 | 无终帧时 disconnect 6 s 未返回（BUG-T25-005 原缺陷） |
| | SynchronousQueryRejectReleasesGateForNextQuery | 98 | 同步拒绝后 gate 不解除，后续持仓查询永不发出，disconnect 挂住 |
| | WorkerBlockedInsideSdkRequestIsUncertainThenRetrySucceeds | 98 | worker 卡在 SDK 请求时 disconnect 无界 |
| | ErrorFinalFrameThenLateErrorCallbackIsIgnored | 1 | 断开后迟到的 OnRspError 仍到达宿主 |
| | ReleaseFromCallbackIsDeferredToSafePoint | 1 | 回调内 release 就地 Release |
| | ReleaseNeverOverlapsInFlightOrderInsert | 1 | Release 与在途 ReqOrderInsert 重叠 |
| | RepeatedConnectKeepsApiAndConnectAfterReleaseIsRejected | 1 | 重复/停止后 connect 再次调用 creator |
| | ConnectFailuresAreObservableEvents | 1 | 无 WTE_Connect 失败事件 |
| | FactoryRemoverQuarantinesInstanceWithSdkAndDeletesOthers | 134 | deleteTrader 删除仍有 worker 的实例 → std::terminate |
| | FinalFrameThenDisconnectReleasesOnce | 1 | 仅新增状态观察哨兵失败；功能路径为正控 |
| TraderCTPOpt | ConcurrentCancelAndReleaseNeverOverlap | 1 | Release 与在途 ReqOrderAction 重叠（T1-CTPOpt-02） |
| | SynchronousCallbackReentryDefersReleaseWithoutDeadlock | 1 | 回调内 release 在 SDK 调用内就地 Release |
| | WorkerBlockedInsideQueryIsUncertainThenRetrySucceeds | 98 | disconnect 无界 |
| | RepeatedConnectKeepsApiAndConnectAfterReleaseIsRejected | 1 | 重复 connect 再建 API |
| | DestructorReturnsQueryArrayOwnership | 1 | 析构不归还数组（retainCount 2，T1-CTPOpt-01） |
| | MissingIdentityIsAllocatedUniqueAndPreallocatedIdKept | 1 | 两张无 tag 单 OrderRef 均为 "100"（T2-CTP-08） |
| | FactoryRemoverQuarantinesInstanceWithSdk | 134 | 删除有 worker 的实例 → terminate |
| TraderCTPMini | InvalidTradingDayFailsClosed | 1 | "2026013X" 被 atoi 为 2026013 并就绪 |
| | InvalidTradingDayMatrixIsRejected | 139 | GetTradingDay 返回 NULL 时 atoi(NULL) SIGSEGV |
| | ConnectFailuresAreObservableEvents | 1 | 无失败事件（T3-006） |
| | NoFinalFrameDisconnectIsBoundedAndInstanceIsSingleUse | 98 | disconnect 无界 |
| | ReleaseNeverOverlapsInFlightOrderInsert / ReleaseFromCallbackIsDeferred / MissingIdentity… | 1 | 重叠 / 就地 Release / OrderRef 重复 |
| | FactoryRemoverQuarantinesInstanceWithSdk | 134 | terminate |
| | ValidTradingDayLoginAndNightDatesStayCorrect | 0 | 正控（改前改后一致） |
| TraderDumper | RepeatedRunIsRejectedAndRepeatedReleaseIsIdempotent | 134 | 重复 run 替换可 join 线程 → terminate |
| | ReleaseOnWorkerThreadIsDeferredThenCompleted | 134 | worker 内 release 自锁/自 join → terminate |
| | ReleaseInsideUserCallbackCutsOffLaterCallbacks | 1 | release 后用户回调仍被调用 |
| | RunOnceReturnsPromptlyAfterRelease | 98 | run(true) 忽略 release |

改前旧测试在红控二进制上：TraderCTP 69 例 25 失败（与改后同一集合，见下）；CTPOpt 45/45；CTPMini 34/40（6 例为 T3-006 fake 模块按 `getBinDir()` 查找、红控二进制不在原构建目录所致，非产品行为）；Dumper 22 通过、其余因 mocker 模块不在红控目录被 GTEST_SKIP。

## 既有失败（非本包引入，未改断言）

TraderCTP lane 25 例旧用例在改前（`runs/ctp-red-lane`）与改后失败集合完全相同：T25 9 例、T2 16 例，均在登录/确认步骤失败——旧夹具登录回报的 BrokerID（"8888"）与配置不符、请求号不按 11-03 已实施的 CTP 身份合同匹配，确认查询未经 worker 执行即注入回报等。11-03 身份 fixture README 已声明“其他尚未迁移的旧测试可能仍依赖空 tag 共用 OrderRef 或仅注入旧缓存”。其中包括 T2-CTP-07 既有证据用例 `rtn_order_untouched_and_known_states_keep_wts_meaning` 与 `queried_order_…`（当前树已不能作为其通过证据；本包另以不依赖登录的新用例 `I03S5TraderCTPOrderState.KnownCtpStatusesKeepWtsMeaning` 覆盖 NotTouched/Unknown/队列/撤销映射）。需另行按身份合同迁移这些旧夹具（建议新编号）。

T2-CTP-08 CTP 部分的既有 fixture（`src/TestUnits/fixtures/i03_ctp_order_identity_20261103`）以 `scripts/build_ctp_identity_fixture.sh` 对当前源码重编，按其 run.sh 的过滤器 21/21 通过（`runs/ctp-identity-green`），证明生命周期改动未破坏既有身份分配。

## 未验证与限制

- 真实 CTP/CTPMini/CTPOpt SDK 的 Release 是否 join SDK 线程、Release 后是否仍回调、`GetTradingDay` 的空/无效值合同均未取得版本匹配证据（X）；本地 fake 只证明插件自身的准入/排空/延后逻辑。
- Windows/MSVC（含 `GetModuleHandleEx` 钉住分支）、Linux（`dlopen` 钉住与非 Apple 动态加载分支）未编译未运行。
- 未运行 ASan/UBSan/TSan（本轮时间限制）；隔离表中的对象为有意常驻，LSan 视为可达。
- 宿主 `WtCore/WtUftCore/TraderDumper::TraderAdapter` 在 remover 后仍 `free_library`；本插件以模块钉住保证隔离实例代码不被卸载，但宿主侧未感知“已隔离”（宿主文件非本包所有）。
- TraderDumper：`g_adapterMgr` 为全局对象，进程退出静态析构时仍会销毁其中的适配器；若供应商线程此时仍回调，存在退出期风险（未修，建议另立编号）。`release` 不释放交易通道（沿用原行为），通道与供应商对象保活到进程退出。
- 查询无终帧时查询 gate 仍永久占用（只影响后续查询，不再影响停止）；是否引入查询超时属 SDK 流控合同，未决定。

## 卡15 Z 类复核

- **T2-CTP-03**（TraderCTP disconnect 残留旧任务 / reconnect stopped 标记）：复核确认为明确缺陷（无 worker 时 disconnect 只入队不释放；connect 不复位停止位、可再建 SDK 对象而泄漏旧对象）。已按卡9c 修复为单次实例：disconnect 与 release 同为有界终止停止、不留任务、只 Release 当前唯一 API 一次；重复/停止后 connect 被拒绝并可观察。本地红绿见 TraderCTP 表。建议：不以“忽略”处置，按卡9c 记本地验证通过（真实 SDK 回调排空为 X）；若用户要求“可重连状态机”而非单次对象，需另行裁定并实现代际 API 区分。
- **BUG-T6-002**（属 CTPLoader，T6 lane）：只读复核确认缺陷仍在——`run(cfg, bAsync=true)` 立即返回后，SPI 回调线程在合约/费率终包或错误时直接 `exit()`（`TraderSpi.cpp` 共 11 处），异步宿主（wtpy 等）会被插件终止进程；无完成/错误通知接口，API 不 Release，迟到回调归属未定义。错误路径现以 EXIT_FAILURE 退出（不再以 0 结束），同步 CLI（LoaderRunner 以 bAsync=false 调用并依赖退出码）语义未破坏。修复需要新的对外合同，**不得以忽略关闭**；保持阻塞并见决策卡草稿 D-T6-002。本包未修改 CTPLoader（`TraderSpi.cpp/.h` 当前有他人未提交改动）。
- **BUG-T25-005**（卡15 例外）：产品停止缺陷已修复（见上），不以忽略关闭。

## 决策卡草稿（需用户裁定）

### D-T2-CTP-07：CTP `Touched` 的 WTS 表示

- 事实：CTP 6.6.9 / CTPMini 1.5.8 / CTPOpt 3.5.8 头文件 `THOST_FTDC_OST_Unknown='a'`、`NotTouched='b'`、`Touched='c'`；WTS `WOS_Submitting='a'`、`WOS_Cancelling='b'`、`WOS_Nottouched='c'`。TraderCTP 已显式映射 NotTouched→WOS_Nottouched、Unknown→WOS_Submitting，Touched 仍按数值强转为 WOS_Nottouched（已触发被报告为“未触发”）。下游：`WTSOrderInfo::isAlive()` 除 AllTraded/Canceled 外均视为存活（两种映射都存活，不影响撤单/在途量）；WtCore/WtUftCore `stateToName` 仅日志；wtpy 等外部消费者按字符值解释。
- 选项：A 新增 `WOS_Touched`（公共枚举追加值，如 `'d'`），三家适配器显式映射；需同步宿主 stateToName、Python 端常量与文档，旧消费者遇到未知值的行为需评估。B 映射到既有 `WOS_Submitting`（已触发、正向交易所提交，后续以真实队列状态更新）；无 ABI 变化，但丢失“条件单已触发”的独立语义。C 维持现状（不推荐，语义错误）。
- 建议：B（短期、无 ABI 变化），如用户需要区分条件单触发事件再走 A 的版本化扩展。
- 红控：`I03S5TraderCTPOrderState.DISABLED_T2CTP07_TouchedIsNotReportedAsUntouched_PendingDecision`（`--gtest_also_run_disabled_tests` 运行当前失败，见结果节）。影响条目：T2-CTP-07；关联新发现 N-1。

### D-T6-002：CTPLoader 异步完成/错误通知与进程退出

- 要决定：异步 `run()` 结束时插件如何通知宿主、谁负责退出进程、API 何时 Release。
- 选项：A 同步模式保留 `exit(code)`（CLI 语义），异步模式改为记录终态 + 新增版本化导出（如 `int ctploader_wait(uint32_t timeoutMs, int* result)` / `int ctploader_status()`），终态后在非回调线程 Release，绝不 `exit`；B 所有模式都改为 `run()` 返回结果码（同步内部等待完成而非 `Join()`，LoaderRunner 把返回码转为进程退出码），插件内不再 `exit`；C 维持现状（不推荐）。
- 建议：B（单一合同、宿主决定退出；LoaderRunner 已按返回值 `return runner(...)` 退出），兼容期保留旧 `run` 符号语义说明。真实 CTP 末包/错误回调时序仍为 X。

## 新发现（编号由主线程分配）

- N-1：`TraderCTPMini::wrapOrderState` 与 `TraderCTPOpt` 的 `wrapOrderState` 对 Unknown 以外一律数值强转：CTP NotTouched('b') 被报告为 `WOS_Cancelling`（条件单被误报为“在撤”），Touched('c') 被报告为 `WOS_Nottouched`。TraderCTP 已修的 NotTouched 子项未推广到这两家。复现：`I03S5TraderCTPMini.DISABLED_NewFinding_NotTouchedIsReportedAsCancelling`、`I03S5TraderCTPOptLifecycle.DISABLED_NewFinding_NotTouchedIsReportedAsCancelling`（`--gtest_also_run_disabled_tests`）。建议：NotTouched 按 TraderCTP 同样显式映射（无需新裁定），Touched 随 D-T2-CTP-07。未修（不属本包条目）。
- N-2：TraderCTP lane 25 例旧用例在当前树基线即失败（登录夹具与 11-03 身份合同不符），含 T2-CTP-07 原证据用例；需迁移夹具。
- N-3：TraderDumper `g_adapterMgr` 全局对象在进程退出静态析构期销毁适配器，供应商线程仍可能回调（退出期 UAF 风险）；测试以 `extern TraderAdapterMgr g_adapterMgr` 引用对象本体，改为常驻需同时调整测试声明。
- N-4（宿主侧，非本包文件）：`WtCore/WtUftCore/TraderDumper` 的 `TraderAdapter` 在 `deleteTrader` 之后无条件 `free_library` 且置空指针，不感知插件“已隔离”（指针保留）信号；本包以插件自钉模块兜底，建议宿主侧按 ParserAdapter 先例在指针未置空时保留模块与句柄。

## 条目建议状态（供主线程整合，不代表已改跟踪表）

| 条目 | 建议 | 依据 / 剩余（解除条件） |
|---|---|---|
| BUG-T25-005 | 本地验证通过（卡15 例外，不以忽略关闭） | 无终帧/有终帧/错误终帧三种 + worker 卡 SDK、回调内停止、迟到回调不重复释放；改前 98/1、改后全过。真实 SDK Release/回调排空为 X |
| T2-CTP-02 | 本地验证通过（macOS 部分） | connect 失败统一 WTE_Connect -1 可观察；隔离实例钉住模块的安全卸载；非 Apple 动态加载缺库/缺符号分支与 Windows 未编译运行（X） |
| T2-CTP-03（Z） | 本地验证通过（按卡9c 单次实例），不建议忽略 | 明确缺陷已修；若需可重连状态机须另裁定 |
| T2-CTP-07（D） | 阻塞 | 待 D-T2-CTP-07 裁定；DISABLED 红控已补 |
| T2-CTP-08 | 待验证 → 本地验证通过（CTP/CTPOpt/CTPMini 进程内） | 三家均原子分配、保留合法预分配、不重复提交；跨进程/跨重启/多实例唯一性不能由单实例计数器与登录 MaxOrderRef 证明（限制如实保留），真实柜台为 X |
| T1-CTPOpt-01 | 本地验证通过 | 析构在闸门排空后归还查询数组（retainCount 红 2→绿 1）；真实 SDK 回调排空为 X |
| T1-CTPOpt-02 | 本地验证通过 | 并发撤单与 teardown 不重叠、同步回调重入不死锁、回调内释放延后；改前重叠/就地 Release |
| T3-006 | 本地验证通过（本机 fake） | void connect 失败以事件可观察；真实 SDK、Windows 为 X |
| T3-012 | 本地验证通过（本机） | 重复 run/release、worker/回调内 release、run(true) 交错均有红绿；交易通道退出期保活（N-3）与真实 provider 未验 |
| T3-015 | 待验证（本机 fail-closed 已实现） | 无效/空/NULL 交易日登录不就绪、不推送合成日期；真实 CTPMini `GetTradingDay` 合同为 X，人工恢复政策仍需确认 |
| BUG-T6-002（Z） | 阻塞（明确缺陷，不建议忽略） | 待 D-T6-002 裁定后在 CTPLoader 实施 |

## 第二轮（主线程追加任务，2026-11-04 晚）

规则同前：只改本包拥有文件；`before/` 为本包开始时快照，`patches/` 已按最终源码重新生成；第一轮“改后”哈希即本轮“改前”（`hashes/after-sha256.txt`），本轮改后见 `hashes/after-round2-sha256.txt`。共享 `CMakeLists.txt`（`73f6c5a950a4`）与 CTPLoader（`85bc0dbb6cfb`）仍未改。

### 1) Sanitizer（Homebrew LLVM 22，`scripts/build_san.sh` / 运行见 `runs/san-*`）

做法参照 `src/TestUnits/fixtures/i03_*`：产品 TU 与新增测试 TU 用 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++` 插桩（`-include stdlib.h` 绕过 Homebrew fmt 在 LLVM 22 libc++ 下的声明问题；产品 TU `-Werror`）；WTSTools/WTSUtils 来自本包非覆盖率 Debug 构建 `/tmp/wt-i03-s5-traders-ctp-dbg`（未插桩），gtest/boost/spdlog/fmt 为 Homebrew 预编（未插桩）。每个用例一个进程，独立 cwd，`perl alarm 180`；ASan 运行 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、UBSan `halt_on_error=1`，TSan `halt_on_error=0`。运行时正控：同一编译器 TSan 数据竞争样例 1 份报告、ASan UAF 样例 1 份报告（`runs/san-positive-control-*.log`）；`otool -L` 确认链接 `libclang_rt.{asan,tsan}_osx_dynamic`。

| 用例集 | 模式 | 进程 | 非零退出 | TSan | ASan | LSan | UBSan |
|---|---|---:|---:|---:|---:|---:|---:|
| `I03S5TraderCTP*`（12：生命周期 10 + 订单状态 2） | ASan+UBSan+LSan | 12 | 0 | — | 0 | 0 | 0 |
| 同上 | TSan | 12 | 0 | 0 | — | — | — |
| T2-CTP-08 身份 `T25TraderCTPTest.I03Identity*`（15） | ASan+UBSan+LSan | 15 | 0 | — | 0 | 0 | 0 |
| 同上 | TSan | 15 | 0 | 0 | — | — | — |
| `I03S5TraderCTPOptLifecycle.*`（8） | ASan+UBSan+LSan | 8 | 0 | — | 0 | 0 | 0 |
| 同上 | TSan | 8 | 0 | 0 | — | — | — |
| `I03S5TraderCTPMini.*`（10） | ASan+UBSan+LSan | 10 | 0 | — | 0 | 0 | 0 |
| 同上 | TSan | 10 | 0 | 0 | — | — | — |
| `I03S5Dumper*`（6：生命周期 4 + 退出保活 2） | ASan+UBSan+LSan | 6 | 0 | — | 0 | 0 | 0 |
| 同上 | TSan | 6 | 0 | 0 | — | — | — |

合计 102 个进程，0 份 ASan/UBSan/TSan 报告。身份用例首次 ASan 运行 15 进程用例体全部 OK，但 LSan 15 份报告：泄漏栈全部根在 `T25TraderCTPTest::SetUp`（夹具自建 `WTSVariant`/合约/品种从未释放），无任何 `TraderCTP::` 帧（`runs/san-asan-ctpid-first-lsan/NOTE.txt`）；在 T25 夹具 `TearDown` 中归还这些对象后重编重跑 0 报告。DISABLED 用例未纳入；未插桩依赖库中的问题不在检测范围内。

### 2) N-2：TraderCTP lane 25 例既有失败逐例归因与迁移

基线（改前产品 + 改前测试，`runs/ctp-red-lane`）与第一轮改后失败集合完全相同，均为测试前提问题。没有发现产品缺陷。按已实施的 11-03 CTP 身份合同与卡9c/6d 迁移夹具/前提，断言不变；只有 2 例因 fail-closed 合同改写了期望（下表），原文见 `before/` 与 `patches/`。

| 归因 | 用例 | 处理 |
|---|---|---|
| A 登录回报 BrokerID "8888" 与配置 "9999" 不符，或缺 Broker/User/TradingDay（身份合同要求一致） | T2 `bringToReady` 夹具（其后 14 例：make_entrust_id…、order_insert_maps…、order_insert_cffex…、order_action_rejects…、rtn_order_conversion…、rtn_order_untouched…、queried_order_untouched…、trade_conversion…、rsp_order_insert_and_action…、rsp_account_position…、rsp_qry_order_and_trade…、login_rsp_carries…、settlement_unconfirmed…、queries_enqueue…）；T25 `login_rsp_with_null_rspinfo_is_taken_as_success` | 夹具补正确的 Broker/User/TradingDay |
| B 回报请求号须匹配本次登录代次已发出的请求（夹具用任意号或未先执行排队的确认查询） | T2 `bringToReady`、`authenticate_failure_marks_login_failed_and_reports`、`settlement_unconfirmed_goes_through_confirm`；T25 `settlement_confirm_query_with_null_field_or_error_keeps_or_skips_state`、`settlement_confirm_rsp_only_acts_in_confirm_qryed_state`（改为真实确认路径进入 WS_CONFIRM_QRYED）、`front_and_heartbeat_paths_are_quiet_when_sink_is_unregistered`（先发起登录） | 用 stub 记录的 `last_req_id_`；先 `drainQueries` |
| C 回报归属须带 Broker/Investor/TradingDay | T2 `order_insert_maps_types_and_ref_for_shfe`、`trade_conversion_amount_and_usertag_chain`；T25 `entrust_rsp_maps_ioc_cv_to_fok_and_usertag_from_cache`、`rsp_order_action_error_carries_action_fields_and_bare_message` | 回报字段补齐（真实 CTP 回报本即携带） |
| D 同一身份不得重复提交（卡6d） | T2 `order_insert_cffex_anyprice_and_close_offsets` | 每次下单前重新 `makeEntrustID` |
| E 确认查询在代次仍为 WS_LOGINED 时才发出；connect 只在尚无 API 时创建（卡9c） | T25 `query_confirm_sync_failure_clears_gate_and_worker_releases` | 先摘 seam 指针再 connect；确认查询失败后再强置就绪并排账户查询 |
| F 迁移后夹具已执行确认查询 | T2 `rsp_account_position_and_settlement` | 队列前提改为 0 |
| G 关联缓存初始化失败时登录 fail-closed（身份合同），旧断言“告警后继续” | T25 `login_warns_and_continues_when_cache_dir_is_not_writable`、`order_insert_rejects_when_user_tag_cache_init_failed` | **改写期望**：登录失败、可观察、不排查询、不分配 ID、下单不达 SDK |
| H 无 API 时 login 不再保留凭据（身份实现顺带改变的旧行为，无合同依据） | T2 `login_logout_and_queries_rejected_without_api` | **产品恢复旧行为**：`TraderCTP::login` 在无 API 时仍保存 user/pass/productInfo（与 TraderCTPMini 现行“Preserve the existing API behavior”一致），不开启登录代次；登录进行中或有在途下单时不改写 |

结果：TraderCTP lane **80/80**（`runs/r2-TraderCTP/result.xml`，exit 0）。T2-CTP-08 身份 fixture 全量（含其内嵌的全部 T25 用例）**58/58**（`runs/ctp-identity-green3`；第一轮仅过滤器 21/21，全量基线有同源失败）。T2-CTP-07 原证据用例 `rtn_order_untouched_and_known_states_keep_wts_meaning`、`queried_order_untouched_…` 恢复通过。

### 3) N-1：CTPMini/CTPOpt NotTouched 映射（已修）

`TraderCTPMini::wrapOrderState` 与 `TraderCTPOpt` 的 `wrapOrderState` 改为显式 switch：Unknown→WOS_Submitting、**NotTouched→WOS_Nottouched**，其余（含 Touched，待 D-T2-CTP-07）保持原值，与 TraderCTP 已修子项一致。原 DISABLED 红控转为常规用例 `I03S5TraderCTPMini.NotTouchedIsReportedAsNottouched`、`I03S5TraderCTPOptLifecycle.NotTouchedIsReportedAsNottouched`。红控（改前产品，`runs/r2-red`）：均 exit 1，实际 98（`'b'`=WOS_Cancelling），期望 99；绿：两 lane 全过。

### 4) N-3：Dumper 全局 g_adapterMgr 退出期析构（按卡9a 已修）

可以在不依赖 SDK 排空的前提下保留到进程退出：`TraderAdapterMgr` 的全部状态（适配器表、互斥、存活计数）移入永不释放的堆对象 `State`（登记在永不释放的表中，LSan 可达），管理器本身只持 `State* const`、删除拷贝，`static_assert` 断言其**平凡可析构**——全局 `g_adapterMgr` 因而没有退出期析构函数，存储期持续到程序结束；供应商线程在静态清理期回调 `decAlive()`、Dumper worker 调 `refresh()` 时状态仍有效。对外接口与 `extern TraderAdapterMgr g_adapterMgr` 声明不变；`_live_cnt` 由未初始化改为 0。每个局部管理器（仅测试）保留一份小状态到进程退出。
红控（改前，`runs/r2-red`）：`AdapterManagerHasNoExitTimeDestructor` exit 1（`is_trivially_destructible` 为 false）；`ProviderCallbackAfterExitTimeDestructionStillSeesManagerState`（子进程中先执行退出期会执行的析构，再以适配器回调 `decAlive` 与查表访问）子进程 SIGBUS（signal 10），并出现 `mutex lock failed: Invalid argument`。改后两例通过，Dumper lane **35/35**（`runs/r2-TraderDumper-b`），ASan/TSan 各 6 进程 0 报告。局限：适配器及其交易 API 对象仍不在退出期释放（有意保活）；真实进程退出时供应商线程行为未验（X）。

### 本轮 lane 结果

| lane | 用例 | 通过 | 退出码 |
|---|---:|---:|---:|
| CoverageTests_t_TraderCTP | 80 | 80 | 0 |
| CoverageTests_t_TraderCTPOpt | 53 | 53 | 0 |
| CoverageTests_t_TraderCTPMini | 50 | 50 | 0 |
| CoverageTests_t_TraderDumper | 35 | 35 | 0 |

本轮修改文件（sha256 前 12 位，本轮前 → 本轮后）：`TraderCTP/TraderCTP.cpp` e898c7ea1b75 → b5b2404c3b9c；`TraderCTPOpt/TraderCTPOpt.cpp` 108e6fb70f5d → 586eaf2a4915；`TraderCTPMini/TraderCTPMini.cpp` e1767f761610 → 37f94224df50；`TraderDumper/TraderAdapter.h` 559d23ce3177 → 9c2e207c4359；`TraderDumper/TraderAdapter.cpp` cdcf5b45e604 → 295723ae4862；`t_TraderCTP/t25_traderctp_gap_test.cpp` a23a50b2c78d → 0fb0552520ff；`t_TraderCTP/t2_traderctp_conversion_test.cpp` f58d7c5526b0 → 545af64823f7；`t_TraderCTPOpt/i03s5_traderctpopt_lifecycle_test.cpp` 65f27b8abe66 → 5e29943d8fda；`t_TraderCTPMini/i03s5_traderctpmini_lifecycle_test.cpp` 10cf286bfef7 → f92fdf9eb06a；`t_TraderDumper/i03s5_dumper_lifecycle_test.cpp` a4f839c12dd8 → 7b45b77705bd。

第一轮“新发现”中 N-1、N-2、N-3 已在本轮处理；N-4（宿主 `TraderAdapter` 卸载模块不感知隔离）仍属宿主文件，未改。
