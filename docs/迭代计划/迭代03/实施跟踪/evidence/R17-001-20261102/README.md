# BUG-R17-001 迟到成交关联子项本机复验

2026-11-02，macOS arm64。以本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 创建独立实体快照；工作区中的 `WtDiffMinImpactExeUnit.cpp/.h` 和两项测试源码与该检查点一致。本次没有修改产品代码或工作区构建文件。

隔离快照中按 `focus.patch` 将 `CoverageTests_exefact` 的测试源限定为 `r17_diff_minimpact_unit_test.cpp`；按 `physical-include.patch` 把实体 Boost 目录置于其他 include 前。不代表原完整测试目标或整个工程通过。实体快照与最终构建目录 `find -type l` 为空（`symlinks.txt`）。最终生成的 1139 条编译命令中，没有出现 `/opt/homebrew/include` 先于实体 Boost 的条目；其中目标产品 TU 和测试 TU 的实体 Boost 目录均在前。

```sh
mkdir -p /tmp/wt-i03-r17-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r17-20261102/snapshot
# 在快照应用 focus.patch 和 physical-include.patch
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r17-20261102/snapshot/src \
  -B /tmp/wt-i03-r17-20261102/physical-build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc \
  -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake \
  --build /tmp/wt-i03-r17-20261102/physical-build --target CoverageTests_exefact -j 6
```

配置和构建均退出 0（`physical-configure.log`、`physical-build.log`）。此前的构建和两次运行虽退出 0，但 Homebrew 通用 include 排在实体 Boost 前，相关日志和 XML 仅作为历史记录，不作为最终验证。

两项分别在独立目录以 30 秒 `alarm` 上限执行，退出码均 0，XML 各为 1 test、0 failure、0 skip：

```sh
cd /tmp/wt-i03-r17-20261102/physical-cancel
LLVM_PROFILE_FILE=/tmp/wt-i03-r17-20261102/physical-cancel.profraw perl -e 'alarm 30; exec @ARGV' -- \
  /tmp/wt-i03-r17-20261102/physical-build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact \
  '--gtest_filter=r17_diff_minimpact.terminal_order_keeps_late_trade_association_without_expiry' \
  --gtest_output=xml:/tmp/wt-i03-r17-20261102/physical-cancel.xml
cd /tmp/wt-i03-r17-20261102/physical-filled
LLVM_PROFILE_FILE=/tmp/wt-i03-r17-20261102/physical-filled.profraw perl -e 'alarm 30; exec @ARGV' -- \
  /tmp/wt-i03-r17-20261102/physical-build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact \
  '--gtest_filter=r17_diff_minimpact.filled_order_keeps_late_trade_until_reconciliation' \
  --gtest_output=xml:/tmp/wt-i03-r17-20261102/physical-filled.xml
```

测试覆盖取消或全成终态后的迟到成交关联、下一 tick 超时资格及通道对账后关联清理。取消路径断言监控状态与后续动作，但本次不额外声称对所有撤单发送路径的端到端覆盖。详见 `physical-cancel.log/.xml` 和 `physical-filled.log/.xml`。

源码 SHA-256：`WtDiffMinImpactExeUnit.cpp` `bfb8c111ac6c224b7b4b46aaa2bcc69dbe210293de7bd279b0f0f647703f32d8`；对应头 `8e9d60e6477f1523005eee1570e50e579ecbbb4e5ffb59467f7bea2d39ff34ca`；测试 `48bc8f8021fe4f337ec3626d7adf8dd8e9916f9c1de315d62128059227126ca0`。最终二进制 SHA-256 `0315494f006153ff6808588f3a144444c2e85e9bb67efc4e9e43a392c3178743`。

结果仅限本机 fake context、上述检查点和测试子项；没有 sanitizer、真实交易通道、Windows 或最终整合树验证。重复 ACK、多订单、复用 localid、跨重连以及 BUG-N05-003 的端到端身份链仍未验证，本条不能据此整体验收。
