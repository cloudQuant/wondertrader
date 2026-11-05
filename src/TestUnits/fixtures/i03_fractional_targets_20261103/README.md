# 迭代 03：执行器小数目标定向 fixture

在 macOS arm64、所列 Homebrew 依赖版本下，从仓库根目录执行：

```sh
src/TestUnits/fixtures/i03_fractional_targets_20261103/run.sh /tmp/i03-fractional-new-run
```

输出目录必须不存在。脚本实体复制工作树 `src` 与 `dist/common`，拒绝包含符号链接的输入，仅在隔离副本裁剪测试目标；编译最多 `-j 2`，并在隔离工作目录运行有超时的测试。`configure.log`、`build.log`、`test.log` 和 `test.xml` 保留在输出目录。脚本依赖 `/opt/homebrew` 下列出的 Boost 1.88.0、GoogleTest 1.17.0、CMake 和 Ninja，以及指定的 Anaconda base Python。

用例检查三个执行器的小数绝对目标、负方向增量、scale、初始化队列、无效输入以及单组整数控制；Diff 使用 fake unit 检查实际派发和队列。输入包含合法条目及 NaN 的同批请求不改变既有目标。该 fixture 不连接交易渠道，也不验证订单最小单位、真实 provider 或多组净额/风控语义。

Diff 并发批次及注册屏障的独立定向测试见 `../i03_diff_batch_atomic_20261103/README.md`；受控测试不代表真实 provider 或完整生命周期验收。Local/Arbi 的单组整数转换仍保留，负方向和小数不执行组合匹配，多组按配置顺序净额累加及双重限额另需实现，卡 8 并未整体关闭。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**STALE_FIXTURE**（已修夹具，不是产品回归）。F3 上 3 例 `ExecutionScaleRequiresCompletePositiveNumber`（Local `R7VwapV2LocalIntegration`、Arbi `WtArbiExecuterSerialization`、Diff `P02DiffExecuterTest`）失败于正向断言：`{"scale":"1.25"}` 与次正规数字符串配置期望 `init` 成功，实际返回 false。
- 根因：卡8b=A（eager 全量预校验，2026-11-02 裁定）后三类执行器的 `validateConfig()` 依次校验 scale、groups、账户限额、clear 与 `exec_policy_config::validatePolicy()`；`policy` 缺失时报 “policy is missing” 并拒绝（`src/WtCore/ExecPolicyConfig.h:44-51`，`WtLocalExecuter.cpp:454-478`、`WtArbiExecuter.cpp:524-548`、`WtDiffExecuter.cpp:723-739`）。夹具的正向配置只有 `scale`，不是完整配置；`readExecutionScale` 对 `"1.25"` 与次正规数字符串仍接受，scale 语义未变。
- 修改（`local.inc`、`arbi.inc`、`diff.inc` 中该用例）：所有配置（10 个非法 JSON、3 个 NaN/±inf 程序化配置、2 个正向配置）统一带合法 `policy`（`{"default":{"name":"I03ScaleFact.I03ScaleUnit"}}`，`validatePolicy` 只校验形状、init 不建单元）。这样非法 scale 的拒绝断言只能由 scale 本身触发（原先缺 policy 时这些断言因 policy 缺失也会通过，失去区分力），正向断言描述完整配置。断言本身（EXPECT_FALSE/EXPECT_TRUE 及其输入 scale 值）未改。
- 派生套件 `i03_execution_groups_20261103`、`i03_diff_batch_atomic_20261103` 经本 run.sh 合并编译运行，F3 上已全部通过，无改动。
