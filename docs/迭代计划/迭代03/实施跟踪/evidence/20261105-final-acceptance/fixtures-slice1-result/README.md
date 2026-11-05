# 最终验收：第 1 段失败夹具套件处置（slice1-result，2026-11-05）

范围：主代理分配的 6 个独立夹具套件。冻结树 F3（`SCRATCH/final3/tree`）上失败结果见 `SCRATCH/final/fixtures/<suite>/`。本段只改 `src/TestUnits/fixtures/<suite>/` 下文件（均已用 `lock.py slice1-result FIXTURE-<suite>` 登记），未改产品代码与迭代跟踪文档；另对共用运行器 `SCRATCH/fixtures/run_fixtures.py` 做了一处最小改动（见 cta_factory_quarantine）。`SCRATCH` = `/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`。

**结论：6 个套件均为 STALE_FIXTURE，无 PRODUCT 类；修复后在“F3 + 夹具修改”树上全部 pass。** 这是夹具定向复跑，不是整体回归。

## 重跑方法

```sh
rsync -a --no-links <repo>/src <repo>/dist SCRATCH/slice1-result-fxtree/
/usr/bin/python3 -I SCRATCH/fixtures/run_fixtures.py --tree SCRATCH/slice1-result-fxtree --build SCRATCH/final3/build \
  --out SCRATCH/slice1-result-fxrun --repo-git <repo>/.git --head ccaad010e9169f610aded4d8dc0f5647f70a5cff \
  --annotations SCRATCH/fixtures/empty-annotations.json \
  --only i03_wp1_identity_gate_20261104,t26_fees_atomic,t26_request_completion,i03_sdk_events_20261103,ctpopt_time_snapshot,cta_factory_quarantine \
  --parallel 2 --jobs 2
```

- 运行前核对：fxtree 与 F3 在 `src/` 下除 `TestUnits/fixtures/` 外无差异（`diff -rq`），涉及的产品文件（TraderAdapter、AccountTradingGate、CTPLoader/TraderSpi、CtaStrategyMgr、TraderCTPOpt、TimeUtils、VendorTdLifecycle）逐个 `cmp` 一致。
- 运行器总日志：`logs/fxrun-1.log`（exit=0）；汇总 `logs/fxrun-summary.json`；各套件 `logs/<suite>/`（result.json、mirror.patch、逐步日志与结果文件）。原始输出目录 `SCRATCH/slice1-result-fxrun/<suite>/`。
- 修改文件的改前/改后副本在 `before/`、`after/`，哈希 `sources.sha256`；运行器改动 `run_fixtures.slice1.diff`。

## 逐套件

### i03_wp1_identity_gate_20261104 — STALE_FIXTURE，pass

- 根因（先确认不是产品回归）：F3 上 current-asan、current-tsan 各 1 例失败，都是 `Wp1Test.OwnerScopeDoesNotLeakIntoSinkCallbacks`（`wp1_test.cpp:961`）。owner `exec:a` 的 `buyAsOwner` 在 `orderInsert` 同步回报中，触发非 owner 观察者在**同一合约**上裸 `buy()`，期望成功，实际返回空。决策 K44（2026-11-04 用户裁定：owner 路由开启时 HFT/人工裸 API 订单参与合约归属）已实现于 `TraderAdapter::doEntrust`：无 owner 的入口以 `manual` owner 独占调用 `AccountTradingGate::claimContract`，该合约已被 `exec:a` 认领，所以拒绝（“manual entrust rejected”）。夹具期望早于 K44。佐证：改写后同一回调中另一合约的裸单被接受且 owner 为空，说明拒绝来自合约归属，而不是一般性的重入拒绝。
- 修改：`wp1_test.cpp` 该例按 K44 改写，仍检查“回调内下单不继承 owner exec:a”：
  - 同合约裸单必须被拒绝；若继承了 exec:a，它会以 exec:a 身份被接受。
  - 另一合约 `SHFE.au.2212` 的裸单必须被接受，账本 owner 为空，不计入 exec:a 的在途；若继承，会记到 exec:a。
  - 其余用例未改；`README.md` 增加说明。`run.sh` 未改：`git rev-parse` 与 docs `s5before` 依赖仍由运行器在镜像中处理，baseline3 红控 not-applicable。
- 重跑：current-asan 41 例 0 失败（39 过、2 个 SIGKILL 子进程辅助例按设计 SKIP），current-tsan 41 例 0 失败；baseline-asan 11/11 红、baseline2-asan 20 例中 2 红，与 F3 相同，为预期红控。改写的用例不在 baseline/baseline2 构建中编译；这是期望更新，无改前红控（K44 之前的产品不在套件内）。
- 日志：`logs/i03_wp1_identity_gate_20261104/`（含 `current-asan-test.log`、`current-tsan-test.log`）。
- 条目：BUG-I02-NEW-008、BUG-I02-NEW-154、BUG-L01-014、BUG-L01-016、BUG-N05-003、BUG-R3-008、BUG-R54-001。

### t26_fees_atomic — STALE_FIXTURE，pass

- 根因（先确认探针 exit 97、“0 sent” 的原因）：K02=B（Loader 所有模式 `run()` 返回结果码、插件内不再 `exit`）之后，`CTraderSpi::Finish()` 只经 `loader_completion::finish()` 发布终局结果并返回。`fee_probe.cpp` 依赖回调内 `exit()` 结束进程，回调序列走完后落到 `return 97`。F3 日志中三份文件已正确写出，终局结果码为 0。“0 sent, 1 confirmed” 是卡14 14a 的诊断计数：探针用 `arm()` 直接登记请求号，不经 `FinishFeeSubmission`，所以 `_fee_sent` 为 0；该计数按设计不作为成功依据，与判定无关。不是产品回归。
- 修改：`fee_probe.cpp` 仅在当前代码构建（`WT_T26_PROBE_ARM_REQUESTS`，normal/asan）中，把两处 `return 97` 改为 `probeResult()`：已有终局结果时以该结果码退出（首个结果生效，与原先首次 `exit()` 一致），否则仍为 97。baseline/unit-baseline 流程不变。`README.md` 增加说明。
- 重跑：`run.sh` exit 0；普通 75/75、ASan/UBSan 75/75；`results.txt` 与套件内 `evidence-20261104/results.txt` 逐字节相同，含 18 项单位红控与 5 项基线红控的退出码。
- 日志：`logs/t26_fees_atomic/`。条目：BUG-T26-007。

### t26_request_completion — STALE_FIXTURE，pass

- 根因：`run.sh` 把 `baseline-cpp.patch` 反向应用到移动中的工作区 `TraderSpi.cpp`；T26-011 之后又经卡14、K02=B 修改，19 个 hunk 中 12 个失败，脚本中止。此外有三处夹具问题：
  - `adapt_fee_probe.py` 的文本锚点在更新后的 `fee_probe.cpp` 中已不存在；
  - `query_probe.cpp`（11-04 已按 K02=B 改）在早于 K02=B 的基线上无法编译；
  - TSan 下以 `std::_Exit` 跳过拆除，会报告加载器线程未 join。
- 修改：
  - 新增 `baseline/CTPLoader/TraderSpi.{cpp,h}`，即 T26-011 改前的固定副本（`065f27b0…`/`903cea67…`）。它由不可变提交 `ccaad010` 加 `t26_fees_atomic/evidence-20261102` 的 T26-007 补丁重建；对它正向应用本目录 `baseline-*.patch`，恰得 2026-11-02 候选 `dd4640b5…`/`aff40149…`，与 `evidence-20261102/source-before.sha256` 一致。`run.sh` 改为复制该副本并校验哈希。
  - `query_probe.cpp`：基线（`-DWT_T26_TEST_BASELINE`）用空实现的 `loader_completion` 垫片编译，流程不变；当前构建在退出前有界调用 `WaitWorkersStopped`，若有 worker 阻塞在 SDK 调用中，只 join 已返回的 watchdog。
  - `adapt_fee_probe.py` 改为在 `fee_probe.cpp` 前定义 `WT_T26_PROBE_ARM_REQUESTS`（该探针已自带同样的请求登记）。
  - `README.md` 增加说明。`run_queries.py` 与期望未改。
- 重跑：`run.sh` exit 0；fee 150 PASS（normal/asan 各 75），query 60 PASS（normal/asan/tsan 各 20）；baseline 两项 known-red：delayed exit=1、sync exit=91。`fee-results.txt` 与 `evidence-20261102/fee-results.txt` 逐字节相同，`query-results.txt` 除耗时外相同。
- 日志：`logs/t26_request_completion/`。条目：BUG-T4-015、BUG-T6-002。

### i03_sdk_events_20261103 — STALE_FIXTURE，pass

- 根因：
  - 卡14 补丁反向应用到工作区失败（13 个 hunk 中 4 个失败）；
  - 探针依赖回调内 `exit()`，K02=B 之后加载器不再退出；
  - `run_t26_regression.sh` 的 TSan 报 `CTraderSpi::AppendQuery` 线程 thread leak。该线程由 `~CTraderSpi`/`WaitWorkersStopped` 回收（I03-NEW-123 的宿主拆除先有界停止并 join），是探针用 `std::_Exit` 跳过拆除所致，属探针问题，不是产品泄漏。
- 修改：
  - 新增 `baseline/CTPLoader/TraderSpi.{cpp,h}`，即卡14 改前的固定副本 = 2026-11-02 T26-011 候选 `dd4640b5…`/`aff40149…`；对它正向应用本目录补丁，恰得 2026-11-03 卡14 状态 `85bc0dbb…`/`7b434cc9…`，与 `t26_fees_atomic` 2026-11-04 的记录一致。
  - `run.sh` 改为复制并校验哈希，基线加 `-DWT_T26_TEST_BASELINE`。
  - `ctploader_events_probe.cpp`：当前构建在原先 `exit()` 会结束进程的位置读取 `loader_completion` 的结果并退出，退出前有界停止并 join 加载器线程；基线以垫片编译，流程不变。
  - `README.md` 增加说明。`run_probe.py` 的场景与期望未改。
- 重跑：normal/asan/tsan 各 11 场景全部 PASS（non-baseline failures 0）；baseline 11 场景全部 RED；`run_t26_regression.sh` exit 0，60 PASS，其中 TSan 20/20，无 ThreadSanitizer 报告。说明：运行器 red 项的说明文字仍写着“反向应用本目录补丁”，这是运行器的旧文案，实际基线来自 `baseline/` 固定副本。
- 日志：`logs/i03_sdk_events_20261103/`。条目：BUG-R65-002、BUG-T26-011、T1-CTPOpt-07、T2-OPT-04。

### ctpopt_time_snapshot — STALE_FIXTURE，pass

- 根因：`build.py` 私有快照的复制清单过期。当前 `TraderCTPOpt.h` 新增 `#include "../TraderCTP/VendorTdLifecycle.hpp"`，快照缺该头，产品 TU 编译失败。
- 修改：`build.py` 额外复制 `src/TraderCTP/VendorTdLifecycle.hpp`（只此一个头）；新增 `README.md`（原目录无 README），写明用法、red 依赖 docs 证据基线以及本次更新。`run.py`、`confirm_snapshot.cpp` 未改。
- 重跑：green 27 个模式全部 exit 0。red 变体需要 `docs/…/I03-NEW-006-ctpopt/baseline`（不在套件内），记为 not-applicable。
- 日志：`logs/ctpopt_time_snapshot/`。条目：I03-NEW-006（CTPOpt 子项）。

### cta_factory_quarantine — STALE_FIXTURE，pass

- 根因：`run.sh` 以浮动的 `git HEAD` 构建历史红控。冻结 HEAD `ccaad010` 已包含该修复，head-red 第一例 exit 0，被脚本判为 `unexpected historical control result`。current 17 例本来就通过。
- 修改：
  - `run.sh` 把历史红控固定为编写夹具时的 HEAD `9094fd8e6de73d19b9f011ef8a70b23416efe7da`（依据 `evidence/NEW-033/cta/pathfix2/run.cUlYnDfP/head-commit.txt`；`CtaStrategyMgr.cpp` 自 `325a4d5f` 起未变），并校验 sha256 `1d1c42ca…`（与该证据的 `sha256.txt` 一致）。`head-commit.txt` 仍记录当前 HEAD，`historical-control-commit.txt` 记录固定提交。`README.md` 增加说明。
  - 运行器 `run_fixtures.py` 的 cta 处理器中，“`git show HEAD:…` → 冻结 HEAD”这一改写项改为可选（min_count 0）。否则新 run.sh 中找不到旧文本，运行器会报错。处理器的说明文字未改。
- 重跑：current 17/17 exit 0；head-red 6 例全部非零（134/1）；head-positive exit 0；基线哈希校验 OK。运行器在 ≤128 字符的短路径下运行。
- 日志：`logs/cta_factory_quarantine/`。条目：BUG-I02-NEW-033。

## 未覆盖与限制

- 以下红控依赖 docs 证据中的改前源码，不在套件内，运行器中均为 not-applicable：wp1 baseline3、ctpopt_time_snapshot red、`cta_factory_quarantine/run_path_asan.sh` 与可选的 `WT_CTA_PREVIOUS_SOURCE`/`WT_CTA_PATH_PREVIOUS_SOURCE` 控制。
- 全部为本机 fake/替身，不证明真实 CTP/CTPOpt SDK 时序、Windows/Linux 或完整回归。
