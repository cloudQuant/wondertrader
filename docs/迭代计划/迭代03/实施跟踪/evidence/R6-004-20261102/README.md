# BUG-R6-004 外部行情空参定向复验

2026-11-02，macOS arm64，Apple clang 17。使用本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src`（Git tree `67f193194f16197bc5a5b0ae9c41ddcf3faed546`）导出隔离源码；工作区中 WtPorter、WtRtRunner、WtCore ParserAdapter 及本项测试源码相对该检查点无差异。其他依赖在检查点后已有变化，不能把结果称作最终整合树通过。本轮未修改产品代码或共享构建文件。

## 构建

```sh
mkdir -p /tmp/wt-i03-r6-004-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r6-004-20261102/snapshot
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python \
  'docs/迭代计划/迭代03/实施跟踪/evidence/R6-004-20261102/prepare_snapshot.py'
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r6-004-20261102/snapshot/src -B /tmp/wt-i03-r6-004-20261102/build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake \
  --build /tmp/wt-i03-r6-004-20261102/build --target CoverageTests_porter -j 6
```

配置、构建分别退出 0，输出见 `configure.log`、`build.log`。`prepare_snapshot.py` 仅修改隔离快照的两个 CMake 文件：将 Boost 实体头目录排在 Homebrew 入口之前，以及把 porter 测试源裁剪为 `r82_porter_runner_edges_test.cpp`。这是裁剪目标，不代表完整 porter 测试目标通过。`target-commands.txt` 记录目标及其依赖的 185 条编译命令，`include-check.txt` 检查全部命令中实体 Boost 优先级，0 条错误；配置产生的 1110 条编译命令也作了同样检查，0 条错误。源码与二进制哈希见 `sha256.txt`。快照、构建及运行目录的 `find -type l` 结果见 `symlinks.txt`（空）；本轮未创建符号链接。

## 定向运行

```sh
sh 'docs/迭代计划/迭代03/实施跟踪/evidence/R6-004-20261102/run.sh' \
  '/Users/yunjinqi/Documents/new_projects/wondertrader/docs/迭代计划/迭代03/实施跟踪/evidence/R6-004-20261102'
```

`run.sh` 只运行 `R6PorterExtParserQuote.null_inputs_are_diagnosed_and_registered_quote_is_dispatched_once`：一遍使用原测试内 `posix_spawn` 子进程，外层 75 秒超时；另一遍显式设置 `WT_R6_QUOTE_DIAGNOSTIC_CHILD=1` 单独执行子进程测试体，60 秒超时。两次均退出 0、各 1/1 PASS、0 skip；日志、XML 与退出码分别为 `parent.*`、`child.*`。父测试检查子进程已启动、未超时、已回收、正常退出 0。子测试体分别断言 runner 的 null ID、已注册 ID/null tick、未知 ID/null tick 和未知 ID/有效 tick 诊断，C API 的 null ID/null tick 诊断，及已注册合法行情恰好一次投递、标准代码和价格正确。

该结论限于本机 POSIX harness、受控数据和检查点源码；未运行 sanitizer、其他平台、真实 provider、后续整合树或整体回归。测试 Windows 分支显式跳过，不能由本结果推断 Windows 行为。
