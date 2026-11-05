# 迭代03 TraderAdapter 限流（卡 7）与拆单未完成量夹具

覆盖 `BUG-R3-002`、`BUG-R3-003`、`BUG-I02-NEW-139`、`BUG-I02-NEW-180`（限流部分）与 `BUG-R3-008`（拆单账本）。
测试使用真实 `src/WtCore/TraderAdapter.cpp`、进程内脚本化 fake `ITraderApi`、单合约 fake 基础数据和真实 `ActionPolicyMgr`；**不连接任何真实柜台/行情，不下真实订单**。

## 已落地的合同（卡 7 用户裁定）

- 7a：以“当前单调时间”为终点的滑动窗口 `(now - T, now]`；已提交和**尚在 provider 调用中的预留**都计数；允许 N 次，第 N+1 次在到达 API 前拒绝。
- 7b：超限不再进入永久排除集合；窗口满即临时封禁（`checkOrderLimits`/`checkCancelLimits` 为 false，`isTradeEnabled` 对下单窗口满返回 false），记录过期后自动恢复。
- 7c：窗口使用 `steady_clock` 毫秒（`TraderAdapter::_rate_clock`，测试可注入）；墙钟仅出现在日志时间戳。
- 7d：撤单（单笔 `cancel(localid)` 和批量 `cancel(code,isBuy,qty)`）使用同一 `FrequencyWindow` 预留/边界。
- 逐请求预留：`orderInsert`/`orderAction` 同步返回负值且调用期间未观测到该单回报时回退名额；异常视为结果未知，保留名额。
- `boundary=0` 或 `timespan=0` 沿用现有 helper 语义：不限流（卡 7 未单列关闭值，未改）。
- 日累计 `order_total_limits`/`cancel_total_limits` 的排除集合与卡 7 频率窗口无关，**未改变**。

## 拆单账本（R3-008）

- `buy()`/`sell()` 不再按请求总量预记未完成量；每个拆分单在调用 provider 前预留本笔数量，被接受才保留，被拒（本地限流、同步负返回、同步拒单回报）则回退或由回报扣减，二者只发生一次。
- 失败标记 `UINT_MAX` 和同步已死的单不再作为活单 ID 返回；首个失败后停止提交余下拆分，买卖对称。
- 同步重入：`doEntrust` 在 `orderInsert` 期间按 localid 登记，`onRspEntrust` 错误、`onPushOrder` 撤销/存活、`onPushTrade` 在调用期间回报时打标；重复拒单在同一调用内只扣一次。负返回但已收到存活回报时保留该单并占用名额。
- 批量撤单先在 `_mtx_orders` 下快照并 retain 候选，再逐一撤单，避免同步 `onPushOrder` 删除订单导致迭代器失效（旧代码 ASan 报 heap-use-after-free）。
- `buy()`/`sell()` 改为复制持仓快照，避免重入回报插入 `_positions` 后引用失效。

## 运行

先构建依赖静态库（Debug，非覆盖率）：

```sh
cmake -S src -B /tmp/wt-i03-trader-ratelimit-20261103/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_WARNINGS_AS_ERRORS=OFF -DWT_ENABLE_COVERAGE=OFF \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_CXX_FLAGS='-include cstdlib'
cmake --build /tmp/wt-i03-trader-ratelimit-20261103/build --target WtCore WtDtCore WTSTools WTSUtils -- -j2
```

再用 `run.sh <库构建目录> <新输出目录> <head|base|fix> <plain|asan|tsan> [gtest过滤]`：

```sh
F=src/TestUnits/fixtures/i03_trader_ratelimit_20261103
L=/tmp/wt-i03-trader-ratelimit-20261103/build
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103/run-fix-plain-NEW fix plain
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103-asan/run-fix-asan-NEW fix asan
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103-tsan/run-fix-tsan-NEW fix tsan
# 红控
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103/run-base-NEW base plain 'I03RateLimit.*-I03RateLimit.batch_cancel_survives_sync_cancel_reports'
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103-asan/run-base-batch-NEW base asan 'I03RateLimit.batch_cancel_survives_sync_cancel_reports'
$F/run.sh $L /tmp/wt-i03-trader-ratelimit-20261103/run-head-NEW head plain 'I03RateLimit.*split*:I03RateLimit.*rule_cap*'
```

- `fix`：当前源码；`base`：本次修改前工作树快照 `baseline/`（`baseline.sha256` 校验）；`head`：`git show HEAD:` 的 TraderAdapter。
- 脚本用 LLVM 22.1.8 编译 TraderAdapter（`-Werror -Wall -Wextra`）、UnresolvedReportJournal 和测试；链接前从 `libWtCore.a` 副本删除归档内的 TraderAdapter/UnresolvedReportJournal 对象，避免惰性抽取混用实现；测试子进程 `perl alarm 180`。
- 输出：`manifest.txt`（HEAD、源/二进制哈希、各步 exit）、`*-build.log`、`link.log`、`test.log`、`result.xml`。
- 时钟注入用例（3 个）在 `head/base` 中因缺少接缝而 `GTEST_SKIP`；`fix` 中无跳过。

## 2026-11-03 本机结果（macOS arm64）

| 变体/模式 | 结果 |
|---|---|
| fix plain | 24/24 PASS，exit 0 |
| fix ASan/UBSan | 24/24 PASS，exit 0，无 sanitizer 报告 |
| fix TSan | 24/24 PASS，exit 0；并发/重入 7 项 `--gtest_repeat=50` 全通过，无 TSan 报告 |
| base plain（除批量撤单重入） | 13 FAIL / 3 SKIP：恢复期间 `isTradeEnabled`、同步失败名额未回退（下单/撤单）、同步拒单/撤单回报重入、负返回后存活单、全部拆单项（含请求 10 提交 3 未完成量 10） |
| base 批量撤单重入 | plain SIGSEGV(139)；ASan `heap-use-after-free`(134) |
| head | N/N+1 边界、批量撤单不记账、窗口恢复（永久排除）、嵌套预留、并发（40 次全部放行后崩溃）、拆单等均红 |

## 直接相关既有用例（覆盖率构建 `/tmp/wt-i03-trader-ratelimit-20261103/cov`）

配置：`cmake -S src -B /tmp/wt-i03-trader-ratelimit-20261103/cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=$(brew --prefix boost)/include`，`cmake --build ... --target CoverageTests_wtcore -- -j2`。首次按默认 `-Werror` 构建因他人在改的 `WTSTools/WTSDataFactory.cpp:1334` 未用参数失败，故关闭 Werror（本次改动的 TraderAdapter 已在夹具中以 `-Werror` 单独通过）。在 `bin/TestUnits` 实路径下逐套件运行，日志/XML 在 `/private/tmp/wt-i03-trader-ratelimit-20261103/related-final/`。

- 通过：`P11PreOrderRisk.*` 8/8（含 I02-NEW-139 的 boundary=2 红控）、`R59*`（除 R59Exec）19/19、`R3TraderTest.*` 16/17、`N01TraderAdapter*` 33/33、其余 `P11*` 18/18、`R90TraderRisk.*` 2/2（改 oracle 后）、`R170` 两条窗口用例（改 oracle 后）。
- 与已裁定合同冲突并已更新的旧 oracle（改前文件与 diff 见 `oracle_before/`）：
  - `R170TraderRisk.OrderWindowBlocksAndPrunes`/`CancelWindowBlocksAndPrunes`：直接写入时间缓存并断言“以最后一次操作为终点”“越界写入 `_exclude_codes` 永久排除”，即 7a/7b 否决的语义；改名为 `*RejectsAtBoundaryAndRecovers`，用注入单调时钟断言第 N+1 次拒绝、不进排除集、过期恢复并裁剪。
  - `R90TraderRisk.FrequencyWindowsRejectStandardCodeAfterRawCodeEvents`：边界 1 却要求连续 3 次提交成功，编码旧的“多放行”计数；改为边界 3、3 次成功后第 4 次预检查被拒，保留“原始码回报命中标准码缓存”的原意。
- 仍失败、经对照确认与本次改动无关（本次改前快照 `baseline/` 编译同一测试 TU 结果相同，日志 `/private/tmp/wt-i03-trader-ratelimit-20261103/legacy-oracles/{base,fix}/`）：`R170TraderRisk.SecondCanceledShortTodayPushRestoresAvail`、`R170TraderRisk.OptionEntrustErrorUsesOptionStdCode`（无本地 tag 的错误回报已被既有外部单保护忽略）、`P11SyncReport.order_report_rebuilds_table_and_undone`、`R3TraderTest.cancel_localid_synchronous_api_failure_is_not_cached`（风控未启用时 helper 不记录，旧 oracle 期待未启用也记账；未改，待裁定是否需要“不限流也记录”）。
- `R59Exec.local_auto_clear_checks_hot_code_and_filters` 在 `WtLocalExecuter::~WtLocalExecuter` 因未 quiesce 调用 `std::terminate`（他人在改的执行器生命周期合同），与 TraderAdapter 无关，未处理。
- 被取代的抽取式探针：`i03_frequency_20261103`、`r3_split_reject`、`r3_cancel_batch_cache` 从生产源码按函数抽取编译，现无法编译（缺少 `submitSplit`/`rateNow`/`EntrustOutcome` 等；后两者在本次改动前已因其他改动无法编译）。`i03_frequency_20261103` 还断言“负返回保留名额”，与本次按卡 7 实现的同步失败回退相反；由本夹具取代，未修改其文件。

## 限制

- fake provider 的返回/回报时序是测试约定，不代表 CTP 等真实 SDK 的负返回含义与回报顺序；真实柜台、Windows/MSVC 未验证。
- 依赖库为本机新构建的非 sanitizer 版本；sanitizer 只覆盖本夹具编译的 TU。
- 迟到/重复的**异步**拒单回报（provider 已同步负返回且已回退之后才到）仍按旧的聚合未完成量启发式处理，可能二次扣减；需要卡 5（订单身份/终态去重寿命）裁定后处理，本次未实现 token/epoch。
- `_undone_qty` 本身仍无锁，跨线程（策略线程下单与 provider 线程回报并发）未在本次闭合。
- 未执行完整回归；直接相关既有用例见执行记录。
