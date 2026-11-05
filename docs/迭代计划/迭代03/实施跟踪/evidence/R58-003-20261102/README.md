# BUG-R58-003：实体 Boost 定向复验与诊断覆盖

2026-11-02，macOS arm64。只修改 `src/TestUnits/coverage/p04_btcore_replayer/r43_replayer_init_basefiles_test.cpp`：新增 commodity 数组中第二个配置文件缺失/解析失败的测试，以及必需 session/commodity 缺失或解析失败、可选 adjfactor 缺失或解析失败时的带具体路径诊断检查。诊断测试通过重新执行当前测试程序创建独立进程并初始化日志器，避免向全量测试进程遗留 logger handler。未修改产品源码。

最终 7 个直接关联测试各在独立进程通过，包含有效全量配置、session/commodity 文件缺失和非法 JSON、商品数组、有效与损坏可选除权因子及诊断路径；`final-results.json`、`final-*.xml` 和 `final-*.log` 给出命令、目录、退出码和输出。运行方式：

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python docs/迭代计划/迭代03/实施跟踪/evidence/R58-003-20261102/run.py
```

`controlled-red.patch` 是仅用于隔离副本的受控负控：保留 commodity 数组加载但忽略其返回，并临时抑制四种错误日志。两个新增测试各 exit 1，分别在两个数组坏文件断言、对应缺失/损坏文件日志断言处失败，见 `red2-results.json` 和 `red2-*.log`。此受控负控在同步 TimeUtils 前运行，不是原始历史产品版本，也不证明原始缺陷当时被动态复现。之后已恢复源文件并重编，才运行最终 7 项。

## 构建和范围

从当时工作树复制实体 `src/` 和 `dist/` 到 `/tmp/wt-i03-r58-20261102-src`，独立构建 `/tmp/wt-i03-r58-20261102-build`。Boost 1.88.0 的真实头目录复制到 `/tmp/wt-i03-r58-20261102-boost`；仅在源快照的根 CMakeLists 中增加此目录的优先搜索（补丁 `snapshot-cmake.patch`），仓库的 CMakeLists 未修改。之后用以下命令配置和构建：

```sh
/opt/homebrew/bin/cmake -S /tmp/wt-i03-r58-20261102-src/src -B /tmp/wt-i03-r58-20261102-build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWT_COV_P07_FIXTURE_ROOT=/tmp/wt-i03-r58-20261102-src -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DCMAKE_CXX_FLAGS= -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
/opt/homebrew/bin/cmake --build /tmp/wt-i03-r58-20261102-build --target CoverageTests_btcore --parallel 4
```

配置与最终构建均 exit 0，日志归档；最后为匹配当前工作树同步 `TimeUtils.hpp`（SHA256 `63fe5f708b9d156606e88bead9158fee451a06f175f20dcf500672e66d41be54`）后重编了 164 个受影响目标并重跑全部 7 例。选定编译命令见 `selected-compile-commands.json`。`dependencies.txt` 中 Replayer、BaseDataMgr 和测试三个对象的 1260 条 Boost 头路径全部来自实体复制目录；测试桥、构建与快照目录没有新建符号链接。其他第三方头和库仍使用系统/Homebrew 路径；`linked-libraries.txt` 列出运行时依赖，不能声称所有外部依赖或库路径均为无链接实体。

`hashes.json` 中当前 Replayer、BaseDataMgr、WTSCollection、TimeUtils、logger、配置加载器和测试源与实际快照哈希一致；`snapshot-vs-worktree.json` 记录最终对比时仍有 17 个源文件不同，其中包括本证据用的 CMake 快照覆盖和其他并行变更；对比未列出快照后工作树新文件。链接了多个工作树项目库，故不能将结果称作当前完整整合树验收。未运行 sanitizer、其他平台、完整回归或真实生产部署；runner 未继续 prepare 的两个调用点仅经代码检查，没有此轮真实入口动态证据。当前测试是局部行为证据，不直接更改迭代台账状态。
