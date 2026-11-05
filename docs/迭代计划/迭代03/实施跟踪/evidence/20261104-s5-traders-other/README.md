# 2026-11-04 s5 子任务 traders-other（TraderFemas / XTP / YD / Mocker，卡9）

macOS arm64，HEAD `ccaad010`（`HEAD.txt`），工作区含大量他人未提交改动。本目录只记录本子任务的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商 SDK 或最终整合树。未修改任何跟踪文件；未创建符号链接（`find -type l` 为 0）；未运行 git 改写命令。

改前副本：`before/`（保留相对路径，`before.sha256`）。改后哈希：`hashes/after-src.sha256`。复现脚本：`src/TestUnits/fixtures/i03_traders_other_20261104s5/`（`run.sh`、`make_cases.py`、`link_only.py`、`red_overlay_build.py`）。

## 构建

- 全新覆盖率构建目录 `/tmp/wt-i03-s5-traders-other-cov`（`-DWT_ENABLE_COVERAGE=ON`，默认 `WT_WARNINGS_AS_ERRORS=ON`）。改前（= 子任务开始时工作区）完整构建 `CoverageTests_t_TraderFemas/Mocker/XTP/YD` exit 0（`logs/build-base.log`），二进制快照与哈希 `hashes/base-binaries.sha256`；当时本包产品源哈希 `hashes/base-owned-src.sha256` 与 `before/` 一致（TraderFemas.cpp `5c90ac11…`、TraderMocker.h `30c5d9c2…`、TraderMocker.cpp `dc790cc5…`，与 20261104-leaks-main-review 记录相同）。
- 改后：再次完整构建在他包进行中的文件上失败（`WtDtCore/DataManager.cpp` 成员未声明、`WtCore/WtArbiExecuter.cpp` 使用不完整类型 `TraderAdapter`，`logs/build-full-attempt-failed-WtArbiExecuter.log`），与本包无关。因此只用 ninja 编译本包三个目标的全部对象（同一 `-Wall -Wextra -Werror` 命令，0 告警，`logs/build-objects-fix.log`），再用 `link_only.py` 执行 ninja 记录的原链接命令（复用构建目录中已有依赖库，哈希 `hashes/link-deps.sha256`，`logs/link-fix*.json`）。TraderXTP 未改，沿用改前二进制。
- 红控：`red_overlay_build.py` 把 `src` 无链接复制为覆盖树，用 `before/` 中改前产品文件覆盖，按同一编译/链接命令构建（Femas 加 `-DWT_T4003_BASELINE`，不调用改前不存在的接口）。**第一次覆盖构建作废**：命令中的裸 `-isystem <src>` 未改写，测试 TU 解析到新头而产品 TU 用旧头（lldb 证实 `m_funcCreator` 偏移不一致），对应结果改名 `*-INVALID-header-mismatch`、`results-*-red-INVALID.json`，不计；修正脚本（未改写即中止）后重建为 `red2`（`logs/red-build2.log`、`red-build3.log`）。
- 二进制哈希：`hashes/fix-red-san-binaries.sha256`。另：首次 `--gtest_list_tests` 误在仓库根目录运行，生成并随即删除了自己产生的 `default.profraw`（13:33）；此后所有运行均在 `/tmp/wt-i03-s5-traders-other-run/...` 独立 cwd，`LLVM_PROFILE_FILE` 指向临时目录。

## 运行结果（每例独立进程/cwd，perl alarm，`runs/results-*.json`，逐例 `runs/cases/<组>/<用例>/output.log|result.xml`）

| 组 | 二进制 | 结果 |
|---|---|---|
| T4-002 原定向 7 例（FemasContractFixture） | 改前完整目标 / 改后 | 7/7、7/7 |
| T6-009 原定向 10 例（含 `t6_init.wtcore_init_ext_rejects_tradermocker_null_config`，完整目标链接 WtCore） | 改前 / 改后 | 改前 9/10：`DestructorAloneReturnsPendingOrderRefs` 一次读到引用计数 2（期望 3）；改后 10/10 |
| 同上 DestructorAlone 改前重复 30 次 | 改前 | 30/30 通过（`runs/t6009-destructoralone-base-x30`）→ 判定为测试竞态：`onPushOrder` 在 handler 把订单加入 `_orders/_awaits` 之前投递。测试加 `queryAccount` 屏障，断言值 3 不变 |
| R60-003 R60MockerTest 11 例（9c 改写的两例、回调内 release、端口/fd、准入、重复 login/connect） | 改前 / 改后 | 11/11、11/11 |
| R60-001 新增 4 例 | 红控 red2 / 改后 | 红 1/4 通过（3 例失败，见下）；绿 4/4 |
| T4-003 新增 9 例 | 红控 red2 / 改后 | 红 0/9（9 例全部失败，见下）；绿 9/9 |
| R93-006 4 例 | 红控 red2 / 改后 | 红 1/4（平今控制通过）；绿 4/4 |
| lane 全量：Femas / Mocker / YD / XTP | 改后（Femas 为最终二进制 `lane2-fix`；XTP 为未改二进制） | 73+3 disabled / 75+5 / 43+2 / 48+3，0 失败 |
| 稳定性：T4003 ×10、R60SessionIsolation+DestructorAlone ×10（`--gtest_repeat`） | 最终二进制 | 90/90、50/50 |

红控失败形态（子进程退出码见测试 `ChildCode`）：

- T4-003：release 后 worker 仍可 join，delete → `std::terminate`（86）；空闲后查询永不执行（7）；查询/下单在 SDK 调用中阻塞时 release 提前返回并 `Release()`（8，两例）；回调内 release 在回调栈上 `Release()`（8）；worker 回调内 `deleteTrader` → terminate（86）；重复 connect 再建 API/Init 两次（10）；release 后迟到回调仍投递（11）；回调未返回时 release 已 `Release()`（8）。
- R60-001：`onRspEntrust` 内 release 后同一 handler 仍投递 `onPushOrder` 与日志；撮合线程 `onPushOrder` 内 release 后仍投递同笔 `onPushTrade` 及第二张单成交（late=3、trade=2）；disconnect 后端口仍占用、查询/下单仍被接受并保留引用。`async_connect_login_then_immediate_release` 改前改后均通过：异步 connect/login 后立即 release 在改前实现中已不会在 release 返回后投递（作为正控记录，**无红控**；N01 早期 abort 未复现、无法归因）。
- R93-006：昨仓单项撤单恢复期望 5 实际 1；混合昨 2/今 3 平 4 冻结后今仓可用期望 1 实际 3、零成交撤单后昨仓可用期望 2 实际 0；两单各自来源：先撤 A 后昨/今可用期望 1/1 实际 0/2。

## 产品修改

- TraderFemas（BUG-T4-003/T4-002，卡9 9a=A+C、9b=A、9c=A、9d=A；最终源码 sha256 `ac8c64fe…`/头 `420fa317…`）：`m_bStopped` 原子化为准入闸；出站 SDK 调用（connect 建 API、login/doLogin/logout、下单/撤单、worker 上的查询、登录回调中的 `GetTradingDay`）持 `ApiLease`；21 个 SPI 回调入口 `CallbackScope` 计数并在停止后不投递；worker 常驻 work guard + `io_context::run()`；外部 `release()` 依次等出站 lease、join worker（线程在锁内发布）、`RegisterSpi(NULL)`、等回调计数归零（有界，默认 5000 ms），全部排空才 `Release()` 并置 `LS_RELEASED`，否则 `LS_UNQUIESCED`、保留 API；worker/回调线程内 `release()` 只请求停止；`disconnect()` 等同 `release()`；已连接/已释放实例拒绝 connect；析构完成 release，未排空或在自身线程析构则 `abort` 并提示用 `deleteTrader`；`deleteTrader` 在回调线程或未排空时 `detachForQuarantine()`、隔离实例到进程退出并 `dlopen(RTLD_NODELETE)` 钉住本模块（Windows 分支 `GetModuleHandleEx(PIN)` 未编译验证）；`lifecycleState()/isQuiesced()/isCallbackContext()` 供宿主观察；自有 strand 由 `unique_ptr` 释放。
- TraderMocker（BUG-R60-001，卡E + 9c/9d）：`get_listener()` 在停止请求后返回空（`_delivery_closed`），所有投递逐次重读；queryAccount/login 的 handler 也持 `_mtx_awaits`，使全部投递与撮合串行；撮合轮次在停止请求后于订单/成交片边界退出并仍保存已应用的持仓；`disconnect()` 共享 `release()` 的收尾（关端口、拒绝请求、实例终止，重连用新实例）。
- TraderYD（BUG-R93-006 本地可确定部分）：首次见到平仓单时按昨仓优先冻结并修正“剩余冻结今仓”比较方向，逐单记录冻结来源 `m_mapFrozen`；撤单按“未成交量先还今仓、再还昨仓且不超过本单冻结量”恢复，重复撤单回报不重复恢复；release 清空记录。

测试/夹具：新增 `t4_003_femas_lifecycle_test.cpp`、`r60_001_session_isolation_test.cpp`、YD 两例（混合例来自 `R93-006-20261102/mixed-test.patch`）；`femas_trader_test.cpp` 夹具 TearDown 清空 `kApiSentinel`（析构现会 release，哨兵不可解引用；未改断言）；`t6_009_mocker_lifecycle_test.cpp` DestructorAlone 加屏障；`i03_leaks_main_20261104/manual_build.sh` 增加可选 `WT_I03_BOOST_INCLUDE`、`WT_I03_FEMAS_EXTRA_TESTS`。

## sanitizer（Homebrew LLVM 22，`manual_build.sh fix <tsan|asan> <femas|mocker>`，根 `/tmp/wt-i03-s5-traders-other-san`，`WT_I03_BOOST_INCLUDE` 指向构建目录实体 Boost 桥，Femas 追加 `t4_003_femas_lifecycle_test.cpp`；日志 `logs/san/`，源码哈希 `logs/san/*-hashes.txt`；只链 gtest，不链 WtCore，故不含 t6_init WtCore 用例）

- 首轮 Femas TSan（`runs/cases/tsan-femas-pre-detach`）发现本次实现自身两处问题并已修正：① 回调线程 `deleteTrader` 隔离后 worker 未 join/detach → “thread leak”（新增 `detachForQuarantine()`）；② worker 在 `m_thrdWorker` 赋值前即运行 handler，handler 内 remover 读该成员 → data race（connect 在锁内发布线程、worker 入口屏障；release 先等出站 lease 再 join，并在锁内取线程指针）。
- 最终：Femas TSan 17 进程 17 通过（T4003 9 + T4-002 原定向 7 + 全组 48 例），0 报告；Femas ASan/UBSan/LSan 同 17/17，0 报告。Mocker TSan 定向 24/24（R60-001 4 + R60-003 11 + T6-009 9），0 报告；Mocker ASan/UBSan/LSan 定向 24/24 0 报告；Mocker lane（除 t6_init）74 例 0 失败但 LSan exit 23：泄漏为 `R214MockerBranches.UnregisteredOrderDoesNotTradeOnOtherContractTick` 测试体自建的 `zz2512` 合约/品种从未释放（测试侧，非产品；见新发现 5）。
- 最终产品源码重编后普通覆盖率二进制复跑：T4003 9/9、T4-002 7/7、Femas lane 73/73（3 disabled）；T4003 ×10 90/90，R60SessionIsolation+DestructorAlone ×10 50/50（`runs/repeat-x10`）。

## 未修改产品、仅审查的条目

- BUG-T4-015：模块为 `src/CTPOptLoader/TraderSpi.cpp:497-501`（`bIsLast` → `DumpToJson(); exit(0)`），不是 TraderFemas，也不在本包所有权内，未修改。既有 `R114OptFixture.dump_to_json_writes_two_files_and_exits_zero` 以死亡测试固化“末包 exit(0)”；错误末包同样走 exit(0)。仓内唯一宿主 `LoaderRunner` 以 `run(cfg,false,true)` 同步调用，`run` 在 `Join()` 上阻塞直到回调 `exit`；无仓内 `bAsync=true` 调用者。需由 loader 包修改：回调只记录完成/错误状态并通知，`run` 等待通知后在非回调线程 `RegisterSpi(NULL)`/`Release()` 并返回状态码，`LoaderRunner` 以返回值退出；`bAsync=true` 合同需裁定（见决策卡草稿 A）。
- I03-NEW-005（Femas 下单返回语义）：WP1 宿主机制（`TraderAdapter::doEntrust`、`AccountTradingGate`）只读核对结论——`orderInsert` 返回 `<0` 且未见在途回报即视为“确认未发送”并回滚、关闭意图；返回 `>=0` 视为已提交；只有抛异常才 `pauseAccount("order_insert", ...outcome unknown)` 且随后重抛；被关闭为 NOT_SENT 的 localid 之后若收到存活回报，`ledgerApplyOrderLocked` 对终态条目幂等忽略，但订单仍进入 `_orders` 并派发，**不会暂停账户**。ITraderApi 没有“提交状态不确定”的返回值。故 Femas 无法仅在适配器内以 fail-closed 方式表达“不确定”：返回负值会在 SDK 实际已发送时丢失账本占用且不暂停；抛异常跨插件边界且宿主重抛。维持现状（返回 0 = 按已提交保留占用、不重发）并保持阻塞。解除条件：取得 Femas 3.02 精确版本 `ReqOrderInsert` 返回码与非零后是否仍有 `OnRspOrderInsert/OnRtnOrder` 的合同，或裁定决策卡草稿 B。
- BUG-R93-005（XTP，只读审查 outbound API lease 与 reconnect gate）：所有公开出站入口（login/doLogin、logout、orderInsert、orderAction、四个查询）持 `ApiCallLease`；reconnect 用独占 lease 等在途调用后重建 API，`handoffToApiCall` 允许同步 `handleEvent→login`；worker 投递与重连定时器按 `_worker_generation` 过滤。缺口（未修，非本条可本地闭合）：① SDK 回调只有重入标记、无在途计数，`release()`/`reconnect()` 不等已进入的回调，`release()` 清空 `_orders/_positions/_trades` 可与回调并发；② `_sink` 非原子且 `OnOrderEvent` 错误分支、`OnCancelOrderError` 未判空，`registerSpi(NULL)` 后迟到回调空指针解引用；③ 重连代际用 worker 代而非 API 实例代，旧 API 迟到的 `OnDisconnected` 会对新 API 触发重登；④ `deleteTrader` 直接 delete，worker 线程上删除时 `stopWorker` 不能自 join，`std::thread` 析构可 terminate（无 9a/9b 隔离）；⑤ `init` 不检查模块/creator 加载失败且可重复（9c）。真实 SDK `Release`/`RegisterSpi(NULL)` 排空合同仍为 X。
- YD 生命周期核对（R93-006 附带）：`ydApi.h:141-150` 规定 `startDestroy` 异步、结束时依次回调 `notifyBeforeApiDestroy`/`notifyAfterApiDestroy`，“listener 应在此之后由用户删除”。现 `TraderYD::release()` 调 `startDestroy()` 后立即置空 API 返回，未覆写 `notifyAfterApiDestroy`、不等待；`deleteTrader` 随即删除作为 listener 的 TraderYD；查询 worker 只置 `m_bStopped`（非原子）不 join，析构为空，删除时 `std::thread` 仍可 join → terminate（现有测试以 `presetWorker()` 占位规避）。与头文件要求不一致，建议另立编号（见新发现）。
- BUG-R60-006（Z，卡15）：见条目结论。

## 新发现（编号由主线程分配）

1. TraderYD release/析构与 YD 头 `startDestroy`→`notifyAfterApiDestroy` 要求不一致，查询 worker 不 join（删除即 terminate 风险），见上。建议：release 在非回调线程等待 `notifyAfterApiDestroy`（有界）并 join worker，超时或回调线程删除则按 9a 隔离；需 YD fake 补正反控。
2. TraderYD `notifyTrade` 平仓（非 CM_CoverToday）`if (decimal::lt(qty, maxQty)) newQty -= (qty - maxQty);` 比较方向与冻结路径同样反向，平仓量超过昨仓时今仓总量不扣减（`TraderYD.cpp` notifyTrade）。不在 R93-006 冻结/回补范围，未改。
3. TraderXTP 缺口 ②③④⑤（见 R93-005），其中 ② 空指针与 ④ terminate 为明确产品缺陷，可在本地 fake 构造红控。
4. T6009 `DestructorAloneReturnsPendingOrderRefs` 的读取竞态（测试问题，已在本包修正）。
5. `t_TraderMocker/r214_mocker_branch_test.cpp:59` `make_unregistered_ct` 创建的合约与品种未释放（1280 字节，ASan lane LSan exit 23）；测试侧泄漏，未改，建议在用例末尾释放 `zz_ct->getCommInfo()` 与 `zz_ct`。

## 条目结论（建议，由主线程裁定与整合）

| 条目 | 建议状态 | 依据 | 剩余/解除条件 |
|---|---|---|---|
| BUG-T4-002 | 本地验证通过 | 完整 coverage 目标（非手工构建）改前、改后原定向 7/7；Femas lane 全量 0 失败；worker 常驻/strand 生命周期随 T4-003 协议闭合，ASan/UBSan/LSan 定向与 lane 0 报告 | 真实 Femas SDK、Windows、整体回归未验；错误终态/ID 映射另由 I03-NEW-005 等跟踪 |
| BUG-T6-009 | 本地验证通过 | 完整目标（链接 WtCore）原定向 10/10，含 `t6_init.wtcore_init_ext_rejects_tradermocker_null_config`；改前唯一失败为测试读取竞态（重复 30 次 0 失败，加屏障后通过，断言值不变）；Mocker lane 75/75（5 disabled），TSan 定向 24/24 | Windows、整体回归未验（TraderMocker 无真实 SDK） |
| BUG-T4-003 | 本地验证通过（本机 fake 范围） | 卡9 统一协议实现；9 例红控全失败（terminate/查询不执行/提前 Release/回调栈 Release/重复 connect/迟到投递）→ 改后 9/9、×10 稳定、ASan/UBSan/LSan 与 TSan 见 sanitizer 节 | 真实 Femas SDK `Release()` 后是否仍有回调（9a）为 X：改后对不可证明者隔离而非 delete；宿主 TraderAdapter 在 remover 后 `free_library` 的行为由模块自钉住缓解，宿主侧未改 |
| BUG-T4-015 | 阻塞 | 模块为 CTPOptLoader（非本包所有），未改；静态与既有死亡测试确认末包/错误末包 `exit(0)` | 由 loader 包按决策卡草稿 A 实施；需 fake API 异步成功/错误末包子进程正反控 |
| BUG-R60-001 | 本地验证通过 | E 合同：重连用新实例（9c），停止请求后零投递；红控 3/4 失败→绿 4/4；异步 connect/login 立即 release 为无红控正控；TSan 0 报告 | 无真实 SDK；整体回归未验 |
| BUG-R60-003 | 本地验证通过 | 9c/9d 改写的同实例两例、回调内 release 不 terminate 等 11 例在完整目标改前/改后均通过，TSan 11/11；“同实例第二轮/重启遗留查询”按 9c 合同为拒绝复活 | 整体回归未验 |
| BUG-R93-005 | 阻塞（X） | outbound lease 覆盖全部公开出站入口、reconnect 独占 gate 与代际过滤已审查；缺口 ①–⑤ 见上 | 真实 XTP SDK callback quiescence/延迟销毁合同；缺口 ②④ 建议另立编号修复 |
| BUG-R93-006 | 阻塞（部分本地修复） | 零成交逐单恢复（昨仓单项 5、混合 0/1→2/3、两单各自来源）红→绿，不依赖 TradeVolume 语义与多包时序 | 部分成交（`TradeVolume` 累计/增量）、首包即终态、成交与撤单回报乱序/重复仍需 YD 官方合同或实测；`notifyTrade` 平仓反向比较另立编号 |
| BUG-R60-006（Z） | 阻塞；处置建议“用户指示忽略” | TraderMocker `isConnected()` = 撮合已启动（`login()` 调用即为 true、早于 `onLoginResult` 投递；logout 为空操作不清；disconnect/release 清 false，已有用例覆盖）。仓内 7 个真实适配器均以 WS/TS_ALLREADY 表示 fully-ready，但 ITraderApi 未写合同、TraderAdapter 等宿主不调用 `ITraderApi::isConnected`，无可观察缺陷；改为 fully-ready 需改写 r214/T6009 断言 | 解除条件：ITraderApi 写明 isConnected 语义或出现消费者（决策卡草稿 C） |
| I03-NEW-005 | 阻塞 | 见上：宿主无“提交不确定”返回通道，NOT_SENT 后的迟到存活回报不暂停账户 | Femas 精确版本返回码/回报时序合同，或决策卡草稿 B |

## 决策卡草稿

- A（T4-015/T6-002 loader 异步合同）：`run(bAsync=true)` 返回后的完成/失败如何交付。选项：A1 废弃异步，`run` 始终等待完成后在非回调线程释放 API 并返回状态码（`bAsync` 仅保留签名）；A2 新增版本化导出 `run_v2`+`poll_status/wait`+`release`，旧 `run(bAsync=true)` 返回后绝不终止进程、结果只能经新接口获取；A3 维持现状。建议 A1（仓内唯一宿主为同步调用）。影响：BUG-T4-015、BUG-T6-002。
- B（ITraderApi 提交不确定）：新增版本化提交结果（已确认未发送 / 已提交 / 不确定）。不确定时宿主保留账本占用与身份、暂停账户、等待对账；同时宿主对 NOT_SENT 关闭后收到的存活回报暂停账户。建议采用并先实现宿主侧迟到回报暂停。影响：I03-NEW-005、XTP/CTP 同类“非零记录失败仍返回 0”。
- C（isConnected 语义）：C1 统一为 fully-ready（登录、确认、初始化完成后 true；logout/断线/登录失败/release 置 false），Mocker 在投递 `onLoginResult(true)` 前置 true；C2 不定义、按卡15 忽略。建议 C2（无消费者）。影响：BUG-R60-006。

## 第二轮（主线程追加：新发现 1–4，2026-11-04 晚）

改前副本 `before-round2/`（`before-round2.sha256`，即第一轮完成后的状态），补丁 `patches-round2/`。构建方式同上：只编本包 YD/XTP/Mocker 目标对象（-Werror 0 告警，含 `yd_r60_factory_fixture.dylib`）+ `link_only.py` 重链；红控由 `red_overlay_build.py`（新增 TraderXTP 覆盖与统一 `-DWT_S5_RED_BASELINE`）以 `before-round2` 构建（`/tmp/wt-i03-s5-traders-other-red2`）。

### 产品修改

- **TraderYD 停止/销毁（新发现 1，卡9 + `ydApi.h:141-150`）**：关闭准入后，外部 `release()` 有界（默认 2000 ms）依次等宿主出站调用 lease、查询 worker 退出并 join（线程在锁内发布）、已进入的 YD 回调，再调用一次 `startDestroy()` 并等待 `notifyAfterApiDestroy()`（新增覆写，不受闸门限制、计数），全部完成才置 `YS_RELEASED`、清空本地表、`m_pUserAPI=NULL`；超时置 `YS_DRAIN_UNCERTAIN`、不删除 listener。回调/worker 线程内 `release()` 只请求停止；`deleteTrader` 在回调线程或未静止时 detach worker、隔离并 `dlopen(RTLD_NOLOAD|RTLD_NODELETE)` 钉住模块；析构在自身线程或未静止时 abort。9 个交易回调计数并在停止后丢弃；connect 拒绝已有 API/已停止实例（9c）；`disconnect/login/logout/orderInsert/orderAction` 持 lease（顺带使 `orderAction` 在无 API 时返回 -1 而非解引用空指针）；`m_bStopped` 原子化，worker 在锁内取队首。
- **TraderYD `notifyTrade`（新发现 2）**：非 CM_CoverToday 平仓成交“先减昨仓、超出部分减今仓”，原比较 `lt(qty, maxQty)` 恒假，今仓总量从不扣减。依据同函数注释、同文件 notifyOrder 冻结路径（第一轮修正的同形比较）和撤单回补顺序可判定为本地算式缺陷，只依赖单笔成交 `YDTrade.Volume`，不涉及 `YDOrder.TradeVolume` 累计语义，已修。
- **TraderXTP（新发现 3 + R93-005 本机部分）**：`_sink` 改原子并在每次投递取本地副本，`OnOrderEvent` 错误分支与 `OnCancelOrderError` 判空；9 个 SDK 回调入口加 `CallbackGate` 计数，停止/替换 API 期间丢弃；`release()` 在 `RegisterSpi(NULL)` 后有界等待回调排空才 `Release()`，超时保留 API（`_retained_apis`）、`_drain_uncertain`、不清容器；`reconnect()` 替换旧 API 前同样等待，超时保留旧 API 并中止重连；被替换/释放 API 的登录会话记入 `_retired_sessions`，其迟到 `OnDisconnected` 及非当前会话的断线不再关闭/重登当前会话（会话号 0 无法归属，保持原行为）；`deleteTrader` 在 SDK 回调/API 调用/worker 线程上或未静止时隔离并钉住模块，析构在自身线程或未静止时 abort；worker 线程标记；`init` 空配置/模块加载失败/creator 缺失返回 false，已初始化或已有 API 的实例拒绝重复 init（9c）。真实 XTP SDK 的回调排空/延迟销毁仍为 X。
- **测试侧泄漏（新发现 4）**：`R214MockerBranches.UnregisteredOrderDoesNotTradeOnOtherContractTick` 末尾释放自建品种/合约（订单只持非拥有指针，线程已在用例内 join）。

### 测试改动（改写旧断言均写明理由，原文件在 `before-round2/`）

- 新增 `t_TraderYD/yd_destroy_lifecycle_test.cpp`（8 例，fork）、`R93YdTest.r93_mixed_close_trade_reduces_yesterday_then_today`、`t_TraderXTP/xtp_lifecycle_s5_test.cpp`（7 例，fork，含 1 例正控）。
- `r93_yd_api_stub.h`：`startDestroy()` 按 `ydApi.h` 向已 `start()` 的 listener 发 Before/AfterApiDestroy（默认同步；可选异步线程或永不发送）。`R93YdTest` 夹具注入 stub 时同时登记 listener；`connect_disconnect_release_drive_stub_lifecycle` 在 connect 前清除夹具注入的 API（9c：已有 API 时拒绝重复 connect），其余断言不变。
- XTP 三处旧断言 `ASSERT_TRUE(init(cfg))` 改为 `ASSERT_FALSE`（r93 夹具、t5 夹具、`init_defaults_flowdir_and_release_is_repeatable`）：本机无 XTP 厂商模块，按主线程指派“init 检查模块加载失败”与 Femas/YD 已有先例（缺库为显式初始化失败）现返回 false；配置在加载前已解析，用例继续注入 fake creator，未弱化其他断言。
- `manual_build.sh` 新增 `yd`、`xtp` 模块。

### 结果

| 组 | 红控（before-round2 覆盖构建） | 改后 |
|---|---|---|
| YD 生命周期 8 例 + 平仓成交 1 例 | 9/9 失败：release 先于 AfterApiDestroy 返回（8）、回调内 release 即 startDestroy（8）、并发回调中 startDestroy 重叠（8）、未收到销毁通知仍删除 listener（13）、worker 未 join 删除 terminate（86）、回调线程 deleteTrader terminate（86）、重复 connect 建两个 API（10）、release 后仍投递（11）；平仓后今仓 3（期望 1） | 9/9 |
| XTP s5 7 例 | 6/7 失败：sink 置空后错误回调 SIGSEGV、回调线程 deleteTrader terminate（86）、回调在途时 Release（8，两例）、旧会话迟到断线触发 WTE_Close 与重登（11）、缺模块 init 返回 true（12）；当前会话断线重登正控改前改后均通过 | 7/7 |
| R214 泄漏用例 | ASan lane LSan exit 23（第一轮记录） | 见 sanitizer |
| lane：Mocker / YD / XTP | — | 75+5 disabled / 52+2 / 55+3，0 失败 |

sanitizer（LLVM 22，`/tmp/wt-i03-s5-traders-other-san`，`logs/san/*-r2.log`、`*-r2-hashes.txt`）：

- TSan：YD 生命周期+平仓 9/9、XTP s5 7/7 + `R93XtpTest.*` 18/18，均 0 报告。
- ASan/UBSan/LSan：Mocker lane（除 t6_init）74 例 0 失败、LSan exit 0（第一轮为 23，R214 泄漏已消除）；YD 生命周期 8/8 0 报告；YD 平仓成交例与改前既有 `R93YdTest.open_cover_order_freezes_yesterday_available` 同样 LSan exit 23，泄漏均为 TraderYD 的 `m_mapOrders/m_mapPosition/m_mapTrades` 等结果容器从不释放（析构不释放，旧问题，见新发现 6）；XTP 全组除 `xtp_map.*` 外 45 例 0 失败，LSan exit 23，泄漏为 TraderXTP `_orders/_trades/_positions` 从不释放（同类旧问题，新发现 6）。`xtp_map.*` 在手工 -O1 构建中因内联映射函数未导出、`dynamic_lookup` 解析为空而 SEGV（构建工件，coverage 目标中该组通过），未计入。
- 最终普通覆盖率二进制哈希 `hashes/round2-binaries.sha256`，源码 `hashes/after-round2-src.sha256`。

### 第二轮新发现

6. TraderYD 与 TraderXTP 的结果容器（YD：`m_mapOrders/m_mapPosition/m_mapTrades/m_ayFunds`；XTP：`_orders/_trades/_positions`）只在 release 中 `clear()`，析构从不 `release()`，与 Femas 的 I03-NEW-016 同类；ASan 定向可见（72/48 字节级）。建议在已证明静止后的析构中各释放一次并补 LSan 用例。未改。
7. TraderXTP `OnOrderEvent` 错误分支在合约未登记时 `makeEntrust` 返回 NULL 后仍 `entrust->release()`（既有 DISABLED 用例记录），本轮只补了 sink 判空，未改该行。

## 第三轮（主线程追加：I03-NEW-067、I03-NEW-068）

改前副本 `before-round3/`（`before-round3.sha256`，即第二轮完成后的状态），补丁 `patches-round3/`，源码哈希 `hashes/after-round3-src.sha256`，二进制 `hashes/round3-binaries.sha256`。构建同前（本包 YD/XTP 对象 -Werror 0 告警 + `link_only.py`；`logs/round3-objs.log`、`logs/link-round3.json`）；sanitizer 由 `logs/round3_chain.sh` 重建（`logs/san/mb-*-asan-r3.log`、`*-r3-hashes.txt`）；红控覆盖构建用 `before-round3`（`logs/red-build-round3.log`）。

- I03-NEW-067：TraderYD 析构在已证明静止（`isQuiesced()`）后对 `m_mapOrders/m_mapPosition/m_mapTrades/m_ayFunds` 各 `release()` 一次并置空；TraderXTP 析构同样归还 `_positions/_trades/_orders`。`release()` 仍只 `clear()`（隔离实例不释放）。
  - YD（`R93YdTest.*:YdDestroyLifecycle.*:R60YdTest.*`，25 例，ASan/UBSan/LSan）：改前（第二轮 ASan 二进制）25/25 通过但 LSan exit 23（5024 字节/74 次分配，28 条 direct leak）→ 改后 25/25、exit 0、0 报告。
  - XTP（全组除 `xtp_map.*` 手工构建工件，同第二轮说明）：改前 45 例通过、LSan exit 23（352 字节/7 次分配）→ 改后 46 例（含下条启用用例）0 失败、exit 0、0 报告。
- I03-NEW-068：`OnOrderEvent` 错误分支 `makeEntrust` 返回 NULL（合约未登记）时不再 `entrust->release()`；错误仍以 `onRspEntrust(NULL, error)` 交给宿主（`TraderAdapter::onRspEntrust` 对 NULL entrust 记录并拒绝）。原 `DISABLED_order_event_error_branch_releases_null_entrust`（改前文本见 `before-round3/`）启用为 `order_event_error_branch_releases_null_entrust`：`EXPECT_EXIT` 子进程中调用（改前 SIGSEGV 判红后在父进程内不再调用，避免整进程崩溃），改后再在进程内断言一次 `onRspEntrust`、entrust 为 NULL、错误码 `WEC_ORDERINSERT` 与消息。红控（`before-round3` 覆盖构建）：子进程 “Terminated by signal 11”，1/1 失败；改后 1/1。
- 普通覆盖率二进制 lane：YD 52/52（2 disabled）、XTP 56/56（2 disabled，原 3 个 disabled 中 1 个已启用），0 失败（`runs/results-r3-fix.json`）。
- 运行与清理说明：本子任务只删除过自己创建的内容——仓库根目录 13:33 由本子任务误生成的 `default.profraw`；`/tmp/wt-i03-s5-traders-other-red/obj-*`、`CoverageTests_*`、`dbg/`（作废的第一次红控构建产物）；`/tmp/wt-i03-s5-traders-other-run/repeat/*.log|*.profraw`；本证据目录 `runs/` 的重建；`manual_build.sh` 对其自身输出目录 `/tmp/wt-i03-s5-traders-other-san/manual/<mod>-fix-<mode>` 的 `rm -rf`（旧版本先改名为 `-prev/-r1/-r2` 保留）。未删除任何他人目录。另：`/tmp/wt-i03-s5-traders-other-configure.log` 与 `-build0.log` 当前已不存在（非本子任务删除），其内容已在第一轮复制为 `logs/configure-base.log`、`logs/build-base.log`。
