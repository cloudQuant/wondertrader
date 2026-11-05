# BUG-P02-005：组合目标本机定向复验

2026-11-02，macOS arm64。将当时工作区的 `src` 和 `dist/common` 实体复制到 `/tmp/wt-i03-p02-005-20261102/snapshot`；该快照包含检查点 `ccaad010` 之后的既有工作区改动，并非 Git 干净树，也不是原子全仓快照。快照文件 SHA-256 保存在 `snapshot-hashes.txt`，主要被测源和测试的工作区哈希保存在 `worktree-input-hashes.txt`。未修改工作区产品代码或共享 CMake。快照及构建目录的 `find -type l` 无输出，见 `symlinks.txt`。

隔离快照中按 `focus.patch` 只将 `CoverageTests_wtcore` 的测试源裁剪为本项两个 TU，按 `physical-boost.patch` 使实体 Boost 头优先。编译命令 `include-and-commands.txt` 中该目标 33 条命令的实体 Boost 均在 Homebrew 通用 include 之前。这是裁剪目标，不代表完整目标构建或最终整合树通过。

```sh
mkdir -p /tmp/wt-i03-p02-005-20261102/snapshot/dist
cp -R src /tmp/wt-i03-p02-005-20261102/snapshot/src
cp -R dist/common /tmp/wt-i03-p02-005-20261102/snapshot/dist/common
# 对隔离快照应用 focus.patch 和 physical-boost.patch
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-p02-005-20261102/snapshot/src -B /tmp/wt-i03-p02-005-20261102/build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake \
  --build /tmp/wt-i03-p02-005-20261102/build --target CoverageTests_wtcore -j 6
```

配置和构建均退出 0，日志见 `configure.log`、`build.log`。以下两例在各自独立工作目录运行，均有 45 秒 `alarm`，退出码均为 0，XML 分别为 1 test、0 failures、0 skipped：

```sh
cd /tmp/wt-i03-p02-005-20261102/run-local
LLVM_PROFILE_FILE=/tmp/wt-i03-p02-005-20261102/local.profraw perl -e 'alarm 45; exec @ARGV' -- \
  /tmp/wt-i03-p02-005-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore \
  '--gtest_filter=R7VwapV2LocalIntegration.LocalGroupTargetsDispatchStablyAndClearOnEmptyInput' \
  --gtest_output=xml:/tmp/wt-i03-p02-005-20261102/local.xml
cd /tmp/wt-i03-p02-005-20261102/run-arbi
LLVM_PROFILE_FILE=/tmp/wt-i03-p02-005-20261102/arbi.profraw perl -e 'alarm 45; exec @ARGV' -- \
  /tmp/wt-i03-p02-005-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore \
  '--gtest_filter=WtArbiExecuterSerialization.PublicSetPositionAppliesGroupTargetsAndClearsOnEmptyInput' \
  --gtest_output=xml:/tmp/wt-i03-p02-005-20261102/arbi.xml
```

两例都通过 fake unit 的实际回调及内部目标账本断言：组合权重各 2、两腿目标 6/8 时，组合得到 3，两腿得到 0/2；重复调用结果稳定，空目标使组合及两腿归零。测试还断言负数和小数仍沿当前原始路径，不能由此认定这些情形的经济合同正确。

本轮仅证明上述本机快照、单组非负整数范围和 fake unit 的可观察行为；未连接真实交易或验证下单、风控/订单限额、负数/小数的经济语义、多组重叠、其他平台、sanitizer 和整体回归。本条仍有待定合同和最终整合验证，不能整项关闭。
