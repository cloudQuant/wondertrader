# s5-btcore：回测引擎 / 回放器 / BtPorter / BtRunner（2026-11-04）

macOS arm64，HEAD `ccaad010`（工作区含大量他人未提交改动），见 `hashes/HEAD.txt`、`hashes/start-owned-sha256.txt`。本目录只记录本包定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商、真实 MQ 或最终整合树。未修改任何跟踪文件。所有构建/运行目录在 `/tmp/wt-i03-s5-btcore-*`，未创建符号链接（红树 `find -type l` = 0）。

## 构建

- 覆盖率构建（默认 `-Werror` 开）：`cmake -S src -B /tmp/wt-i03-s5-btcore-cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`；目标 `CoverageTests_btcore CoverageTests_btporter CoverageTests_m_WtBtRunner R160FakeWtMsgQue WtDataStorage CovExecV2Valid CovExecV2InitFail CovExecV2FactoryPreflightFalse`，`-j 3 -k 0`。
  - build1（13:36）：改前树、无本包新测试，二进制存 `/tmp/wt-i03-s5-btcore-bins/pre/`（“pre”）。
  - build2：他人进行中的 `WtDtCore/DataManager.cpp`（头/实现不匹配）编译失败，与本包无关（`logs/wt-i03-s5-btcore-cov-build2-red.log`）；build3 exit 0 → **红二进制**（含本包新测试，不含本包产品改动），`hashes/red-binaries-sha256.txt`。
  - build4 被会话中断终止（无 exit 行，不计）；build5/6/8 exit 0 → **最终二进制**；build9 只重编 r222 夹具修正后的 btporter。哈希 `hashes/final-binaries-sha256.txt`。产品代码在默认 `-Werror` 下编译通过。
  - 本机 `/tmp` 中本包的 ASan 构建目录、build1 日志、覆盖率 configure 日志在 18:4x 前后被外部删除（原因未知，可能是其他任务按通配清理）；ASan 树已重新配置。
- 独立红绿夹具 `src/TestUnits/fixtures/i03_btcore_s5_20261104s5/`：绿=工作区 `src`；红=`/tmp/wt-i03-s5-btcore-redsrc/src`（`rsync -a --no-links` 当前 src 后用 `before/` 的 9 个产品文件覆盖，`hashes/redsrc-wtbtcore-sha256.txt`）。红绿共用同一份 WTSTools/WTSUtils（build3 产物副本），只差 WtBtCore 产品源。
- R20 独立夹具 `p08_btcore_mockers3/r20_mq_fixture/`：红=从红树副本配置（新增模式调用 `release()`，旧头无此成员→编译失败，**静态红控**，`logs/wt-i03-s5-btcore-r20-red.build.log`）；绿=工作区。
- “改前”副本 `before/`（`hashes/before-edit-sha256.txt`，18:10 取自工作区，即红二进制所编译版本）。13:23 首次快照 `hashes/initial-snapshot-1323-sha256.txt` 之后，他人（卡4b 策略插件 V2、卡1 日期映射迁移）于 17:42 修改了 SelMocker/HftMocker/HisDataReplayer，本包在其基础上增量修改，前后哈希 `hashes/product-before-after.txt`，补丁 `patches/product.patch`。
- 过程失误（已纠正）：首次用 Python 文本模式改写导致 `SelMocker.cpp`、`HftMocker.cpp`、`WtBtPorter/WtBtRunner.cpp` 的 CRLF 被改成 LF（整文件差异）；已按改前文件逐行恢复原行尾（新增行用 CRLF），恢复后补丁仅含本包改动。build5 二进制由 LF 版编译，最终二进制（build6/8）由恢复后的文件编译，二者语义相同。
- 运行：每例独立进程与 cwd（`/tmp/wt-i03-s5-btcore-run/<组>/work/<用例>`），`perl alarm` 硬超时，`LLVM_PROFILE_FILE` 指向临时目录（`scripts/runcases.sh`）。各组 `results.txt`（用例 退出码）、日志、XML 见 `runs/`。

## 产品修改

| 条目 | 文件（改前→改后 sha256 前12位） | 内容 |
|---|---|---|
| BUG-Q02-003（卡3 3d=C） | `WtBtCore/SelMocker.h` `d38e498416a5→21b5b02d11b6`；`SelMocker.cpp` `e04f23d12a34→8442a044e987` | 新增 `_frozen_tdate`；`dump_stradata` 每个持仓写 `frozen`（归属 `fund.tdate`）；`on_session_begin` 仅当交易日与冻结量所属交易日不同才清零。SEL 无快照加载入口，不存在续跑路径。 |
| BUG-Q02-003（卡3 3d=C） | `WtBtCore/HftMocker.h` `ed1f69824a48→12e4d1fac533`；`HftMocker.cpp` `3bbd453cc9aa→faaac8b332f5` | 同样的 session-begin 条件清零；HFT 无快照 dump/load，“保存”不适用。 |
| BUG-R103-002（E） | `WtBtCore/HisDataReplayer.cpp::get_last_tick` 历史分支 | 左闭右开：可见前缀为 `time <= now` 的全部记录、返回其末条（同刻按稳定加载序取最后一条）；首前与严格尾后返回空；派发快照分支未改。原实现精确命中同刻组时返回首条。 |
| BUG-I02-NEW-109（卡15，明确缺陷） | `HisDataReplayer.cpp::simTicks` 日线臂 | 日线模拟 tick 按 O/H/L/C 累计写入 `_day_cache`（与分钟臂一致），tick 关闭时 `get_day_price`（公开接口注释“读取当日价格”）返回当日开高低收；派发的模拟 tick 本身不变。 |
| BUG-I02-NEW-029（卡E，主线程追加） | `HisDataReplayer.cpp` 自然时间回放 `enqueue` + `reset`/新 getter；`HisDataReplayer.h` `5dda2ebeb6d4→0f59ff764669`（20:4x 取改前副本） | tick/order detail/order queue/transaction 四类流统一按代码交易时段与时刻合法性过滤：时段外记录不派发、不进入任何派发/价格/对账簿记；逐流计数 `get_out_of_session_count(kind)`（0 detail、1 trans、2 tick、3 queue，原子计数，`prepare()` 清零）并按“流#代码#交易日桶”首条 WARN。参照 dt-parsers 草稿 `20261104-s5-dt-parsers/patches/NEW-029-HisDataReplayer.proposed.diff`。 |
| I03-NEW-019 | `HisDataReplayer.cpp::report_replay_error` | 首个失败被记录时写 `Replay failed with replay error {code}: {message}`。 |
| （合计） | `HisDataReplayer.cpp` `633f47b5296c→4c48babd2702`（第一轮后中间版 `9a81cdd29200`，副本 `before/src/WtBtCore/HisDataReplayer.after-s5-round1.cpp`） | 全部 hunk 均为本包改动（已逐段核对），未触及日期映射调用点（date-session 包）。 |
| I03-NEW-019 | `WtBtRunner/WtBtRunner.cpp` `ecf21098bba0→e3cd9b2645a0`；`WtBtPorter/WtBtRunner.cpp` `efed05205e60→5b2d950a7f32` | prepare 失败日志附 `replay error {code}: {message}`（取自 `replay_status()`）。 |
| R20_wtbtcore#3（卡9） | `WtBtCore/EventNotifier.h` `11b834b22cde→6bec8ec2f49b`；`EventNotifier.cpp` `1fae3e706946→d474a814474d` | 新增幂等 `release()`：关闭发布准入→条件变量等待在途发布归零→恰好一次 `destroy_server`→清空指针/id；析构复用；释放后 `notify*` 空操作、`init` 拒绝（9c=A）；`init/release` 串行化；WtMsgQue 无版本化回调静默能力，按 9a=A 保存句柄、常驻到进程退出（不 `free_library`）。唯一宿主回调 `on_mq_log` 为空操作，不存在发布内同步重入 release 的路径（静态分析）。 |
| R20_wtbtcore#3 | `WtBtPorter/WtBtRunner.cpp::finalizeRelease` | 回放结束、线程 join、执行器 quiesce 之后调用 `_notifier.release()`。 |

## 测试修改（新增/夹具修正，未弱化断言）

| 文件 | 改前→改后 | 说明 |
|---|---|---|
| 新增 `q02_btcore3/s5_sel_hft_frozen_tdate_test.cpp` | `a35723b42c9e` | Sel/Hft 同日重复 session-begin 保留、次日清零、SEL dump 含 frozen+tdate；两项无归属冻结量正控。自带最小回放夹具（共享 r62 夹具在当前树 init 失败，见新发现 A）。 |
| 新增 `q02_btcore3/s5_r103_last_tick_interval_test.cpp` | `02a2e03eb46b` | 同刻组精确命中（中段/尾组）取最后插入、组间取前组末条、尾后空、首前空、单条精确/尾后。 |
| 新增 `q06_btporter/s5_r20_runner_notifier_release_test.cpp` | `13c8406836f0` | R160 fake MQ（`WT_R66_FAKE_MQ_DIR`）：空闲/同步回放/异步回放后 `release_backtest` 恰好一次 destroy、之后无发布、Runner 析构不重复。 |
| 新增 `m_WtBtRunner/s5_prepare_failure_diagnostic_test.cpp` | `877484375eff` | 缺日历使 prepare 以错误码 5 失败，子进程输出须含 `replay error 5: <非空消息>`、exit 255。 |
| `q02_btcore3/q02_cta_replayer_integration_test.cpp` | `4b8c01e702e5→1d4f57cbb992` | 仅夹具：按卡13d（成交须可核验物理合约且缺费即错误、零费率合法）补 SSE.000001 合约文件与零费率模板；断言未改。 |
| `q02_btcore3/r80_replayer_slice_unsub_test.cpp` | `c93c60e4c2f5→f34d2246c835` | 仅夹具：`DayPriceReadsSimulatedDayCache` 回放起点 Jan01 09:30→Jan02 09:30（Jan01 日线在起点时未收盘，onInit 取不到历史切片；与迭代02 对 `HotRuleFutureCsvUsesRuleDsbName` 的同类修正一致）；断言未改。 |
| `q06_btporter/r222_bt_runner_shutdown_test.cpp` | `ec2c98b05647→73f943659a8d` | 仅夹具：补默认日历 `holidays.json` 与商品 `holiday:"CHINA"`（卡1 A+C checked 映射需要，同 I03-NEW-018 处理）；断言未改。 |
| `p08_btcore_mockers3/r20_mq_fixture/{fake_mq.cpp,notifier_check.cpp,CMakeLists.txt,README.md}` | `98d294b04539→4932b7477ed6`、`06db9ef5b756→c5752aea3081`、`892b35c04846→5275681b3b0d`、`8bc3e3841021→56823431ea60` | 新增 release/release-before-init/concurrent-release 三项；fake 可放慢发布并记录 destroy 与在途发布重叠。 |
| 新增 `fixtures/i03_btcore_s5_20261104s5/{CMakeLists.txt,run.sh,README.md}` | | 红绿夹具（含 NEW-029 目标 `s5_new029`，链接 WtDataStorage 以读取 bin 数据）。 |
| 新增 `q02_btcore3/s5_new029_out_of_session_streams_test.cpp` | | NEW-029 四流正负控 + 计数。 |
| NEW-053 夹具/卡13d 旧断言（12 个文件） | 见 `hashes/new053-tests-before-after.txt` | 见 NEW-053 节。 |

## 结果（红=改前产品；终=最终产品）

| 组（过滤器） | pre | 红 | 终 | 说明 |
|---|---|---|---|---|
| `S5Q02FrozenTDate.*`（独立夹具） | - | 2/5（3 项新行为失败，2 正控通过）`runs/fix-red` | 5/5 `runs/fix-green` | 覆盖率树中红二进制因旧版夹具依赖 r62（init 失败）0/5 `runs/red-q02frozen`，不作红控；终 5/5 `runs/f-q02frozen`。 |
| `Q02CtaIntegrationTest.*` | 3/7（含 R80 一例）`runs/pre-q02int` | 独立夹具红树+新夹具 4/6 `runs/fix-red` | 4/6 `runs/f-q02int`、独立夹具绿 4/6 | 两项信号恢复用例：夹具补合约/零费后红绿均通过 → 原失败为卡13d 夹具缺口（lldb：`fill contract does not match subscription metadata`）；剩 2 项 rawcode（决策卡 S5-A）。 |
| `S5R103LastTickInterval.*:R103LastTickBoundaryTest.*` | - | 21/23（同刻精确两项失败）`runs/red-r103` | 23/23 `runs/f-r103` | 既有 17 项（含派发序、过滤 tick、旧快照、尾后）均通过。 |
| `R80SliceUnsub.*` | 3/4 | `DayPrice…` 0 值失败 `runs/red-q02int` | 4/4 `runs/f-r80` | NEW-109。 |
| `S5R20RunnerNotifierRelease.*:R66BtPorterNotifier.*` | - | 1/4（release 后 destroy=0）`runs/red-r20runner` | 4/4 `runs/f2-r20runner`（无 SKIP） | R20#3。 |
| R20 独立夹具 ctest | - | 编译失败（静态红） | 13/13，`concurrent_release` 另重复 20 次全过（`logs/*r20-green*`） | 并发释放日志：22 次发布后 destroy 为最后一次调用。 |
| `S5New029OutOfSession.*`（独立夹具，链接 WtDataStorage） | - | 分派例失败：tick 流已过滤，detail/queue/trans 三流派发全部 6 条（含盘前、午休、收盘后、非法秒）；计数例因旧版无 API 不编译 `runs/fix-red-new029` | 2/2 `runs/fix-green-new029` | NEW-029；覆盖率树终版复跑见下。既有 `R91BinHft_TickReplaySkipsPreOpenAndAdvancesSessions`（15:01 detail 被派发）红=失败。 |
| `S5BtRunnerPrepareDiagnostic.*:r14_wtbtrunner_cli.*` | - | 新例 0/1 `runs/red-new019` | 7/7 `runs/f-new019` | NEW-019。 |
| `R91Fixture.R2AdjFactors_*:R91Fixture.R91BinBars_Adjusted*` | - | - | 4/4 `runs/f-r2adj` | R2#2 普通构建重跑。 |
| `R20EventNotifier.*` | - | - | 6/6 `runs/f-r20notifier` | R20#1。 |
| r222 组（`WtBtRunnerShutdown.*:N04ExecMocker.*:R205…:Z15…:R113…`） | - | 红 0/6（3 例 120 s 超时、3 例缺插件）`runs/red-btrunner-life` | 夹具修正后 9/10 `runs/f2-btrunner-life` | `N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely`（R20#1 原定向）通过；剩 `ImmediateStop…` 的 `isAsync()` 断言（见新发现 D）。 |
| `P07TickLoaderTest.*` | 32/41 | 32/41 | 32/41 `runs/f-p07` | NEW-074：out-of-source 资源与 WtDataStorage 动态加载正常；9 例失败 pre/红/终一致，与本包改动无关（见新发现 C）。 |
| `Z15MatchEngineUnsinked.*:MatchEngineTest.*:R20ExecMocker.CancelByIdDoesNotRestoreUndoneTwice` | Z15 3/3 | - | 24/24 `runs/f-new093` | NEW-093。 |
| `R200Fixture.*` | 1/1 | - | 7/7 `runs/f-new090` | NEW-090。 |
| `R2PublicTest.*` | - | 4/7 | 4/7 `runs/f-r2pub` | NEW-101、NEW-102 现状失败；`feeTemplateAndCalcFeeMatrix` pre/红/终均失败（新发现 B）。 |
| `Q02Cta*:C3CtaFrozen.*` | - | - | 25/27 `runs/f-q02cta` | 仅 2 项 rawcode。 |
| 回归集（`scripts/regfilter.txt`，Sel/Hft mocker、replayer 相关 217 例） | - | 155/217 `runs/red-reg` | 155/217 `runs/final-reg` | 逐例对比红终无任何通过/失败翻转；62 例失败红终一致，为当前树既有（主要是新发现 A/C 类夹具漂移：R62 共享夹具、P04 系列、R151/R72/R131 等），本包未改。 |

### 终版复跑（build12，含 NEW-029 与 NEW-053 夹具修正；二进制哈希 `hashes/final4-binaries-sha256.txt`，`runs/f4-*`）

| 组 | 结果 | 说明 |
|---|---|---|
| `S5Q02FrozenTDate.*` | 5/5 | |
| `S5New029OutOfSession.*:R91Fixture.*` | 21/22 | 失败 1 为 `R91BinMisc`（非法 offset 3 的两处旧断言，build13 已按卡13d 改写，见 f5） |
| `S5R103LastTickInterval.*:R103LastTickBoundaryTest.*` | 22/23 | 失败 1 为 `FilteredTickIsNotVisibleDuringOtherStreamCallback`（NEW-029 后夹具需改，build13 已改，见 f5） |
| `Q02CtaIntegrationTest.*` | 4/6 | 2 例 rawcode（S5-A） |
| `R80SliceUnsub.*` | 4/4 | |
| `R2PublicTest.*` | 5/7 | 费用矩阵按卡13d 修正后通过；剩 NEW-101、NEW-102 |
| `R200Fixture.*` | 7/7 | |
| NEW-093 组 | 24/24 | |
| `R20EventNotifier.*` | 6/6 | |
| R2#2 组 | 4/4 | |
| `P07TickLoaderTest.*` | 38/41 | 剩 3 例回放状态合同冲突（S5-B） |
| `Q02Cta*:C3CtaFrozen.*` | 25/27 | 2 例 rawcode |
| `S5R20RunnerNotifierRelease.*:R66…` | 4/4 | |
| r222 组 | 9/10 | 剩 `ImmediateStop…isAsync()`（新发现 D） |
| `S5BtRunnerPrepareDiagnostic.*:r14…` | 7/7 | |
| NEW-053 组（`P04*:R43*:P07*:R67*:ReplayTimeRange*:R103*:R2*:R80*:R91*:S5*:HisDataReplayerRuntime*`） | 337/353 | date-session 列出的 56 例中 47 例转为通过；其余见 NEW-053 节；另 `R201MockerFixture` 5 例（r62 夹具，新发现 A，红二进制同样 0/5 `runs/red-r201`） |
| 回归集 217 例 | 176/217（红 155/217） | 逐例对比：0 例由过转败，21 例由败转过；剩余 41 例为 r62/r151 共享夹具（新发现 A）、R72/P08/R151HftUft 成交类（卡13d 合约/费率夹具缺口，未在本次范围内逐例处理）与 S5-B 类 |

### LLVM 22 ASan/UBSan 有界重跑（R2#2、R20#1）

构建：`/tmp/wt-i03-s5-btcore-asan`，Homebrew LLVM 22.1.8，`-fsanitize=address,undefined -fno-omit-frame-pointer`（C/CXX/链接），`WT_ENABLE_COVERAGE=ON`、`WT_WARNINGS_AS_ERRORS=OFF`（LLVM 22 下有既有 `-Wnontrivial-memcall` 告警，位于 HftMocker.h:1008 等旧代码），`-include stdlib.h`；`compile_commands.json` 1187/1187 条命令带 sanitizer 参数（Homebrew 预编 gtest/boost 与系统库未插桩）。首次链接失败因 PATH 中 Anaconda 的旧 `ld64-530` 不识别 `-no_warn_duplicate_libraries`，改以 `PATH=/usr/bin:...` 重跑（`scripts/wt-i03-s5-btcore-asan2.sh`）。构建期间产品头文件变化，追加 build3/build4 增量重编以保证各 TU 布局一致（`logs/wt-i03-s5-btcore-asan-build*.log`）。运行 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:detect_container_overflow=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，每例 300 s 硬超时（`scripts/asan_runs.sh`），未出现挂起。哈希 `hashes/asan-binaries-sha256.txt`。

| 组 | 结果 | 说明 |
|---|---|---|
| R2#2：`R91Fixture.R2AdjFactors_*:R91Fixture.R91BinBars_Adjusted*` | 4/4，无 ASan/UBSan 报告 `runs/asan-r2adj` | 迭代02“15 s 无输出”为 Apple clang ASan 进 main 前挂起，LLVM 22 下正常结束 |
| R20#1：`R20EventNotifier.*` | 6/6 `runs/asan-r20notifier` | |
| R20#1：`N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely`（halt_on_error=1） | 134 中止 `runs/asan-n04` | 唯一报告为 vendored zstd 1.4.4 `zstd_compress.c:1326:46`（CSV bar 压缩缓存路径），第三方库 UB（新发现 E） |
| 同上（`UBSAN_OPTIONS=halt_on_error=0`，诊断用，不计作 UBSan 通过） | 1/1 `runs/asan-n04-ubsan-nohalt` | 全部 UBSan 报告仅 3 处，均在 zstdlib（`zstd_compress.c:1326`、`zstd_compress_internal.h:876/877`）；本仓代码无 UBSan/ASan 报告 |
| `S5R20RunnerNotifierRelease.*` | 3/3 `runs/asan-r20runner` | |
| 本包新测试 + R103 + R80 DayPrice | 30/31 `runs/asan-s5btcore` | 唯一失败为 R80 DayPrice 经 CSV 缓存触发同一 zstd UBSan |

所有 ASan 日志中无 `AddressSanitizer` 报告。

## 条目结论

- **BUG-Q02-003**：卡3 3d=C 已落到 SelMocker（保存 frozen + session-begin 条件清零）与 HftMocker（session-begin 条件清零；无快照故“保存”不适用）。红绿：独立夹具红 3 失败/2 正控通过→绿 5/5。集成 4 例既有失败中 2 例为卡13d 夹具缺口（已补合约/零费率，红绿均通过，说明非本包产品问题）；剩 2 例 rawcode 断言需裁定（决策卡 S5-A）。UftMocker 未在范围内、未改。
- **BUG-R103-002**：按 E 合同实现并新增定向；红 2 失败→终 23/23。
- **R20_wtbtcore#3**：EventNotifier 幂等 release + 排空 + 恰好一次 destroy + 9a 常驻；Runner `finalizeRelease` 在回放停止与 quiesce 后释放。R20 夹具 13/13（并发 20 次重复通过）、Runner 级 4/4（红 1/4）。真实 WtMsgQue/nanomsg 内部线程排空无法证明（X），按 9a 常驻；Windows/Linux 未验。
- **R20_wtbtcore#1**：无链接实体构建中 `R20EventNotifier.*` 6/6、`N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely` 1/1（普通构建，需 r222 夹具补日历）。LLVM 22 ASan/UBSan：`R20EventNotifier.*` 6/6；Runner 回放在 halt 模式下被 vendored zstd 1.4.4 UBSan 中止，no-halt 诊断显示本仓代码无 UB/ASan 报告、用例 1/1——UBSan 结论受第三方库阻塞（新发现 E，WTSUtils 非本包）。
- **R2_wtbtcore#2**：无链接普通构建 4/4、LLVM 22 ASan/UBSan 4/4 无报告、无挂起。同实例并发不在合同内（独立实例并发键已覆盖）。
- **BUG-I02-NEW-074**：无符号链接 out-of-source 构建中 P07 32/41 实际执行并动态加载 `WtDataStorage`（CMake 注入绝对路径）；9 例失败与资源定位无关（新发现 C）。Windows/Linux 未验。
- **BUG-P04-011**：仓内夹具 `p04_cross_session_clock` 在当前整合树无法运行（会话 `0–2400` 交易段被卡2b 拒绝、缺默认 `TRADING` 会话被卡1 prepare 拒绝，`runs/p04-runs`、`runs/p04-runs00`）。按卡1/卡2 调整的临时副本（`scripts/p04_cross_session_test_card1_card2_adapted.cpp`，未改仓内夹具）：正控 `pureTickContinuousAndFutureDispatchOnlyWithNoDataDay` 通过（5 笔按自然时间派发、时钟不倒退、末毫秒）；已知红控 `…NoDataDayAndFinalMillisecond` 仍只在 `visible[2]>=1` 失败（默认 `get_tick_slice` 用全局交易日桶，产品缺陷仍在）；负 offset 混合例仍 fail-closed（合同未裁定）。未修产品：查询桶切换会改写派发共用的 `_ticks_cache` 工作副本，且按品种交易日需新增日期映射调用（date-session 包），需设计后实施。真实 provider 同步回调为 X。
- **I03-NEW-019**：红 0/1→终 7/7（含 r14 既有 6 例）。
- **BUG-I02-NEW-029**（卡E）：四类流统一按交易时段过滤并可观察（计数 + 首条 WARN）。独立夹具红（三流派发越界记录）→绿 2/2；覆盖率树终版 `S5New029OutOfSession.*` 2/2、`R91Fixture.*` 20/20（含原失败的 `R91BinHft_TickReplaySkipsPreOpenAndAdvancesSessions`，`runs/f5-r103-r91`）。R103 既有用例按 NEW-029 调整夹具后 23/23。真实行情与其他平台未验。

## I03-NEW-053 回测部分（主线程追加）：btcore 回放组 prepare/init 失败逐例归因

date-session 运行（`../20261104-s5-date-session/runs/btcore/result.xml`）276 例中 56 例失败。逐例归因与处置（只改夹具/按已裁定卡改写冲突旧断言，产品未放宽；改前副本在 `before/`，前后哈希 `hashes/new053-tests-before-after.txt`，补丁 `patches/tests.patch`）：

| 类别 | 用例 | 根因 | 处置 |
|---|---|---|---|
| 卡1 缺日历 | R43ReplayerTickCsvTest 19、P04ReplayTermTest 4、P04ReplayCsvTest 6（含 2 例 death test 子进程）、P04ReplayerConfigTest 3（prepare）、R43SelMockerDumpTest 1（n04，date-session 组未含、本包回归集发现） | basefiles 无 `holiday`，prepare 起点 checked 映射 MissingCalendar（同 I03-NEW-018） | 夹具补 `dist/common/holidays.json` |
| 卡13d 费用/合约 | P04ReplayerConfigTest 3、R2TickTest 3、R80ConfigTick 1、R91BinMisc 1、R2PublicTest.feeTemplateAndCalcFeeMatrix 1、P07TickLoaderTest.calcFeeUsesLoadedFeeTemplate 1 | 成交/计费需可核验的实际合约（夹具无合约元数据）；缺费、非法 offset、缺失或损坏的费用模板现为显式错误（NaN / init 失败） | 夹具补合约元数据（及必要的零费率模板）；“缺费=0 / 坏模板仍 init 成功”的旧断言与已裁定卡13d“缺费报错”冲突，改写为 NaN/init 失败（逐处注释） |
| 卡2b + 卡1 会话 | P07TickLoaderTest 5（continuous24Hour* 3、minuteEnd…、sameTimestamp…） | 交易段 `0–2400` 被卡2b 拒绝（仅 `0–0` 表示全天）；自带 sessions 缺默认 `TRADING`，prepare 起点映射失败 | 夹具改 `0–0` 并补默认 `TRADING` 全天会话 |
| NEW-029 产品缺陷（已修） | R91BinHft_TickReplaySkipsPreOpenAndAdvancesSessions | 15:01 order detail 被派发 | 产品修复（见上） |
| 回放状态合同与旧 oracle 冲突（未改，需裁定，归入决策卡 S5-B） | P04ReplayTermTest.invertedTimeRangeIsRejectedBeforePrepare、P07MinuteReplayTest 2、P07TickLoaderTest.hftTickCallbackExceptionAllowsReplayByDayRetry、P07TickLoaderTest.emptyNullTickLoaderCallbackIsAValidEmptyBucket、P07TickLoaderTest.emptyReplayBucketsStayBoundedAcrossLongNoDataRange、R2PublicTest.stopBeforeRunQuitsReplayEarly（NEW-101） | 回放状态 V1 规定每代恰好一次 `handle_replay_done`（准备被拒/回调抛异常/停止也送达，状态区分）；区间内无任何 HFT 事件判 `WT_REPLAY_ERROR_NO_SOURCE`（终止） | 不改；旧断言期望 done=0 或“空桶不终止” |
| 合同待裁定（决策卡 S5-A/C） | R2PublicTest.metadataAndTimelineQueries（NEW-102） | rawcode 非规则码、未知合约商品层级 | 不改 |

结果：终版（build12/13）中 date-session 列出的 56 例有 48 例通过（`runs/f4-new053` 47 例 + `runs/f5-r103-r91` 中 `R91BinMisc`），剩 8 例：回放状态合同类 7（含 NEW-101）与 NEW-102 1，均未改断言、待裁定。另在同组发现并修复 `R43SelMockerDumpTest.replayDoneWithoutScheduleWritesArtifacts`（缺日历，`runs/f5-r103-r91` 通过）。

R103 既有用例 `FilteredTickIsNotVisibleDuringOtherStreamCallback` 原以“同一非法时刻”的逐笔委托触发回调；NEW-029 后该委托同样被过滤，夹具改为紧随其后的合法时刻 09:32:00，断言不变。

## 卡15 Z 类复核

- **BUG-R9-001**：`R205BtRunnerConfig.*`、`Z15BtRunnerConfigLock.*` 3/3（`runs/f2-btrunner-life`）。状态机：成功后锁定（重复拒绝）、preflight 失败可重试、replayer 已初始化后的失败永久显式拒绝（需新 Runner 实例）——与卡9 9c=A（重复/再次初始化拒绝并保留原实例）一致。建议：以卡9c 为合同依据记本地验证通过；若主线程认为卡9 不覆盖 Runner 配置，则按卡15 记“用户指示忽略”。
- **BUG-I02-NEW-090**：`R200His_BarReplaySkipsEmptyTickList` 1/1（组 7/7），无产品缺陷；用例已在 bar 回放中消费有效 tick 并把 j 的两个交易日桶登记为成功空加载。尝试用行覆盖证明 `replayHftDatas` 空列表分支命中，模板 lambda 行计数归属不清，未能确证。建议“用户指示忽略”（或主线程接受现测试为原目标），状态阻塞。
- **BUG-I02-NEW-093**：z15 已修 `MatchEngine::handle_tick` 无 sink 守卫（exec-units 文件，本包未改）。复跑 24/24（含 Z15 3/3、MatchEngineTest、`R20ExecMocker.CancelByIdDoesNotRestoreUndoneTwice`）。建议：空 sink 解引用缺陷记本地验证通过；“未初始化是否允许接单”无合同，保持 z15 的不拒单现状。
- **BUG-I02-NEW-101**：现状失败（停止后 `_doneCnt==1`）。HEAD/上游 `run_by_bars` 终止后仍调 `handle_replay_done`；迭代03 回放状态 V1 合同为“每代恰好一次、状态 STOPPED 可区分”。非产品缺陷，旧断言与现合同冲突但无已裁定卡→决策卡 S5-B；未裁定前建议“用户指示忽略”。
- **BUG-I02-NEW-102**：现状两项失败。商品半项：公开接口 `stra_get_comminfo` 注释为“获取**品种**信息”，实盘/回测均按交易所+品种查找，`SSE.STK.999999` 返回 STK 品种对象合乎接口，测试期望 `nullptr` 与接口冲突（决策卡 S5-C，建议批准改断言）。rawcode 半项同 Q02-003（决策卡 S5-A）。
- **BUG-I02-NEW-109**：判为明确产品缺陷并修复（见上）；夹具起点修正后红（四个 flag 均 0）→终 4/4。建议本地验证通过（日线回放 tick 关闭路径）；tick 开启时仍走 last tick，未改。

## 新发现（编号由主线程分配）

- A：`dist/common/fees.json` 第 109/115 行重复根键 `DCE.pg`（按量 6.0 与按额 0.0002 两套费率），严格费用加载器拒绝整份模板（lldb：`Fees template … failed: duplicate root key`），因此所有使用发行 `fees.json` 的回放初始化失败；`r62_mocker_common.h` 共享夹具（R62Sel/Hft/Cta/Uft MockerGap 等）在当前树 `_rp.init` 失败即此原因（`runs/red-r62probe`）。需裁定保留哪套费率并修 dist 文件（或加载器策略）。实盘 WtEngine 是否同样拒绝未验证。
- B：`R2PublicTest.feeTemplateAndCalcFeeMatrix` 在 pre/红/终均失败：`calc_fee` 因夹具无合约元数据返回 NaN（卡13d 合约核验），且旧断言“未登记费率恒为 0”与卡13d“缺费报错”冲突。需按卡13d 补夹具并改写该旧断言（本包未改）。
- C：`P07TickLoaderTest` 9 例在 pre/红/终一致失败：5 例 init 失败（会话 `{"from":0,"to":2400}` 交易段被卡2b 拒绝）、其余为回放终止/费用（卡1 checked 映射、卡13d）。属卡1/2/13d 引起的夹具漂移，非 NEW-074 资源定位问题；`p04_cross_session_clock` 夹具同因不能运行。
- D：`WtBtRunnerShutdown.ImmediateStopWaitsForPrepareThenJoinsTheWorker` 补日历后仅 `runner.isAsync()` 断言失败：停止发生在 prepare 的 init 回调内，回放状态合同使 prepare 返回 false，Runner 按“准备失败”恢复原 `_async`。属测试 oracle 与回放状态合同的冲突，需裁定 `isAsync()` 在“准备期被停止”时的语义。
- E：vendored zstd 1.4.4 `ZSTD_reset_matchState` 对空指针加偏移的 UBSan（已有 `fixtures/r20_zstd_ubsan`）仍阻塞全插桩 Runner 回放的 UBSan 结论；需评估升级 zstd（WTSUtils，非本包）。
- F：本包 `/tmp` 下若干目录/日志被外部删除（见“构建”），提示并行任务可能有过宽的 `/tmp/wt-*` 清理。

## 决策卡草稿

见 `decision-cards.md`（S5-A rawcode 非规则码合同；S5-B 停止是否送达 replay_done；S5-C 未知合约商品查询层级）。

## 删除记录

本包未删除任何他人创建的目录或文件。删除过的仅有本包自建对象：`/tmp/wt-i03-s5-btcore-run/new090-cov`（覆盖率诊断目录，重建前清理）、scratchpad 下 `dryrun/`、本包新建后放弃的 `src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture/runner_notifier_check.cpp`。本包的 `/tmp/wt-i03-s5-btcore-asan`（首次配置）、`/tmp/wt-i03-s5-btcore-asan-configure.log`（首次）、`/tmp/wt-i03-s5-btcore-cov-build1.log`、覆盖率 configure 日志曾被外部删除（非本包所为），ASan 树已重建；关键改前副本均在本目录 `before/`。
