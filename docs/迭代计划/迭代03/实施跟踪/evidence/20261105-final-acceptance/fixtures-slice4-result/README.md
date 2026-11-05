# 最终验收：失败夹具套件处置（slice4-result）

按 `SCRATCH/fixtures/FIXFIX-PROMPT.md` 执行，SCRATCH=`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`，OUT_NAME=`slice4-result`。

- **基础树**：最终冻结树 F3（`SCRATCH/final3/tree`、`SCRATCH/final3/build`）加本段夹具修改，经 `rsync -a --no-links` 同步为 `SCRATCH/slice4-result-fxtree`，HEAD 为 `ccaad010`。
- **重跑**：`/usr/bin/python3 -I SCRATCH/fixtures/run_fixtures.py --tree SCRATCH/slice4-result-fxtree --build SCRATCH/final3/build --out SCRATCH/slice4-result-fxrun --repo-git <repo>/.git --head ccaad010… --annotations SCRATCH/fixtures/empty-annotations.json --only <8 个套件> --parallel 2 --jobs 2`，exit 0。总日志复制为 `logs/fxrun.log`，各套件 `result.json` 复制为 `logs/<suite>.result.json`。
- **p04 复跑**：p04 在还原源文件后单独重跑，`--out SCRATCH/slice4-result-fxrun-p04`，日志见 `logs/fxrun-p04-reverted.log`。
- **快照**：修改前快照 `before/`（`before.sha256`），修改后 `after/`（`after.sha256`）。
- **锁**：所有修改的夹具文件都先经 `lock.py slice4-result FIXTURE-<suite>` 登记，均返回 ok。
- **范围**：未改产品代码，未改迭代跟踪文档，无符号链接。

机读结果见 `SCRATCH/fixtures/fixfix-slice4-result.json`。

## 结论汇总

本段没有 PRODUCT 类。

| 套件 | 分类 | 重跑结论 |
|---|---|---|
| `p04_cross_session_clock` | SUPERSEDED | fail（未修，已被 coverage 取代） |
| `i03_parser_event_contract_20261103` | STALE_FIXTURE | pass |
| `i03_parser_lifecycle_v2` | STALE_FIXTURE | pass（9/9） |
| `i03_uft_parser_lifecycle_20261104s5` | STALE_FIXTURE | pass（plain 7/7，TSan 7/7） |
| `i03_date_mapping_20261103` | STALE_FIXTURE | pass（5/5） |
| `i03_replayer_date_range_20261103` | STALE_FIXTURE | pass（24/24） |
| `r202_nextday` | STALE_FIXTURE | pass（12/12） |
| `l03_commodity_state` | STALE_FIXTURE | pass |

## 各套件

### p04_cross_session_clock：SUPERSEDED（协调者要求先确认是否产品回归：不是）

- **根因**：
  - **`replayer.init(cfg)` 返回 false**：本夹具新增用例把 24h 会话写成 `{"from":0,"to":2400}`。卡2（2026-11-03 用户确认）规定交易时段只有 `0–0` 表示全天，其他起止相同的写法拒绝（`WTSSessionInfo::tryAddTradingSection`，日志 `Invalid section 0 for session P07_24H`）。
  - **改为 `0–0` 后的诊断**：init 通过，`prepare()` 又以回放错误 5 终止（`Cannot map initial replay trading date, status 5`）。会话表缺默认 `TRADING` 会话，而日期映射 V1（BUG-R12-13）显式检查初始交易日，按 `DEFAULT_SESSIONID` 映射。当前 `p07_tick_loader_test.cpp` 同类用例都已带上 `TRADING`。日志：`logs/p04_cross_session_clock.diagnostic-with-0-0-sections.green.log`。
  - **两项已知红均已失效**：slice 红控对应的 P04-011 产品缺陷已修复（默认 tick 查询按品种自身交易日取桶）；负 offset 红控已由 2026-11-05 用户裁定“维持 fail-closed”成为合同。
  - 以上均是夹具落后于合同，回放器行为符合合同，不是产品回归。
- **处置**：源文件不修。为诊断临时改过的 `cross_session_test.cpp` 已按字节还原（sha256 `6c947051…`，与 before 一致）。README 追加“已被取代”说明。
- **取代用例**（均在 `SCRATCH/final3/run/results.jsonl` 中为 pass）：
  - `CoverageTests_btcore::P07TickLoaderTest.p04011TickQueryUsesTheInstrumentsOwnTradingDayInMixedSessions`：同一 5 笔构造、Jan 9 无数据日、Jan 10 夜盘映射 Jan 13、末笔 `23:59:59.999`、派发顺序、查询严格早于时钟，以及原 slice 红控场景。
  - `CoverageTests_btcore::P07TickLoaderTest.p04011NegativeOffsetMixedTickReplayFailsClosed`：原负 offset 场景。
  - `verify.py` 正控列表中的 17 个既有 coverage 用例，本来就是 coverage 用例：
    - `R43ReplayerTickCsvTest.` 下 8 例：sessionBeginClockUsesRangeStartThenNextNaturalSessionOpen、overnightTickRangeLoadsByTradingDateAndFiltersByActionDate、nightSessionBeginClockUsesNaturalOpenAcrossTradingDates、mainBarsUseReplayableHftSessionForMixedNaturalDate、replayableHftEventsKeepLaterSessionBoundary、missingInRangeHftEventsKeepLaterSessionBoundary、mainBarWithoutInRangeHftEventsKeepsLaterSessionBoundary、mainBarAndNightSessionBucketsReplayInNaturalTimeOrder；
    - `P07TickLoaderTest.` 下 9 例：emptyReplayBucketsStayBoundedAcrossLongNoDataRange、overnightTickBucketAcrossWeekendUsesTradingCalendarAndResets、continuous24HourReplayReadsFullSaturdayNaturalDateBucket、continuous24HourPositiveOffsetUsesNaturalDateCut、continuous24HourNegativeOffsetUsesNaturalDateCut、asynchronousOvernightBucketDrainsAfterMainBarAcrossWeekend、sameTimestampOrdersRoundsAcrossCodesAndTradingDateBuckets、tickInSessionGapIsSkippedUntilNextSession、emptyLevel2StreamsDoNotBlockTickReplay。
- **重跑**：在还原后的树上复现原失败，`logs/p04_cross_session_clock.reverted.result.json`、`logs/p04_cross_session_clock.reverted.green.log`。
- **关联条目**：BUG-P04-011。

### i03_parser_event_contract_20261103：STALE_FIXTURE

- **根因**：红控目标 `i03_xele_oracle_head` 编译失败。
  - 当前 `t_ParserXeleSkt/r76_xele_boundary_test.cpp` 含两个红例（`PrepareCommitsOnlyCompleteTcpSnapshotBatches`、`SecondInitIsRejectedKeepsOriginalAndReleaseIsIdempotent`），已改为调用 `extract_buffer(length, data, login_epoch)`。
  - `baseline_head/` 的 HEAD 源码只有 `extract_buffer(length)`。
  - 一个目标失败使整次默认构建失败，绿控 `i03_parser_events` 与红控 `i03_parser_events_before` 都没有构建。
  - 与 ParserCTP/CTPMini 的 IsErrorRspInfo 修复无关。
- **修改**：
  - `CMakeLists.txt`：`i03_xele_oracle_head` 加 `EXCLUDE_FROM_ALL` 与说明注释。该红控在当前树上不适用，保留为 not-applicable，2026-11-03 的结果仍在 `evidence/oracle-head.*`。
  - `README.md` 追加说明。
  - 未改任何期望。
- **重跑**：pass。
  - 绿 13/13；
  - `red-events-before` 预期红，6/13 失败，与 README 记录的 6 个成功事件合同用例一致；
  - ASan 13/13，TSan 13/13；
  - `red-xele-oracle-head` 不构建，运行器标为“自带红控无法与当前树组合构建”。
- **关联条目**：R76-02、R76-03、R76-08、T2-XELE-02。

### i03_parser_lifecycle_v2：STALE_FIXTURE

- **根因**：`ParserV2.ExternalWithoutV2IsNotDestroyed` 断言 initExt 外部 API 的 `release()` 为 0 次。决策 K15=A（2026-11-04）规定：外部 API 归调用方，显式释放时恰好通知停止一次，不视为排空，宿主永不销毁。当前 `ParserAdapter::requestStopLifecycle` 即此实现。
- **修改**：
  - `check.cpp`：断言 release 恰好 1 次，再次请求停止仍为 1 次，`hasApi()` 为真，API 未被销毁（替身新增析构计数）。
  - `README.md` 追加说明。
- **重跑**：pass，9/9。
- **关联条目**：BUG-R18-006、BUG-R8-006、BUG-I02-NEW-009（经 K15 关联，跟踪文件未直接引用本套件）。

### i03_uft_parser_lifecycle_20261104s5：STALE_FIXTURE

- **根因**：决策 K18=否（2026-11-04，卡9a）规定：带 remover 的外部 API 同样不能证明回调已停，隔离到进程退出，永不调用 remover。两例仍按旧语义断言：
  - `ReleaseInsideCallbackIsDeferred` 期望 remover 1 次、`hasApi()=false`；
  - `ConcurrentCallbacksNeverOverlapTeardown` 循环等待 `hasApi()` 变为 false，因而跑到 120 秒 alarm。
- **修改**：
  - `parser_test.cpp` 两例改写为 K18 合同：
    - 回调内的 release 只请求停止（`isTeardownPending()`）；
    - 安全点的 release 进入 `isResidentQuarantined()`，`hasApi()` 为真，remover 0 次，外部对象未删除；
    - 隔离后回调不再被接纳；三个 provider 线程并发时，拆除后不再接纳回调；
    - 测试替身在 owner 与线程结束后由测试回收。
  - 新增 `README.md`。
- **重跑**：pass，plain 7/7，TSan 7/7。
- **关联条目**：BUG-R78-001。

### i03_date_mapping_20261103：STALE_FIXTURE

- **根因**：共享会话表含 `"EMPTY":{"sections":[]}`。决策 K28=A（2026-11-04）对空 sections 整文件拒绝，5 例都在 SetUp 失败。
- **修改**：
  - `check.cpp`：从共享表去掉 EMPTY；保留“未知 EMPTY 映射为 MissingSession”的断言；另断言含 EMPTY 的会话文件被拒绝、且同文件的 POS 也未发布。
  - `README.md` 追加说明。
- **重跑**：pass，5/5。
- **关联条目**：BUG-R12-13。

### i03_replayer_date_range_20261103：STALE_FIXTURE

- **根因**：
  - 同上，K28=A 拒绝 EMPTY 会话，20 例都在 SetUp 失败。
  - 品种 EX.E 挂在 EMPTY 上。按商品就绪合同（卡4a=A），品种所指会话未加载时整文件拒绝，所以原“EX.E → status 5（MissingSession）”这一行已无法通过加载器构造。
- **修改**：
  - `check.cpp`：从共享配置去掉 EMPTY 与 EX.E。
  - EX.E 那一行改为新用例 `EmptySessionAndCommodityWithoutSessionAreRejectedAtLoad`，覆盖四类订阅：会话文件被拒、品种文件被拒、不发布任何内容、订阅 EX.E 报 status 4。
  - `README.md` 追加说明。
- **重跑**：pass，24/24（原 20 例加新增 4 例）。
- **关联条目**：BUG-R12-13。

### r202_nextday：STALE_FIXTURE（原注释建议“退役或改为正向断言”，采用后者）

- **根因**：
  - 脚本要求 `R202NextDay.*`、`R202Reentry.*` 失败（“已知红”）。卡3 3a=A 与卡9 9d=A（2026-11-04）已修复这些缺陷，6 例当前全部通过，脚本按旧期望抛错。
  - coverage 二进制中没有这些用例，只有 `i03_card3_dayboundary_20261104` 夹具复用 `nextday.inc` 运行它们，所以找不到可在 results.jsonl 中确认的等价 coverage 用例。按规则改为修复，而不是判 SUPERSEDED。
- **修改**：
  - `run.py`：判定改为当前合同下的正向断言。12 例（6 个新日/重入探针加 6 个收盘正控）都必须 exit 0 且 PASSED；TSan 变体须无竞争报告。
  - `README.md` 追加说明。旧观测保留在 `evidence-20261102/`。
- **运行器**：`run_fixtures.py` 的 `s_r202_nextday` 由 demo（已知红）改为 primary `green`，run.py exit 0 即 pass。这是最小改动，只涉及该套件函数。差异见 `logs/run_fixtures.slice4.diff`；改前副本 `SCRATCH/fixtures/run_fixtures.py.bak-slice4-*`；改后 sha256 见 `logs/run_fixtures.sha256`。
- **重跑**：pass，12/12。
- **关联条目**：BUG-R202-001。

### l03_commodity_state：STALE_FIXTURE

- **根因**：`probe.cpp` 是修复前的缺陷观测：exit 0 表示 setter 写入的字节仍是 `0xa5` 预填值，即字段未初始化。BUG-L03-002 已按卡4a=A 实施：构造零初始化、初始未就绪、`setTradingMode()` 前有效交易模式为 `TM_None`。因此探针每项都报 DIFFERENT，exit 1。
- **修改**：
  - `probe.cpp`：沿用同一对象表示手法，改为对 4a=A 合同的正向断言：
    - 未配置时 `!isReady()`、`TM_None`、不可做空；
    - 每个 setter 改动的字节在构造后都已为 0，没有残留预填值（交易模式 setter 允许多改 1 个状态字节）；
    - getter 与工厂正控通过。
  - `README.md` 追加说明。
  - `run-init.sh`（init.cpp）与 `run-config.sh` 未改。
- **重跑**：pass。probe normal 与 sanitizer 均 exit 0；config 与 config-sanitize 均 exit 0；init 信息项 exit 0。
- **关联条目**：BUG-L03-002。

## 其他

运行器启动时提示 `fixture directories without handler: i03_trader_oes_query_s4`。这是本段第二轮为 I03-NEW-101 新增的 OES 隔离夹具（入口 `run.sh`），运行器尚无对应处理函数。它不在本次清单内，未改运行器为其增加入口，请主代理决定。
