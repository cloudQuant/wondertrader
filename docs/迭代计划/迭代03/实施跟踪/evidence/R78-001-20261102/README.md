# BUG-R78-001 缺符号及引用生命周期局部验证

macOS arm64，本机 Apple clang。产品与测试基于 `ccaad010` 的 `src` 实体快照；将本次工作区 `r78_uftcore_adapter_init_test.cpp` 和专用无符号 fixture 复制进去。隔离快照 CMake 只对 uftcore 测试源筛选该测试 TU，并将实体 Boost 1.88.0 include 排在 Homebrew 入口之前；不代表完整目标构建。创建的快照、构建和运行目录未发现符号链接（见 `symlinks.txt`）。

```sh
mkdir -p /tmp/wt-i03-r78-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r78-20261102/snapshot
# 快照复制本轮测试文件；快照内将 uftcore 的 srcs 限定为 r78_uftcore_adapter_init_test.cpp，
# 根 CMake 中 include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r78-20261102/snapshot/src -B /tmp/wt-i03-r78-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
/opt/homebrew/bin/cmake --build /tmp/wt-i03-r78-20261102/build --target CoverageTests_uftcore ParserUDP -j 6
/usr/bin/c++ -dynamiclib src/TestUnits/fixtures/r78_no_parser/no_parser.cpp -o /tmp/wt-i03-r78-20261102/libR78NoParser.dylib
# 更新快照测试文件后重新编译：
/opt/homebrew/bin/cmake --build /tmp/wt-i03-r78-20261102/build --target CoverageTests_uftcore -j 6
cd /tmp/wt-i03-r78-20261102/run
R78_NO_PARSER_FIXTURE=/tmp/wt-i03-r78-20261102/libR78NoParser.dylib LLVM_PROFILE_FILE=/tmp/wt-i03-r78-20261102/final.profraw perl -e 'alarm 40; exec @ARGV' -- /tmp/wt-i03-r78-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_uftcore '--gtest_filter=R78UftParserAdapterInit.missing_parser_factory_releases_config_and_module_reference:R78UftParserAdapterInit.loads_udp_plugin_with_filter:R78UftParserAdapterInit.loads_udp_plugin_without_filter:R78UftParserAdapterInit.precreate_failures_retry_and_quiescent_init_failure_can_be_released:R78UftParserAdapterInit.release_does_not_register_a_null_spi' --gtest_output=xml:/tmp/wt-i03-r78-20261102/final.xml
```

配置、构建、重编及最终运行均退出 0；5/5、0 skip。日志和 XML 随附，关键源码与产物哈希见 `sha256.txt`。缺符号动态模块通过构造/析构标志记录 `LU`：在此 macOS 进程观察到模块加载后释放，init 失败后 cfg 引用为 1 且重复 release 安全。ParserUDP 只执行 init/release，不 connect；另一个本地 fake 的 initExt/run/release 分别计数。本测试未访问真实网络或柜台。

该标记只证明本机该 fixture 的实际加载和卸载，不可推广其他平台的 `dlclose` 物理卸载；真实 provider callback drain、活动回调、析构并发和跨平台未验证。现有测试文件的 POSIX dlfcn 依赖未解决。以下更新后的 CMake 目标自动提供动态 fixture；缺失路径宏时测试显式失败，不再依赖环境变量或静默 SKIP。前述环境变量运行是先前版本的历史证据。整体回归未运行。

## 项目目标集成修正

最终测试已不依赖环境变量：`CoverageTests_uftcore` 和总目标 `CoverageTests` 均依赖同源 `R78NoParserFixture` MODULE，并通过 CMake `TARGET_FILE` 传入路径；缺宏时测试失败。复制 fixture 的目标文件名由 `DLLHelper::wrap_module` 生成。fixture 的加载/卸载标志使用 C++ 静态对象，CMake 指定对应平台模块后缀。

以上节相同隔离快照中复制最终产品树的测试、fixture 和 coverage CMake 后，再仅对隔离快照应用同一 uftcore 测试 TU 裁剪和实体 Boost 排序：

```sh
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r78-20261102/snapshot/src -B /tmp/wt-i03-r78-20261102/build
/opt/homebrew/bin/cmake --build /tmp/wt-i03-r78-20261102/build --target CoverageTests_uftcore -j 6
cd /tmp/wt-i03-r78-20261102/run
LLVM_PROFILE_FILE=/tmp/wt-i03-r78-20261102/final-cmake.profraw perl -e 'alarm 40; exec @ARGV' -- /tmp/wt-i03-r78-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_uftcore '--gtest_filter=R78UftParserAdapterInit.missing_parser_factory_releases_config_and_module_reference:R78UftParserAdapterInit.loads_udp_plugin_with_filter:R78UftParserAdapterInit.loads_udp_plugin_without_filter:R78UftParserAdapterInit.precreate_failures_retry_and_quiescent_init_failure_can_be_released:R78UftParserAdapterInit.release_does_not_register_a_null_spi' --gtest_output=xml:/tmp/wt-i03-r78-20261102/final-cmake.xml
```

配置、构建和运行 exit 0，5/5 PASS、0 skip，见 `final-configure.log`、`final-build.log`、`final-cmake.log/xml` 和 `sha256-final.txt`。`umbrella-dependency.txt` 为 Ninja 查询，确认总目标依赖 fixture；总目标未构建或运行。此证据仍仅 macOS；Windows 及 Linux 未编译运行，且本测试 TU 原有 dlfcn POSIX 依赖仍在，不能声称跨平台测试可用。
