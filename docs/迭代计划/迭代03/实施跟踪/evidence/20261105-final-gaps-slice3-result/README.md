# 第 3 段本地缺口修复（slice3-result），2026-11-05

范围：第 3 段只读复核（`scratchpad/review/slice3-result.json`）中标为 C_LOCAL_GAP 的条目：BUG-R13-004、BUG-N05-003，以及新增问题 I03-NEW-055、063、072、077。T3-015 是 E 类裁定项，本次没有实现。本目录只记录本段的工作。执行跟踪、执行记录、新增问题等台账由主代理统一更新。

环境：macOS arm64，Apple clang 17，Debug + `WT_ENABLE_COVERAGE=ON`，HEAD `ccaad010`，工作区含其他代理未提交的改动。没有运行 sanitizer：需要 sanitizer 的用例列在文末，由主代理在冻结树上运行。

## 构建与运行方式（三棵树）

| 树 | 源 | 选项 | 用途 |
|---|---|---|---|
| 主构建 `scratchpad/slice3-result-build` | 仓库 `src/` | 默认 `-Werror`（`WT_WARNINGS_AS_ERRORS=ON`） | 绿控：execv2、uftcore、m_WtRunner；本段全部新改 TU 的 `-Werror` 编译检查（`logs/build-werror-objects.log.gz`，0 告警） |
| 快照树 `scratchpad/slice3-red-src` + `slice3-red-build` | 仓库 `src/`、`dist/common` 用 `rsync -a --no-links` 复制的物理副本，无符号链接 | `-DWT_WARNINGS_AS_ERRORS=OFF`：快照里含其他代理尚在修改、当时无法编译的文件 | 先作红控：按 `scripts/red_patch.py` 定向回退产品改动；再作绿控：重新同步仓库 `src/`，把同步后 mtime 较早的文件 touch 后增量重建（清单见 `logs/build-green-touched-files.txt.gz`） |
| 主代理冻结树 build-cov（`a64516e7…/scratchpad/build-cov`） | 只读使用 | — | I03-NEW-055、077 的改前红控二进制（CoverageTests_uftcore sha256 `07b05005…62ce`） |

快照树中只为能够编译，处理过以下其他代理的在制品文件。这些处理不在仓库里，不影响本段结论：

- 移出 `q01_wtcore3/s1_engine_run_guard_test.cpp`（红控阶段）和 `s4_k26_lifecycle_test.cpp`：二者在当时的仓库中编译失败。
- 在 `n01_wtcore_tradepaths/s4_parser_adapter_null_comm_test.cpp` 中删掉析构函数上的 `override`：`IBaseDataMgr` 没有虚析构，原写法无法编译。

被移出文件的副本保存在 `logs/red-tree-excluded/`。

主构建中的 wtcore、porter 目标同样被其他代理的在制品挡住（`s4_parser_adapter_null_comm_test.cpp`、`q06_porter/i03_s2_runner_teardown_test.cpp` 在 `-Werror` 下报错），所以这两个二进制的绿控在快照树上运行。本段新改的 TU 已在主构建 `-Werror` 下单独编译通过。

运行脚本：`scripts/rt.sh`（定向 filter，日志和 XML 写入 `logs/<tree>-<name>.*`，命令记录在 `logs/commands.txt`）、`scripts/percase.py`（按用例逐个起进程，每例独立 cwd，4 路并行，汇总写入 `logs/*-percase-summary.json`）。

## BUG-R13-004（K27=A：w/m/y/min 触发正控 + 公开 API 端到端）

- 缺口：实盘 WtSelEngine 对 w/m/y/min 只有“不触发”用例。K27 对引擎的三处改动（min 周期不再要求 `_time==nextTime`，min 周期 session 判空，月任务改为 `%100`）没有正控和红控。另缺“公开 `create_sel_context` → 调度 → `on_schedule`”的端到端用例。
- 修改：只新增测试，没有改产品代码。
  - `src/TestUnits/coverage/q01_wtcore3/s3_k27_sel_period_fire_test.cpp`，`CoverageTests_wtcore::S3K27SelPeriodFire.*`，6 例：周任务、月任务、月任务节假日顺延、年任务（MMDD）、min 间隔整除触发（同一分钟不重复）、缺 session 的 min 任务不触发且不崩溃（子进程）。
  - `src/TestUnits/coverage/q06_porter/s3_r13_sel_public_schedule_test.cpp`，`CoverageTests_porter::S3R13SelPublicSchedule.*`，2 例：
    - 经 C 导出 `create_sel_context` 按文档创建 d/w/m/y/min 周期，驱动 `WtSelEngine::on_minute_end`，宿主 SEL calc 回调对命中周期各收到一次，未命中周期不收到；
    - 别名 d1/w1/m1、HHMMSS（93000）和未知周期返回 0 且不登记任务。
- 红控（补测，在快照副本中定向回退 K27）：回退引擎三处改动和 `createSelContext` 的 `invalidReason` 校验。
  - `logs/red-k27-fire.*`：4/6 失败，即两例月任务、两例 min。周任务和年任务在 K27 中未改，作为正控通过。缺 session 一例的子进程在空 session 处崩溃，被判为失败。
  - `logs/red-r13-public.*`：2/2 失败。月任务和 min 不触发；HHMMSS 输入返回非零句柄 3008。
- 绿控：
  - `logs/snapgreen-k27-fire.*` 6/6，`logs/snapgreen-r13-public.*` 2/2。
  - 同进程重复：`snapgreen-s3-all-repeat10` 10 轮全过，`snapgreen-r13-public-repeat10` 10 轮全过。
- 相关既有用例：快照树 wtcore 逐例 1045 过；porter 逐例 269/269（见文末“整体复跑”）。
- 仍未覆盖：Windows/MSVC。

## BUG-N05-003（K41 加载期诊断正反控）

- 缺口：`WtExecuterFactory::loadFactories`（`src/WtCore/WtExecuterFactory.cpp:1359-1364`）对未声明 `WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY` 的 V2 插件记 WARN，但没有任何用例覆盖。
- 修改（只改测试）：
  - `src/TestUnits/coverage/v2_exec_plugin/fake_exec_plugin.cpp`：新增 `COV_EXEC_PLUGIN_MODE == 28`，在能力位上加 HOST_ORDER_IDENTITY。
  - `src/TestUnits/coverage/CMakeLists.txt`：新增夹具插件 `CovExecV2HostIdentity`（host_identity，mode 28）；把新测试加入 `CoverageTests_execv2`，并加入编译定义 `WT_COV_EXEC_V2_HOST_IDENTITY_PATH`、依赖项和 `MOVED_SRCS`。
  - 新用例 `src/TestUnits/coverage/v2_exec_plugin/s3_k41_identity_diagnostic_test.cpp`，`CoverageTests_execv2::S3K41IdentityDiagnostic.*`，2 例：
    - 未声明能力的插件加载后仍能创建单元，K41 WARN 恰好 1 次，其他级别不出现该文本；
    - 声明能力的插件不出现该诊断。
- 红控：快照副本中把诊断条件改为 `if (false)`，`logs/red-k41.*` 1/2 失败（WARN 0 次）。声明能力那一例作为正控通过。
- 绿控：
  - 主构建 `-Werror`：`logs/green-k41.*` 2/2，`green-k41-repeat10` 10 轮全过；execv2 全体同进程 55/55（`green-execv2-all.*`），逐例 55/55。
  - 快照树：`snapgreen-k41.*` 2/2。
- 仍未覆盖（外部条件）：真实 provider 重启和跨进程身份、硬断电、Windows 审计日志。WP1 夹具 `src/TestUnits/fixtures/i03_wp1_identity_gate_20261104` 本次未运行，留给主代理在冻结树上跑。

## I03-NEW-055（隔离 CoverageTests_uftcore 链接集）

- 缺口：CoverageTests_uftcore 走通用分支，链接了 `WT_COV_SHARED_LIBS`，其中 WtExecMon/WtPorter/WtBtPorter/WtDtPorter/WtDtServo 等 DSO 内嵌 WtCore 等静态核心，默认可见性。
- 红控（静态 + 运行时绑定，冻结树二进制）：
  - `logs/n055-red-otool-L.txt` 显示 16 个本地 DSO 被链接。
  - 用 `DYLD_PRINT_BINDINGS=1` 运行 `R11TraderDeep.adapterMgrAddGetRunRelease`，汇总见 `logs/n055-red-crossbind-summary.txt`，完整日志为 `n055-red-dyld-bindings.log.gz`：
    - 涉及 wtp:: 符号的跨镜像弱绑定共 2440 处；
    - 其中 libWtExecMon、libWtPorter 各有 189 处 `wtp::TraderAdapter`（如 `getContract`、`getUndoneQty`、`printPosition`）绑定到本测试二进制里 WtUftCore 的同名定义。也就是说，同一进程中两个布局不同的 `wtp::TraderAdapter` 被 dyld 合并。
- 修改：只改测试 CMake。`src/TestUnits/coverage/CMakeLists.txt` 的分派循环为 `uftcore` 新增分支，只链接 `GTest::gtest_main Threads::Threads WTSTools WTSUtils boost_filesystem boost_thread ${ICONV_LIBRARY}` 和 `CMAKE_DL_LIBS`，与 m_WtUftRunner 的做法一致。没有改产品内联成员：隔离链接集之后，这些 DSO 不再进入该进程，冲突也就不存在。
- 绿控：
  - `logs/n055-green-otool-L.txt`：只剩 boost、iconv 和系统库。
  - `logs/n055-green-crossbind-summary.txt`：跨镜像 wtp:: 绑定 0 处。
  - uftcore 逐例 307/307（`logs/green-uftcore-percase-summary.json`）。
- 附记：同进程整体运行 uftcore 时 q04_uftctx_local 11 例、R133ShareManagerFork 1 例失败。改前的冻结树二进制同进程运行时失败集合完全相同（`logs/red-uftcore-single-process.log`、`logs/green-uftcore-all.log`），是既有的同进程顺序问题，与本改动无关。另 2 例 I03New112* 是其他代理新加的用例。
- 仍未覆盖：Linux/Windows 的符号可见性规则不同，本次未验证。

## I03-NEW-063（实盘 SEL 增量开仓费正控 + 红控）

- 缺口：`SelStraBaseCtx::do_set_position` 已改为按增量收费（`src/WtCore/SelStraBaseCtx.cpp:982-987`），但没有用例。
- 修改（只加测试）：新增 `src/TestUnits/coverage/n02_wtcore_engines/s3_new063_sel_open_fee_test.cpp`，`CoverageTests_wtcore::S3New063SelOpenFee.SameDirectionAddIsChargedOnTheOpenedIncrement`。使用版本化品种费率 2 元/手：先开 3 手，资金费用为 6；再同向加到 5 手，费用为 10，即只按新增 2 手收费；同时 `fee_error_count` 为 0。
  - 注：曾在锁表登记 `q01_wtcore3/s3_new063_sel_open_fee_test.cpp`，实际文件放在 n02 lane，q01 下的同名文件未创建。
- 红控：快照副本中把该行的 `abs(diff)` 改回 `abs(qty)`，`logs/red-new063.*` 失败：实际 16，期望 10。
- 绿控：`logs/snapgreen-new063.*` 1/1，同进程重复 10 轮全过。

## I03-NEW-072（Diff 四个回调在单元创建在途时不阻塞）

- 缺口：`on_tick`、`on_trade`、`on_order`、`on_entrust` 调用 `getUnit(code, false)` 时，`waitForCreation` 默认为 true。同一代码的单元正在创建时，适配器回调线程会被阻塞。
- 先写测试：`src/TestUnits/coverage/n01_wtcore_tradepaths/s3_new072_diff_callbacks_nonblocking_test.cpp`，`CoverageTests_wtcore::S3New072DiffCallbacks.*`，2 例。
  - 用真实 WtExeFact 的 WtDiffMinImpactExeUnit，阻塞桩把它的初始化卡在第一次商品查询上。
  - feeder 线程依次投递四个回调，要求 500 ms 内全部返回。
  - 成功例：放行后状态为 READY；排队的 tick 在目标恢复之后到达单元，恰好下单 1 次；quiesce 完成。
  - 失败例：放行时让商品查询返回空，初始化失败；回调同样及时返回，排队事件被丢弃，WARN 恰好 1 次，不下单，quiesce 不完整。
- 红控（改前产品，快照树源码与 `before/` 中 WtDiffExecuter 逐字节一致）：`logs/red-new072.*` 2/2 失败，两例都是回调等待创建，失败例另缺 WARN。单例分别运行见 `red-new072-ready-alone`、`red-new072-failed-alone`。
- 产品修改：按卡 9d=A 实现，事件先排队，READY 后按序投递，FAILED 时丢弃并告警。
  - `src/WtCore/WtDiffExecuter.h`（CRLF 保持）：`UnitLifecycleState` 新增 `deferred_events`；新增私有函数 `deferPendingUnitCallback`。只扩展私有嵌套结构，不涉及插件 ABI。
  - `src/WtCore/WtDiffExecuter.cpp`：
    - `deferPendingUnitCallback`：持 `_mtx_units` 和 state 锁检查。单元未登记时，事件存入 `deferred_events`；已登记但未 READY 时，事件直接进入创建者在发布 READY 前会排空的 `callback_events`；创建已完成则返回 false，由调用方重新查找单元。
    - `registerCreatedUnit`：把 `deferred_events` 按到达顺序排在 channel_ready/recover 回调之后。
    - `getUnit` 中的 `failCreation`：清空两类队列；若丢弃数不为 0，置 `_quiesce_incomplete` 并记一次 WARN。
    - 四个回调改用 `getUnit(code, false, false, &pending)`，pending 时调用 `deferPendingUnitCallback`。`on_trade` 仍立即记入 diff 账本，只把单元通知排队。
- 绿控：
  - `logs/snapgreen-new072.*` 2/2；`snapgreen-new072-repeat20` 20 轮全过。
  - 为保证可重复运行，绿控前对用例做了两处调整，断言没有变：执行器名改为每次运行唯一，避免上一轮的 Diff 快照在 init 时直接建出单元；TearDown 中先 quiesce 再析构。这两处调整之前的用例版本已有红控结果。
- 相关既有用例（快照树，均包含本修复）：
  - wtcore 逐例 1045 过、12 败、1 跳过：
    - 2 例 R59Exec.diff_stale_target_* 需要 `WT_COV_PLUGIN_BIN`，1 例 S5MainK31DateBinding 需要 ParserXeleSkt 模块；补齐环境后 3/3 通过（`logs/snapgreen-r59-k31-env.*`）。
    - 其余 9 例都是其他代理在制品新加的 S4OwnerRelease×4、S4ParserAdapterNullComm×4、R66Fixture.S4EnginePushQuoteWithoutContractSkipsHotFanOut，不涉及 Diff 执行器。
    - 跳过 1 例是卡 15 的符号链接用例。
    - Diff 相关的 R71ExecTest、K06、n02 diff、R10ExecuterTest、I03New014 全部通过。
  - exefact 逐例 418/418，porter 逐例 269/269，m_WtRunner 逐例 105 过、2 跳过（R42 需 `R42_HFT_PLUGIN`）。
- 仍未覆盖：TSan/ASan（见文末）；真实插件和外部 provider；创建期间另一线程又开新代（`bAutoCreate`）时的极端交错只做了静态分析。

## I03-NEW-077（同进程测试隔离）

- 红控（冻结树二进制，单进程运行）：
  - m_WtRunner `r28_*:R28*`，按回归工具做法预置模块，排除自行安装模块的 `real_cta_factory…` 一例：`router_sidecar_uses_preflight_snapshot_after_file_removed` 失败（`logs/red-n077-r28-inprocess.log`）；`--gtest_repeat=5` 每轮都失败（`red-n077-r28-inprocess-repeat5.log`）。
  - 根因：`WTSLogger::init` 每个进程只生效一次。前面的用例已把日志初始化为 error 级，本例依赖的 info 级 “Data manager initialized” 被过滤，导致观察器从未触发。
- 修改（只改测试）：`src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp` 中该用例改为 threadsafe 死亡测试。子进程重新执行测试二进制，自己的 logcfg 成为进程内第一份；用例主体和全部断言不变，子进程只有在没有任何断言失败时才以 0 退出。
- 绿控：
  - 单进程整组 77/77（`logs/green-n077-r28-inprocess.*`）；单例 1/1（`green-n077-sidecar-single.*`）；`real_cta_factory…` 在不预置模块时单例 1/1；m_WtRunner 逐例 105 过。
  - `--gtest_repeat=5` 下该用例 5 轮全过。
- 未修、另行登记：
  - `r28_wtrunner_entry.help*` 5 例在 `--gtest_repeat` 的第 2 轮起，fork 子进程以 SIGABRT 结束。改前与改后表现相同，不在 077 所列子项内，临时编号 S3-NEW-2。
  - porter `ExternalReleaseDoesNotWaitForInFlightProviderSnapshot`：冻结树单例重复 300 次、`ExecUnitStatusApiV2Fixture.*` 整组重复 20 次均未复现（`logs/red-n077-porter-*.log`）。没有红控，按规则未改。
  - 077 所列其余子项（n01、R28ParserCommands、r95_uft_parser）在冻结树上同进程重复运行均未复现。
- 新发现（临时编号 S3-NEW-1）：冻结树 CoverageTests_porter 整个二进制在单进程中不能完整运行：`R35PorterFeed.InitPorterStampsInstDirOnce…` 失败，之后在 `R82PorterCapiReal.CtaEntriesAndSliceMisses` 处 `libc++abi: terminating`（`logs/red-porter-whole-binary-single-process.log.gz`）。porter 单例为进程级单例，用例设计上按每例独立进程运行；回归工具逐例运行全部通过。需要另行裁定是否要求可单进程运行。

## 需要主代理在 sanitizer 树上运行

| 二进制 | filter | 期望 |
|---|---|---|
| CoverageTests_wtcore | `S3New072DiffCallbacks.*` | TSan 无数据竞争报告；ASan+UBSan 无报告（多线程回调排队与创建者排空） |
| CoverageTests_wtcore | `S3K27SelPeriodFire.*:S3New063SelOpenFee.*` | ASan+UBSan 无报告 |
| CoverageTests_porter | `S3R13SelPublicSchedule.*` | ASan+UBSan 无报告 |
| CoverageTests_execv2 | `S3K41IdentityDiagnostic.*` | ASan+UBSan 无报告；LSan 无泄漏（插件加载与卸载） |
| CoverageTests_wtcore | `R71ExecTest.*:*Diff*` | TSan、ASan 与改前一致，无新增报告 |
| CoverageTests_m_WtRunner | `r28_wtrunner_config.router_sidecar_uses_preflight_snapshot_after_file_removed` | ASan 下子进程退出 0 |

## 锁表说明

- 本段在 `review/locks.txt` 登记的文件见该表的 slice3-result 行。
- 其中 `src/__probe_nonexistent__` 一行是查询时误用 lock.py 登记的，不对应任何文件；主代理已删除该行。
- `src/TestUnits/coverage/q01_wtcore3/s3_new063_sel_open_fee_test.cpp` 已登记但未创建。

## 小节：DISABLED_ 用例处置

224 个 DISABLED_ 用例的逐例分类、启用改动与 20 次运行证据见 [disabled/README.md](disabled/README.md)。

## 小节：后续（例 195、I03-NEW-130、I03-NEW-131）

例 195 启用、ParserCTPMini 错误应答谓词修复（I03-NEW-130）、TraderCTP 结算单无载荷末帧修复（I03-NEW-131）的改动、红绿对照与相关 lane 逐例结果见 [followup/README.md](followup/README.md)。
