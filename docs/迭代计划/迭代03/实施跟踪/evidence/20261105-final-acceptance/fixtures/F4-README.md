# 迭代03 独立夹具套件统一运行结果（src/TestUnits/fixtures）

本文件由 `run_fixtures.py` 生成（人工归因来自同目录上级的 `annotations.json`）。**只是独立夹具套件的定向运行，不是整体回归或整体验收。**仅本机 macOS arm64、本地 fake/受控输入；Windows/MSVC、真实厂商 SDK、生产环境均未验证。

- 冻结源码树 `--tree`：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/tree`
- 构建目录 `--build`：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/build`（Apple clang Debug+coverage，-Werror）
- 输出目录：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures`；生成时间 2026-11-05T22:32:58
- 运行器：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/fixtures/run_fixtures.py`（sha256 `157e300b4279ae6a64e823fe1c04634a4bab8c49cc2632b2817e24aa7def4b6a`）
- 完整性：运行前后 --tree 变化文件 0 个、--build 变化文件 1 个（`_integrity.json`）
- 只读核对：--tree 文件 6717 个，与 meta/files.txt 一致=None；首个套件开始（2026-11-05T20:43:43）后 --tree 被修改的文件 0 个、--build 被修改的文件 0 个（summary.json meta.integrity_check）
- 套件目录 113 个，结论 113 条；★ 标记 priority.txt 所列（复核员认定的条目验收证据）42 个

| 结论 | 数量 |
|---|---|
| 通过（pass） | 90 |
| 预期红（expected-red） | 2 |
| 失败（fail） | 7 |
| 超时（timeout） | 0 |
| 无法运行（blocked-env） | 1 |
| 不适用（not-applicable） | 13 |

## 运行约定

- 每个套件在 `--out/<suite>/` 下独立运行，日志在 `logs/NN-<label>.log`，结果在 `result.json`；`summary.json` 汇总全部条目（字段 suite/entry/command/exit/seconds/verdict/reason/log，另含 variants 明细）。
- 同时最多 2 个套件，构建并行度 `-j 2`；每个变体（一次文档化的调用组）上限 20 分钟，超时记 `timeout`。
- 脚本内写死的 `/tmp/...` 旧构建、`conda run`、Anaconda ninja、`git rev-parse --show-toplevel`、`mktemp /tmp/...`、`-j3` 等，均在 `--out/<suite>/mirror/`（`cp -Rc` APFS 克隆的 --tree，无符号链接）中改写为 --tree/--build/--out 位置，差异逐条记录在 `mirror.patch`；不改断言。
- 需要非覆盖率依赖库的套件，使用从 --tree 以 Homebrew LLVM 22 构建的共享依赖 `--out/_deps/{plain,asan,tsan}`（WT_ENABLE_COVERAGE=OFF，WT_WARNINGS_AS_ERRORS=OFF，`/usr/local/include` 作隐式目录并以 -isystem 置后，Boost 1.88 优先）。
- python 一律用系统 `/usr/bin/python3 -I`；脚本中的 `rg` 由运行器生成的 `grep -E` 替身提供（`_tools/bin/rg`）。
- red 模式只在套件自带基线（baseline/、before/、补丁或脚本内置构造）时运行，按预期失败记入 variants（role=red）；`demo` 角色表示在当前树上用于复现现存缺陷的反例，复现时整套记 `expected-red`。
- `git show <commit>:…` 只用于脚本以不可变提交重建并哈希校验红控（cta_factory_quarantine 固定为冻结 HEAD、t26_fees_atomic 固定 ccaad010），经 GIT_DIR 只读对象库。

## 通过（pass）（90）

| 套件 | 入口 | 耗时 s | 依据 |
|---|---|---|---|
| `cta_factory_quarantine` | src/TestUnits/fixtures/cta_factory_quarantine/run.sh | 61.7 | green+head-red[pass]: run.sh exit 0；current 17 例；HEAD 红控 6 例（非零 6） |
| `ctpm_empty_module_path` | src/TestUnits/fixtures/ctpm_empty_module_path/run.py | 100.5 | green[pass]: run.py（ParserCTPMini/TraderCTPMini empty/valid/absolute + 控制用例） exit 0 |
| `ctpmini_api_init` | src/TestUnits/fixtures/ctpmini_api_init/run.py | 39.0 | init[pass]: run.py 6 模式 exit 0；unload[pass]: run_unload.py 2 模式 exit 0 |
| `ctpopt_empty_module_path` | src/TestUnits/fixtures/ctpopt_empty_module_path/run.py | 30.0 | green[pass]: run.py（ParserCTPOpt/TraderCTPOpt empty/absolute/valid） exit 0 |
| `ctpopt_time_snapshot` | src/TestUnits/fixtures/ctpopt_time_snapshot/build.py + run.py | 37.0 | green[pass]: green 27 模式，非零 0  |
| `hft_factory_quarantine` ★ | src/TestUnits/fixtures/hft_factory_quarantine/run.sh | 8.2 | green[pass]: run.sh exit 0；current 用例 25 个；历史红控 0 行；基线红控 NOT RUN（套件未自带基线源码，WT_HFT_BASELINE_DIR 未提供） |
| `huax_module_path` | src/TestUnits/fixtures/huax_module_path/build_and_run.py | 39.1 | green[pass]: build_and_run.py（ParserHuaX/TraderHuaX 当前 + 去掉路径守卫的对照） exit 0 |
| `i03_backtest_fee_consumer_20261103` | src/TestUnits/fixtures/i03_backtest_fee_consumer_20261103/run.sh | 147.1 | green[pass]: run.sh（WtBtCore 构建 + fee_consumer_test + p03 选定用例） exit 0 |
| `i03_bar_fee_provenance_20261103` | src/TestUnits/fixtures/i03_bar_fee_provenance_20261103/CMakeLists.txt | 107.6 | green[pass]: i03_bar_fee_provenance exit 0 (tests=13 failures=0 errors=0) |
| `i03_btcore_s5_20261104s5` ★ | src/TestUnits/fixtures/i03_btcore_s5_20261104s5/run.sh | 47.7 | green[pass]: green 13 例，非零 0 |
| `i03_card3_dayboundary_20261104` ★ | src/TestUnits/fixtures/i03_card3_dayboundary_20261104/CMakeLists.txt + run_cases.sh + r202_run.py | 522.7 | green-plain[pass]: c3_actday_ParserCTP total=9 pass=9 fail=0；c3_actday_ParserCTPMini total=9 pass=9 fail=0；c3_actday_ParserFemas total=9 pass=9 fail=0；c3_ad_daybar total=8 pass=8 fail=0；c3_cta_frozen total=6 pass=6 fail=0；green-asan[pass]: c3_actday_ParserCTP asan-c3_actday_ParserCTP exit 0 (tests=9 failures=0 errors=0)；c3_actday_ParserCTPMini asan-c3_actday_ParserCTPMini exit 0 (tests=9 failures=0 errors=0)；c3_actday_ParserFemas asan-c3_actday_ParserFemas exit 0 (tests=9 failures=0 errors=0)；c3_ad_daybar asan-c3_ad_daybar exit 0 (tests=8 failures=0 errors=0)；c3_cta_frozen asan-c3_cta_frozen e… |
| `i03_commodity_ready_20261103` | src/TestUnits/fixtures/i03_commodity_ready_20261103/run.sh | 383.6 | red[expected-red/red]: red（baseline/ 四文件）ready/mocker：2/2 非零；green[pass]: green ready/mocker：2/2 exit 0；regression(info)[pass/info]: 相关回归（README 记 l03 26/27 既有失败）：5/5 exit 0；green-asan[pass]: green ASan/UBSan/LSan：2/2 exit 0 |
| `i03_config_embedded_nul_20261103` | src/TestUnits/fixtures/i03_config_embedded_nul_20261103/CMakeLists.txt | 14.1 | green[pass]: i03_config_nul exit 0 (tests=17 failures=0 errors=0) |
| `i03_container_ownership_20261103` ★ | src/TestUnits/fixtures/i03_container_ownership_20261103/run.sh | 53.6 | green[pass]: green 构建/运行：5/5 exit 0；red-baseline[expected-red/red]: check-red(baseline/Includes) 与旧 oracle 对新头：2/2 非零 |
| `i03_csv_block_type_20261103` ★ | src/TestUnits/fixtures/i03_csv_block_type_20261103/CMakeLists.txt | 5.3 | green[pass]: csv_block_check exit 0 (tests=10 failures=0 errors=0)；导出符号 _dump_bars,_dump_bars_v2,_dump_ticks,_dump_ticks_v2 |
| `i03_csv_status_20261103` | src/TestUnits/fixtures/i03_csv_status_20261103/CMakeLists.txt | 94.6 | green[pass]: csv_status exit 0 (tests=14 failures=0 errors=0)；导出符号 _dump_bars,_dump_bars_v2,_dump_ticks,_dump_ticks_v2 |
| `i03_ctp_order_identity_20261103` ★ | src/TestUnits/fixtures/i03_ctp_order_identity_20261103/run.sh | 178.3 | green[pass]: run.sh（TraderCTP/WtCore 生产构建 + i03_ctp_identity + P11 host tag） exit 0 |
| `i03_date_mapping_20261103` | src/TestUnits/fixtures/i03_date_mapping_20261103/CMakeLists.txt | 6.9 | green[pass]: i03_date_mapping exit 0 (tests=5 failures=0 errors=0) |
| `i03_date_mapping_20261104s5` | src/TestUnits/fixtures/i03_date_mapping_20261104s5/run.sh | 172.9 | green[pass]: 当前目标：6/6 exit 0 |
| `i03_diff_batch_atomic_20261103` | src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh（编入 atomic.inc） | 0.0 | 无独立入口：README 规定经 i03_fractional_targets_20261103/run.sh 合并编译运行；本目录 filter.txt 匹配 17 例，失败 0；来源套件结论 pass |
| `i03_dt_parser_host_20261104s5` ★ | src/TestUnits/fixtures/i03_dt_parser_host_20261104s5/run.sh | 99.5 | plain[pass]: run.sh exit 0；6/6 exit 0；thread[pass]: run.sh thread exit 0；6/6 exit 0 |
| `i03_dt_writer_lifecycle_20261104s5` ★ | src/TestUnits/fixtures/i03_dt_writer_lifecycle_20261104s5/run.sh + run_cases.sh | 433.5 | normal[pass]: normal：18 例，异常 0；asan[pass]: asan：18 例，异常 0；tsan[pass]: tsan：18 例，异常 0 |
| `i03_dualthrust_config_20261103` ★ | src/TestUnits/fixtures/i03_dualthrust_config_20261103/CMakeLists.txt | 9.8 | green[pass]: ctest + 2 组既有用例过滤：ctest=0, existing-0=0, existing-1=0 |
| `i03_engine_followup_20261104s5` ★ | src/TestUnits/fixtures/i03_engine_followup_20261104s5/run.sh | 383.0 | asan[pass]: asan：5 例，异常 0；tsan[pass]: tsan：5 例，异常 0 |
| `i03_exec_eager_validation_20261103` ★ | src/TestUnits/fixtures/i03_exec_eager_validation_20261103/run.sh | 1654.3 | （自带红控无法与当前树组合构建，未形成红绿对照）green[pass]: green：3/3 exit 0；red[red-unbuildable/red]: run.sh exit 1，未生成 exit_codes.txt（构建失败？见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/i03_exec_eager_validation_20261103/run-red/build.log）；green-asan[pass]: green-asan：3/3 exit 0 |
| `i03_exec_units_20261104s5` | src/TestUnits/fixtures/i03_wp1_ledger_20261104s5/run_r202_003.sh（编译本目录 r202_003_risk_unknown_code_test.cpp） | 0.0 | 本目录只有测试源码，无独立入口；由 i03_wp1_ledger_20261104s5/run_r202_003.sh 编译运行：green[pass]: run_r202_003.sh 当前 TraderAdapter exit 0（tests=2 failures=0） |
| `i03_execution_groups_20261103` | src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh（编入 pure/local/arbi.inc） | 0.0 | 无独立入口：README 规定经 i03_fractional_targets_20261103/run.sh 合并编译运行；本目录 filter.txt 匹配 18 例，失败 0；来源套件结论 pass |
| `i03_fee_consumer_pipeline` | src/TestUnits/fixtures/i03_fee_consumer_pipeline/run.py | 7.3 | green[pass]: run.py exit 0（卡13d 四例逐进程）：[('UnmigratedExportedContractKeysAreAnObservableMissingFee', 0), ('VersionedContractVolumeFeeIsUsedForThePhysicalContract', 0), ('ContractAmountFeeRequiresPhysicalContractMetadata', 0), ('LegacyProductAmountFeeStillWorks', 0)] |
| `i03_fee_context_20261103` | src/TestUnits/fixtures/i03_fee_context_20261103/run.sh | 8.7 | green[pass]: run.sh normal+ubsan（FeeContext.* + FeeScheduleCandidate.*） exit 0 |
| `i03_fee_engine_gate` | src/TestUnits/fixtures/i03_fee_engine_gate/run.sh | 38.8 | green[pass]: run.sh（I03FeeEngineGate.* + R130EngineFee.*） exit 0 |
| `i03_fee_schedule_candidate` | src/TestUnits/fixtures/i03_fee_context_20261103/run.sh（编入 i03_fee_schedule_candidate/fee_schedule_test.cpp） | 0.0 | 本目录 fee_schedule_test.cpp 无独立入口，由 i03_fee_context_20261103/run.sh 与 FeeContext 一起编译运行（FeeScheduleCandidate.*）：green[pass]: run.sh normal+ubsan（FeeContext.* + FeeScheduleCandidate.*） exit 0 |
| `i03_fractional_targets_20261103` | src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh | 974.2 | green[pass]: run.sh exit 0（tests=44 failures=0 errors=0，fractional+groups 合并过滤） |
| `i03_hft_uft_fee_commit_20261103` | src/TestUnits/fixtures/i03_hft_uft_fee_commit_20261103/CMakeLists.txt | 12.3 | green[pass]: check exit 0 (tests=23 failures=0 errors=0) |
| `i03_initext_login_20261104` ★ | src/TestUnits/fixtures/i03_initext_login_20261104/run.sh | 133.2 | fix-plain[pass]: fix-plain exit 0；8/8 exit 0；fix-asan[pass]: fix-asan exit 0；8/8 exit 0；base-plain[expected-red/red]: base(baseline/)-plain：5/8 非零；base-asan[expected-red/red]: base(baseline/)-asan：5/8 非零 |
| `i03_leaks_main_20261104` ★ | src/TestUnits/fixtures/i03_leaks_main_20261104/manual_build.sh + run_cases.py | 336.1 | femas-normal[pass]: femas-normal 7 例，非零/缺 XML 0；ALL(filter *) exit 0，最后运行 FemasContractFixture.l01014_unconvertible_query_rows_are_forwarded_raw_when_host_supports_it（README：该用例在手工链接中无效，不计）；femas-asan[pass]: femas-asan 7 例，非零/缺 XML 0；ALL(filter *) exit 0，最后运行 FemasContractFixture.l01014_unconvertible_query_rows_are_forwarded_raw_when_host_supports_it（README：该用例在手工链接中无效，不计）；mocker-normal[pass]: mocker-normal 9 例，非零/缺 XML 0；ALL(filter *) exit -11，最后运行 t6_init.wtcore_init_ext_rejects_tradermocker_null_config（README：该用例在手工链接中无效，不计）；mocker-asan[pass]: mocker-asan 9 例，非零/缺 XML 0；ALL(filter *) exit -… |
| `i03_monitor_date_20261103` | src/TestUnits/fixtures/i03_monitor_date_20261103/CMakeLists.txt | 13.2 | green[pass]: monitor_date exit 0 (tests=8 failures=0 errors=0)；q05_monitor_control exit 0 (tests=8 failures=0 errors=0) |
| `i03_parser_event_contract_20261103` ★ | src/TestUnits/fixtures/i03_parser_event_contract_20261103/CMakeLists.txt | 101.5 | （自带红控无法与当前树组合构建，未形成红绿对照）green[pass]: i03_parser_events exit 0 (tests=13 failures=0 errors=0)；red-events-before[expected-red/red]: red-events-before exit 1 (tests=13 failures=6 errors=0)；失败：XeleEvents.ConnectThenLoginOnlyAfterFirstValidFrame,XeleEvents.InvalidFramesDoNotConfirm,XeleEvents.ReconnectStartsNewEventEpoch,UdpEvents.ConnectThenLoginOnlyAfterFirstValidMessage,UdpEvents.InvalidMessagesDoNotConfirm,UdpEvents.ReconnectStartsNewEventEpoch；red-xele-oracle-head[red-unbuildable/red]: i03_xele_oracle_head 未构建；asan[pass]: asan-i03_parser_events exit 0 (tests=13 failures=0 errors=0)；tsan[pass]:… |
| `i03_parser_lifecycle_v2` | src/TestUnits/fixtures/i03_parser_lifecycle_v2/CMakeLists.txt | 14.5 | green[pass]: i03_parser_v2 exit 0 (tests=9 failures=0 errors=0) |
| `i03_parser_shm_lifecycle_v2` ★ | src/TestUnits/fixtures/i03_parser_shm_lifecycle_v2/CMakeLists.txt | 75.1 | green[pass]: i03_parser_shm_v2 exit 0 (tests=10 failures=0 errors=0)；i03_parser_shm_legacy exit 0 (tests=19 failures=0 errors=0)；asan[pass]: asan-i03_parser_shm_v2 exit 0 (tests=10 failures=0 errors=0)；asan-i03_parser_shm_legacy exit 0 (tests=19 failures=0 errors=0)；tsan[pass]: tsan-i03_parser_shm_v2 exit 0 (tests=10 failures=0 errors=0)；tsan-i03_parser_shm_legacy exit 0 (tests=19 failures=0 errors=0) |
| `i03_parser_udp_lifecycle_v2` ★ | src/TestUnits/fixtures/i03_parser_udp_lifecycle_v2/CMakeLists.txt | 105.1 | green[pass]: i03_parser_udp_v2 exit 0 (tests=8 failures=0 errors=0)；i03_parser_udp_legacy exit 0 (tests=20 failures=0 errors=0)；asan[pass]: asan-i03_parser_udp_v2 exit 0 (tests=8 failures=0 errors=0)；asan-i03_parser_udp_legacy exit 0 (tests=20 failures=0 errors=0)；tsan[pass]: tsan-i03_parser_udp_v2 exit 0 (tests=8 failures=0 errors=0)；tsan-i03_parser_udp_legacy exit 0 (tests=20 failures=0 errors=0) |
| `i03_parser_vendor_v2_20261104` ★ | src/TestUnits/fixtures/i03_parser_vendor_v2_20261104/run.sh | 87.9 | green[pass]: green：10/10 exit 0；red-baseline[expected-red/red]: 基线（baseline/ 自带）v2 与场景红控：42/50 非零 |
| `i03_parser_xele_lifecycle_v2` ★ | src/TestUnits/fixtures/i03_parser_xele_lifecycle_v2/CMakeLists.txt | 96.6 | green[pass]: i03_parser_xele_v2 exit 0 (tests=11 failures=0 errors=0)；i03_parser_xele_scenarios exit 0 (tests=5 failures=0 errors=0)；i03_parser_xele_prepare exit 0 (tests=1 failures=0 errors=0)；i03_parser_xele_legacy exit 0 (tests=35 failures=0 errors=0)；legacy-all(info)[pass/info]: legacy-all exit 0 (tests=37 failures=0 errors=0)（README 另行运行的全量 legacy，含 3 条已排除的旧 oracle）；red-baseline[expected-red/red]: 基线场景红控（baseline/ 自带）：5/5 非零；asan[pass]: asan-i03_parser_xele_v2 exit 0 (tests=11 failures=0 errors=0)；asan-i03_parser_xele_scenarios exit 0 (tests=5 failures=0 errors=0)；asan-i03_parser_xele_pre… |
| `i03_porter_replay_status_20261103` | src/TestUnits/fixtures/i03_porter_replay_status_20261103/run.sh | 83.2 | green[pass]: run.sh（WtBtPorter 构建 + 7 模式 + launch-failure 注入） exit 0 |
| `i03_r15_hft_tags_20261104s5` | src/TestUnits/fixtures/i03_r15_hft_tags_20261104s5/run.sh | 257.0 | current-asan[pass]: current-asan exit 0（tests=44 failures=0）；current-tsan[pass]: current-tsan exit 0（tests=44 failures=0） |
| `i03_r42_fundinfo_leak_20261103` ★ | src/TestUnits/fixtures/i03_r42_fundinfo_leak_20261103/run.sh | 243.3 | （自带红控无法与当前树组合构建，未形成红绿对照）red[red-unbuildable/red]: red run.sh exit 12（基线与当前树组合构建/链接失败，红控未运行）；green[pass]: green 5 用例（ASan/UBSan/LSan）：5/5 exit 0 |
| `i03_replay_fee_identity_20261103` | src/TestUnits/fixtures/i03_replay_fee_identity_20261103/CMakeLists.txt | 63.6 | green[pass]: i03_replay_fee_identity exit 0 (tests=13 failures=0 errors=0) |
| `i03_replay_status_20261103` ★ | src/TestUnits/fixtures/i03_replay_status_20261103/CMakeLists.txt | 21.4 | green[pass]: i03_replay_status exit 0 (tests=16 failures=0 errors=0) |
| `i03_replayer_date_range_20261103` | src/TestUnits/fixtures/i03_replayer_date_range_20261103/CMakeLists.txt | 136.7 | green[pass]: i03_replayer_date_range exit 0 (tests=24 failures=0 errors=0) |
| `i03_report_quarantine_20261103` | src/TestUnits/fixtures/i03_report_quarantine_20261103/run.sh | 160.4 | green[pass]: run.sh（I03Quarantine.*，UBSan） exit 0 |
| `i03_resampled_fee_provenance_20261103` | src/TestUnits/fixtures/i03_resampled_fee_provenance_20261103/CMakeLists.txt | 362.1 | green[pass]: i03_resampled_fee_provenance exit 0 (tests=21 failures=0 errors=0) |
| `i03_riskmon_quiesce_20261104s5` ★ | src/TestUnits/fixtures/i03_riskmon_quiesce_20261104s5/run.sh | 208.6 | green[pass]: green 3 用例：3/3 exit 0 |
| `i03_runner_parser_drain_20261103` | src/TestUnits/fixtures/i03_runner_parser_drain_20261103/run.sh | 472.7 | green[pass]: run.sh exit 0（tests=5 failures=0） |
| `i03_runner_quarantine_exit_20261104s5` | src/TestUnits/fixtures/i03_runner_quarantine_exit_20261104s5/run.sh | 19.9 | cta[pass]: cta：进程退出码 0，retained 诊断 1 行 （期望非 134/SIGABRT、未超时、诊断≥1）；uft[pass]: uft：进程退出码 0，retained 诊断 1 行 （期望非 134/SIGABRT、未超时、诊断≥1） |
| `i03_sdk_events_20261103` ★ | src/TestUnits/fixtures/i03_sdk_events_20261103/run.sh + run_t26_regression.sh | 134.7 | normal+asan+tsan[pass]: run.sh exit 0；non-baseline failures: 0；red-baseline[expected-red/red]: baseline（反向应用本目录补丁）11 场景：RED 11、PASS 0；t26-regression[pass]: run_t26_regression.sh exit 0（PASS 行 60） |
| `i03_servo_date_20261103` | src/TestUnits/fixtures/i03_servo_date_20261103/CMakeLists.txt | 124.0 | green[pass]: servo_date exit 0 (tests=8 failures=0 errors=0) |
| `i03_session_contract_20261103` | src/TestUnits/fixtures/i03_session_contract_20261103/CMakeLists.txt | 13.7 | green[pass]: i03_session_contract exit 0 (tests=49 failures=0 errors=0) |
| `i03_slice_shared_owner_20261103` ★ | src/TestUnits/fixtures/i03_slice_shared_owner_20261103/CMakeLists.txt | 73.4 | green-asan[pass]: asan-i03_slice_rdm exit 0 (tests=9 failures=0 errors=0)；asan-i03_slice_servo exit 0 (tests=5 failures=0 errors=0)；asan-i03_slice_replay exit 0 (tests=3 failures=0 errors=0)；asan-i03_slice_unit exit 0 (tests=6 failures=0 errors=0)；green-tsan[pass]: tsan-i03_slice_rdm exit 0 (tests=9 failures=0 errors=0) |
| `i03_storage_multiwriter_20261104` ★ | src/TestUnits/fixtures/i03_storage_multiwriter_20261104/CMakeLists.txt | 90.7 | green-asan[pass]: asan-mw_storage exit 0 (tests=6 failures=0 errors=0)；asan-mw_reader exit 0 (tests=2 failures=0 errors=0)；asan-mw_ad exit 0 (tests=9 failures=0 errors=0)；green-plain[pass]: plain-mw_storage exit 0 (tests=6 failures=0 errors=0)；plain-mw_reader exit 0 (tests=2 failures=0 errors=0)；plain-mw_ad exit 0 (tests=9 failures=0 errors=0)；green-tsan[pass]: tsan-mw_storage exit 0 (tests=6 failures=0 errors=0)；tsan-mw_reader exit 0 (tests=2 failures=0 errors=0)；tsan-mw_ad exit 0 (tests=9 failures=0 errors=0) |
| `i03_storage_read_20261104s5` ★ | src/TestUnits/fixtures/i03_storage_read_20261104s5/run.sh | 21.0 | green[pass]: green exit=0 tests="33" failures="0" |
| `i03_strategy_id_20261103` | src/TestUnits/fixtures/i03_strategy_id_20261103/run.sh | 25.4 | green[pass]: run.sh（Hft/Sel 策略 ID 检查） exit 0 |
| `i03_strategy_plugin_abi_20261104s5` ★ | src/TestUnits/fixtures/i03_strategy_plugin_abi_20261104s5/run.sh | 61.6 | green[pass]: live-green-results.txt 11/11 exit 0；mocker-green-results.txt 5/5 exit 0 |
| `i03_trader_oes_query_s4` | src/TestUnits/fixtures/i03_trader_oes_query_s4/run.sh | 14.3 | green[pass]: run.sh exit 0；4/4 exit 0 |
| `i03_trader_ratelimit_20261103` ★ | src/TestUnits/fixtures/i03_trader_ratelimit_20261103/run.sh | 93.7 | fix-plain[pass]: fix-plain exit 0（tests=24 failures=0）；fix-asan[pass]: fix-asan exit 0（tests=24 failures=0）；fix-tsan[pass]: fix-tsan exit 0（tests=24 failures=0）；base-plain[expected-red/red]: base(baseline/)-plain exit 1（tests=23 failures=13）；base-asan-batch[expected-red/red]: base(baseline/)-asan exit 134（no XML） |
| `i03_traders_other_20261104s5` ★ | src/TestUnits/fixtures/i03_traders_other_20261104s5/run.sh | 480.9 | fix[pass]: run.sh fix exit 0；t4002 7/7，t6009 10/10，r60003 11/11，r60001 4/4，t4003 9/9，r93006 4/4，lane 4/4 |
| `i03_uft_ledger_ctx_concurrency_20261104s5` ★ | src/TestUnits/fixtures/i03_uft_ledger_ctx_concurrency_20261104s5/run.sh | 77.2 | thread[pass]: run.sh thread exit 0；2/2 exit 0；address,undefined[pass]: run.sh address,undefined exit 0；2/2 exit 0 |
| `i03_uft_parser_lifecycle_20261104s5` ★ | src/TestUnits/fixtures/i03_uft_parser_lifecycle_20261104s5/run.sh | 54.7 | plain[pass]: run.sh exit 0；7/7 exit 0；thread[pass]: run.sh thread exit 0；7/7 exit 0 |
| `i03_uft_strategy_mgr_v2_20261104s5` ★ | src/TestUnits/fixtures/i03_uft_strategy_mgr_v2_20261104s5/run.sh | 29.4 | green[pass]: run.sh exit 0；4/4 exit 0 |
| `i03_wp1_identity_gate_20261104` ★ | src/TestUnits/fixtures/i03_wp1_identity_gate_20261104/run.sh | 260.7 | current-asan[pass]: current-asan exit 0（tests=41 failures=0）；current-tsan[pass]: current-tsan exit 0（tests=41 failures=0）；baseline-asan[expected-red/red]: baseline-asan exit 1（tests=11 failures=11）；baseline2-asan[expected-red/red]: baseline2-asan exit 1（tests=20 failures=2） |
| `i03_wp1_ledger_20261104s5` | src/TestUnits/fixtures/i03_wp1_ledger_20261104s5/run_r202_003.sh | 106.2 | green[pass]: run_r202_003.sh 当前 TraderAdapter exit 0（tests=2 failures=0） |
| `l03_commodity_state` | src/TestUnits/fixtures/l03_commodity_state/run.sh + run-init.sh + run-config.sh | 43.1 | probe[pass]: run.sh（probe normal+sanitizer） exit 0；config[pass]: run-config.sh（L03CommodityReady.*） exit 0；config-sanitize[pass]: run-config.sh L03_SANITIZE=1（L03CommodityReady.*） exit 0；init-target-probe(info)[pass/info]: run-init.sh exit 0（README 记 2026-11-02 未修复时 exit 1，当前已通过） |
| `l04_contract_import` ★ | src/TestUnits/fixtures/l04_contract_import/run.sh | 60.7 | normal[pass]: run.sh exit 0；sanitize[pass]: run.sh L04_SANITIZE=1 exit 0 |
| `l04_lmdb_key_bounds` ★ | src/TestUnits/fixtures/l04_lmdb_key_bounds/run.sh | 43.0 | green[pass]: run.sh（key_bounds ASan + 产品编译 + reader_rejection） exit 0 |
| `l05_csv_failure_signal` ★ | src/TestUnits/fixtures/l05_csv_failure_signal/run.sh | 23.4 | green+known-red[pass]: run.sh exit 0（green 全部 + KnownRed 子集 exit=1，脚本要求 1） |
| `lmdb_bar_key_v2` | src/TestUnits/fixtures/lmdb_bar_key_v2/run.sh | 7.3 | green[pass]: run.sh（normal + sanitized key test） exit 0 |
| `lmdb_bar_migration` | src/TestUnits/fixtures/lmdb_bar_migration/run_capability.sh + run_controlled_regression.sh (+ run.sh) | 133.4 | capability[pass]: run_capability.sh exit 0；14/14 exit 0；controlled-legacy[pass]: run_controlled_regression.sh exit 0；normal 30/30，asan 30/30；legacy-run.sh(info)[fail/info]: run.sh exit 1；normal 4/34 exit0，asan 0/0 exit0，tsan-raw 0/0 exit0，tsan-annotated 0/0 exit0（旧 callback 34 场景，evidence-controlled-legacy README 说明 5 项不再适用） |
| `lmdb_bar_migration_perf` | src/TestUnits/fixtures/lmdb_bar_migration_perf/run.sh | 23.4 | green[pass]: run.sh（cache guard normal+asan 5 模式） exit 0 |
| `module_helper_posix` | src/TestUnits/fixtures/module_helper_posix/run.sh | 5.3 | green[pass]: run.sh（fake dladdr 6 模式 + real dladdr） exit 0 |
| `p10_first_insert` ★ | src/TestUnits/fixtures/p10_first_insert/run.sh + run_tests.py | 68.0 | normal+tsan[pass]: run_tests.py exit 0：tsan Minute1 0 tsan Minute5 0 |
| `parser_module_dir` | src/TestUnits/fixtures/parser_module_dir/build.py | 12.8 | green[pass]: build.py（ParserXTP/ParserFemas after empty/valid/absolute） exit 0 |
| `r103_dispatched_snapshot` | src/TestUnits/fixtures/r103_dispatched_snapshot/run.sh | 35.0 | asan[pass]: run.sh asan exit 0；tsan[pass]: run.sh tsan exit 0 |
| `r14_runner_startup_overlap` ★ | src/TestUnits/fixtures/r14_runner_startup_overlap/run.sh | 253.8 | normal+asan+tsan[pass]: run.sh exit 0；36/36 exit 0 |
| `r15_tag_snapshot` | src/TestUnits/fixtures/r15_tag_snapshot/run.sh | 55.3 | asan[pass]: run.sh asan exit 0；tsan[pass]: run.sh tsan exit 0 |
| `r202_nextday` | src/TestUnits/fixtures/r202_nextday/run.py | 43.8 | green[pass]: R202NextDay/R202Reentry 及收盘正控在卡3/9d 合同下全部通过：R202NextDay.Cta 0，R202NextDay.Sel 0，R202NextDay.Hft 0，R202Reentry.Cta 0，R202Reentry.Sel 0，R202Reentry.Hft 0，R202CtaTicker.FullDayMidnightCloseEndsSessionAndStoresTradingDate 0，R202CtaTicker.OrdinaryCloseStillEndsSessionAndStoresTradingDate 0，R202SelTicker.FullDayMidnightCloseEndsSessionOnce 0，R202SelTicker.OrdinaryCloseStillEndsSession 0，R202HftTicker.FullDayMidnightCloseEndsSessionOnce 0，R202HftTicker.OrdinaryCloseStillEndsSession 0 |
| `r20_zstd_ubsan` | src/TestUnits/fixtures/r20_zstd_ubsan/run.sh | 71.6 | green[pass]: run.sh exit 0（ASan+UBSan 与普通 direct/wrapper/sweep）：direct normal=0 sanitized=0 sanitizer_reports=0 roundtrip_pass=1，wrapper normal=0 sanitized=0 sanitizer_reports=0 roundtrip_pass=1，sweep normal=0 sanitized=0 sanitizer_reports=0 roundtrip_pass=1 |
| `r22_timezone` | src/TestUnits/fixtures/r22_timezone/run.sh | 26.2 | green+red-baseline[pass]: run.sh exit 0；30 项，MISMATCH 0（含 baseline-tsan-race 红控） |
| `r23_old_mapping` ★ | src/TestUnits/fixtures/r23_old_mapping/build.sh | 177.7 | green+known-red[pass]: build.sh exit 0；OldViewMustNotExposeRecordsBeyondItsMappedLength=1/期望1，RollbackMustNotTruncateAnotherProcessesAppend=1/期望1，FailedRollbackKeepsPublishedCapacityAndAllowsRetry=0/期望0，FailedRemapPreservesOldProcessViewAndRetryCanRemap=0/期望0 |
| `r23_retry_wait` ★ | src/TestUnits/fixtures/r23_retry_wait/build.sh + run_tests.py | 568.3 | green+baseline[pass]: run_tests.py exit 0（baseline shutdown 期望 5，其余 0）：tsan shutdown 0 expected 0 tsan normal 0 expected 0 |
| `sel_factory_quarantine` | src/TestUnits/fixtures/sel_factory_quarantine/run.sh | 14.6 | green[pass]: run.sh exit 0；current 用例 18 个；历史红控 1 行 |
| `t26_fees_atomic` ★ | src/TestUnits/fixtures/t26_fees_atomic/run.sh | 594.1 | normal+asan+baselines[pass]: run.sh exit 0；baseline response_error old-failure-observed 0 \| baseline empty_terminal old-failure-observed -6 \| baseline earlier_error old-failure-observed 0 |
| `trader_module_path` | src/TestUnits/fixtures/trader_module_path/build_and_run.py | 30.2 | green[pass]: build_and_run.py exit 0 |

## 预期红（expected-red）（2）

| 套件 | 入口 | 耗时 s | 依据 |
|---|---|---|---|
| `i03_hft_fee_pipeline` | src/TestUnits/fixtures/i03_hft_fee_pipeline/run.sh | 23.6 | control[pass]: 控制用例 R71HftTest.TradeAccountingAndDynProfitExtremes 通过；fee-probes(known-red)[expected-red/demo]: README：四项按目标行为编写的红控当前应失败（现存缺陷）；FAILED 4：R71HftTest.FeeProbeActualFillMustUsePhysicalContractRate,R71HftTest.FeeProbeCloseMustUseProviderOffset,R71HftTest.FeeProbeFillMustReachOnlyItsOwner,R71HftTest.FeeProbeMissingMustRemainObservableAfterFill |
| `n05_timeout_guard_race` | src/TestUnits/fixtures/n05_timeout_guard_race/README.md（c++ 编译 + 两个进程） | 2.7 | sequential-control[pass]: sequential 控制 exit 0；race-counterexample[expected-red/demo]: 并发反例 exit 1（README：当前实现已知反例） |

## 失败（fail）（7）

### `i03_frequency_20261103`

- 入口：src/TestUnits/fixtures/i03_frequency_20261103/run.py
- 主命令：`/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/tree/src/TestUnits/fixtures/i03_frequency_20261103/run.py /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/i03_frequency_20261103/run`（exit 1，0.6s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/i03_frequency_20261103/logs/01-run.py.log`
- 结论依据：green[fail]: run.py（抽取的生产请求方法 + 本地 fake） exit 1
- 归因：待分析

### `p04_cross_session_clock`

- 入口：src/TestUnits/fixtures/p04_cross_session_clock/run.py + verify.py
- 主命令：`/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/p04_cross_session_clock/mirror/src/TestUnits/fixtures/p04_cross_session_clock/verify.py /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/p04_cross_se`（exit 1，12.8s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/p04_cross_session_clock/logs/02-verify.py.log`
- 结论依据：green+known-red[fail]: run.py exit 1；verify.py exit 1（green + known-red 期望）：green exit 1 expected 0
- 归因：待分析
- 备注：使用 --tree 的 APFS 克隆副本 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/p04_cross_session_clock/mirror 运行；路径改写见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/p04_cross_session_clock/mirror.patch

### `r13_real_schedule`

- 入口：src/TestUnits/fixtures/r13_real_schedule/run.py
- 主命令：`/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_real_schedule/mirror/src/TestUnits/fixtures/r13_real_schedule/run.py /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_real_schedule/run`（exit 1，26.2s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_real_schedule/logs/01-run.py.log`
- 结论依据：controlled-probe[fail/demo]: run.py exit 1，观测与脚本期望不符：AssertionError
- 归因：待分析
- 备注：使用 --tree 的 APFS 克隆副本 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_real_schedule/mirror 运行；路径改写见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_real_schedule/mirror.patch

### `r13_sel_period_contract`（★ 优先：条目验收证据）

- 入口：src/TestUnits/fixtures/r13_sel_period_contract/run.sh
- 主命令：`/bin/bash /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_sel_period_contract/mirror/src/TestUnits/fixtures/r13_sel_period_contract/run.sh /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_sel_period_contract/r`（exit 1，2.7s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_sel_period_contract/logs/01-run.sh.log`
- 结论依据：green+baseline-extract[fail]: run.sh exit 1（脚本逐项断言 baseline/current 期望；0 行）
- 归因：待分析
- 备注：使用 --tree 的 APFS 克隆副本 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_sel_period_contract/mirror 运行；路径改写见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r13_sel_period_contract/mirror.patch

### `r3_cancel_batch_cache`

- 入口：src/TestUnits/fixtures/r3_cancel_batch_cache/run.py
- 主命令：`/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/tree/src/TestUnits/fixtures/r3_cancel_batch_cache/run.py /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r3_cancel_batch_cache/run`（exit 1，2.3s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r3_cancel_batch_cache/logs/01-run.py.log`
- 结论依据：normal[fail]: run.py（single/failure 期望 0，batch 期望 1 已内置） exit 1；sanitize[fail]: run.py R3_SANITIZE=1（single/failure 期望 0，batch 期望 1 已内置） exit 1
- 归因：待分析

### `r3_split_reject`

- 入口：src/TestUnits/fixtures/r3_split_reject/run.py
- 主命令：`/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/tree/src/TestUnits/fixtures/r3_split_reject/run.py /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r3_split_reject/run`（exit 1，1.0s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/r3_split_reject/logs/01-run.py.log`
- 结论依据：green[fail]: run.py exit 1
- 归因：待分析

### `t26_request_completion`（★ 优先：条目验收证据）

- 入口：src/TestUnits/fixtures/t26_request_completion/run.sh
- 主命令：`/bin/bash /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/t26_request_completion/mirror/src/TestUnits/fixtures/t26_request_completion/run.sh /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/t26_request_completion/run`（exit 1，390.1s）
- 日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/t26_request_completion/logs/01-run.sh.log`
- 结论依据：green+known-red[fail]: run.sh exit 1；fee PASS 行 150，query PASS 行 51，baseline known-red 行 0
- 归因：待分析
- 备注：使用 --tree 的 APFS 克隆副本 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/t26_request_completion/mirror 运行；路径改写见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/final4/fixtures/t26_request_completion/mirror.patch

## 无法运行（blocked-env）（1）

### `r7_twap_sell_sign`

- 入口：src/TestUnits/fixtures/r7_twap_sell_sign/README.md（手工复现步骤）
- 结论依据：README 复现步骤依赖 `git archive ccaad010 src` 快照、docs/…/R7-002-total-times-ccaad010/CMakeLists.ubsan.patch 与前一节工作目录，且各节红控属于当时版本；套件无脚本入口，冻结树不含 docs
- 归因：夹具问题
- 建议：为当前版本提供一个自包含 run.sh：复制 --tree、追加本目录 .inc 与 position_fake_contract.patch、构建 CoverageTests_exefact 并运行 README 末节三个过滤集（unsettled_red 期望 exit 1）；UBSan CMake 补丁需随套件提供

## 不适用（not-applicable）（13）

| 套件 | 入口 | 耗时 s | 依据 |
|---|---|---|---|
| `femas_insert_return` |  | 0.0 | 目录只有 insert_return_test.cpp，无 README/脚本/CMake，CoverageTests 也未编入；仓库内无运行命令（docs 中仅出现在快照哈希清单） |
| `module_helper_odr` |  | 0.0 | 源码由 coverage CMake 编入 CoverageTests_modulehelper_odr（WT_COV_MODULE_HELPER_ODR_DIR / add_subdirectory windows），随 CoverageTests 回归运行 |
| `new034_monitor` |  | 0.0 | 目录只有 monitor_lifecycle.cpp，无 README/脚本/CMake，CoverageTests 未编入；无可执行入口 |
| `owner_delivery_result` |  | 0.0 | 空目录（0 个文件），无入口 |
| `r22_monitor_lifecycle` |  | 0.0 | 目录只有 probe.cpp，无 README/脚本，无运行入口 |
| `r22_monitor_owner` |  | 0.0 | 目录只有两个 probe 源码，无 README/脚本，无运行入口 |
| `r60_yd` |  | 0.0 | YD fixture 动态库源码由 coverage CMake 编入 CoverageTests_t_TraderYD（WT_COV_R60_YD_FIXTURE_DIR），无独立入口 |
| `r68_index_factory` |  | 0.0 | 由 coverage CMake add_subdirectory 编入 CoverageTests_dtcore；README 命令只是运行该 CoverageTests 目标的过滤集 |
| `r78_no_parser` |  | 0.0 | no_parser.cpp 由 coverage CMake 编为 R78NoParserFixture 供 CoverageTests 加载，无独立入口 |
| `t1_ctpopt_init` |  | 0.0 | missing_symbol.cpp 由 coverage CMake 编入（WT_COV_T1_CTPOPT_FIXTURE_DIR），无独立入口 |
| `t1_xtp` |  | 0.0 | 两个 fixture 源码由 coverage CMake 编入 CoverageTests_t_ParserXTP（WT_COV_T1_XTP_FIXTURE_DIR），无独立入口 |
| `t4_ctpopt_loader` |  | 0.0 | 两个 fixture 源码由 coverage CMake 编入（WT_COV_T4_CTP_OPT_FIXTURE_DIR），无独立入口 |
| `t4_femas` |  | 0.0 | 两个 fixture 源码由 coverage CMake 编入（WT_COV_T4_FEMAS_FIXTURE_DIR），无独立入口 |

## 在新冻结树上重跑

```sh
/usr/bin/python3 -I /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/fixtures/run_fixtures.py \
  --tree <新冻结根，含 src/ dist/> --build <该树 Apple clang Debug+coverage 构建> \
  --out <新输出目录> --repo-git /Users/yunjinqi/Documents/new_projects/wondertrader/.git \
  [--head <冻结 HEAD；缺省读 <tree>/../meta/HEAD.txt>] [--only a,b,c] [--parallel 2 --jobs 2]
```

