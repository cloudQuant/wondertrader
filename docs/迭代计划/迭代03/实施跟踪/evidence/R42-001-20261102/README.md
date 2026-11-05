# BUG-R42-001 SEL 生命周期本机定向复验

2026-11-02，macOS arm64。按照当前任务只运行四个直接相关的 SEL 生命周期用例，不运行整体回归。没有修改产品源码、共享 CMake 或执行跟踪。历史迭代02的严格 LSan 测试在四项断言通过后报告两处 `WtEngine::load_datas` 的 `WTSPortFundInfo` 各 144 字节泄漏（另有旧测试全局对象 72 字节）；本轮没有压制或关闭 LSan。

## Sanitizer 控制

Homebrew LLVM 22.1.8 编译 `asan_control.cpp` 和 `lsan_leak.cpp`，均使用 `-fsanitize=address,undefined -fno-omit-frame-pointer`。设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`，在 20 秒上限下运行：无泄漏控制退出 0；线程内 4096 字节泄漏控制退出 1 并报告确切泄漏，见 `control.log`、`leak-thread.log`。先前另一个主线程短生命周期泄漏探针退出 0 未检出，保留 `leak.log` 与 `leak-verbose.log`，不能将它解释为有效泄漏正控。第一次 shell 尝试错误使用 zsh 只读变量 `status`，退出 1，随后使用专用变量完成上述控制。

同一编译器对简单程序执行 `-fsanitize=memory` 报当前 `arm64-apple-darwin25.5.0` 不支持，编译退出 1（`msan-control.log`）；本机 `command -v valgrind` 无结果。ASan/UBSan 不检测所有未初始化读取，不能代替 MSan 或同等工具。

## 源码快照与构建

在 `/tmp/wt-i03-r42-001-20261102/snapshot` 中实体复制当时工作区 `src` 和 `dist/common`，基础 HEAD 为 `ccaad010e9169f610aded4d8dc0f5647f70a5cff`，包含当时已有未提交修改，**不是原子冻结的完整工作区**。`snapshot-manifest.sha256` 记录快照每个文件；`current-before.sha256`、`snapshot.sha256`、`current-after.sha256` 表明 `WtSelEngine`、`WtEngine`、`WTSSessionInfo.hpp` 与本测试等六个关键文件三次哈希一致，不证明所有文件均未并发变化。

仅在隔离快照运行 `prepare.py`：将 `CoverageTests_wtcore` 测试源裁剪为 `r42_sel_ctx_test.cpp`，并将 `/opt/homebrew/Cellar/boost/1.88.0/include` 放在通用 Homebrew include 前。配置及首次构建：

```sh
PATH=/opt/homebrew/Cellar/llvm/22.1.8/bin:/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
 -S /tmp/wt-i03-r42-001-20261102/snapshot/src -B /tmp/wt-i03-r42-001-20261102/build -G Ninja \
 -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
 -DCMAKE_C_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang \
 -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
 -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
 -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g -include cstdlib' \
 -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined \
 -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
 -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/Cellar/llvm/22.1.8/bin:/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r42-001-20261102/build --target CoverageTests_wtcore -j 6
```

配置退出 0；首次构建因无关 `HftMocker.h` 的 `-Wnontrivial-memcall` 被 `-Werror` 拒绝，退出 1，见 `build1.log`。仅在隔离快照运行 `focus_warning.py` 对 `WtBtCore/HftMocker.cpp` 与 `WtBtPorter` 三个受影响源文件降级该警告后，同命令重编退出 0，见 `build2.log`。这不是原始完整目标通过。`build-commands.txt`、`compile-summary.txt` 显示依赖目标 229 个编译命令中实体 Boost 顺序检查 0 个违例，所选三个关键源码的编译命令含 ASan/UBSan；系统 libc++、Boost、GTest 等预编依赖并非全部插桩，见 `linked-libraries.txt`。`symlinks.txt` 为空，本次未创建符号链接。

## 实际定向结果

`sh run.sh` 在各自独立进程和目录中对四项设置 40 秒硬上限，启用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，逐项退出码在 `results.txt`；脚本本身退出码不代表四项全部通过。GTest 四项断言各 1/1、0 skip，但按进程及 sanitizer 结果：

| 测试 | 进程退出 | 结果 |
|---|---:|---|
| FreshAutomaticStorageDestructionIsSafe | 0 | 定向通过 |
| ReleaseThenAutomaticStorageDestructionIsSafe | 0 | 定向通过 |
| InvalidFeeInitializationCanRunFailClosedAndReleaseConfig | 1 | LSan 报告 144 字节 `WTSPortFundInfo::create → WtEngine::load_datas` |
| SuccessfulInitializationReleasesConfigAfterDestruction | 1 | LSan 同路径报告 144 字节 |

各自原始日志与 XML 保存在本目录；本次两个初始化 case 的泄漏与历史 BUG-I02-NEW-034 资金对象栈一致。本轮未复测旧测试全局字符串 72 字节的那一额外翻译单元，不能据此否定历史结果。对于 `WtSelEngine::_tm_ticker`，构造初始化的源码证据仅能排除该字段构造路径的未初始化；MSan/等效工具、风险监控对资金对象的借用和停机、活动 worker、真实 provider、Windows/Linux、最终整合树及整体回归仍未验证。不得据此释放借用对象或关闭本条。
