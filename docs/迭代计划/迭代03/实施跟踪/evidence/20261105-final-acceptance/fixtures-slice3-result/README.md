# 最终验收：失败夹具套件处置（slice3-result）

- 任务：`SCRATCH/fixtures/FIXFIX-PROMPT.md`（SCRATCH=`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`）；本目录只有 slice3-result 写入。
- 机器可读结果：`SCRATCH/fixtures/fixfix-slice3-result.json`。
- 重跑树：`SCRATCH/slice3-result-fxtree`（`rsync -a --no-links` 工作区 src/dist，含夹具修改）；构建复用 `SCRATCH/final3/build`；运行输出 `SCRATCH/slice3-result-fxrun/`（全部 10 个套件）、`SCRATCH/slice3-result-fxrun2/`（构建型 4 个套件的第二轮）与 `SCRATCH/slice3-result-fxrun3/`（i03_exec_eager_validation 修正 asan 链接参数后的第三轮），运行器总日志 `SCRATCH/s3fx/fxrun.log`、`fxrun2.log`、`fxrun3.log`。运行器 `run_fixtures.py` 未修改。
- 未发现 PRODUCT 类问题；i03_fractional_targets 的 3 个 scale 用例已确认不是产品回归（见下）。不修改产品代码与迭代跟踪文档。

## i03_fractional_targets_20261103

- 分类：**STALE_FIXTURE**
- 条目：BUG-P02-005、BUG-P02-002、BUG-R10-001
- 根因：卡8b=A eager 全量预校验后，Local/Arbi/Diff 执行器 validateConfig() 要求 policy（含 default、<factory>.<unit> 形状），缺 policy 报 "policy is missing" 拒绝（ExecPolicyConfig.h:44-51）。夹具 3 例 ExecutionScaleRequiresCompletePositiveNumber 的正向配置只有 scale，不是完整配置，故 init 返回 false。readExecutionScale 对 "1.25"/次正规数字符串仍接受，不是产品回归。
- 改动：src/TestUnits/fixtures/i03_fractional_targets_20261103/{local,arbi,diff}.inc：该用例全部 15 个配置统一带合法 policy，使非法 scale 拒绝只由 scale 触发、正向配置完整；断言与 scale 输入不变。README.md 追加处置说明。
- 重跑结论：pass：green[pass]: run.sh exit 0（tests=44 failures=0 errors=0，fractional+groups 合并过滤）
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_fractional_targets_20261103/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_fractional_targets_20261103/result.json`）
- 第一轮（`SCRATCH/slice3-result-fxrun/`）该套件因机器负载（load average 55–94）在快照构建阶段超过运行器 1200s 上限被中止，未进入测试；上面结论来自负载下降后的第二轮 `SCRATCH/slice3-result-fxrun2/`。

## i03_execution_groups_20261103

- 分类：**STALE_FIXTURE**
- 条目：BUG-P02-005
- 根因：无独立入口，经 i03_fractional_targets_20261103/run.sh 合并编译运行；F3 上本目录过滤用例 18/18 通过，来源套件失败仅因上项 3 例。
- 改动：无（随来源套件修复）。
- 重跑结论：pass：无独立入口：README 规定经 i03_fractional_targets_20261103/run.sh 合并编译运行；本目录 filter.txt 匹配 18 例，失败 0；来源套件结论 pass
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_execution_groups_20261103/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_execution_groups_20261103/result.json`）
- 第一轮（`SCRATCH/slice3-result-fxrun/`）该套件因机器负载（load average 55–94）在快照构建阶段超过运行器 1200s 上限被中止，未进入测试；上面结论来自负载下降后的第二轮 `SCRATCH/slice3-result-fxrun2/`。

## i03_diff_batch_atomic_20261103

- 分类：**STALE_FIXTURE**
- 条目：BUG-P02-005
- 根因：无独立入口，经 i03_fractional_targets_20261103/run.sh 合并编译运行；F3 上本目录过滤用例 17/17 通过，来源套件失败仅因上项 3 例。
- 改动：无（随来源套件修复）。
- 重跑结论：pass：无独立入口：README 规定经 i03_fractional_targets_20261103/run.sh 合并编译运行；本目录 filter.txt 匹配 17 例，失败 0；来源套件结论 pass
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_diff_batch_atomic_20261103/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun2/i03_diff_batch_atomic_20261103/result.json`）
- 第一轮（`SCRATCH/slice3-result-fxrun/`）该套件因机器负载（load average 55–94）在快照构建阶段超过运行器 1200s 上限被中止，未进入测试；上面结论来自负载下降后的第二轮 `SCRATCH/slice3-result-fxrun2/`。

## i03_exec_eager_validation_20261103

- 分类：**STALE_FIXTURE**
- 条目：BUG-R10-001、BUG-P02-002、BUG-R134-003、BUG-WSN06-001
- 根因：green：runner 用例 I03RunnerSelStrategies.ValidSelConfigCreatesInitializesAndRunsOnInit 的合法 SEL 配置中 period: min 条目沿用 time: 1500，被 K27=A（min 周期 time 为 1..1440 分钟间隔）拒绝。red：baseline/ 改前 6 个产品源与当前树不可组合（WtPorter/WtExecMon 链接缺 WtLocalExecuter::validateConfig），red-unbuildable。green-asan：整树 ASan 构建受运行器 1200s 上限约束。
- 改动：src/TestUnits/fixtures/i03_exec_eager_validation_20261103/runner.inc：i03SelEntry() 增加 time 参数（默认 1500），min 条目传 5。run.sh：asan 分支显式 -DCMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS 后追加调用方 ${LDFLAGS:-}（显式参数会覆盖 CMake 从 LDFLAGS 的初始化，运行器的 nanomsg rpath 适配因此丢失；第二轮 green-asan exec 因 dyld 找不到 @rpath/libnanomsg.6.dylib 而 abort 134）。README.md 追加处置说明（red 记为 blocked/not-applicable）。
- 重跑结论：pass：（自带红控无法与当前树组合构建，未形成红绿对照）green[pass]: green：3/3 exit 0；red[red-unbuildable/red]: run.sh exit 1，未生成 exit_codes.txt（构建失败？见 /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun3/i03_exec_eager_validation_20261103/run-red/build.log）；green-asan[pass]: green-asan：3/3 exit 0
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun3/i03_exec_eager_validation_20261103/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun3/i03_exec_eager_validation_20261103/result.json`）
- 运行经过：第一轮（`SCRATCH/slice3-result-fxrun/`）因机器负载（load average 55–94）快照构建超过 1200s 上限被中止；第二轮（`SCRATCH/slice3-result-fxrun2/`）green 3/3 通过（exec 5、runner 15、r104 8），red 不可构建，green-asan 的 exec 步骤 dyld 找不到 `@rpath/libnanomsg.6.dylib` 而 abort 134（runner/r104 asan 通过）；修正 run.sh asan 链接参数后第三轮（`SCRATCH/slice3-result-fxrun3/`）green 3/3、green-asan 3/3（exec 5、runner 15、r104 8，0 条 sanitizer 报告），red 记为 blocked/not-applicable。

## r13_sel_period_contract

- 分类：**SUPERSEDED**
- 条目：BUG-R13-004
- 根因：extract.py 按固定文本从 WtRtRunner.cpp/WtPorter.cpp 抽取函数体链接替身；K27=A 后源码已改写，抽取锚点断言失败，未编译探针。
- 改动：src/TestUnits/fixtures/r13_sel_period_contract/README.md：追加处置与取代说明（含 NULL/空 period 子项无 coverage 直接用例的缺口）。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 fail：green+baseline-extract[fail]: run.sh exit 1（脚本逐项断言 baseline/current 期望；0 行）
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `CoverageTests_porter::R13PorterSelExport.create_sel_context_registers_contexts_for_supported_period_tokens`
  - `CoverageTests_porter::R13PorterSelExport.create_sel_context_rejects_schedules_that_can_never_fire`
  - `CoverageTests_porter::S3R13SelPublicSchedule.DocumentedPeriodsCreatedThroughTheCApiFireOnTheirSchedule`
  - `CoverageTests_porter::S3R13SelPublicSchedule.AliasesAndUnschedulableInputsAreRejectedAndNeverFire`
  - `CoverageTests_porter::I03S5RtRunnerSel.InvalidSelPeriodIsRejectedByPreflight`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r13_sel_period_contract/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r13_sel_period_contract/result.json`）

## r13_real_schedule

- 分类：**SUPERSEDED**
- 条目：BUG-R13-004、BUG-R192-002、BUG-R192-003
- 根因：run.py 依赖 /tmp 旧隔离快照并在复制的 WtSelEngine.cpp 按固定文本注入测试 join；当前调度实现已改写，锚点断言失败；original/joined 是旧缺陷已知红。
- 改动：src/TestUnits/fixtures/r13_real_schedule/README.md：追加处置与取代说明。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 fail：controlled-probe[fail/demo]: run.py exit 1，观测与脚本期望不符：AssertionError
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `CoverageTests_wtcore::S5SelScheduleThread.FiredTaskRunsOnceOnItsContextAndIsJoined`
  - `CoverageTests_wtcore::S5SelScheduleThread.TasksOnSeveralDaysAreAllJoinedByRelease`
  - `CoverageTests_wtcore::R211SelEngine.ExpiredTaskTerminatesBeforeSchedule`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.WeeklyTaskFiresOnTargetWeekdayOnceAtItsMinute`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.MonthlyTaskUsesTheDayOfMonth`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.MonthlyTargetDayInsideHolidaysIsCarriedForward`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.YearlyTaskUsesMonthAndDay`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.MinuteTaskFiresOnSessionMinuteMultiplesOfItsInterval`
  - `CoverageTests_wtcore::S3K27SelPeriodFire.MinuteTaskWithoutSessionNeverFires`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r13_real_schedule/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r13_real_schedule/result.json`）

## r3_cancel_batch_cache

- 分类：**SUPERSEDED**
- 条目：BUG-R3-002、BUG-R3-003
- 根因：run.py 抽取 TraderAdapter doCancel/cancel 方法体与替身编译；当前实现引用 RiskParams/getRiskParams/_risk_mon_enabled/CodeTimeCacheMap/预留等替身缺失标识符，两种模式均编译失败；其红控为修复前已知红。
- 改动：src/TestUnits/fixtures/r3_cancel_batch_cache/README.md：追加处置与取代说明（批量撤单缓存记账的等价证明在 i03_trader_ratelimit 夹具，coverage 二进制无直接用例）。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 fail：normal[fail]: run.py（single/failure 期望 0，batch 期望 1 已内置） exit 1；sanitize[fail]: run.py R3_SANITIZE=1（single/failure 期望 0，batch 期望 1 已内置） exit 1
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `CoverageTests_wtcore::R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`
  - `CoverageTests_wtcore::R3TraderTest.cancel_localid_synchronous_api_failure_is_not_cached`
  - `CoverageTests_wtcore::R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache`
  - `CoverageTests_wtcore::R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code`
  - `CoverageTests_wtcore::R170TraderRisk.CancelWindowRejectsAtBoundaryAndRecovers`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.cancel_boundary_batch_uses_same_window`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.batch_cancel_survives_sync_cancel_reports`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r3_cancel_batch_cache/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r3_cancel_batch_cache/result.json`）

## r3_split_reject

- 分类：**SUPERSEDED**
- 条目：BUG-R3-002、BUG-R3-003、BUG-R3-008
- 根因：run.py 抽取 TraderAdapter::buy/sell 方法体与替身编译；当前实现调用 submitSplit 等新辅助函数，14 个编译错误；其红控为修复前已知红，限流器只是测试接缝。
- 改动：src/TestUnits/fixtures/r3_split_reject/README.md：追加处置与取代说明（拆单子单被拒的等价证明在 i03_trader_ratelimit 夹具，coverage 二进制无直接用例）。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 fail：green[fail]: run.py exit 1
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.buy_split_partial_rate_reject`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.sell_split_partial_rate_reject`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.buy_and_sell_split_partial_api_failure`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.buy_and_sell_split_all_rejected`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.sell_close_long_split_partial_failure`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.nested_buy_inside_split_keeps_both_ledgers`
  - `CoverageTests_wtcore::R3TraderTest.buy_splits_by_rule_cap_and_single_order_max_volume`
  - `CoverageTests_wtcore::R59Buy.open_splits_by_limit_volume_and_handles_market_price`
  - `CoverageTests_wtcore::R59Sell.open_splits_by_limit_volume_and_handles_market_price`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r3_split_reject/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r3_split_reject/result.json`）

## i03_frequency_20261103

- 分类：**SUPERSEDED**
- 条目：BUG-R3-002、BUG-R3-003
- 根因：run.py 按签名抽取 TraderAdapter 6 个方法体与 check.cpp 替身编译；当前方法体引用 account_net_limits::Registry、_account_gate、getPosition、getUndoneQty 等替身缺失成员，20 个编译错误，探针未运行。
- 改动：src/TestUnits/fixtures/i03_frequency_20261103/README.md：追加处置与取代说明。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 fail：green[fail]: run.py（抽取的生产请求方法 + 本地 fake） exit 1
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `CoverageTests_wtcore::R3TraderTest.order_freq_window_denies_submission_after_boundary`
  - `CoverageTests_wtcore::R3TraderTest.order_freq_window_precheck_uses_standard_code_after_raw_submission`
  - `CoverageTests_wtcore::R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`
  - `CoverageTests_wtcore::R170TraderRisk.OrderWindowRejectsAtBoundaryAndRecovers`
  - `CoverageTests_wtcore::R170TraderRisk.CancelWindowRejectsAtBoundaryAndRecovers`
  - `CoverageTests_wtcore::R90TraderRisk.FrequencyWindowsRejectStandardCodeAfterRawCodeEvents`
  - `CoverageTests_wtcore::P11PreOrderRisk.order_frequency_window_rejects_standard_code_after_successful_orders`
  - `fixture:i03_trader_ratelimit_20261103::I03RateLimit.*`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/i03_frequency_20261103/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/i03_frequency_20261103/result.json`）

## r7_twap_sell_sign

- 分类：**SUPERSEDED**
- 条目：BUG-R7-012
- 根因：无脚本入口；README 复现依赖 git archive ccaad010、docs/ 下 UBSan CMake 补丁与前一节工作目录，各节红控属于修复前版本。
- 改动：src/TestUnits/fixtures/r7_twap_sell_sign/README.md：追加处置与取代说明（回报顺序反例无 coverage 直接用例的缺口）。
- 重跑结论：SUPERSEDED，不修；运行器原样结论 blocked-env：README 复现步骤依赖 `git archive ccaad010 src` 快照、docs/…/R7-002-total-times-ccaad010/CMakeLists.ubsan.patch 与前一节工作目录，且各节红控属于当时版本；套件无脚本入口，冻结树不含 docs
- 由以下用例取代（coverage 用例在 `SCRATCH/final3/run/results.jsonl` 中均为 pass；`fixture:` 前缀为 F3 夹具运行通过的独立夹具用例）：
  - `CoverageTests_exefact::r7_twap.sell_side_keeps_sign_and_splits_evenly`
  - `CoverageTests_exefact::r7_twap.opposite_live_order_canceled_first_without_new_order`
  - `CoverageTests_exefact::r7_twap.cancel_report_refires_with_cancel_times_price_step`
  - `CoverageTests_exefact::r7_twap.tiny_diffqty_floors_to_min_open_lots`
  - `CoverageTests_exefact::r7_twap_legal_lot.sell_rounds_down_to_legal_unit_and_keeps_odd_remainder`
  - `CoverageTests_exefact::r7_twap_legal_lot.t1_sell_is_capped_by_available_then_legal_unit`
  - `CoverageTests_exefact::r7_twap_legal_lot.buy_and_target_change_use_legal_units_only`
  - `CoverageTests_exefact::r7_twap_legal_lot.whole_lot_future_control_and_fractional_target_remainder`
  - `CoverageTests_exefact::r7_twap_legal_lot.unavailable_lot_rules_hold_every_order`
  - `CoverageTests_exefact::n05_unmanaged_cancel_latch.twap_ready_suppresses_latched_cancel_and_keeps_unlatched_behavior`
- 重跑日志：`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r7_twap_sell_sign/logs/`（结果 `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/slice3-result-fxrun/r7_twap_sell_sign/result.json`）

## 缺口（需主代理决定）

- r13_sel_period_contract：`create_sel_context` 以 NULL/空 period 返回零句柄的子项，coverage 无直接用例。
- r3_cancel_batch_cache / r3_split_reject：批量撤单缓存记账、拆单子单被拒的返回值与未完成量，coverage 二进制无直接用例，仅由独立夹具 `i03_trader_ratelimit_20261103`（真实 TraderAdapter，F3 plain/ASan/TSan 24/24）覆盖。
- r7_twap_sell_sign：撤单回报与仓位落账先后顺序的反例无 coverage 直接用例（原 README 亦说明 fake 不证明真实柜台合同）。
- i03_exec_eager_validation：red 模式 blocked/not-applicable（基线源码与当前树不可组合）。
