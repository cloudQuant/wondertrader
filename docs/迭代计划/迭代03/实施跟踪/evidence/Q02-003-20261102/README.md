# BUG-Q02-003：CTA 信号字段恢复定向复验

2026-11-02，macOS arm64。本次只复验信号字段恢复子项，不修改产品代码或测试断言；不确定的同日/跨日 frozen 策略仍未处理。使用检查点 `ccaad010` 的 `src`、`dist/common` 实体快照。`CtaMocker.cpp/.h`、`HisDataReplayer.cpp` 和目标测试文件在运行时与工作区一致；`WTSCfgLoader` 工作区存在检查点后的变更，因此不能将结果称为当前完整树验证。

仅在隔离快照中把 `CoverageTests_btcore` 的测试源裁剪至 `q02_cta_replayer_integration_test.cpp`，并把实体 Boost 头目录前置；实际改动见 `coverage-cmake.patch` 和 `root-cmake.patch`。203 条编译命令中凡使用 `/opt/homebrew/include` 均以实体 Boost 目录优先；`symlinks.txt` 是快照和构建目录符号链接检查，结果为空。未使用或创建符号链接；动态依赖与系统环境见构建日志，结果不代表完整 btcore 目标或所有依赖插桩。

```sh
mkdir -p /tmp/wt-i03-q02-003-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-q02-003-20261102/snapshot
# 将上述两个 patch 仅应用于隔离快照。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-q02-003-20261102/snapshot/src -B /tmp/wt-i03-q02-003-20261102/build \
  -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-q02-003-20261102/build --target CoverageTests_btcore -j 5
```

配置与构建均退出 0，分别见 `configure.log`、`build.log`；逐条编译命令见 `build-commands.txt`，关键输入和二进制摘要见 `sha256.txt`。初次启动测试命令因指定的工作目录尚不存在而未能创建进程；随后创建目录重新运行，该次不是测试结果。

以下每项分别在独立工作目录和进程中运行，限定 60 秒：

```sh
LLVM_PROFILE_FILE=/tmp/wt-i03-q02-003-20261102/modern.profraw DYLD_LIBRARY_PATH=/opt/homebrew/lib:/usr/local/lib \
  perl -e 'alarm 60; exec @ARGV' -- /tmp/wt-i03-q02-003-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore \
  --gtest_filter=Q02CtaIntegrationTest.incrementalSignalRestoresDesiredPriceAndConditionCallback \
  --gtest_output=xml:/tmp/wt-i03-q02-003-20261102/modern.xml
LLVM_PROFILE_FILE=/tmp/wt-i03-q02-003-20261102/legacy.profraw DYLD_LIBRARY_PATH=/opt/homebrew/lib:/usr/local/lib \
  perl -e 'alarm 60; exec @ARGV' -- /tmp/wt-i03-q02-003-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore \
  --gtest_filter=Q02CtaIntegrationTest.legacyIncrementalSignalDefaultsToMarketFillWithoutConditionCallback \
  --gtest_output=xml:/tmp/wt-i03-q02-003-20261102/legacy.xml
```

两项各退出 0、1/1 PASS、0 skip；见 `modern.log`/`modern.xml` 和 `legacy.log`/`legacy.xml`。覆盖非零 `desprice`、`sigtype=2` 的恢复、成交价格、条件回调和信号清除，以及旧快照缺字段的默认行为。未修改历史 rawcode 断言；这些断言位于其他测试，本轮未运行，不能由上述两项通过推定已解决。

本轮未运行 sanitizer、同日或跨日 frozen 恢复用例、其他平台、最终整合树或整体回归。frozen 保存/拒绝及 session-begin 清零的合同仍不明确，该条保持未闭合。
