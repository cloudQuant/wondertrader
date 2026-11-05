# BUG-T27-004 登录失败排队回调的本机反例

本轮未修改产品代码。快照为 `ccaad010` 的 `src`，只将工作区新增的 `t27_parserxtp_api_seam_test.cpp` 拷贝到快照；测试使用既有 `t27_seam.hpp` 布局镜像注入本地 fake QuoteApi、受控 `io_context::poll()`，不连接厂商或启动 worker。源码与二进制哈希见 `sha256.txt`。该镜像是测试专用方式，不能证明其他 ABI 或真实 SDK 行为。

新增回归在 POSIX GTest 子进程中分别令 fake Login 返回 `-1` 与 `2`，登录完成/handler 入队后调用 `registerSpi(nullptr)`，再执行队列。每个子进程设置 3 秒 alarm；父进程运行命令另有 25 秒硬限时。两次登录返回码均在运行 handler 时由 SIGSEGV（signal 11）终止，测试进程 exit 1（`regression2.log`、`regression2.xml`）。两个 sink 持续有效的既有正控共同 2/2 通过（`control2.log`、`control2.xml`）；单独的 `-1` 正控 1/1 通过（`control.log`、`control.xml`）。首次反例尝试无输出且被外层 15 秒 alarm 终止（exit 142，`regression.log`），不能视作已触及或证明崩溃；保留此失败。

构建过程：

```sh
mkdir -p /tmp/wt-i03-t27-004/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-t27-004/snapshot
cp src/TestUnits/coverage/t_ParserXTP/t27_parserxtp_api_seam_test.cpp /tmp/wt-i03-t27-004/snapshot/src/TestUnits/coverage/t_ParserXTP/t27_parserxtp_api_seam_test.cpp
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-t27-004/snapshot/src -B /tmp/wt-i03-t27-004/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-t27-004/build --target CoverageTests_t_ParserXTP -j 6
```

首次目标构建 exit 1，已有无关 `cov_parserxtp_spi_test.cpp` 的 GTest `sign-compare` 警告被 `-Werror` 拒绝（`build.log`）；仅在独立快照 CMake 中将 ParserXTP 测试源裁剪到本项 t27 TU、并把实体 Boost 目录置于 include 首位后重新配置并构建，exit 0（`focused-configure.log`、`focused-build.log`）。差异在 `root-cmake.patch`、`coverage-cmake.patch`；并非完整目标构建通过。快照和构建目录 `find ... -type l` 无输出，未创建符号链接。运行命令在独立临时目录：

```sh
LLVM_PROFILE_FILE=/tmp/wt-i03-t27-004/regression2.profraw perl -e 'alarm 25; exec @ARGV' -- /tmp/wt-i03-t27-004/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_ParserXTP --gtest_filter=XtpLoginF.pending_login_failure_rechecks_unregistered_sink --gtest_output=xml:/tmp/wt-i03-t27-004/regression2.xml
LLVM_PROFILE_FILE=/tmp/wt-i03-t27-004/control2.profraw perl -e 'alarm 25; exec @ARGV' -- /tmp/wt-i03-t27-004/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_ParserXTP '--gtest_filter=XtpLoginF.login_minus_one_pump_delivers_connect_event_carrying_the_raw_code:XtpLoginF.login_other_failure_sends_one_sync_and_one_posted_connect_event' --gtest_output=xml:/tmp/wt-i03-t27-004/control2.xml
```

`registerSpi` 直接替换裸指针，队列 handler 和多个厂商回调均访问同一指针；只在两个 lambda 加 null 检查不能使并发注销与销毁安全。需要宿主与 parser 明确注册/注销线性化、正在执行回调的租约及注销屏障，处理回调内注销的重入和延迟销毁，再确认 SDK 的回调排空及 worker stop/join；在 `void registerSpi` 和裸借用接口未提供这些保证前不能声称实现安全。已新增失败的定向回归保留在工作区；本项继续阻塞，Windows、本机 sanitizer、真实 SDK 与整体回归均未运行。
