# R42-002：未知交易通道创建预检定向证据

2026-11-02，macOS arm64。本次只处理未知 trader 的创建和配置预检，**不声明 R42-002 关闭**：未绑定持仓/未完成量查询仍无机器可区分失败合同。已有调用方把查询值直接用于比较及撤单；单独返回 NaN 会让撤单停止条件失效，未采用。

## 本轮差异范围

- `src/WtPorter/WtRtRunner.cpp` 的 `createHftContext`：空、null 或未注册 trader 预先返回 0，不创建或登记上下文；有效 trader 先绑定再登记，然后注册 sink。其 `initHftStrategies` 在创建策略之前先检查全部 active 项的 trader，并在 pending 列表持有 adapter。
- `src/WtRunner/WtRunner.cpp` 的 `initHftStrategies` 使用同样的批量 trader 预检，未知 trader 不发表前面的 context/sink/strategy。
- `src/TestUnits/coverage/q06_porter/r13_porter_sel_hft_export_test.cpp`：扩展现有真实 TraderMocker 公开 C API 测试；`r220_trader_ready_owner_registration_test.cpp`：新增已知在前、未知在后的预检用例；`r82_porter_runner_edges_test.cpp`：原 factory 正负控补上真实本地 mocker trader，使其仍测 factory 本身。
- `src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp`：新增批量预检和已知 trader 两个用例；未设置 `R42_HFT_PLUGIN` 时明确 SKIP，复跑须设置它。

以上源码文件包含其他既有未提交改动，不能把它们相对 HEAD 的整个 diff 归为本轮。预检后的 plugin init 抛异常、注册 sink 失败或并发释放，不属于本轮未知 trader 测试的回滚证明。

## 源码和构建

隔离快照 `/tmp/wt-i03-r42-binding-20261102/snapshot` 是本轮从工作区复制的 `src` 与 `dist/common` 实体目录；复制时工作区存在其他未提交修改，且快照并非原子冻结。`source.sha256` 为六个关键工作区文件与最终快照逐项比较一致的哈希，不能证明所有依赖在复制期间不变。`binaries.sha256` 记录最后一次增量构建的测试二进制与 HFT 测试插件。`CoverageTests_porter` 在快照内裁剪为三个测试 TU；`CoverageTests_m_WtRunner` 未裁剪。构建日志见 `config.log`、`build.log`、`build2.log`、`build-final2.log`。下列命令要求 Homebrew Ninja/CMake、LLVM 系统工具链、Boost 1.88.0、GTest 1.17.0 和仓库其余现有构建依赖；不修改工作区的 CMake。

```sh
work=/tmp/wt-i03-r42-binding-replay
mkdir -p "$work/snapshot/dist"
cp -R src "$work/snapshot/"
cp -R dist/common "$work/snapshot/dist/"
perl -0pi -e 's@porter\|WtPorter\|dir:q06_porter@porter|WtPorter|q06_porter/r13_porter_sel_hft_export_test.cpp+q06_porter/r82_porter_runner_edges_test.cpp+q06_porter/r220_trader_ready_owner_registration_test.cpp@' "$work/snapshot/src/TestUnits/coverage/CMakeLists.txt"
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S "$work/snapshot/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-I/opt/homebrew/Cellar/boost/1.88.0/include
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$work/build" \
  --target CoverageTests_porter CoverageTests_m_WtRunner -j 6
```

分别在独立 cwd 中运行下面五组过滤器。示例展示第一组；后三个变量仅指向上述隔离目录内构建产物，所有用例都使用 90 秒上限：

```sh
bin="$work/build/build_x64/Debug/bin"
mkdir -p "$work/run-api"
(cd "$work/run-api" && LLVM_PROFILE_FILE="$work/run-api/profile-%p.profraw" \
  perl -e 'alarm 90; exec @ARGV' -- "$bin/TestUnits/CoverageTests_porter" \
  '--gtest_filter=R13PorterHftExport.map_getters_userdata_and_logging_use_registered_dummy_trader' \
  --gtest_output=xml:result.xml)
mkdir -p "$work/run-config"
(cd "$work/run-config" && perl -e 'alarm 90; exec @ARGV' -- "$bin/TestUnits/CoverageTests_porter" \
  '--gtest_filter=r220_wtrt_runner_ready_owner_registration.hft_unknown_trader_preflight_has_no_partial_context' --gtest_output=xml:result.xml)
mkdir -p "$work/run-factory"
(cd "$work/run-factory" && perl -e 'alarm 90; exec @ARGV' -- "$bin/TestUnits/CoverageTests_porter" \
  '--gtest_filter=R82RunnerEdges.LaterInvalidHftFactoryDoesNotLeaveEarlierContextInstalled' --gtest_output=xml:result.xml)
mkdir -p "$work/run-valid"
(cd "$work/run-valid" && perl -e 'alarm 90; exec @ARGV' -- "$bin/TestUnits/CoverageTests_porter" \
  '--gtest_filter=R82RunnerEdges.HftFactoryNullUnitFailsAndSupportedUnitConfigures' --gtest_output=xml:result.xml)
mkdir -p "$work/run-runner"
(cd "$work/run-runner" && R42_HFT_PLUGIN="$bin/libWtHftStraFact.dylib" \
  perl -e 'alarm 90; exec @ARGV' -- "$bin/TestUnits/CoverageTests_m_WtRunner" \
  '--gtest_filter=R42HftTraderBinding.*' --gtest_output=xml:result.xml)
```

五个进程均 exit 0，六个 GTest case 全通过且无 SKIP，见 `v2-*.log/xml`。API 用例在子进程里以 TraderMocker 对有效 trader 取空仓，比较未知、空和 null trader 均返回 0 句柄，并通过连续 ID 验证失败分支未分配上下文；它不启动真实交易连接。两个配置预检用例直接调用对应 runner 的 `initHftStrategies`，验证第一项不被安装；WtRunner 合法 trader 用例验证对应上下文及策略登记。两个 R82 用例验证有效 trader 下原 factory 失败与成功行为。

## 红控

`red-control.patch` 仅将隔离快照中的公开 `createHftContext` 替换为本轮修改前的实现；其余配置实现保持候选。应用该补丁，重建 `CoverageTests_porter`，以同一 API 过滤器运行，应 exit 1：子进程报告 `unknown trader produced a handle`，见 `porter-api-red.log/xml`。该红控仅证明公开创建路径能区分旧实现，并非两个配置预检路径的独立旧代码红控。将快照恢复候选、重建后，API 测试再次通过。

```sh
patch -d "$work/snapshot" -p1 < 'docs/迭代计划/迭代03/实施跟踪/evidence/R42-002-hft-binding-20261102/red-control.patch'
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$work/build" --target CoverageTests_porter -j 6
# 再执行上面的 run-api；验证失败后使用 patch -R 恢复并重建。
```

本机测试未覆盖其他操作系统、真实 provider、活动交易、跨语言封装、完整集成树、并发解绑/重入和异常分配的通用事务回滚，也未运行迭代全量回归。`TraderAdapter::addSink` 当前为 void，无法由本次用例证明停止中的 adapter 一定接收注册。测试快照只对列出的源码与平台有效。
