# s5 dt-parsers：行情 parser 宿主与数据服务生命周期（卡9 落地）定向证据

2026-11-04，macOS arm64（Darwin 25.5），HEAD `ccaad010`（工作区含大量他人未提交改动，见 `hashes/start-owned-sha256.txt`、`hashes/HEAD.txt`、`hashes/start-time.txt`）。本目录只记录本子任务的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商 SDK、真实行情或最终整合树。未修改任何跟踪文件。

构建目录：覆盖率 `/tmp/wt-i03-s5-dt-parsers-cov`（默认 `-Werror`，`-DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`）；专用夹具 `/tmp/wt-i03-s5-dt-parsers-fixture`（普通）与 `/tmp/wt-i03-s5-dt-parsers-fixture-tsan`（Homebrew LLVM 22 `-fsanitize=thread`）；UDP 间歇用例 `/tmp/wt-i03-s5-dt-parsers-udpflaky`；R22 监控 TSan `/tmp/wt-i03-s5-dt-parsers-r22tsan`。所有用例在 `/tmp/wt-i03-s5-dt-parsers-run/<label>` 独立 cwd 用 `perl alarm` 硬超时运行（`scripts/runcase.sh`），日志/XML/退出码/二进制哈希复制到 `runs/<label>/`。修改前文件在 `before/`（保留相对路径），前后 sha256 前缀见 `hashes/before-after-sha256-prefix.txt`（完整哈希可对 `before/` 与工作区重算）。没有创建符号链接；没有运行改动工作区/历史的 git 命令（`WtBlkIdxWorker.h` 修改前副本取自 `git show HEAD:`，该文件此前未被修改）。

## 1. 合同落地（卡9 9a=A+C / 9b=A / 9c=A / 9d=A）

新增 `src/WtDtCore/ParserLifecycleHostV2.hpp`（header-only，WtDtCore 与 WtDtServo 共用）：解释 `IParserLifecycleV2.h`（不改 ABI），并提供宿主回调准入闸门 `CallbackGate`（seq_cst 原子：回调要么看到闸门关闭，要么被关闭方计数；线程局部帧识别“本线程正在本适配器回调内”）。

`WtDtCore::ParserAdapter` 与 `WtDtServo::ParserAdapter`：
- 动态模块必须导出合法 V2 表并通过 preflight，才调用 `createParser`；旧模块在创建前拒绝（无 API、无 SPI 注册，cfg/模块立即归还）。表/preflight 抛异常或 ERROR → 模块隔离（不卸载）。`createParser` 返回 NULL 按 V2 头“无异步工作”归还；抛异常 → 隔离。
- provider `init` 返回 false/抛异常、或订阅前闸门已关、或订阅抛异常 → **失败回滚**：关闭宿主闸门，`request_stop`→`can_wait`→`wait_drained`（≤2000 ms）→`destroy_api`；只有 DRAINED 才销毁 API 并归还模块与 cfg 引用；PENDING 保留待 `release()` 重试；其余（ERROR/异常/destroy 不合规）永久隔离、不重试（9a）。
- 成功路径的 `release()` = `requestStopLifecycle()`（关闭闸门 + 非阻塞 V2 stop）+ 有界 `finishReleaseLifecycle()`；在 provider 自身线程或本适配器回调内调用只请求停止并延后到安全点（9d）；重复 `init` 拒绝并保留原实例（9c）；重复 release 幂等。
- 析构不是排空屏障：对可证明的 V2 owner 做一次有界排空；否则**泄漏** API/模块/cfg（不卸载正在执行的代码）。
- `initExt` 外部 API 无 V2 证明：失败时关闭闸门（不再派发到借用管理器）并保留 API；其 `release()` 仍为调用方断言屏障后的拆除（保持旧语义，WtDtPorter 的 ExpParser 由宿主自身驱动，闸门关闭且排空即可证明）。
- `ParserAdapterMgr`：`requestStopLifecycleAll()` 先停全部再 `finishReleaseLifecycleAll()` 轮询；未完成者留在 map（隔离 owner）。WtDtCore 新增 `runWithResult()`。

Runner/宿主：
- `WtDtServo::WtDtRunner::shutdown()`：停全部（含失败候选）→有界排空→仅全部释放才返回 true；析构先 `shutdown()`，失败时把 adapter 移入进程级泄漏保管（闸门已关）。`WtDtServo.cpp` 的 `getRunner()` 改为堆上 runner + 退出时 `shutdown()` 成功才 `delete`，否则保留整张借用图到进程结束（“保留资源到进程退出”）。
- `WtDtPorter::WtDtRunner::shutdown()`（R5-007 顺序）：`_to_exit`→停 StateMonitor→在注册表锁内停止接纳并快照（有在途创建则延后返回 false）→锁外先停全部 parser 再排空→锁内移除已释放者→全部释放后才 `IndexWorkerMgr::release()`、`DataManager::release()`；析构/`getRunner()` 同上。
- `QuoteFactory/main.cpp`：BUG-R104-002——active parser `init` 失败不注册、不计入 loaded、不 connect；仍持有 API 者进 `g_failedParsers` 保留；日志区分 loaded/failed；`runWithResult()` 报告连接命令提交失败（I03-NEW-003 端到端可见）。新增 `shutdownQuoteFactory()`：停监控→停全部 parser（含失败候选）→排空→`g_idxFactory.release()`→`g_dataMgr.release()`；不完整时 `WTSLogger::stop()` 后 `std::_Exit(0)` 跳过静态析构，保留 provider 仍可触达的全局对象到进程结束；Windows CTRL_CLOSE 走同一顺序（原先先释放 writer 再……parser 仍可能写入）。
- R68-003：`IndexWorkerMgr::release()`（析构调用，幂等）：停止接纳 `handle_quote`→`pool->wait()` 排空→经各自工厂 `delete_worker` 删除 worker→工厂与模块常驻到进程结束：导出的 `delete_index_factory(IIndexWorkerFact*)` 按值，而 `FuncDeleteIndexFact` 声明按引用，且无版本化导出可识别模块实现哪种签名，故宿主**不调用任何 remover**（不能调用不兼容 remover）。`WtBlkIdxWorker` 新增析构：置停止、唤醒、join 触发线程（原实现从不停止该线程，删除带 timeout 的 worker 时 joinable `std::thread` 析构即 `std::terminate`）；`_stopped/_process` 改原子，等待带停止条件。

## 2. 定向结果（数值以 `runs/*/result.xml`、`runs/*/exit` 为准）

### 2.1 宿主生命周期专用夹具 `src/TestUnits/fixtures/i03_dt_parser_host_20261104s5/`

本地 fake provider（自带回调线程；旧 `release()` 不停线程；旧 `deleteParser` 在线程运行时拒删并计数；V2 表 stop/can_wait/wait_drained(join)/destroy），见夹具 README 与 `fixture-src/`。当前源码与修改前副本（`before/`）各编一份：

| 运行 | 结果 |
|---|---|
| `runs/dtcore_host_cur`（WtDtCore 适配器/管理器矩阵） | 10/10，exit 0 |
| `runs/servo_host_cur`（WtDtServo 适配器/管理器/Runner 矩阵，含 R123-003 在途回调用例） | 9/9，exit 0 |
| `runs/dtcore_base_*`（7 例红控，旧源码） | 7/7 判红：1 例断言失败 exit 1，其余 SIGSEGV/SIGBUS（exit 139/138）；`runs/dtcore_base_SecondInitIsRejectedAndKeepsOriginal/lldb.log`（早一轮构建）定位为 provider 线程在 `release()` 后经已析构适配器调用 `handleParserLog`（UAF） |
| `runs/servo_base_*`（7 例红控，旧源码） | 7/7 判红（SIGSEGV/SIGBUS；R123-003 用例 `libc++abi: ... mutex lock failed`，exit 134：在途回调放行后访问已析构 runner 的互斥量/成员） |
| `runs/tsan_{dtcore,servo}_host_cur_{1,2,3}`（LLVM 22 TSan，`report_thread_leaks=0`） | 各 3 次 10/10、9/9，0 报告；`*_threadleak_report` 为开启线程泄漏报告时的唯一报告——隔离用例按设计泄漏的 fake 线程 |

`runs/tsan_stale_pre_waitdrained/` 保留一次过时二进制（加入“等待在途宿主回调”之前）的失败：provider 线程恰在回调内时失败回滚立即判 PENDING 未销毁，正是随后修正的窗口。首轮 TSan 曾报 fake 模块测试钩子指针的竞争（夹具缺陷，已改原子后复跑）；首轮普通运行曾因测试按值构造 `DataManager` 而与 dt-writer 正在修改的头/旧归档布局不一致挂起，已改为不实例化 `DataManager`（`_dt_mgr` 传空，用例只走日志回调），二者均非产品问题。

红控汇总：`logs/red-controls-summary.txt`。

### 2.2 R68-003 指数 worker

- 独立编译（`/tmp/wt-i03-s5-dt-parsers-idxred`，同一测试源，分别链接当前与修改前 `WtBlkIdxWorker`）：新用例 `n03_blkidx.timeout_trigger_thread_is_joined_when_worker_is_deleted` 当前 1/1（`runs/idx_trigger_join_cur`），旧源码 `libc++abi: terminating` exit 134（`runs/idx_trigger_join_base`，红控）；当前该 TU 全部 14/14（`runs/idx_all_cur`）。
- R68 lane 新增 `ReleaseDrainsPoolThenDeletesWorkersAndPinsFactoryModule`（池任务先于 worker 删除、release 后不再接纳、模块不卸载、幂等），结果见 2.4。

### 2.3 ParserXTP / ParserCTPMini（卡9 vendor V2 夹具复跑，`runs/vendor_v2_xtp_ctpmini/`）

`i03_parser_vendor_v2_20261104` 以本构建 `/tmp/wt-i03-s5-dt-parsers-vendor`（`WT_FAMILIES=XTP;CTPMini`）复跑：XTP `scenarios_xtp_cur` 9/9、`v2_xtp` 11/11（含真实 WtCore `ParserAdapter` 隔离 SDK parser）；红控 `v2_xtp_base` exit 1，`scenarios_xtp_base` 9 例中 6 例失败（回调内 release/disconnect、在途回调等待、迟到回调、回调内 connect、重复 release），其余 3 例为正控通过。CTPMini 撤回候选后 `scenarios_ctpmini_cur` 9/9、`v2_ctpmini` 11/11（`runs/vendor_v2_ctpmini_after_withdraw/`）。

### 2.4 覆盖率 lane（默认 `-Werror` 构建 `/tmp/wt-i03-s5-dt-parsers-cov`）

| 目标 / 过滤 | 结果 | 说明 |
|---|---|---|
| `CoverageTests_dtcore` R18ParserAdapterTest + R44ParserAdapterEdgesTest | 33 例，0 失败，2 disabled | R18 两例“失败 init 后仍持有 cfg 至析构”的旧断言与 R18-012/卡9 冲突，改为“失败即归还且只归还一次”（`before/` 保留） |
| `CoverageTests_dtcore` R145* | 11 例，0 失败，1 disabled（工作日 20:05 运行；周末分支未覆盖） | NEW-026 夹具：补 pricetick/volscale（卡4）、holiday 模板（卡1 MissingCalendar）、PAUSE 交易段由 0-0 改 23:58-23:59（卡2 2b：0-0 即全天） |
| `CoverageTests_dtcore` R68IndexWorkerFactoryTest + R18IdxWorkerMgrTest + R44IndexWorkerFreshTest | 27 例，0 失败，4 disabled | 含新增 release 顺序用例 |
| `CoverageTests_idxfactory` 全部 | 14/14 | 含新增触发线程 join 用例 |
| `CoverageTests_m_QuoteFactory` 全部 | 24 例，0 失败，4 disabled | 含改写的 R104 用例 |
| `CoverageTests_i03_dtporter_start` 全部 | 6/6 | |
| `CoverageTests_dtservo` **完整**（R8-003） | 125 例，0 失败，5 disabled，exit 0（`runs/lane_dtservo_full_colocated`） | 4 个 `Exported*` 用例依赖“可执行文件目录/parsers 与探针 dlopen 为同一镜像”，故把二进制复制到 R45 模块所在目录运行；放在其他目录时这 4 例因加载到另一份镜像而失败（`runs/lane_dtservo_full`，环境因素，非产品） |
| `CoverageTests_t_ParserXTP` 全部 | 76 例，0 失败，3 disabled | 含 `XtpLoginF.pending_login_failure_rechecks_unregistered_sink`（T27-004） |
| `CoverageTests_t_ParserUDP` 全部 | 46 例，0 失败，1 disabled | WtCore initExt 旧断言按卡9a 改写（WtCore 保留无 V2 证明的外部 API） |
| `CoverageTests_t_ParserShm` 全部 | 23 例，0 失败，3 disabled | 两处“同实例可重复 init”旧断言按 9c 改写；NEW-116 三例在内 |
| `CoverageTests_t_ParserCTPMini` 全部 | 27 例，0 失败，3 disabled | 撤回 registry 候选后的原产品源码 |
| `i03_parser_udp_lifecycle_v2` 夹具（T6-010） | v2 8/8、legacy 20/20 | `runs/udp_v2_fixture/` |
| `CoverageTests_dtcore` R5StateMonitorBdtTest | 14 例中 13 例 SetUp 失败（新发现，见第 5 节） | 与本包修改无关的夹具漂移 |

### 2.5 I03-NEW-012 ParserUDP 间歇失败

根因（`runs/udp_flaky_new012/diag-fail.log`）：用例 `OldTwoChannelReadCompletionsBelongToOneReconnectEpoch` 启动真实 worker，`make_udp_config()` 的 `sport=0` 使订阅包发往 `127.0.0.1:0`，macOS 在 worker 上以 `EADDRNOTAVAIL(49)` 完成发送，属于“恢复后新 epoch 的真实故障”，按合同开启第二个 Close 周期，与测试主线程注入的读错误断言竞争；此外偶见真实接收 `ECONNRESET`。不是产品竞态。修正：仅该用例给订阅通道绑定本地 UDP 接收端口（`LocalUdpReceiver`），断言未改。独立编译同一产品源：修改前测试 300 次失败 101（`before-x300.txt`），修改后 300 次失败 0（`after-x300.txt`）。

### 2.6 R5-007 / R22-002 StateMonitor（当前树）

`src/TestUnits/fixtures/r22_monitor_lifecycle/probe.cpp` 的证据专用变体（删去依赖 `DataManager::_writer` 的 writer 回调模式——dt-writer 已改为 `WriterHost`，旧探针无法编译；变体在 `runs/r22_monitor_tsan/probe_nowriter.cpp`）与当前 `StateMonitor.cpp`、`DataManager.cpp`、`WtHelper.cpp` 用 LLVM 22 `-fsanitize=thread` 编译：repeat/concurrent/runstop/multiple/queries 各 3 次，15/15 exit 0、0 个 TSan 报告（`runs/r22_monitor_tsan/summary.txt`，输入哈希 `inputs.sha256`）。self-stop、外部 join 中回调 stop、writer 抛异常三种模式需 dt-writer 提供 writer 注入接缝后重跑，本次未运行。

## 3. Z 类（卡15）复核

- **BUG-R104-002**：最终复核发现明确缺陷——QuoteFactory 把 init 失败的 active parser 注册、计入 loaded 并 connect（原 `adapter->init()` 返回值被丢弃）。已修复（见第 1 节），旧断言 `active_entries_with_and_without_id_land_in_manager` 与原验收“缺模块 active 项不计为 loaded”冲突，改写为 `failed_active_entries_are_not_registered`（修改前副本在 `before/`）。剩余“active 失败是否终止启动”属策略（决策卡草稿 D1）；该子项若不裁定，建议按卡15记“用户指示忽略”，缺陷部分不以忽略处理。
- **BUG-I02-NEW-026**：z15 发现 F 已确认是夹具问题——卡4（4a=A）后 `loadCommodities` 只登记 `pricetick>0`、`volscale` 为正整数的就绪品种，`r145_test_util.h` 的品种缺这两项；只更新夹具，未改产品校验。R145 结果见 2.4。
- **BUG-I02-NEW-116**：订阅编码合同可由现有调用者确定——三个宿主 `ParserAdapter` 均以 `WTSContractInfo::getFullCode()/getFullAltCode()`（`<exchg>.<rawcode>`）组装 `CodeSet`；`ParserShm::subscribe/unsubscribe` 以第一个 `.` 分割、保留其后多段原始代码。定向 3 例结果见 2.4。建议依调用者合同记本地验证通过（若主线程认为调用者不足以构成合同，则记“用户指示忽略”）。

## 4. 逐项建议状态（仅本地范围；真实 SDK、Windows/MSVC、整体回归均未验）

| 条目 | 建议状态 | 依据 | 剩余/解除条件 |
|---|---|---|---|
| BUG-R18-006 | 本地验证通过 | 2.1 夹具 dtcore 10/10，旧源码 7/7 判红（断言失败/UAF 崩溃），TSan 3×；R18/R44 lane 见 2.4 | 真实 provider 回调排空（X）；vendor V2 parser 在 SDK 对象存在后按设计报 ERROR→隔离 |
| BUG-R18-012 | 本地验证通过 | 失败/成功 init 后 cfg refcount 回基线且只归还一次（夹具 4 例 + R18 两例按新合同改写） | 同上 |
| BUG-R8-006 | 本地验证通过 | servo 夹具 8/8（成功/失败/显式与重复 release/直接析构/Manager/Runner 析构与 shutdown/不可证明 owner 隔离），旧源码 6/6 判红，TSan 3×；R45 三例按 9a/9b 改写为 V2 等价；完整 dtservo 125/125（5 disabled） | 真实 provider（X）；WtDtPorter Runner 无运行时用例（静态 + 适配器级） |
| BUG-R123-003（storage-read 交来） | 本地验证通过 | 计数 fake：provider 回调正阻塞在 runner 的 tick 回调中（放行后继续进入 `WtDataManager::update_bars`）时 `delete runner`；当前实现先关闸门、有界等待在途宿主回调离开、再 V2 排空销毁，回调退出序号早于析构完成，provider 销毁 1 次、无残留线程（9/9、TSan 3×）；旧源码同用例在途回调放行后访问已析构 runner 互斥量而 abort（exit 134，红控）。无 store 配置，reader 为空：reader 模块“不卸载”仅静态核对 | 真实 provider（X）；带真实 reader 的运行时用例未做 |
| BUG-I02-NEW-009 | 本地验证通过 | 失败候选 V2 回滚或保留、Runner 不启动失败候选、借用图由 `RunnerHolder` 保留到进程结束；R45 `RunnerQuarantinesFalseAndThrowingParserCandidates` 等通过 | 真实 provider stop/drain（X） |
| BUG-R5-007 | 待验证 | WtDtPorter `shutdown()` 顺序（监控→parser→指数→writer）、QuoteFactory 同序；StateMonitor 当前树 TSan 15/15 | writer 回调三模式需 dt-writer 接缝后重跑；WtDtPorter shutdown 运行时用例未写；R5StateMonitorBdtTest 夹具漂移（新发现） |
| BUG-R8-003 | 阻塞（运行项已完成） | 无链接构建完整 `CoverageTests_dtservo` 125 例 0 失败 5 disabled exit 0（`runs/lane_dtservo_full_colocated`） | 原剩余“无 store 合同冲突 / `IRdmDtReader::init` 为 void”仍需合同（未改） |
| BUG-R22-002 | 待验证 | 并发 run/stop、重复 stop、多实例 `lastTime`、状态并发查询 TSan 15/15；provider drain 由 Runner shutdown 承担 | self-join/回调内 stop/异常三模式需重跑；TimeUtils 99 哨兵属 date-session |
| T1-XTP-02 | 本地验证通过 | vendor 夹具 XTP 9/9 + V2 11/11，旧源码红控；重连语义：`disconnect()` 为终止性关闭，重连须新 parser 实例（与 E 类 R60-001“重连使用新会话对象”一致），SDK 断线后 provider 内部 relogin 保留；自线程 `delete` fail-stop、回调内 release 延后 | 真实 SDK 回调排空（X） |
| BUG-T27-004 | 本地验证通过 | 红：`evidence/T27-004-20261102/` ccaad010 快照两例 SIGSEGV；绿：t_ParserXTP lane `XtpLoginF.pending_login_failure_rechecks_unregistered_sink`（2.4）+ vendor 夹具 | 真实 SDK（X） |
| BUG-T6-010 | 本地验证通过（待 2.4/UDP V2 夹具复跑确认） | ParserUDP V2 生命周期与独立发送队列锁（`ParserUDPQueueLockTest`）在 t_ParserUDP lane；UDP V2 夹具复跑见 2.4 | 真实组播/行情 |
| BUG-R68-003 | 待验证 | worker 触发线程 join 红绿（134→通过）、release 排空顺序用例、remover 不调用；QuoteFactory/WtDtPorter 关闭顺序 | D4 remover ABI 裁定；QuoteFactory 关闭顺序无运行时用例 |
| BUG-I02-NEW-029 | 阻塞 | 产品文件 `HisDataReplayer.cpp` 不属于本包；合同（卡E）与补丁草稿 `patches/` | 分配 WtBtCore owner 实施 + R91 四流正负控（D5） |
| BUG-R104-002（Z） | 本地验证通过（缺陷部分） | 见第 3 节；r104 用例结果见 2.4 | 启动策略 D1；未裁定时该子项按卡15“用户指示忽略” |
| BUG-I02-NEW-026（Z） | 本地验证通过（待 2.4 R145 复跑确认） | 夹具补 pricetick/volscale（卡4）与 holiday 模板（卡1） | 无 |
| BUG-I02-NEW-116（Z） | 本地验证通过（调用者合同） | 第 3 节；t_ParserShm 定向 3 例 | 若不认可调用者合同则记“用户指示忽略” |
| I03-NEW-003 | 阻塞 | 三个 manager 均有 `runWithResult()`，QuoteFactory 改为报告；跨实例 opens=2 的进程级登记候选与卡9 vendor V2 冲突已撤回 | D6（跨实例再加载）与 D2（部分启动回滚） |
| I03-NEW-012 | 本地验证通过 | 2.5：101/300 → 0/300，测试缺陷非产品竞态 | 无 |

## 5. 新发现（请主线程分配编号）

- **A：R5StateMonitorBdtTest 夹具漂移**（`q05_dtsrv/r5_state_monitor_bdt_test.cpp`）。14 例中 13 例在 SetUp `loadSessions` 失败：`R5EQUAL` 会话 `120-120` 被卡2（2b=A，仅 0-0 表示全天，其余相同端点拒绝）拒绝，致整份 sessions 不发布；其后商品缺 pricetick/volscale（卡4）、未加载 holiday 模板（卡1 MissingCalendar）也会失败。复现：`CoverageTests_dtcore --gtest_filter='R5StateMonitorBdtTest.*'`（`runs/lane_dtcore_monitor`，为早一轮二进制，夹具未改）。建议由 StateMonitor owner（date-session）按卡1/2/4 更新夹具（`R5EQUAL` 用例改为验证拒绝），不改产品。
- **B：WtDtServo `initialize()` 内部即调用 `start()`**，测试/调用方再调 `start()` 会第二次 `connect`（`servo_host_test` 首版据此失败后改为不重复调用）。属公开语义（重复 start 是否幂等）问题，未改产品。建议与卡9 9c 一并裁定：重复 start 拒绝或幂等。
- **C：WtRunner 析构 `if (!shutdown()) std::terminate();`**（WtCore 包）。vendor V2 parser 一旦创建过 SDK 对象即按设计报 ERROR→隔离，`shutdown()` 永远返回 false，进程正常退出时 `WtRunner` 析构将 abort。仅静态分析，未运行；建议参照本包 `RunnerHolder`（隔离时保留整张图到进程结束）处理。
- **D：WtDtPorter 外部 parser（ExpParser）`release()` 会回调 Python**，进程退出期调用不安全；本包 `shutdown()` 已改为闸门关闭并排空后转入进程级保管、不调用其 release。WtPorter（WtCore）`initExt` 是否有同类问题需 WtCore owner 复核（与 z15 发现 E 相关）。
- **E（已在本包内修正，供记录）**：本轮实现中途发现“失败回滚/释放时若恰有宿主回调在途则立即判 PENDING 或在析构中放行成员销毁”，已改为有界等待在途宿主回调（`CallbackGate::waitDrained`），TSan 复跑通过。

## 6. 修改文件

前后 sha256 前缀见 `hashes/before-after-sha256-prefix.txt`，完整哈希 `hashes/before-full-sha256.txt` / `hashes/after-full-sha256.txt`。产品：`src/WtDtCore/{ParserLifecycleHostV2.hpp(新增),ParserAdapter.h,ParserAdapter.cpp,IndexWorkerMgr.h,IndexWorkerMgr.cpp}`、`src/WtIndexFactory/{WtBlkIdxWorker.h,WtBlkIdxWorker.cpp}`、`src/WtDtServo/{ParserAdapter.h,ParserAdapter.cpp,WtDtRunner.h,WtDtRunner.cpp,WtDtServo.cpp}`、`src/WtDtPorter/{WtDtRunner.h,WtDtRunner.cpp,WtDtPorter.cpp}`、`src/QuoteFactory/main.cpp`。`src/ParserCTPMini/ParserCTPMini.cpp` 曾改后撤回，现与修改前一致（`withdrawn/`）。测试/夹具：新增夹具目录 `src/TestUnits/fixtures/i03_dt_parser_host_20261104s5/`；修改 `fixtures/r45_dtservo_parser_fixture.cpp`、`coverage/q05b_dtservo/r45_parseradapter_test.cpp`、`coverage/q05_dtsrv/{r18_parser_adapter_test.cpp,r145_test_util.h,r145_state_monitor_run_test.cpp,r68_idxworker_factory_test.cpp}`、`coverage/n03b_idxfactory/n03_idxfactory_worker_test.cpp`、`coverage/m_QuoteFactory/r104_quotefactory_init_test.cpp`、`coverage/t_ParserUDP/{udp_handle_write_lock_test.cpp,t6_parserudp_reachable_test.cpp}`、`coverage/t_ParserShm/{r76_shm_boundary_test.cpp,t5_shm_parser_test.cpp}`。按已裁定合同改写的旧断言均列于 2.4 表并保留修改前副本。未修改共享 `coverage/CMakeLists.txt`；CRLF/BOM 已按原文件恢复（脚本 `scripts/restore_eol.py`）。

## 7. 决策卡草稿

见 `decision-drafts.md`（D1 启动策略、D2 部分启动回滚、D3 initExt 释放归属、D4 指数工厂 remover ABI、D5 NEW-029 归属、D6 跨实例再加载）。NEW-029 建议补丁：`patches/NEW-029-HisDataReplayer.proposed.diff`（未应用）。

## 8. 限制

本地 fake 只证明构造输入下的宿主行为；真实 vendor SDK 回调排空、真实组播/共享内存行情、Windows/MSVC、LSan/ASan（本次未跑 ASan）、整体回归均未验证。覆盖率构建在高负载下与其他子任务共享源码树，构建期间他人修改的头文件导致多次重编；最终 lane 二进制哈希见 `hashes/coverage-binaries-sha256.txt`。最后一次产品修改（`CallbackGate::waitDrained` 有界等待、WtDtPorter 外部 parser 有界静止检查，约 20:20）之后，在默认 `-Werror` 增量重编（exit 0）的二进制上重跑了 R18/R44（33，0 失败）、`m_QuoteFactory`（24，0 失败）、`i03_dtporter_start`（6/6）、完整 `dtservo`（125，0 失败）以及专用夹具/TSan；R145、R68、idxfactory、t_ParserXTP/UDP/Shm/CTPMini 结果来自该修改前一轮（这些用例不经过被改的 WtDtCore/WtDtServo 适配器等待路径，t_ParserUDP 中的适配器为 WtCore 的）。

## 9. 增量（2026-11-04 晚，主线程追加四项）

### 9.1 ASan+UBSan（LSan）

Homebrew LLVM 22 `-fsanitize=address,undefined`（依赖的覆盖率静态库未插桩）：

| 运行 | 结果 |
|---|---|
| 宿主夹具 `runs/asan_{dtcore,servo,dtporter}_host_cur`（`detect_leaks=0`，含隔离用例） | 10/10、10/10、4/4，0 个 ASan/UBSan 报告 |
| 宿主夹具 LSan `runs/lsan_*_host_cur`（`detect_leaks=1`，排除按设计泄漏的隔离用例） | 9/9、9/9、2/2，无泄漏报告；另用一个故意泄漏的小程序确认本机 LSan 生效（报告 6400 字节） |
| vendor V2（XTP、CTPMini）`runs/asan_vendor_v2/` | scenarios 各 9/9、v2 各 11/11，exit 0；按夹具说明 `detect_leaks=0`（SDK 隔离 parser 按设计泄漏） |
| UDP V2 `runs/asan_udp_v2/` | v2 8/8、legacy 20/20，0 报告 |
| StateMonitor writer 回调探针 `runs/asan_monitor_writer/`（LSan 开） | self / self-external-join / exception 3/3 exit 0，无报告 |

### 9.2 R22-002 self-join / 异常模式与 R5-007 WtDtPorter shutdown 运行时用例

- 按 dt-writer 新 `DataManager` 接口（`init()` 加载 writer 模块；legacy 无 V1 表时 release 后隔离）新增本地 fake writer `libI03DtFakeWriter`（`fixture-src/fake_writer.cpp`）与探针 `monitor_writer_probe.cpp`。writer 的 `transHisData` 在 StateMonitor 线程内回调：`self`（回调内 stop，不得自 join）、`self-external-join`（外部线程已持锁 join 时回调内 stop）、`exception`（回调抛异常：`hasFailed()`、普通谓词不报 PROCING/PROCED、记录态可诊断、不再重跑，writer 调用 1 次）。普通 3/3，TSan 3 轮 9/9 无报告（`runs/tsan_monitor_writer/`），ASan+LSan 3/3。
- `dtporter_host_test.cpp`（真实 `WtDtPorter::WtDtRunner` + StateMonitor 运行 + fake V2 parser）：析构先停监控与 parser（common，当前 4/4 中之一；旧 runner + 旧适配器红控 SIGSEGV exit 139，`runs/dtporter_base_RunnerDestructionStopsParsersBeforeMembers`）；`shutdown()` 按 监控→parser→指数→writer 释放且幂等、关闭后 `start()` 不再 connect；不可证明 parser 时 `shutdown()` 返回 false 并保留。TSan 3 轮 4/4、ASan 4/4。顺序由代码保证，用例验证外部可观察结果（provider 销毁 1 次、回调停止、二次 shutdown 无副作用），未直接观测 writer 内部时序。

### 9.3 I03-NEW-044：WtDtServo 重复 start（卡9c）

`WtDtRunner::start()` 新增 `_parsers_started` 原子标记：`initialize()` 内首次启动后，再调 `start()` 记 WARN 并拒绝，不再向同一实例发 connect。新用例 `ServoHost.RepeatedStartDoesNotReconnect`：当前 10/10 中通过（TSan 3×、ASan 通过）；旧源码 `connect` 计数 3（期望 1），随后析构 SIGSEGV（`runs/servo_base_RepeatedStartDoesNotReconnect`，红控）。既有 R8/R85/R45 中对失败或未初始化 runner 调 `start()` 的用例不受影响（见 9.5 lane 复跑）。

### 9.4 I03-NEW-047：WtDtPorter `_dumpers` 析构先于 `_data_mgr`（关联 BUG-P10-006）

原 `shutdown()` 在 parser 全部释放时先 `_data_mgr.release()`（同步：等待在途 writer 调用，再释放或隔离 writer），覆盖“shutdown 成功”路径；但 (a) writer 被隔离（legacy 或排空不可证明）时 writer 仍可能经 dumper 回调，(b) shutdown 未完成的析构路径不会 release writer，成员析构时 `_dumpers` 先于 `_data_mgr` 销毁。修正：`shutdown()` 在 writer 未达 `WLS_Released/WLS_None` 时返回 false（runner 与 dumper 一并保留，`RunnerHolder` 不 delete）；析构未完成路径先关闭全部 parser 闸门，再 `release()` 指数与 data manager（dumper 仍存活），writer 未释放则把 `_dumpers` 移入进程级保管（`retainDumpersUnlessWriterReleased()`）。新用例 `DtPorterHost.WriterReleaseReachesLiveExtDumpers`（fake writer 在 `release()` 内调用已注册的外部 dumper）：当前 ASan/TSan/普通均通过；旧源码 ASan `heap-use-after-free`（`FakeWriter::release` ← `~DataManager` ← `~WtDtRunner`，dumper 已由 `~ExpDumper` 释放；`runs/asan_dtporter_base_WriterReleaseReachesLiveExtDumpers`，红控）。只覆盖 legacy（隔离）writer；带 V1 表、可证明排空的 writer 释放路径由 dt-writer 的用例覆盖，本包未单独构造。

### 9.5 默认 `-Werror` 覆盖率复编与 lane 复跑

NEW-044/NEW-047 产品修改后在 `/tmp/wt-i03-s5-dt-parsers-cov` 默认 `-Werror` 增量重编（`CoverageTests_dtservo/dtcore/m_QuoteFactory/i03_dtporter_start`，exit 0），复跑：R18/R44 33 例 0 失败（2 disabled）、`m_QuoteFactory` 24 例 0 失败（4 disabled）、`i03_dtporter_start` 6/6、完整 `CoverageTests_dtservo`（同目录运行）125 例 0 失败（5 disabled），见 `runs/lane9_*`，二进制哈希 `hashes/coverage-binaries-cur6-sha256.txt`。宿主夹具普通构建当前 dtcore 10/10、servo 10/10、dtporter 4/4。

### 9.6 I03-NEW-061：R130 parser 夹具补 V2 表

`src/TestUnits/fixtures/r130_parser_lifecycle.cpp` 新增 `getParserLifecycleApiV2`：preflight/request_stop/can_wait 返回 OK；`wait_drained` 仅在测试显式调用 `r130_parser_prove_quiescent()` 后返回 DRAINED，否则 PENDING；`destroy_api` 在模块内完成最终 `IParserApi::release()`（计入 release/unsafe 计数）与删除（计入 delete 计数）。原测试意图（仅在显式静默证明之后 release 与 delete 各一次；init=false 不订阅、不连接，owner 保留到证明）保持不变，计数含义不变。复跑结果见 9.8。UFT 侧（`m_WtUftRunner` r95）由 uftcore 包复跑。

### 9.7 BUG-R42-003：R42 parser 替身改为 V2

`p02_wtcore_engines2/r42_parser_adapter_test.cpp` 的 7 个生命周期用例原以 `setRemover` 注入删除函数，现改为 `ParserAdapterTestAccess::setLifecycle` 给 initExt 替身挂进程内 V2 表（文件头注释写明映射）：`prove_callbacks_drained()` 对应 DRAINED；原“删除函数抛异常”对应 `destroy_api` 返回 ERROR（永久隔离、不重试）；原“删除函数延后”对应 `wait_drained` PENDING（保留、可重试）。两处断言按卡9a 改写并保留修改前副本：忙碌时转入的候选 API 无 V2 证明→保留（不 release/不销毁，`hasApi()` 保持 true）；延后用例第一轮 provider release 尚未发生（V2 下 release 在 DRAINED 后的 `destroy_api` 内），第二轮各一次。其余断言未改。复跑结果见 9.8。

### 9.8 WtCore 侧复跑

在默认 `-Werror` 覆盖率构建（`R130ParserLifecycle`、`CoverageTests_wtcore`、`CoverageTests_m_WtRunner` 构建 exit 0；二进制快照与哈希 `hashes/wtcore-binaries-sha256.txt`）上：

| 运行 | 结果 |
|---|---|
| `R42ParserAdapterTest.*`（`runs/lane_wtcore_r42`） | 11/11 通过（替身更新前同一产品下 4/11，见 `evidence/20261104-s5-engine-lifecycle/README.md`） |
| `R130MiscTail.Parser*`（`runs/lane_wtcore_r130_parser`） | 5/5 通过 |
| 红控：把构建目录中的 R130 模块临时换成修改前夹具编译的模块（无 V2 表），同一测试二进制（`runs/lane_wtcore_r130_parser_oldfixture`） | 3/5 失败（成功 init / init=false / 订阅连接三例被“缺 V2”拒绝），运行后已恢复新模块并核对哈希 |
| `WtRunnerParserInitFailure.*`（m_WtRunner，`runs/lane_m_WtRunner_parser_init`） | 1/1 失败，但与本夹具无关：lldb 证实 `WtRunner::config()` 在到达 `initParsers` 之前就返回 false（create 计数 0），属 runner 包配置前置条件漂移（新发现 F） |

### 9.9 增量逐项建议状态（覆盖第 4 节对应行）

| 条目 | 建议 | 依据 / 剩余 |
|---|---|---|
| BUG-R5-007 | 本地验证通过 | WtDtPorter `shutdown()` 运行时用例（监控运行中，析构/shutdown 先停 parser，幂等，不可证明时保留）普通/TSan 3×/ASan 通过，旧代码红控 SIGSEGV；StateMonitor 多实例与 writer 回调模式 TSan/ASan 通过。真实 writer/provider 为 X |
| BUG-R22-002 | 本地验证通过 | 并发 run/stop、重复 stop、多实例 lastTime、状态并发查询（15/15 TSan）与 self-join、外部 join 中回调 stop、writer 异常（9/9 TSan、3/3 ASan+LSan）均在当前树通过；时区 99 哨兵属 date-session 条目（不在本项剩余中闭合） |
| I03-NEW-044 | 本地验证通过 | 9.3 红绿 |
| I03-NEW-047 | 本地验证通过（legacy writer 路径） | 9.4 ASan 红绿；V1 可证明排空 writer 路径由 dt-writer 覆盖 |
| I03-NEW-061 | 本地验证通过（WtCore 侧） | R130 Parser* 5/5，旧夹具模块红控 3/5 失败；UFT 侧由 uftcore 包复跑 |
| BUG-R42-003 | 本地验证通过 | R42 11/11（原验收：manager.release 后 adapter->run 为 false 且不 connect、重复 release 安全、release 前只连接一次、map 清空，见 `ManagerReleaseStopsRetainedSuccessfulAdapter`）；产品侧由 WtCore ParserAdapter V2 生命周期承接（排空证明后 destroy_api 清空句柄，生命周期互斥 try_lock 使重入返回 busy）。真实 provider（X）、并发 run/release 未单独压测 |

### 9.10 增量新发现

- **F：m_WtRunner `WtRunnerParserInitFailure.PublicConfigFailsAndQuarantinesUninitializedParser` 前置条件漂移**。`WtRunner::config()` 在到达 `initParsers` 之前返回 false（parser create/register/init 计数均为 0），与 R130 夹具无关；疑为 runner 包新增配置校验（`basefiles:{}`、data store 等）使测试配置不再能走到 parser 初始化。建议 runner owner 更新该测试配置后复跑（WtRunner 不属本包，未改）。
- 本会话未删除任何非本子任务创建的目录或文件；删除仅限本子任务自己的运行目录 `/tmp/wt-i03-s5-dt-parsers-run/<label>`（每次运行前重建）、证据目录内本子任务生成的 `runs/<label>` 子目录、4 个误命名运行目录（同属本子任务）、`/tmp/wt-i03-s5-dt-parsers-asan-chain.done` 与 `/tmp/wt-i03-s5-dt-parsers-build-r130.log` 标记文件。中间修改前副本已复制到 `intermediate/`。
