# BUG-R202-003 跨翻译单元交易开关本机复验

2026-11-02，macOS arm64。以 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 和 `dist/common` 导出实体源码快照；被测 `TraderAdapter.cpp/.h` 和两个测试源与运行时工作区哈希一致，见 `source-sha256.txt`。隔离快照将 `CoverageTests_wtcore` 的测试源裁剪为 `r202_wtcore_public_surface_test.cpp` 和 `r202_trade_switch_header_probe.cpp`；产品 TU 仍按目标构建。仅快照 CMake 调整实体 Boost 头优先，变更见 `focus.patch` 和 `root-boost.patch`，不代表原完整目标构建。

```sh
mkdir -p /tmp/wt-i03-r202-003-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r202-003-20261102/snapshot
# 在隔离快照 src/TestUnits/coverage/CMakeLists.txt 把 wtcore 测试项替换为：
# q01_wtcore3/r202_wtcore_public_surface_test.cpp+q01_wtcore3/r202_trade_switch_header_probe.cpp
# 在隔离快照 src/CMakeLists.txt 的 Boost include 后增加：
# include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r202-003-20261102/snapshot/src -B /tmp/wt-i03-r202-003-20261102/build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r202-003-20261102/build --target CoverageTests_wtcore -j 6
```

配置与构建均退出 0，日志见 `configure.log`、`build.log`。`build-commands.txt` 证明公开头探针、产品 TraderAdapter 和测试文件各为独立编译命令，最终链接入同一测试目标；`include-check.txt` 检查了目标 230 条编译命令，实体 Boost 均先于 Homebrew include。快照及构建目录的 `symlinks.txt` 为空，没有创建符号链接。测试二进制摘要见 `binary-sha256.txt`。

分别在新建的独立运行目录执行，均加 40 秒 alarm：

```sh
perl -e 'alarm 40; exec @ARGV' -- /tmp/wt-i03-r202-003-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore \
  --gtest_filter=R202TraderAdapter.PublicTradeSwitchIsExternallyCallable --gtest_output=xml:RESULT.xml
perl -e 'alarm 40; exec @ARGV' -- /tmp/wt-i03-r202-003-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore \
  --gtest_filter=R202TraderAdapter.PublicStateAfterInitExt --gtest_output=xml:RESULT.xml
```

两项分别退出 0，XML 均为 1 test、0 failure、0 skip。前者从只包含 `TraderAdapter.h` 的另一 TU 调用 `isTradeEnabled`；风险监控关闭时，已知 `SHFE.ag.2512` 和未知 `SHFE.xx.9999` 均返回 true。后者为正常初始化状态对照。没有连接真实交易通道。

本证据只覆盖列出的快照、本机和风险关闭路径；其他依赖共享该快照但未进行完整工程回归。风险开启时未知代码和排除集应如何处理的产品合同仍未确定，不能依据现行实现推定正确，也没有运行相关风险开启场景。Windows/Linux、最终整合树及整体回归未验证；本条仍未闭合。
