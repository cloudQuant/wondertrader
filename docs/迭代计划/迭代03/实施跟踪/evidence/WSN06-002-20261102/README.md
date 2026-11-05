# BUG-WSN06-002 UFT 策略初始化局部定向复验

2026-11-02，macOS arm64。从检查点 `ccaad010` 导出独立实体 `src` 快照。`WtUftRunner.cpp/.h`、目标测试和 fake strategy 的快照与本轮执行时工作区哈希一致，见 `sha256.txt`；其他依赖以该检查点为准，不代表当前完整工作树。无产品源码变更。独立快照和构建目录未发现符号链接（`symlinks.txt` 为空），实体 Boost 目录优先；原测试目标在隔离快照裁剪为目标 TU，差异见 `root-cmake.patch`、`coverage-cmake.patch`。

配置和构建退出 0，日志分别见 `configure.log`、`build.log`。复现方式：

```sh
mkdir -p /tmp/wt-i03-wsn06-002-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-wsn06-002-20261102/snapshot
# 在快照 src 目录应用本证据中的两个 CMake 差异（路径以差异内容为准）。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-wsn06-002-20261102/snapshot/src -B /tmp/wt-i03-wsn06-002-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-wsn06-002-20261102/build --target CoverageTests_m_WtUftRunner -j 6
```

四个测试分别在独立工作目录中运行，每次外层 `perl -e 'alarm 45; exec @ARGV'` 限时；测试内部按设计用独立子进程执行策略。每次设置单独 `LLVM_PROFILE_FILE` 并保存 XML，命令形式：

```sh
LLVM_PROFILE_FILE="$PWD/run.profraw" perl -e 'alarm 45; exec @ARGV' -- /tmp/wt-i03-wsn06-002-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner --gtest_filter=r95_uft_strategy.TEST_NAME --gtest_output=xml:$PWD/result.xml
```

将 `TEST_NAME` 分别替换为以下名称：

- `active_init_false_rejects_full_config_and_keeps_run_closed`
- `later_init_false_does_not_publish_earlier_context_or_ready_sink`
- `duplicate_active_ids_fail_before_strategy_creation_or_publication`
- `ready_callback_is_synchronous_after_successful_context_publication`

四个进程均退出 0，各 1/1 PASS，XML 不含 skip；日志及 XML 按名称在 `runs/`。fake 策略明确返回 false，测试观察阻止发布、重复 ID 和成功 ready 时序；不能证明任意非法 params schema、策略初始化已经产生的外部副作用可回滚、真实 provider、其他平台或 sanitizer。最终整合树、整体回归仍未验证，BUG-WSN06-002 不因该子项通过关闭。
