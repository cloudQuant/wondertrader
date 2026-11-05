# T3-006 TraderCTPMini 本机 fake 模块复验

2026-11-02，macOS arm64，Apple clang 17。源码来自本地检查点 `ccaad010` 的 `src` 与 `dist/common` 实体快照，额外拷贝本轮专用测试补丁 `test.patch`：直接使用未手工重置内部状态的 `TraderCTPMini` 派生探针，验证只有 `flowdir` 时初始化失败、不产生 API/worker/模块并记录缺 `front`。主要源码、测试、实际二进制及三个 fake 模块哈希见 `hashes.txt`；不代表后续工作区整体源码快照。

三个 fixture 分别模拟缺 creator 符号、creator 返回空和成功创建 API。测试使用本地 stub，不连接真实 CTP；`connect()` 的产品接口返回 `void`，按诊断、API/worker 状态与异常安全验证，不称返回 false。目标 suite 六个用例合组运行，退出 0，**6/6 PASS、0 skip**（`run-final.log`、`result-final.xml`）：四个必需键逐项缺失；新增仅 flowdir；缺库和缺符号；空 API；成功创建和释放；失败模块后重试成功。所有运行限定在隔离目录并设置 45 秒 alarm。

## 构建与运行

以下在仓库根目录执行；`cmake-root.patch`、`cmake-coverage.patch` 记录仅作用于快照的构建差异：实体 Boost 头优先，目标测试源裁剪为本项 TU，以及该 TU 对既有无关 `EXPECT_EQ(1u, api.action.FrontID)` 的 sign-compare 告警不视为错误。首次构建因该既有告警被 `-Werror` 拒绝，退出 1（`build.log`）；裁剪并调整单一告警后的构建退出 0（`build-final.log`），新增用例移除 `harden()` 后重新编译退出 0（`build-final2.log`）。这不是原完整测试目标通过。

```sh
mkdir -p /tmp/wt-i03-t3-006-20261102/snapshot /tmp/wt-i03-t3-006-20261102/run
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-t3-006-20261102/snapshot
cp src/TestUnits/coverage/t_TraderCTPMini/r61_traderctpmini_api_test.cpp /tmp/wt-i03-t3-006-20261102/snapshot/src/TestUnits/coverage/t_TraderCTPMini/r61_traderctpmini_api_test.cpp
# 按同目录两个 cmake patch 对快照应用独立构建变更。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-t3-006-20261102/snapshot/src -B /tmp/wt-i03-t3-006-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-t3-006-20261102/build --target CoverageTests_t_TraderCTPMini -j 6
cd /tmp/wt-i03-t3-006-20261102/run
LLVM_PROFILE_FILE=/tmp/wt-i03-t3-006-20261102/t3-final.profraw perl -e 'alarm 45; exec @ARGV' -- /tmp/wt-i03-t3-006-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderCTPMini --gtest_filter='T3_006_TraderCTPMini.*' --gtest_output=xml:/tmp/wt-i03-t3-006-20261102/result-final.xml
```

`include-order.txt` 核验所有 1137 条生成的编译命令均使实体 Boost 1.88 目录先于 Homebrew 入口；其中仅目标及其依赖实际构建。对快照和构建目录的符号链接扫描 `symlinks.txt` 为空，未创建符号链接。本次未运行 sanitizer、整体回归、真实厂商 SDK、Windows/Linux、真实 callback 排空或跨模块卸载生命周期；fake 模块的结果不能代替这些验证。
