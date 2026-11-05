# BUG-R41-004 本机 TSan 定向复验

2026-11-02 macOS arm64，源码固定为本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 实体快照；`WtEngine.cpp`、`WtEngine.h` 及测试文件哈希见 `sha256.txt`，它们与本轮检查时工作区一致。没有修改共享产品、测试或 CMake 文件。

## TSan 环境自检

使用 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -fsanitize=thread -g tsan_control.cpp -o tsan_llvm` 编译，15 秒 alarm 下无竞争线程控制退出 0（`tsan_llvm.log` 为空）。用相同工具链编译 `tsan_race.cpp` 并以 `TSAN_OPTIONS=halt_on_error=1:exitcode=66` 运行，产生明确的 `ThreadSanitizer: data race` 报告，退出 134；报告见 `tsan_race.log`。该控制只证明本机这次能启动并报告该受控竞争，不证明所有竞争均可检测。Apple clang 的无竞争控制退出 139，不以其为通过依据。

## 隔离构建

```sh
mkdir -p /tmp/wt-i03-r41-004-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r41-004-20261102/snapshot
# 仅在快照应用 root-cmake.diff、coverage-cmake.diff、btcore-cmake.diff、btporter-cmake.diff。
PATH=/opt/homebrew/Cellar/llvm/22.1.8/bin:/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
 -S /tmp/wt-i03-r41-004-20261102/snapshot/src -B /tmp/wt-i03-r41-004-20261102/build -G Ninja \
 -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
 -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang \
 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
 -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g' \
 -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g -include cstdlib' \
 -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
 -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
 -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/Cellar/llvm/22.1.8/bin:/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r41-004-20261102/build --target CoverageTests_wtcore -j 6
```

快照中仅把 wtcore 测试源裁剪为 r214 一个 TU、实体 Boost 排在通用 include 前。首次构建（`build.log`）因无关 `HftMocker.h` 的 LLVM 22 `-Wnontrivial-memcall` 被 `-Werror` 拒绝；后两次分别在 `WtBtCore/HftMocker.cpp`、`WtBtPorter` 三个包含该头的 TU 上局部降级该警告，但仍显示警告，`build2.log`、`build3.log` 保留中间失败，`build4.log` 最终退出 0。这不是原始完整测试目标的构建通过。`build-commands.txt` 的 229 条编译命令中，含通用 Homebrew include 的命令均以实体 Boost 头目录优先；`symlinks.txt` 为空，没有创建符号链接。

## 定向运行

在独立 `/tmp/wt-i03-r41-004-20261102/run` 目录，设置 `LLVM_PROFILE_FILE=/tmp/wt-i03-r41-004-20261102/run/single-%p.profraw`、`TSAN_OPTIONS=halt_on_error=1:exitcode=66`，执行：

```sh
perl -e 'alarm 90; exec @ARGV' -- /tmp/wt-i03-r41-004-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore '--gtest_filter=R214EngineTaskShutdown.*' --gtest_output=xml:/tmp/wt-i03-r41-004-20261102/single.xml
```

单次 exit 0，四个用例 4/4、0 skip；同一命令加 `--gtest_repeat=20` 并使用不同 profile/XML 路径，exit 0，日志 20 轮、80/80 次通过，未见 TSan 报告。`stress.xml` 为 GTest 最后一轮，全部轮次见 `stress.log`。每个测试内部有 8 秒子进程 alarm，外层有 90 秒总上限。

GTest 静态库、Boost、nanomsg、iconv、系统 libc++ 等预编依赖没有 TSan 插桩，动态依赖见 `linked-libraries.txt`；本机 fake 与有界工作任务不证明外部 callback 永远返回、任意长期压力或其他平台。Windows/MSVC、后续工作区改动与最终整合树、整体回归未验证。结果不能外推到未列出的线程或全项目验收。
