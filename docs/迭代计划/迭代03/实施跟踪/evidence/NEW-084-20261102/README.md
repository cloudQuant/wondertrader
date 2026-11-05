# BUG-I02-NEW-084：回测执行器插件定向复验

2026-11-02，macOS arm64。使用本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 实体快照，未修改工作区产品源码。现工作区的 WtDataReader、WtDtHelper 等依赖在该检查点之后已有变更，因此此结果仅针对检查点，不代表最终整合树。

## 构建

```sh
mkdir -p /tmp/wt-i03-new084-snapshot /tmp/wt-i03-new084-evidence
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-new084-snapshot
# 在快照中应用本目录 root-cmake.patch、coverage-cmake.patch
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-new084-snapshot/src -B /tmp/wt-i03-new084-build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-new084-build \
  --target CoverageTests_btporter CovExecV2Valid -j 6
```

首次配置成功后、构建前检查到 Homebrew include 链接先于实体 Boost，因此仅在隔离快照里通过 `root-cmake.patch` 将 `/opt/homebrew/Cellar/boost/1.88.0/include` 提前，重新配置成功；`coverage-cmake.patch` 只将 btporter 测试源裁剪到 r69/r46/r222。编译和链接退出 0，见 `build.log`。对生成的 `target-commands.txt` 中 165 个编译命令检查，实体 Boost 路径均先于 `/opt/homebrew/include`；快照与构建树的链接扫描为空（`symlinks.txt`），本轮未创建符号链接。该裁剪目标不是原始完整测试目标。

构建出的实际插件 `/tmp/wt-i03-new084-build/TestUnits/coverage/exec_v2_plugins/valid.dylib` 为实体 arm64 Mach-O bundle，测试路径搜索的首个命中即此文件。`hashes.txt` 记录插件、测试二进制及三个测试和插件源码哈希；插件通过同次 `CovExecV2Valid` 目标从该快照构建。其他系统动态依赖未重编或插桩。

## 定向运行

每例分别在 `/tmp/wt-i03-new084-evidence/{r69,r46,r222}` 运行，独立进程且设 60 秒 alarm；`LLVM_PROFILE_FILE` 分别指向对应目录的 `profile.profraw`。命令为：

```sh
/usr/bin/perl -e 'alarm 60; exec @ARGV' -- /tmp/wt-i03-new084-build/build_x64/Debug/bin/TestUnits/CoverageTests_btporter \
  --gtest_filter=CASE --gtest_output=xml:RESULT.xml
```

| CASE | 结果 |
|---|---|
| `R69BtRunnerConfig.ExecModeValidPluginConfigurationSucceeds` | 1/1 PASS，退出 0；公开 `config_v2` 与显式 release |
| `R46BtRunner.ValidExecModePassesPreflightAndCompletesConfiguration` | 1/1 PASS，退出 0；配置成功对照 |
| `N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely` | 1/1 PASS，退出 0；测试断言 replay 回调先于 quiesce、unit/factory 销毁 |

每个目录保存 stdout、XML 和退出码；均无跳过。R46 子进程在配置成功后使用 `_Exit`，不能单凭其证明释放；R69 和 R222 各自覆盖明确的释放路径。此模块是仓库受控 fixture，不证明外部执行器插件、真实部署或 Windows/Linux 行为；并未运行整体回归、sanitizer 或最终源码整合构建。
