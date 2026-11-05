# 2026-11-04 s5 子任务 wp1-ledger：交易身份、账户门禁与订单账本（WP1）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动，约 12 个子任务并行修改共享源码树）。本目录只记录本子任务的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实 provider/柜台、硬断电或最终整合树。未修改任何跟踪文件。

- 起点：`hashes/HEAD.txt`、`hashes/start-time.txt`（13:06）、`hashes/start-sha256.txt`；改前副本在 `before/`（保留相对路径）。
- 终点：`hashes/final-sha256.txt`；相对改前副本的差异在 `patches/`。
- 构建目录：非覆盖率 Debug 库 `/tmp/wt-i03-s5-wp1-ledger-dbg`（WtCore 等，默认 -Werror）、覆盖率 `/tmp/wt-i03-s5-wp1-ledger-cov`（默认 -Werror）；运行目录 `/tmp/wt-i03-s5-wp1-ledger-run`。
- 会话中途两次额度中断（约 13:40–17:40、18:20–18:25），恢复后核对改动完整且可编译再继续。
- 过程失误（已纠正）：用 Python 文本模式改写 `TraderAdapter.cpp/.h`、`HftStraBaseCtx.cpp` 时把原 CRLF 行尾改成了 LF；18:32 已按改前副本逐行恢复原行尾（`TraderAdapter.h` 原为混合行尾，未改行保持原样，新增行沿用相邻原行风格），恢复后差异见 `patches/`。恢复前后内容相同，只影响行尾；18:32 前的运行所用源码只差行尾。


## 1. 切片5复核（卡5 5a–5d=A、卡6 6a–6c=A/6d 自动分配、2026-11-04 补充裁定）

逐条核对（静态 + 夹具）。结论：切片5方向与裁定一致；发现并修复 7 处缺口，另有 3 处属他包/外部，见第 7 节。

| 合同 | 实现 | 复核结论 |
|---|---|---|
| 5a 发送前 WAL 登记、重启恢复、无法恢复暂停不重发 | 切片1–4（`prepareOrderIntent`、`reconcileIdentityRecovery`）；切片5把 owner 写入 journal owner（`adapter:<id>/<owner>`），恢复时还原 owner 与 verified | 通过（`CrashDuringSend…`/`CrashAfterSend…` SIGKILL 子进程两例） |
| 5b 旧 ABI 仅非共享 owner、禁止自动超时撤单 | `ExeUnitWrapper` 只对声明 `CAP_HOST_ORDER_IDENTITY` 的插件、且宿主核验身份时注入保留键=1，其余一律覆写为 0；执行器按 `hostOrderIdentitySupported()` 决定共享/独占 claim | 通过；新增端到端用例（第 4 节）。限制：第三方旧插件若不读保留键仍可自行调用 `cancel(localid)`，宿主无法区分超时撤单与其他撤单，只能经 owner+账本核验（决策卡草稿 B） |
| 5c 同账户同合约多执行器默认允许，须先有 owner 路由与账户门禁 | `AccountTradingGate::claimContract`（共享仅当路由开启且单元具身份能力）；`dispatchOrderEvent` 按 owner 投递；`cancelForOwner`/`getOrdersForOwner`/`getUndoneQtyForOwner` | 修复 4 处缺口（下表 F1–F4） |
| 5d 快照 epoch/version，旧 epoch 忽略，缺席 tombstone | 订单快照 epoch；切片5增加持仓 epoch | 修复空快照与持仓基线歧义（F5、F6），成交去重（F7） |
| 6a 账户级门禁 / 6b 显式审计恢复 | `AccountTradingGate` 共享门禁；`resume()` 先写 0600+fsync 审计、各参与方 acknowledge 后放开；`TraderAdapterMgr::resumeAccountTrading`、WtRtRunner/WtPorter `resume_account_trading`/`get_account_pause_generation` | 通过（`ManagerResumeCommandIsAuditedAndGenerationChecked` 等）；Porter 导出为薄转发，未单独运行 Porter 目标；WtRunner 无恢复入口（属 runners 包） |
| 补充裁定：暂停期允许对身份可核验的存活订单撤单（含自动超时），不可核验撤单与一切新下单拒绝 | `cancelAdmissibleLocked`（paused 时仅 verified && Live）、`cancelVerified` 同规则 | 通过（`PausedAccountCancelsOnlyVerifiedIdentities`、owner 路由用例的暂停段；TWAP 能力单元超时撤单经 `cancelForOwner` 同规则） |
| 锁顺序 | gate mutex → participant（`_unresolved_report_mutex`、`_identity_mutex`、journal）；`_ledger_mutex` 持有期间只调用无锁 `key()`；claim 在不持适配器锁时调用 | 未发现逆序；TSan 夹具 0 报告 |

修复（`src/WtCore/TraderAdapter.*`）：

- F1 owner/observer 作用域泄漏进 sink 回调：执行器 A 的 `buyAsOwner` 期间，provider 同步回报触发非 owner 观察者（如 HFT 上下文）回调里裸下单，该单被记为 A 的订单（计入 A 的未完成量、受 A 的 claim）。现 `invokeSink/invokeOwnerSink` 在回调期间清空本线程下单上下文（`EntryContextIsolation`）。红控 `OwnerScopeDoesNotLeakIntoSinkCallbacks`。
- F2 `removeSink` 不清 owner 声明：①同地址新 sink 静默继承旧 owner；②重建同名执行器时 `declareOwnerSink` 命中旧 token 直接返回，若在 addSink 之后声明则永远不注册，作为普通 sink 收到其他 owner 的回报。现 `removeSink` 同步删除该 sink 的声明与对应代次 token。红控 `RemovedOwnerSinkDeclarationEndsWithTheSink`。
- F3 路由开启时无 owner 事件（直连适配器订单、外部或不可解析订单）仍广播给 owner sink，可被 Diff 执行器计入差量。现只送非 owner 观察者。红控 `UnownedEventsNeverReachOwnerSinksUnderRouting`。
- F4 路由开启时 owner 无法核验的本 adapter tag 成交（账本无项或非本次启动核验的项）：先查身份 WAL，查不到则保留快照、暂停账户（L01-014 策略），不投递 owner sink。红控 `UnresolvableOwnerTradeIsQuarantinedUnderRouting`。
- F5（NEW-154）`onRspOrders` 空数组：CTP `OnRspQryOrder`（`TraderCTP.cpp:1399-1428`）与 XTP（`TraderXTP.cpp:535-560`）复用并清空同一数组，查询出错时也交付空数组，接口无法区分“无订单/查询失败”。现空快照按失败处理（不 tombstone、不释放预占，日志可见）；缺席只在非空快照中采信；非空但部分失败仍无法识别（决策卡草稿 C）。红控 `EmptyOrderSnapshotIsAmbiguousAndKeepsLiveOrders`；旧用例 `AbsentOrderBecomesRevivableTombstone` 改为非空快照（含一笔外部单）保留原意，原文件见 `before/`。
- F6（NEW-154）持仓是水平量、成交是增量：查询后到达的实时成交使该合约快照行被忽略时，若该合约此前从未有快照基线，适配器只持有增量、无法判断快照是否已含该成交（启动期可少计昨仓）。现忽略行且无基线时暂停账户。红控 `PositionSnapshotWithoutBaselineAndNewerTradePausesAccount`（含有基线时保持 baseline+增量的正控）。
- F7（NEW-154）重复实时成交（同成交号+委托引用+方向）会重复计入持仓、统计并再次投递 sink；成交查询行与实时推送重复计统计。现实时推送去重（不影响自成交两侧：同成交号不同引用仍各自处理），统计跨推送/查询只计一次，交易日变更清空。红控 `DuplicateTradePushIsAppliedOnceButSelfMatchSidesAreNot`。
- F8 `cancelByCode` 中 `doCancel` 抛异常时 claim 永不释放，该单此后无法再撤。改为 RAII。红控 `ThrowingCancelReleasesItsClaim`。
- F9（L01-014 调用者审计）`buy/sell/doEntrust` 遇未知/不可解析标准码（`getContract` 为空或缺商品/时段）原为空指针解引用；现 fail-closed 拒绝且不预占、不发送；`getContract/getCommodify` 在 `_bd_mgr` 为空（release 后）时返回空。

## 2. 当前失败用例归因

- `R170TraderRisk.OptionEntrustErrorUsesOptionStdCode`（:206 期望 4 实际 5）：旧 oracle 用不带本地 tag 的拒单去扣预置的 `_undone_qty=5`。T2-CTP-08（卡6d，2026-11-03）起无本地 tag 的拒单不消耗本适配器未完成量，R3-008 逐单账本（卡5/卡7）起未完成量只由逐单账本释放；与已裁定合同冲突，非产品回归（WP1 基线、11-03 限频改前基线同样失败）。改写为本适配器真实期权订单（账本预占 1）被拒：按期权标准码通知、5→4、重复拒单不再扣、无 tag 外部拒单不扣。
- `R170TraderRisk.SecondCanceledShortTodayPushRestoresAvail`（:165/:166）：①`getPosition(...,true,2)` 自 2021（8cd46907）起按净头寸约定空头返回负值，旧断言 +2 符号错误，从迭代01创建起即与实现不符；改为直接断言 `s_newavail==2` 并断言 `getPosition==-2`；②未完成量按 R3-008 逐单账本：首次存活回报预占 3、撤单终态恰好释放，结果 0（旧期望 2 为“撤单时加回”的聚合启发式）。非产品回归。
- `UnresolvedReportJournal.InterruptedPrimaryAppendAfterProcessExitExportsForensicPrefix`（:561 子进程 exit 10＝子进程 `open` 失败）：13 时本机旧覆盖率二进制顺序 100 次 + 6 并发×60 次共 460 次 0 失败；为归因在子进程 `open` 失败时把状态与错误写到日志目录外的诊断文件、由父进程在断言消息中输出（不改断言）。新二进制随后在隔离运行与顺序重跑中稳定复现（顺序 3/3），并带出同类 `AmbiguousAckCommit…`/`AmbiguousAckPrimaryFlush…`（fork 子进程内 open/ack 失败），诊断为 `status=9 UnsafePath`：“journal guard must be owned by this user, mode 0600, single-link, and ACL-free”或“path identity or private permissions changed during open”。独立探针（`runs/urj-flake/probe.cpp`）确认根因：同一文件无 ACL，父进程 `acl_get_fd_np` 返回 NULL/ENOENT，**fork 子进程内首次调用偶发返回 NULL/EINVAL**（是否出现随二进制而异：同源码两次编译的探针一个 1/4 失败、一个 0/4），`noExtendedAcl` 只接受 ENOENT/ENODATA/ENOATTR，于是把文件判为“带 ACL”而 fail-closed。产品修复（`UnresolvedReportJournal.cpp`，属 L01-014 的 WAL）：仅对 EINVAL/EINTR 有界重试 3 次，其余错误或持续错误仍 fail-closed；真实 ACL 总是返回对象，不受影响。红绿：DYLD 插桩让每个进程首次 `acl_get_fd_np` 返回 EINVAL，改前探针 `createNew failed`，改后 3 轮 fork 打开全部成功（`runs/urj-flake/einval-injection-red-green.log`）；新增负控 `GenuineExtendedAclOnJournalFileIsRejected`（`chmod +a` 真实 ACL 仍 `UnsafePath`）。故该例原失败为产品对 macOS 偶发 errno 过严导致的不稳定，非 WP1 回归；需新编号跟踪（第 7 节）。

## 3. WAL 独立安全复审 NO-GO 项（本机可证明部分）

| NO-GO 项 | 对应用例（本轮运行） | 结论 |
|---|---|---|
| torn tail 后有效前缀只读导出 | `TruncatedPrimaryExportsCompletePrefixAndFirstBadOffset`、`CorruptPrimaryChecksumPreservesBytesAndForensicPrefix`、`TruncatedGuardPreservesItsGoodPrefix`、`InterruptedPrimaryAppendAfterProcessExitExportsForensicPrefix`（子进程部分写后退出，重开不改字节，导出前缀与坏偏移） | 本机证据见第 5 节 |
| journal 权限与首次建档 | `UnsafeDirectoryPermissionsAreRejected`、`ExistingWalGuardAndLockPermissionsAreValidated`、`MissingOpenDoesNotBootstrapAndFreshRecordSurvivesRestart`、`BootstrapCannotBeImplicitlyRepeated`、`BootstrapRejectsOrphanArtifactsWithoutCreatingOthers`、`BootstrapProbeErrorDoesNotCreateArtifacts`、`MissingWalWithOtherJournalArtifactsRequiresRecovery` | 同上 |
| 路径检查到写入 TOCTOU | `ReplacedContainingDirectoryIsDetectedBeforeMutation`、`GuardAppearingAfterPreflightIsNeverOverwritten`、`MarkerAppearingAfterBootstrapPreflightHasImmediateEvidence`、新拆出的 `VisiblePathAndLockReplacementFailClosed`、新增 `SystemLinkInDirectoryPathIsCanonicalizedToOneJournal`（目录链接按合同规范化、别名不能成为第二写者）与 `GenuineExtendedAclOnJournalFileIsRejected` | 同上 |
| ACK flush 失败重启语义 | `AmbiguousAckCommitRemainsRecoveryRequiredAfterProcessExit`、`AmbiguousAckPrimaryFlushRemainsRecoveryRequiredAfterProcessExit`、`FailedAckCommitImmediatelyExposesPriorReportsAndAckEvidence`、`AckCleanupFailuresExposeReportAndMatchedCommitBeforeAndAfterReopen` | 同上 |

仍不能在本机关闭：硬断电/设备 flush 语义（进程退出不等于断电）、Windows（`PlatformUnsupported`）、操作员身份认证（审计只记录 operator_id，不认证）、provider 在转换为 WTS 之前丢弃的原始 SDK 回报。

## 4. 其余条目推进

- N05-003（5b 端到端）：新夹具插件 `wp1_twap_plugin.cpp` 把真实 `WtTWapExeUnit`（只读编译 `src/WtExeFact`）包装成 ABI 2.2 插件，分别声明/不声明 `CAP_HOST_ORDER_IDENTITY`。宿主均核验身份时：能力版本单元收到保留键=1，超时后经上下文 `cancel(localid)` 撤单恰好 1 次；旧 ABI 版本单元收到 0，6 轮行情 0 次撤单、0 次新单，日志“cannot be auto-canceled”；其 `hostOrderIdentitySupported()=false` 使执行器 owner 只能独占合约（第二个 owner 被拒），能力版本可与他人共享。用例 `OldIdentityAbiPluginNeverAutoCancelsAndOwnsContractsExclusively`（覆盖用例，非红控）。
- NEW-052（卡9a=A+C 宿主侧）：新增 SPI 入口闸（`SpiIngress`）：12 个 `ITraderSpi` 回调先计数再检查关闭标志（计数在 `_spi_mutex` 内递减，等待方不会在通知方仍使用互斥量时析构）；`release()` 在 `registerSpi(NULL)`/`api->release()` 后关闭入口但不等待（release 原本就不是排空屏障，`N01TraderAdapterSubscribe.request_stop_is_idempotent_and_waits_for_broadcast` 依赖此合同，首版阻塞实现曾使其超时，已改），之后到达的回调直接丢弃、不触及账本/持仓/日志；`run()` 重新注册 SPI 时重开；析构在 sink 排空后再等待仍在适配器内的 SPI 回调结束，然后释放适配器自有 `_orders`（原从不释放）。红控 `ReleaseClosesSpiIngressAndDestructorDrainsBeforeFreeingOrderMap`（改前：release 后迟到成交仍记持仓、析构不等待在途回调、析构后订单引用未归还）。适配器对象销毁之后 provider 仍回调属 provider 合同（ITraderApi 无版本化 quiescence 能力），仍为 X；未改变 DLL 卸载（`release()` 仍 `free_library`），按卡9a 无法证明时应常驻，需生命周期/runners 包统一处理（决策卡草稿 D）。
- R15-001 + I03-NEW-002（HftStraBaseCtx）：标签改为互斥保护 + 驻留不可变字符串（回调拿到的指针在同 localid 被替换/删除后仍有效，上界为不同标签值数）；`stra_buy/stra_sell` 经新增 `TraderAdapter::EntrustObserverScope` 在 localid 预留后、provider 调用前登记，未被接受的预留删除标签；终态（撤单/全成/委托失败）进入 16384 笔有界退役窗口，存活标签永不淘汰，旧退役记录不会误删同 localid 的新登记；未绑定的 `stra_get_position/stra_get_undone` 不再空指针解引用，返回 0 并记错误（0 与合法零值不可区分，决策卡草稿 A）。新增 lane 用例 6 个（另有费用 2 个，见 4B）+ 夹具 `src/TestUnits/fixtures/i03_r15_hft_tags_20261104s5/`（ASan/UBSan、TSan、改前红控）。
- L01-014 调用者审计（第 1 节 F9 + 只读核对）：TraderAdapter 的 8 个回报/查询入口在标准码为空时均隔离（暂停+保留）或 fail-closed，不写空键；`doCancel` 空码不发撤单；`onRspEntrust` 空码回退账本码（账本项必存在）。WtCore `ParserAdapter`（dt-parsers 包，只读）：期权预校验、分月空码丢弃；但平码分支及 `handleOrderQueue/OrderDetail/Transaction` 未检查空码，且 `cInfo->getCommInfo()` 为空时解引用（新发现，见第 7 节）。`WtDtServo/WtDtRunner`：空码丢弃并记错误，`sub_tick` 拒绝空/仅后缀代码（只读核对通过）。
- NEW-137（Z）：不创建任何链接。首版用系统既有链接 `/tmp -> private/tmp` 做“目录组件不跟随”差分，运行发现与产品合同不符：`splitStablePath` 用 `std::filesystem::canonical` 一次性解析**目录**路径（含其中链接）再以描述符钉住，只有**文件**末端组件以 `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW` 打开。该用例已改为记录真实合同的 `SystemLinkInDirectoryPathIsCanonicalizedToOneJournal`：经 `/tmp/<dir>/reports.wal` 建档的文件只落在 `/private/tmp/<dir>`、为普通文件；另一种写法在前者持有期间打开得 `Busy`（别名不能成为第二写者）；关闭后以真实路径读回同一份待对账记录。WAL 末端组件本身为链接的负控仍需创建链接，未执行；静态核对 `UnresolvedReportJournal.cpp` 全部文件 `open/openat/fstatat` 均带 `O_NOFOLLOW`/`AT_SYMLINK_NOFOLLOW`，`ELOOP` 映射为 `UnsafePath`（:446-458、:506-518、:549-592、:826-837、:901-1070、:1370-1449）。原 `SymlinkAndVisiblePathReplacementFailClosed` 会调用 `fs::create_symlink`（主线程 414 例清单第 334 行含此例，按清单运行即违反禁链规定）：现仅在 `WT_ALLOW_SYMLINK_TESTS=1` 时运行，否则 SKIP（不计通过），无链接部分拆为 `VisiblePathAndLockReplacementFailClosed`。

## 4A. 主线程追加：I03-NEW-057（逐单账本两项静态隐患）

运行时确认两项均成立（红控 `runs/n057-red-asan`，改前源码副本 `before057/`）：

1. 同一笔成交先经订单回报（`volLeft` 变小）、再经成交回报到达时被扣两次：`OrderReportThenTradeOfSameFillDeductsOnce` 改前 5→3 后再扣到 1（应为 3），后续各步全部偏小。修复：账本项记录 `volume`（provider 回报量，缺省为下单量）、订单流最小剩余 `order_left`、成交流去重累计 `trade_filled`，未完成量统一为 `min(order_left, volume - trade_filled)`（`ledgerRecomputeLocked`），两条流任意先后到达都只扣一次；终态 tombstone 与迟到成交行为不变。
2. tombstone 超过 65536 条时 `ledgerPruneLocked` 从 `ankerl::unordered_dense::map`（稠密存储，erase 为末元素移入空位）删除最旧项，会移动其他条目；`onRspEntrust` 在 tombstone 之后仍经旧迭代器读取 `std_code/intent_id/owner_id`。`TombstonePruneDuringRejectionKeepsTheRejectedOrdersIdentity`（先造 65536 条终态，再让最新一笔 owner 订单被拒）改前 owner sink 收到 0 条拒单通知（读到移动后的空 owner，回报被当作无 owner 投给观察者）；ASan 未报告（读的是向量容量内已移走对象，libc++ 未注解），属功能性红控。修复：先读字段再 tombstone，并在 `ledgerTombstoneLocked` 声明处注明其可能使 `_ledger` 引用/迭代器失效；其余调用点核对后均在 tombstone 后不再使用旧引用（`ledgerApplyOrderLocked` 立即返回、`onPushOrder/onRspOrders` 重新查找、Absent 不触发裁剪）。

绿控见第 5 节 `fxF-*`（及过程记录 `fx10-current-asan`）。

## 4B. 主线程追加：I03-NEW-008（HFT 实盘费用接入 FeeSchedule）

`HftStraBaseCtx::do_set_position` 三处 `commInfo->calcFee(...)` 改为 `_engine->calc_fee(stdCode, ...)`（engine-lifecycle 包已在 `WtEngine::calc_fee` 接入版本化费用表：合约优先、品种回退、合法零费率有效；缺费经 `report_fee_error` 计数/记录并返回 0，成交照常记账）。新增 lane 用例 `R15CtxTest.live_fees_come_from_the_engine_fee_schedule`（费用表同时给出合约级 SHFE.ag2512 与品种级 SHFE.ag 费率：开 2 手按合约费率 1.5/手=3.00，平今 1 手 2.0/手=2.00，无费用错误）与 `R15CtxTest.missing_live_fee_is_observable_and_the_fill_is_kept`（费用表缺 SHFE.ag：成交仍写入 trades.csv、费用 0.00、`fee_error_count` 增加、`last_fee_error` 非空）。红绿：改前（`commInfo->calcFee`，费率来自 `load_fees` 写入商品的品种费率）开仓记 18.00、缺费不计错误，2/2 失败；改后 2/2（`runs/r15fee-*`）。连续合约按实际映射合约计费由 `calc_fee_checked` 负责，本用例只覆盖普通合约。

## 4C. 主线程追加：BUG-R202-003、I03-NEW-071、I03-NEW-073

- BUG-R202-003（卡8c=A）：风控开启时 `isTradeEnabled`、`checkOrderLimits`、`checkCancelLimits` 对解析不到实际合约（`getContract` 为空，含无基础数据管理器）的代码一律拒绝（下单/撤单记错误日志），风控关闭时不变；排除集原已拒绝。红绿用 exec-units 已写的合同用例 `src/TestUnits/fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp`（只读），经新脚本 `src/TestUnits/fixtures/i03_wp1_ledger_20261104s5/run_r202_003.sh` 复制进 overlay 的 n01 lane、与指定 TraderAdapter 源一起 ASan+UBSan 编译，旁置复制的 `libTraderMocker.dylib` 运行：改前（`before202/`）`RiskOnRejectsUnknownCodeAndKeepsKnownCode` 失败（:97/:98 未知码被放行）、`RiskOff…` 通过（`runs/r202-red`）；改后见 `runs/r202-green`。按卡8c=A 更新 `R170TraderRisk` 两个窗口用例：原用例无基础数据管理器，改为登记 ag2512/au2512 后保持原窗口断言，并新增未知码三入口被拒断言（改前副本在 `before/`）。
- I03-NEW-071：`buy/sell` 对未知代码原解引用空合约（`doEntrust` 同样），本包已在第 1 节 F9 修复（fail-closed、不预占、不发送）。新增 `Wp1Test.BuySellOnUnknownCodeFailClosedWithoutDereferencingNull`：在子进程中执行 `buy/sell/openLong("SHFE.xx.9999")` 并断言无发送、无未完成量，父进程把子进程崩溃记为失败（旧实现崩溃不会中断整组）；红控为 baseline3 变体（会话起点源码），绿控为 current 变体（第 5 节 `fxF-*`）。
- I03-NEW-073：`r15_hft_stra_context.cpp` 访问 `HftStraBaseCtx` 受保护成员——为本包 R15 改动的中间态（13:32 头文件把退役窗口常量设为受保护后，测试在 18:2x 前直接引用 `HftStraBaseCtx::kRetiredOrderTagLimit`）。现测试派生类以 `using HftStraBaseCtx::kRetiredOrderTagLimit;` 暴露（改前红控变体以 `-DR15_S5_BASELINE` 去掉该行），默认 -Werror 覆盖率构建编译通过，lane `R15CtxTest.*` 在覆盖率二进制与 R15 夹具中运行通过（第 5 节）。

## 5. 运行结果（逐例独立进程、独立 cwd、硬超时；最终源码哈希 `hashes/final-sha256.txt`）

| 运行 | 目录 | 结果 |
|---|---|---|
| WP1 夹具起点 | `runs/fx-start-asan` | 26 例 25 过 1 跳过（与主线程一致） |
| WP1 夹具最终 ASan+UBSan | `runs/fxF-current-asan`（源码复核通过） | 41 例 39 过、2 跳过（`ChildScenario`、`UnknownCodeChild` 为子进程辅助），exit 0 |
| WP1 夹具最终 TSan | `runs/fxF-current-tsan` | 41 例 39 过 2 跳过，0 条 TSan 报告，exit 0 |
| WP1 红控 baseline3（会话起点切片5源码） | `runs/fxF-baseline3-asan` | 11 例失败：F1–F8、NEW-052、NEW-057(1)、NEW-071（旧实现子进程崩溃被父进程记为失败）；27 过。NEW-057(2) 在起点源码因“无 owner 仍广播”而不可观察，其红控为 `runs/n057-red-asan`（修 057 前源码，2 例失败） |
| 中间版本 | `runs/fx6-*`、`fx7-*`、`fx8-*`、`fx9-current-asan`、`fx10-current-asan` | 各阶段绿控（36/37/39 例），仅作过程记录 |
| R202-003 | `runs/r202-red`、`runs/r202-green` | 改前 2 例 1 失败（风控开启时未知码放行）；改后 2/2 |
| R15 夹具最终 ASan+UBSan / TSan | `runs/r15F-current-asan`、`runs/r15F-current-tsan` | `R15CtxTest.*` 44/44；TSan 0 报告 |
| R15 红控（会话起点 HftStraBaseCtx） | `runs/r15F-before-asan`（逐例重跑日志同目录） | 同步回报标签、退役窗口两例失败；回调内替换/删除与并发读写 heap-use-after-free；未绑定查询 UBSan；缺费不计错误失败 |
| HFT 费用红绿（合约优先费率） | `runs/r15fee-current-asan`、`runs/r15fee-before-asan` | 改后 2/2；改前 2/2 失败（改前按商品费率记 18.00，应为合约费率 3.00；缺费不计错误） |
| 覆盖率相关用例（主线程 414 例 + R15 全组 + 新增 journal/费用用例，461 项） | `runs/coverage-related/per-final2.txt`（二进制 `binary-final2.sha256`；默认 -Werror 编译；因他包 `WtBtPorter` 链接失败，本目标手工链接并去掉对 `libWtBtPorter.dylib` 的依赖，脚本 `link-nobtporter.sh`） | 453 过；2 失败（`R59Exec.diff_stale_target_*`，属 exec 包 `r59_exec_autoclear_test.cpp`，主线程清单中同样失败）；1 SKIP（`SymlinkAndVisiblePathReplacementFailClosed`，禁链策略）；5 未执行（4 个 `DISABLED_` 用例按名过滤运行 0 例、`R10ExecuterTest.diff_load_data_broken_json_and_empty_file_are_ignored` 不在当前二进制）——这 6 项在 per 文件中记为 exit 0，**不计通过**。主线程 41 例失败中 39 例本次通过（R170 两例、URJ 一例为本包；其余为 exec 包同期修复）。随后仅改写正向费用用例（合约优先费率），最终二进制 `binary-final3.sha256` 上两条费用用例 2/2（`logs-final3/`）。 |
| 首轮覆盖率运行 | `runs/coverage-related/per-first.txt`、`logs-first/` | 448 过 10 失败，暴露的问题均已处理：`N01TraderAdapterSubscribe.request_stop_is_idempotent_and_waits_for_broadcast` 超时（首版 release 等待 SPI，违背“release 不是排空屏障”合同，已改为析构等待）、`P11PushNotify.self_match_via_push_trade`（旧 oracle 期待重复推送重复投递，按卡5d/R3-008 更新）、journal fork 子进程三例（ACL errno，已修）、NEW-137 首版用例（合同理解错误，已改写） |
| journal 不稳定归因 | `runs/urj-flake/` | 旧二进制 460 次 0 失败；新二进制顺序 3/3 失败；探针定位 fork 子进程 `acl_get_fd_np` 偶发 EINVAL；DYLD 注入红绿 |

改前副本：`before/`（会话起点）、`before057/`（NEW-057 修复前）、`before202/`（R202-003 修复前）。被改写的旧断言：`AbsentOrderBecomesRevivableTombstone`（卡5d 空快照歧义）、`R170TraderRisk.OptionEntrustErrorUsesOptionStdCode`（卡6d/T2-CTP-08、R3-008）、`R170TraderRisk.SecondCanceledShortTodayPushRestoresAvail`（getPosition 净头寸符号、R3-008）、`R170TraderRisk.*WindowRejectsAtBoundaryAndRecovers`（卡8c=A：补登记合约并新增未知码断言）、`P11PushNotify.self_match_via_push_trade`（卡5d、R3-008）。

## 6. 条目建议状态（主线程独立复核后定）

| 条目 | 建议 | 依据 / 剩余 |
|---|---|---|
| BUG-L01-014 | 实施中 | 本轮：调用者审计（F9 修复、ParserAdapter/WtDtRunner 只读核对）、WAL NO-GO 四项本机证据（第 3 节用例在最终覆盖率运行中全部通过）、journal ACL errno 修复。剩余：WtRunner 无恢复入口（runners 包）；操作员身份不认证；provider 在转 WTS 前丢弃的原始回报；硬断电、Windows（X）；WtCore ParserAdapter 平码/明细路径空码未检（dt-parsers 包，见第 7 节）。 |
| BUG-R54-001 | 待验证 | lookup 失败的回报隔离、账户暂停、审计恢复（Porter/管理器入口）与 WAL 持久化均有本地定向证据；剩余同 L01-014 的外部项与 WtRunner 入口。 |
| BUG-N05-003 | 待验证 | `cancelVerified`/`cancelForOwner` 身份核验、暂停期可核验撤单、6 单元宿主核验超时撤单（exec 包用例）、旧 ABI 端到端（本轮新增）、重启 SIGKILL 两例。剩余：真实 provider 重启/跨进程唯一性（X）；第三方旧插件自行撤单不可区分（决策卡 B）。 |
| BUG-I02-NEW-008 | 待验证 | owner 路由修 F1–F4、执行器接入（exec 包，切片5）。剩余：两个真实 Diff 执行器同账户同合约端到端复跑（建议主线程/exec 包）；provider usertag 往返与成交号稳定能力只能由配置声明（X）；裸 API 订单不受 claim 约束（决策卡 E）。 |
| BUG-I02-NEW-154 | 待验证 | 订单 epoch + 空快照 fail-closed、持仓 epoch + 无基线歧义暂停、成交去重；接口无法区分失败/空/部分（决策卡 C，provider 需版本化状态）。 |
| BUG-I02-NEW-052 | 阻塞（X） | 宿主侧 SPI 入口闸 + 析构排空 + `_orders` 释放已实现并红绿；解除条件：ITraderApi 版本化 quiescence 能力或逐 provider 停止合同证据；DLL 常驻策略待决（决策卡 D）。 |
| BUG-R15-001 | 本地验证通过（建议） | 标签同步存储/发送前登记/有界退役/回调指针不悬空，ASan、TSan 全组 44/44，改前 4 项红控（含 2 个 UAF）。限制：16384 笔退役窗口外迟到成交读回空串；不同标签值终身驻留。 |
| I03-NEW-002 | 阻塞（D） | 未绑定查询不再空指针解引用（UBSan 红控→绿）；double 查询失败合同待裁定（决策卡 A）；绑定须在发布前完成，不支持并发改绑（文档化）。 |
| BUG-R202-003 | 本地验证通过（建议） | 卡8c=A 三入口实现；exec-units 合同用例红绿 1→0 失败；R170 窗口用例按 8c 更新后通过。剩余：其他引擎层未知码门禁（若有）不在 TraderAdapter 范围。 |
| I03-NEW-057 | 本地验证通过（建议） | 两项均运行时确认并修复，红绿见第 4A/5 节。 |
| I03-NEW-071 | 本地验证通过（建议） | buy/sell/doEntrust 未知码 fail-closed，子进程用例红（起点崩溃）绿。 |
| I03-NEW-073 | 本地验证通过（建议） | 默认 -Werror 覆盖率构建可编译，R15 lane 44/44（覆盖率二进制与夹具）。 |
| I03-NEW-008（HFT 费用子项） | 待验证 | HFT 三处改用 `WtEngine::calc_fee`，合约优先/缺费可观察红绿；整项其余消费者属 engine-lifecycle 包。 |
| BUG-I02-NEW-137（Z） | 阻塞，建议处置“用户指示忽略”末端组件链接负控 | 目录组件经系统既有链接的合同、别名不能成为第二写者已运行验证；末端组件 O_NOFOLLOW 仅静态核对；原用例默认 SKIP。解除条件：允许在隔离目录创建链接，或接受静态核对替代。 |

## 7. 新发现（编号由主线程分配）

1. UnresolvedReportJournal：macOS fork 子进程内 `acl_get_fd_np` 偶发 EINVAL 使 `noExtendedAcl` fail-closed（journal 打开/ACK 失败→暂停账户）；已在本包修复（有界重试），需新编号登记并纳入回归。
2. WtCore `ParserAdapter`（dt-parsers 包，只读）：平码分支及 `handleOrderQueue/OrderDetail/Transaction` 未检查标准码为空，且 `cInfo->getCommInfo()` 为空时解引用（`src/WtCore/ParserAdapter.cpp:645、668、691、714`）。
3. `HftStraBaseCtx::on_trade` 把收到的每笔成交（同 adapter 上其他上下文/执行器的成交，路由下非 owner 观察者也会收到）都计入本策略 `_pos_map`，未按本上下文 localid 过滤（既有行为，R15 范围外）。
4. 执行器从不调用 `TraderAdapter::releaseOwner`：账户 claim 终身保留，重建的同名执行器若能力变化被拒、他人永远无法在原 owner 销毁后独占该合约（exec 包）。
5. 主线程 414 例清单含 `UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`（`fs::create_symlink`），按清单运行会在 TMPDIR 创建符号链接；本轮已门控为默认 SKIP。
6. 当前树 `WtBtPorter` 链接失败（`WtBtRunner::finalizeRelease` 引用未定义 `wtp::EventNotifier::release()`，他包进行中），导致 `CoverageTests_wtcore` 因隐式依赖无法经 ninja 正常链接；17:4x 时 `WtDtCore/DataManager.cpp` 与头文件不一致亦曾阻塞 WtDtCore 构建。
7. `r15_tag_snapshot` 旧夹具脚本依赖已不存在的 `/private/tmp/wt-r14-resume` 构建，无法复跑（由新夹具 `i03_r15_hft_tags_20261104s5` 取代）。

## 8. 决策卡草稿

- A（I03-NEW-002/R42-002）未绑定 HFT 上下文的 double 查询失败如何表达：A1 维持 0+错误日志（现状，交易动作已 fail-closed）；A2 返回 NaN（R42-002 已指出会让调用方比较/撤单停止条件失效）；A3 新增版本化 typed 查询（状态+值），旧接口保留 A1。建议 A3+A1。
- B（5b）第三方旧 ABI 插件不读保留键时，宿主无法区分其“超时撤单”与普通撤单：B1 接受限制，撤单仍须 owner+账本身份核验、暂停期仅可核验身份（现状）；B2 旧 ABI 单元一律禁止 `cancel(localid)`（破坏其正常撤单）；B3 实盘拒绝加载无 `CAP_HOST_ORDER_IDENTITY` 的单元。建议 B1 并加载时诊断。
- C（NEW-154）查询结果状态：C1 版本化 SPI 扩展（如 `onRspOrdersV2/onRspPositionV2(array, status{complete,failed,partial})`）并逐 provider 迁移；C2 维持 fail-closed 启发式（空=未知，非空部分失败无法识别）。建议 C1，过渡期 C2。
- D（NEW-052/卡9a）无法证明 provider 回调已停时：D1 宿主保留适配器对象与 DLL 到进程退出（不 `free_library`、管理器不销毁），改变现有 release/析构语义与相关用例；D2 现状（入口闸+析构排空，对象销毁后回调仍属 provider 合同）。按卡9a 建议 D1，由生命周期/runners 包统一实现。
- E（NEW-008/5b/5c）未经执行器的裸 API 订单（HFT、人工）是否受合约 claim 约束：E1 路由开启时裸 API 入口记为 `adapter-direct` owner 参与 claim；E2 不约束（现状）。建议 E1（需确认对 HFT 与执行器混用账户的影响）。

## 9. 限制

未删除任何非本子任务创建的文件或目录；本子任务只删除过自己创建的内容（`/tmp/wt-i03-s5-wp1-ledger-run/rel/final`、探针在 TMPDIR 自建的 `wt_urj_probe_*`/`wt_acl_probe_*` 临时目录与文件、循环脚本自建的输出与 profraw、证据目录内一份重复副本 `before202-pre-copy`）。主线程 414 例清单原路径 `/tmp/wt-i03-wp1-20261104-covrun3` 与 `/tmp/wt-i03-s5-run` 在本会话中已被外部删除，改用主线程证据目录 `20261104-s5-main/wp1-related/tests.txt`。

本机 macOS arm64、本地 fake provider；不代表真实 provider/柜台回报时序、跨进程其他客户端、硬断电、Windows/MSVC。sanitizer 为局部插桩（夹具编译的 TU 插桩，其余来自非插桩库）。覆盖率二进制为手工链接（去掉 WtBtPorter 隐式依赖），依赖该 dylib 的用例若存在则在运行期缺符号——本清单内未出现。整体回归仍为 NOT_RUN，本目录结果不得当作整体验收。
