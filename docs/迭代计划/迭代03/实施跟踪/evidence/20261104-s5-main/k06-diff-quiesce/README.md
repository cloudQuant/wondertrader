# 决策 K06=B（BUG-I02-NEW-035、BUG-R7-002、BUG-R190-001、I03-NEW-069）：Diff 无副作用创建失败后重试成功可完成协调 quiesce

裁定（2026-11-04）：只有“可证明无副作用的创建失败”且后续代创建成功时，才允许完成协调 quiesce；需补“失败→重试成功→quiesce 完成”的正控与反控。

## 产品（`src/WtCore/WtDiffExecuter.{h,cpp}`，头文件 CRLF 保持）
- 新增 `_clean_failure_codes`（加锁）与 `quiesceIncomplete()`：只有 `_quiesce_incomplete` 未置位、且无“干净失败待重试成功”的代码时，才视为可完成。原有 7 处读取都改用 `quiesceIncomplete()`。
- `failCreation` 不再一进入就永久置位 `_quiesce_incomplete`：
  - 失败可能有插件副作用（`persistFailure`，即自动重试被禁止）时，永久置位，与现状相同；
  - 已证明无副作用（ABI 2.2 失败清理被证明、未登记、未接收宿主工作，允许自动重试）时，记入 `_clean_failure_codes`；
  - 同一代码后续代创建成功（进入 READY）时，从该集合移除。
- `submitPositionTargets` 中“目标已记录但没有可用单元”：若该代码属于干净失败，由集合跟踪；其他情形仍永久置位。

## 用例（`n01_wtcore_tradepaths/r71_executers_test.cpp`，死亡测试隔离进程）
- `K06DiffCleanFailureFollowedBySuccessfulRetryLetsQuiesceComplete`（正控）：干净失败 → 重试成功（READY）→ `quiesce_all()` 与 `is_complete()` 都为 true。
- `K06DiffCleanFailureWithoutSuccessfulRetryKeepsQuiesceIncomplete`（反控）：干净失败、未重试 → `is_complete()` 为 false。
- 有副作用失败禁止重试、永久不完整，由既有用例覆盖（如 `InitialReadyExceptionBlocksRetryBeforeCreationCompletes`）。
- 红控：用改前源码重编，正控失败、反控通过（`k06-red.log`）；恢复改后源码后两例通过（`k06-green.log`）。
- 定位：先用临时插桩确认，干净失败后还有一处“目标已记录但无单元”的路径永久置位，修正后正控才通过。插桩已撤销，改后文件哈希见 `sources.sha256`。
- 逐例回归：wtcore、porter、exefact 共 1682 例，pass 1675、skipped 7（`regression-summary.json`）。
