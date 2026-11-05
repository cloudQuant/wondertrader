# 最终验收：失败夹具套件处置（fixtures-agent）

2026-11-05。范围：`SCRATCH/final/fixtures/summary.json` 中分给本代理的失败、受阻套件 `i03_fee_consumer_pipeline`、`i03_fee_engine_gate`、`r20_zstd_ubsan`、`r22_timezone`，外加主代理补派的 `i03_trader_oes_query_s4` 运行器入口。其余失败套件已分给其他代理，不在本目录。

**无 PRODUCT 类结论。** 四个套件均为 STALE_FIXTURE，已修夹具；重跑后 4/4 pass。本目录只是独立夹具的定向重跑，不是整体回归；仅本机 macOS arm64、本地 fake/受控输入。

- 重跑树：`SCRATCH/fixtures-agent-fxtree`（`rsync -a --no-links` 工作区 `src` 与 `dist`；产品与测试源码与 F3 一致，另含各代理的夹具修改）
- 构建：`SCRATCH/final3/build`
- 输出：`SCRATCH/fixtures-agent-fxrun/`（各套件 `logs/`、`result.json`）；控制台 `SCRATCH/fixtures-agent-fxrun.log`（副本 `logs/fxrun.console.log`），汇总副本 `logs/fxrun-summary.json`
- 只读核对：首个套件开始后 `--build` 被修改的文件 0 个
- 命令：`/usr/bin/python3 -I SCRATCH/fixtures/run_fixtures.py --tree SCRATCH/fixtures-agent-fxtree --build SCRATCH/final3/build --out SCRATCH/fixtures-agent-fxrun --repo-git <repo>/.git --head ccaad010e9169f610aded4d8dc0f5647f70a5cff --annotations SCRATCH/fixtures/empty-annotations.json --only i03_fee_consumer_pipeline,i03_fee_engine_gate,r20_zstd_ubsan,r22_timezone --parallel 2 --jobs 2`
- 夹具文件与运行器哈希：`logs/fixture-files.sha256`
- 机器可读结论：`SCRATCH/fixtures/fixfix-fixtures-agent.json`

（SCRATCH = `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`）

| 套件 | 分类 | 重跑结论 | 关联条目 |
|---|---|---|---|
| `i03_fee_consumer_pipeline` | STALE_FIXTURE | pass（卡13d 四例逐进程全部 exit 0） | I03-NEW-008 |
| `i03_fee_engine_gate` | STALE_FIXTURE | pass（10/10） | I03-NEW-008 |
| `r20_zstd_ubsan` | STALE_FIXTURE | pass（direct/wrapper/sweep，ASan+UBSan 与普通均 exit 0、无报告） | R20_wtbtcore#1、I03-NEW-082 |
| `r22_timezone` | STALE_FIXTURE | pass（30 项期望全部 OK，含红控） | BUG-R22-002 |
| `i03_trader_oes_query_s4`（补加入口） | — | pass（4/4） | I03-NEW-101 |

## i03_fee_consumer_pipeline — STALE_FIXTURE

**根因。** 原套件是 2026-11-02 的“现存缺陷反例”，期望 [1,1,0]。卡13d（合约优先、品种回退；缺费报错；显式零费率有效；按实际成交合约）实现后，`HisDataReplayer` 以 `wt_fee::Schedule` 严格解析费用文件：无版本文件只解释为品种表，每个条目都必须有 open/close/closetoday/byvolume；缺费返回 NaN。F3 上观测为 [0,1,1]：

- 反例 1 的旧断言 `calc_fee != 0` 因缺费变为 NaN 而空泛通过；
- 反例 2 因条目不完整并缺合约元数据被拒；
- 旧品种正控的条目只给了 open/byvolume，被严格解析拒绝。

运行器还依赖已不存在的 `/tmp/wt-i03-l06-fees-20261102` 隔离构建。产品行为符合卡13d。CTPLoader 导出仍未迁移（无版本、`交易所.合约` 为键），属于 I03-NEW-008 已知的导出端阻塞项，现在表现为可观察的缺费，而不是静默 0，不是回归。coverage 没有“实际导出文件 → 回放计费”链路的等价用例（`HisDataReplayerRuntimeTest.*` 只覆盖手写品种键），所以修夹具，不判 SUPERSEDED。

**修改。**

- `consumer_test.cpp` 改为卡13d 合同下的四个正向用例：
  1. 未迁移导出键：初始化成功，`SHFE.au.2512` 计费为 NaN，`prepare_fee_context` 失败且不改写输出；
  2. 同一组导出费率写成 v2 `contracts`：按物理合约 `SHFE.au2512` 计费，按手 -0.25×2 = -0.5；导出的 `SHFE.rb2510` 显式零费率为 0；
  3. 合约级按金额费率：缺合约元数据时初始化失败；加载导出合约后按价格×数量×合约乘数×费率计费；
  4. 完整的旧品种键按金额费率经品种回退计费。
- `run.py` 改为 `run.py <out> <coverage build>`：
  - 由构建的 compdb 推导源码根，并核对 HisDataReplayer.cpp/.h、CodeHelper.hpp、FeeSchedule.hpp 哈希；
  - 去掉 `-MD/-MF`，不向构建目录写入；
  - 四例逐进程运行，必须全部 exit 0。
- 新增 `exported-contracts.json`：同一次 `t26_fees_atomic` normal-valid 导出的合约文件，取自 F3 运行（另两份导出文件与该次运行的哈希相同）。
- README 改写为当前合同，并保留原记录。

**结论。** pass。日志在 `SCRATCH/fixtures-agent-fxrun/i03_fee_consumer_pipeline/`，观测见 `logs/fee-consumer-outcomes.txt`。

## i03_fee_engine_gate — STALE_FIXTURE

**根因。** `RejectsUnsupportedEnvelopeWithoutChangingFees` 和 `InvalidFileAtInitIsRejectedByCtaRun` 用 `version: 2` 信封（含空表）作为“不支持/非法”样本。卡13d 后 `WtEngine::load_fees` 已经接入 v2 并接受这些文件，于是 `fee_config_valid()` 为 true，CTA `run()` 门禁不触发，访问空 `WtDtMgr`，UBSan 报 SEGV（`WtDtMgr.h:234`，exit 134），其后的用例都没有运行。产品符合卡13d。

**修改。**

- `fee_engine_gate.cpp` 的样本改为仍然非法的输入：`version: 3`（不支持的版本），以及缺必填字段的 v2 品种条目。后者与 `CoverageTests_wtcore::S5LiveFeeSchedule.InvalidVersionedTemplateBlocksStart` 同一合同。
- 其余断言全部保留：整文件拒绝、原费率快照与商品费率不变、run 门禁、恢复旧配置后重新有效。
- README 追加卡13d 更新说明。

**结论。** pass，10/10（6 个 R130EngineFee 与 4 个门禁用例）。

## r20_zstd_ubsan — STALE_FIXTURE

**根因。** 原脚本断言 vendored zstd 在 UBSan 下于 `zstd_compress.c:1326:46` 报 null+offset（已知红）。2026-11-05 R20_wtbtcore#1 / I03-NEW-082 已按上游修法修复该 UB（`evidence/20261105-s5-main-r20-zstd/`），F3 上 sanitized 探针 exit 0，脚本中的 `test "$code" -ne 0` 失败。脚本还依赖已不存在的 `/tmp/wt-i03-r20-20261102` 预编库。coverage 普通回归不做插桩，没有等价的 sanitizer 覆盖，所以把它改为修复后的回归检查。

**修改。**

- `run.sh` 改为自包含：只用本树 `src/WTSUtils/zstdlib/*.c`，分别以 LLVM 22 ASan+UBSan（遇报即停）和普通方式构建。direct、wrapper、sweep 三个模式在两种进程下都必须 exit 0、无 sanitizer 报告且往返通过。
- `probe.cpp` 新增 `sweep` 模式：级别 1–19 × 0/1/88/4096/200000 字节，单次调用与复用 CCtx 各一次，覆盖修复涉及的 fast/double_fast/lazy 策略。
- README 重写。

**红控。** 用 docs 证据 `before/` 中的四个改前文件覆盖临时副本（`SCRATCH/fixtures-agent-diag/r20-red/`）后运行同一脚本：三个模式的 sanitizer 进程都 exit 134，报 `zstd_compress.c:1326:46: runtime error: applying non-zero offset 1 to null pointer`，脚本 exit 1（`logs/r20-red-results.txt`、`logs/r20-red-ubsan-line.txt`）。检查能区分修复前后。仓库内不保存改前源码，脚本本身不跑红控。

**结论。** pass（`logs/r20-green-results.txt`）。

## r22_timezone — STALE_FIXTURE

**根因。** `build.sh` 无条件从 `docs/` 证据复制改前 TimeUtils 头，而冻结树只含 src/dist，set -e 即中止。套件内没有运行入口，`run.py` 只在 docs 证据目录里，所以记为 blocked-env。产品 `getTZOffset` 的实现与探针的宏替换点一致，探针仍然适用。coverage 只有单线程的 `r16_timeutils.tz_offset_is_cached_and_in_whole_hours`，没有首次并发竞态（TSan）与失败重试的等价覆盖。

**修改。**

- 新增 `baseline/TimeUtils.hpp` 与 `baseline/SHA256SUMS`。该文件是 docs 证据 `TimeUtils.baseline.hpp` 原件，sha256 `4a649159…f`，只作红控，不是当前实现。
- `build.sh` 改用夹具内的基线，并先校验哈希。
- 新增 `run.sh`，复现 docs `run.py` 的全部场景并逐项判定，共 30 项：
  - `baseline-tsan-race` 红控，须非零退出且报 data race；
  - 当前头的 TSan 竞态 3 次；
  - 10 个时区下的当前头与改前头；
  - cache；
  - 4 种受控失败与重试；
  - r16 既有用例。
- 新增 README。

**结论。** pass：30/30 OK，红控 exit 134 且报 1 处 data race（`logs/r22-results.txt`）。

## i03_trader_oes_query_s4 — 运行器入口（主代理补派）

slice4 新建的夹具（I03-NEW-101）在 `run_fixtures.py` 里原先没有入口，已加处理器 `s_trader_oes_query`：调用 `sh run.sh <out>`，判定依据是 `results.txt` 中每行 exit=0 且脚本 exit 0。脚本只有绿模式，套件内没有改前 TraderOES 源码，红控不运行，已记入 notes。重跑 4/4 pass。

## 运行器修改（`SCRATCH/fixtures/run_fixtures.py`，均为单套件处理器）

- `s_fee_consumer_pipeline`：删除原 mirror 路径改写，直接调用 `run.py <out> <--build>`，主变体判定。
- `s_r20`：删除依赖共享 asan/plain WTSUtils 与 mirror 改写的旧逻辑，直接调用自包含的 `run.sh <out>`，主变体判定。
- `s_r22_tz`：由 Blocked 改为调用新的 `run.sh <out>`，按 `results.txt` 判定（含红控）。
- 新增 `s_trader_oes_query`。

修改前都先读了文件的最新内容；slice4 对 `r202_nextday` 入口的修改未被改动。
