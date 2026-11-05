# BUG-R93-006：TraderYD 本机持仓冻结/撤单定向诊断

本轮以本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 和 `dist/common` 导出隔离实体快照，未修改工作区产品或常规测试。`TraderYD.cpp/.h` 与常规测试运行时工作区一致；项目其他文件已有检查点后修改，故这不是最终整合树证据。只连接进程内 YD fake，不触发真实服务。产品和测试使用 Apple clang 17 及实体 Boost 1.88.0 头构建，实体头在 Homebrew 通用 include 之前；编译命令见 `compile-commands.txt`，目标源码和最终测试二进制哈希见 `sha256.txt`。快照和构建目录扫描无符号链接（`symlinks.txt` 为空），没有创建符号链接。

## 已运行的关联用例

各进程使用独立临时工作目录并设 30 秒 alarm，XML 和标准输出均保存在本目录。构建仅保留 R93 测试 TU，并非完整项目目标，快照 CMake 调整见 `cmake-root.patch`、`cmake-coverage.patch`。

- `R93YdTest.open_cover_order_freezes_yesterday_available`：exit 1，首个冻结后昨仓可用从 5 降为 1，但零成交撤单后预期恢复为 5，实际仍为 1；`open_cover_order_freezes_yesterday_available.log/xml`。
- `R93YdTest.close_today_order_freezes_and_releases_today_available`：exit 0，1/1 PASS；该现有控制从昨仓 10、今仓 0 构造平今 4，冻结后今仓可用为 -4，`TradeVolume=1` 撤单后为 -1。这是现行代码的受控分支控制，不能证明一般平今业务语义；`close_today_order_freezes_and_releases_today_available.log/xml`。
- `R93YdTest.r93_mixed_yesterday_today_zero_fill_cancel_restores_origin`：新反例只写入隔离快照，完整测试补丁为 `mixed-test.patch`。先用昨仓初始化 2，再经 fake 开仓成交产生今仓 3，断言冻结前持仓及可用量均为昨 2/今 3；同一订单 ID 卖平 4 后实际可用昨 0/今 3（验收预期 0/1），零成交撤单后实际 0/3（验收预期 2/3）。exit 1、两处 EXPECT 失败；`mixed.log/xml`。此输入序列是受控 fake 假设，不证明厂商回报时序或 `TradeVolume` 的正式含义。

## 精确构建及测试命令

在仓库根目录，使用当前 Python 的用户 Anaconda base 环境处理快照文件；以下用 `<evidence>` 代表本目录，先将 `cmake-root.patch` 和 `cmake-coverage.patch` 按其内容应用于快照：

```sh
mkdir -p /tmp/wt-i03-r93-006-20261102/snapshot /tmp/wt-i03-r93-006-20261102/run
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r93-006-20261102/snapshot
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r93-006-20261102/snapshot/src -B /tmp/wt-i03-r93-006-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r93-006-20261102/build --target CoverageTests_t_TraderYD -j 6
```

配置及首次构建均 exit 0。混合用例在运行前两个原用例后添加到快照，补丁见 `mixed-test.patch`；重新构建 exit 0（`build-mixed.log`）。从 `/tmp/wt-i03-r93-006-20261102/run`，每次执行如下命令，将 `CASE` 换为上述三个完整测试名、`RESULT` 换为独立输出路径：

```sh
LLVM_PROFILE_FILE=/tmp/wt-i03-r93-006-20261102/CASE.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r93-006-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderYD --gtest_filter=CASE --gtest_output=xml:RESULT
```

`sha256.txt` 中二进制哈希是补丁后的最终二进制；原两项测试使用补丁前二进制，构建日志与各自 XML 保留原记录。未运行 sanitizer、部分成交/重复回报、真实 SDK、跨平台或整体回归。没有修改产品计算公式：订单身份、回报先后/重复与 `TradeVolume` 的厂商合同仍需确认；本条保持阻塞。
