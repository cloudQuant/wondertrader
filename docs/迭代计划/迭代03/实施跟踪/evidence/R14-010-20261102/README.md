# BUG-R14-010 同步与异步 run 本机定向复验

2026-11-02，本地 macOS arm64。以本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 导出独立实体快照，当前工作区 `WtUftRunner.cpp/.h` 和对应测试与快照的 SHA-256 相同，见 `sha256.txt`。其他依赖以快照为准，不代表当前整合树。未修改产品代码或共享构建文件。

隔离快照做两处构建限定：根 `CMakeLists.txt` 在 `PROJECT(WonderTrader)` 后加入 `include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)`；测试 `CMakeLists.txt` 中 `file(GLOB mod_tests ...)` 后对 `WtUftRunner` 设定唯一测试 TU 为 `${CMAKE_CURRENT_SOURCE_DIR}/m_WtUftRunner/r95_wtuftrunner_config_run_test.cpp`。这是裁剪目标而非完整测试目标。快照和构建目录 `find -type l` 无输出；未创建符号链接。生成的 86 条编译命令中，所有出现 Homebrew 通用 include 的命令均将实体 Boost 头目录置于其前，见 `build-commands.txt`。

```sh
mkdir -p /tmp/wt-i03-r14-010-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r14-010-20261102/snapshot
# 仅在此隔离快照中实施上文两处构建限定。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r14-010-20261102/snapshot/src -B /tmp/wt-i03-r14-010-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r14-010-20261102/build --target CoverageTests_m_WtUftRunner -j 6
```

配置、构建均退出 0，见 `configure.log`、`build.log`。每项分别在独立临时工作目录运行，以 `perl -e 'alarm 30; exec @ARGV'` 限时 30 秒，测试二进制为 `/tmp/wt-i03-r14-010-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner`，设置独立 `LLVM_PROFILE_FILE`，并分别指定以下过滤器和 XML 输出：

- `r95_uft_run.async_mode_returns_after_startup_and_owner_can_shutdown`：退出 0，1/1，0 skip；验证异步启动返回、owner 显式 shutdown 后状态。
- `r95_uft_run.synchronous_mode_waits_for_exit_signal`：退出 0，1/1，0 skip；等待 2 秒后以 SIGINT 退出并验证 shutdown 完成。

原始输出及 XML 分别为 `async.log`、`async.xml`、`sync.log`、`sync.xml`。测试使用本机受控配置和空管理器，不触发真实柜台；未验证并发或重复 run、回调内 shutdown、真实 provider 的停止/回调排空、Windows/Linux、sanitizer 或最终整合树。调用方仍须持有 runner 直至 shutdown 完成。整体回归未运行；本证据仅支持上述两个局部路径。
