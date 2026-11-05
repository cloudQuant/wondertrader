# R20_wtbtcore#1 自动通知器与无通知器回放局部验证

2026-11-02 macOS arm64；产品和测试取自本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` Git archive 实体快照。源码与二进制哈希见 `sha256.txt`。本轮未修改共享产品、测试或构建文件；其他代理的后续工作树修改不属于该快照。

原验收要求自动存储期 notifier 所有通知和析构安全、未配置 notifier 的 Runner 正常回放且 sanitizer 无 UB。已有 `R20EventNotifier.AutomaticStorageNotifierCanNotifyAndDestructBeforeInit` 涵盖默认实例通知与析构；`N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely` 使用真实 WtBtRunner，配置函数 `exec_config` 没有 notifier 字段，读取一根 CSV bar 并断言真实 runner 的插件回调和有序释放。插件是受控 fixture，不连接真实 provider。

## 工具链控制

```sh
printf 'int main(){return 0;}\n' > /tmp/wt-i03-r20-20261102/sanitizer-control.cpp
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer /tmp/wt-i03-r20-20261102/sanitizer-control.cpp -o /tmp/wt-i03-r20-20261102/sanitizer-control
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /usr/bin/perl -e 'alarm 10; exec @ARGV' -- /tmp/wt-i03-r20-20261102/sanitizer-control
```

LLVM 22 控制编译/运行退出 0。macOS 不支持该工具链 LSan，因此 `detect_leaks=0`。Apple clang 同源码同选项编译成功但 10 秒 alarm 中止，退出 142，不能计通过。

## 独立构建

```sh
mkdir -p /tmp/wt-i03-r20-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r20-20261102/snapshot
```

仅在快照 `src/CMakeLists.txt` 的 `#basic libraries` 前加入 `INCLUDE_DIRECTORIES(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)`；快照 `src/TestUnits/coverage/CMakeLists.txt` 中将 btcore 测试来源裁剪为 `p08_btcore_mockers3/r20_event_notifier_test.cpp`，btporter 测试来源裁剪为 `q06_btporter/r222_bt_runner_shutdown_test.cpp`。产品源与 fixture 仍由本快照构建；不是完整测试目标。

```sh
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
 -S /tmp/wt-i03-r20-20261102/snapshot/src -B /tmp/wt-i03-r20-20261102/build -G Ninja \
 -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
 -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=OFF \
 -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang \
 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
 -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
 -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
 -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
 -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -include stdlib.h' \
 -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
 -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r20-20261102/build --target CoverageTests_btcore CoverageTests_btporter CovExecV2Valid -j 6
```

配置/构建成功；初次误将 `WT_ENABLE_COVERAGE=OFF`，缺测试目标退出 1，保存为 `build.log`。重配为 ON 的日志为 `configure.log`、成功构建为 `build2.log`。有向相关 TU 的完整编译命令见 `relevant-commands.txt`；系统动态库及预编 GoogleTest 未保证 sanitizer 插桩。`find` 快照和两个构建目录未发现符号链接，见 `symlinks.txt`；未创建符号链接。正常对照重新以 `/usr/bin/cc`、`/usr/bin/c++`、相同配置但无 sanitizer 选项在 `/tmp/wt-i03-r20-20261102/build-normal` 独立重建 btporter 与插件，配置/构建退出 0，日志 `configure-normal.log`、`build-normal.log`。

## 定向运行

以下每项在单独目录执行，硬超时 90 秒，筛选仅含指定测试。插桩环境为 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，各目录独立 `LLVM_PROFILE_FILE`：

```sh
/usr/bin/perl -e 'alarm 90; exec @ARGV' -- /tmp/wt-i03-r20-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_btcore --gtest_filter=R20EventNotifier.AutomaticStorageNotifierCanNotifyAndDestructBeforeInit --gtest_output=xml:RESULT.xml
/usr/bin/perl -e 'alarm 90; exec @ARGV' -- /tmp/wt-i03-r20-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_btporter --gtest_filter=N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely --gtest_output=xml:RESULT.xml
/usr/bin/perl -e 'alarm 90; exec @ARGV' -- /tmp/wt-i03-r20-20261102/build-normal/build_x64/Debug/bin/TestUnits/CoverageTests_btporter --gtest_filter=N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely --gtest_output=xml:RESULT.xml
```

- 自动栈对象：插桩 1/1 PASS，退出 0，无 skip；日志/XML `notifier-asan-ubsan.log`、`notifier.xml`。
- 同一无 notifier 的真实 Runner CSV 回放：完整插桩运行在首次读取 CSV 并压缩历史 bars 时 UBSan 报 `WTSUtils/zstdlib/zstd_compress.c:1326` 对空指针应用非零偏移，退出 134，未到回放结束，见 `runner-asan-ubsan-failure.log`。这是必经压缩路径的实际失败，不是通过；未修改或屏蔽该代码。
- 独立普通构建的同一 Runner 正控 1/1 PASS、退出 0，日志/XML `runner-normal.log`、`runner-normal.xml`；不能抵消 sanitizer 失败。

因此本项自动通知器局部通过、真实 Runner 的 sanitizer 要求未通过，整项不能关闭。未验证实际 OOM、外部 provider、跨平台、最终整合树和整体回归；未运行 active MQ 或 null-config 路径。
