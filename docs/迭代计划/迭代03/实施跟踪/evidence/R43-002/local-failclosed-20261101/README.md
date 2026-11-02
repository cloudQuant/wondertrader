# R43-002 本机 fail-closed 修复及定向证据

状态：**BLOCKED / 局部修复待审查**。本目录是一次工作树快照和本机定向测试记录，不是整体回归、生产验收或发布证据。

## 基线和归属

- 仓库根目录 `/Users/yunjinqi/Documents/new_projects/wondertrader`，抓取时 HEAD 见 `checkout-head.txt`。六个范围文件当时均已是修改状态，见 `capture-status.txt`；不得把 HEAD 等同于本次实施前状态。
- `baseline/` 是本次子任务**首次编辑前**，于 2026-11-01 23:39 本机复制的现存脏树文件，保留已有的未提交修改；`final/` 是完成本次修改后的六个同名文件。`baseline-to-final.diff` 是两份快照之间的完整差异，`SOURCE-SHA256.txt` 是原始快照和命令的 SHA-256；包含未变的 `MatchEngine.h` 作为对照。该基线不是干净 HEAD，也不是另一次历史会话的基线。
- 提取基线时的源文件目录分别为：`src/WtBtCore/MatchEngine.cpp`、`src/WtBtCore/MatchEngine.h`、`src/WtExeFact/WtStockVWapExeUnit.cpp`、`src/WtExeFact/WtStockVWapExeUnit.h`、`src/TestUnits/coverage/n04_btcore_mockers/r43_match_engine_flow_test.cpp`、`src/TestUnits/coverage/r7_exefact/r7_stock_vwap_unit_test.cpp`。复制时因六个 basename 唯一而平铺保存。
- `results/baseline_vwap.log`、`results/baseline_matcher.log` 及 `baseline-*.xml` 的产品实现取自上述实施前 `baseline/*.cpp`，**不是**干净 HEAD；但为编译新增的红测，它们使用了本次最终版测试文件和最终版头文件，并链接同一构建缓存中的依赖。特别是 VWAP 基线 `.cpp` 用当前头文件中的新增字段布局；这是功能层面红绿对比，不是原始脏树所有 TU 的独立原样重建。`results/*` 原始文件的哈希见 `RESULTS-SHA256.txt`。
- 最终快照和工作树文件在本记录生成时以 `cmp` 逐文件一致。早先编译的测试程序未在编译前记录输入源码哈希，因此事后哈希无法单独证明二进制的完整构建输入；现提供完整快照、构建命令、依赖与程序哈希以支持复核。

## 修改和原有测试合同

撮合器遇到双侧价格都为零的最新 tick，在生成订单序号、local ID 和订单前返回空，tick 引用正常释放；新行情恢复后本机撮合器允许显式新请求。通用 VWAP `ExecuteContext` 返回空 ID 时不能确认提供方是否已经接单，因此该执行单元设置进程内不可自动清除的未确认状态，阻断后续自动发单及重连批量撤单。空 ID 不增加 `_fired_times` 或更新 `_last_fire_time`；显式更新目标仍按照原逻辑重置计数。

本次相对实施前修改了两个已有测试，均位于 `src/TestUnits/coverage/r7_exefact/r7_stock_vwap_unit_test.cpp`：

1. `r7_stkvw.empty_retry_order_ids_obey_configured_fire_span` 改为 `r7_stkvw.empty_retry_ids_quarantine_across_ticks_and_reconnect`。旧用例假定空 ID 一定意味着未接单，要求等待 1200 秒后再次尝试、最终返回成功订单。当前通用接口无法证明这一假定，自动重试可能造成重复订单；现断言空 ID 后锁存、次数和上次发单时间不变，并检查后续 tick、次日、重连、目标更新、清仓、直接 `fire_at_once`、未验证超时锁存及无自动撤单。
2. `r7_stkvw.zero_fire_span_empty_retry_uses_one_second_failsafe_only_for_retry` 改为 `r7_stkvw.empty_initial_ids_quarantine_without_counting_or_auto_cancellation`。旧用例在零间隔下要求空 ID 后 1 秒自动重试，仍依赖同一不成立的接单假设；现检查初始请求空 ID 不计次、无自动撤单或再次发单，且同一对象重新 `init` 后仍锁存。

以上两个旧用例“空 ID 后自动成功”的原断言因合同改变被删除，不能声称原样保留；它们不是已知未接单的正控。**正常已确认撤单后的正向能力仍有独立控制**：原 `r7_stkvw.canceled_sell_refires_with_safe_sell_quantity`（普通重试、原切片/可卖上限）、`r7_stkvw.canceled_retry_keeps_sublot_remainder_pending_without_invalid_order`、`r7_stkvw.timeout_without_verified_identity_keeps_monitor_and_never_cancels` 均保留并通过。另增本目录 `commands/positive-controls.cpp`，不修改产品测试，分别对 `fire_span=0` 和 `fire_span=1200` 验证已确认撤单后在新 tick 正常重试、数量不超过原始切片和可卖仓、满足 100 股单位、`_fired_times` 仍为 1；基线和修复版各运行 1 个测试，内部各执行两个场景，均通过。它不证明空 ID 结果可靠可重试，也不覆盖完整 T+1、所有并发或实盘路径。

`src/TestUnits/coverage/n04_btcore_mockers/r43_match_engine_flow_test.cpp` 只增加 `emptyBookRejectsWithoutSideEffectsAndLaterQuoteAllowsOrders` 和序号辅助读取，原有测试未删除；它验证空盘口买卖均无序号/订单/回调/成交，后续有效盘口允许显式发单。原 `passiveBuyBelowLimitFillsRemainingQty` 验证正常成交；`oneSidedBidIsSafeAndDoesNotCreateAFillWithoutAnAsk`、`oneSidedAskAndEmptyQuoteSidesAreIteratorSafe`、`bidAndAskDepthUseTheirOwnQuantities` 验证单边、无虚构成交和买卖深度数量。已有旧档清理策略未获合同确认；迭代既有测试明确不判断旧档应清理还是保留，不能作为清理已完成的证明。

## 原始结果

| 证据 | 本机结果 | 原始 XML |
| --- | --- | --- |
| 实施前 VWAP 源码 + 最终红测 | 0/2，预期失败，exit 1 | `results/baseline-vwap.xml` |
| 实施前 matcher 源码 + 最终红测 | 0/1，预期失败，exit 1 | `results/baseline-matcher.xml` |
| 修复后 VWAP 定向用例 | 5/5，exit 0 | `results/final-vwap.xml` |
| 修复后 matcher 定向用例 | 2/2，exit 0 | `results/final-matcher.xml` |
| UBSan matcher 定向用例 | 2/2，exit 0 | `results/final-matcher-ubsan.xml` |
| UBSan 单边和深度控制 | 3/3，exit 0 | `results/final-depth-ubsan.xml` |
| 旧源码 / 新源码普通撤单补充正控 | 各 1/1；各含两种间隔 | `results/baseline-positive.xml`、`results/final-positive.xml` |

`results/exit-codes.txt`、对应 `*.stdout.log` 保留各进程的原始退出码及输出。非 GoogleTest 的 UBSan 独立探针以 `results/probe-matrix.stdout.log` 和退出码记录买/卖 × 空/仅买/仅卖/双边八种组合：空盘口 ID 数 0，其余为 1，8/8 exit 0；该探针没有 XML。相关原始 UBSan 故障的实施前记录见上一层 `README.md`、`buy-empty.log` 和 `sell-empty.log`。该探针和 matcher 源码使用 `undefined,float-divide-by-zero,float-cast-overflow`，链接的 WTSTools/WTSUtils 静态库未全部 UBSan 插桩。

## 命令和复现边界

在仓库根目录，依赖路径、编译选项和测试过滤器的原始具体命令分别见 `commands/build.sh`、`commands/baseline_build.sh`、`commands/build-ubsan.sh`、`commands/build-positive-controls.sh`、`commands/run-results.sh`；GTest 测试输入由工作树对应路径提供，独立正控使用此目录 `final/r7_stock_vwap_unit_test.cpp` 快照。前两个构建脚本是当时使用的记录，后两个 UBSan/正控脚本整理为同等编译选项。命令依赖本机 `/opt/homebrew/Cellar/llvm/22.1.8`、`/usr/local/include/rapidjson`、`/opt/homebrew` 库、`/tmp/ctpm-i03-build/build_x64/Debug/libs/` 静态库以及 `/tmp/wt-i03-r43-final/`；不能假设这些临时目录会永久存在。`compiler-version.txt` 与 `build-inputs-and-binaries.sha256` 记录现存编译器和链接依赖/程序哈希，不是可移植的完整工具链封存。

在上述依赖仍存在且工作树六文件与 `final/` 一致时，从仓库根目录执行：

```bash
EVIDENCE='docs/迭代计划/迭代03/实施跟踪/evidence/R43-002/local-failclosed-20261101'
mkdir -p /tmp/wt-i03-r43-final/baseline
cp "$EVIDENCE"/baseline/* /tmp/wt-i03-r43-final/baseline/
cp "$EVIDENCE/commands/stubs.cpp" /tmp/wt-i03-r43-final/stubs.cpp
bash "$EVIDENCE/commands/baseline_build.sh"
bash "$EVIDENCE/commands/build.sh"
bash "$EVIDENCE/commands/build-ubsan.sh"
bash "$EVIDENCE/commands/build-positive-controls.sh"
bash "$EVIDENCE/commands/run-results.sh" /tmp/wt-i03-r43-final/reproduced-results
```

脚本基线红测 exit 1 是预期结果，需查看输出目录 `exit-codes.txt` 而非把所有进程视作通过；脚本本身保留全部结果后退出，不以单个红测 exit 1 中止。保存的 XML 是运行时 GoogleTest 原始输出，非重新编写的测试摘要。上述命令只针对本问题选定用例和正控，没有运行 5000+ 全量回归。

## 未解决限制

- 本锁存只存在于该对象/进程；重新创建对象或进程重启将丢失，重启不能证明历史委托不存在或允许安全重发。没有提供可信人工/账户对账和恢复接口。
- 不保护同账户其他执行单元、其他进程或人工/外部交易，也没有账户级互斥或持久化门禁。
- 原子标记只保证标志可见性，不提供跨回调和外部提交的原子事务；锁存产生前或与产生并发的在途发送/撤单无法排除。以上定向测试为串行假上下文，不能证明并发安全、真实 provider 行为或实盘资金安全。
- 盘口旧档清理语义、无效报价的全面合同、真实环境以及完整回归仍未验证。不可将此局部修复作为 R43-002 已关闭、生产可用或整体迭代验收通过。
