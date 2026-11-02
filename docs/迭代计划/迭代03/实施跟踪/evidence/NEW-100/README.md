# BUG-I02-NEW-100 prepare 缺少 sink

公开 prepare 返回 bool，WtBtRunner 和 WtBtPorter 的调用方都在 prepare 前注册 sink，并检查失败后拒绝运行。旧实现 `_listener->handle_init()` 之前已经写入 running、重置状态、调整时间和通知 BT_START；缺 listener 崩溃，且不保证副作用回滚。现于状态变化和事件通知前检查 listener，缺失返回 false；正常注册后可再次 prepare。`register_sink` 和其他接口 ABI 没有改变，公开头增加契约说明。

## 定向证据

- 使用 `/tmp/wt-i03-r58-build` 中已有的隔离对象与库，单独编译当前 `r2_replayer_public_api_test.cpp`、前/后 `HisDataReplayer.cpp` 并重链到 `/tmp/wt-i03-new100/test-red` 和 `test-green`。确切命令和退出码分别见 `red-commands.json`、`green-commands.json`；其他依赖复用隔离快照，非完整当前树重建。
- 真实对象按有效配置 init，不注册 sink 运行新用例 `R2PublicTest.PrepareWithoutSinkFailsBeforeStateChangesAndAllowsRetry`：旧实现 exit 139/SIGSEGV（`red-run/output.log`），新实现 1/1 PASS、exit 0（`green-run/result.xml`）；验证两次失败保持公开时间状态、注册后 prepare 成功且 sink 初始化恰一次。
- 同一隔离链接的合法 sink 正控：`clearCacheForcesReloadOnNextPrepare` 和 `Q02CtaIntegrationTest.marketSignalQueuesAndDumpsWithoutFill` 各 PASS。
- 两个关联但独立的历史未决测试在新、旧实现下均 FAIL：`R2PublicTest.stopBeforeRunQuitsReplayEarly`（预期 replay_done 0、实际 1，对应既有 NEW-101）；`Q02CtaIntegrationTest.queriesDelegateToReplayer`（rawcode 预期 000001、实际空，对应既有 NEW-102）。相关日志和 XML 分别见无前缀、red- 前缀目录；不得把它们算入通过或据此推定新增修复导致。

本项不定义 prepare 之前历史查询的生命周期、并发 register_sink 与 prepare、listener 回调抛异常后的外部副作用回滚；其他依赖复用旧快照，macOS 本机未做 ASan/跨平台或整体回归。所有测试使用独立目录和 20 秒上限，无新符号链接。
