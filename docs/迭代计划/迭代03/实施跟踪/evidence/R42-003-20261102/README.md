# BUG-R42-003：parser manager release 本机定向复验

2026-11-02，macOS arm64。测试基于本地检查点 `ccaad010` 的 `src`、`dist/common` 实体快照；运行时 `ParserAdapter.cpp`、`ParserAdapter.h` 和本测试源码与工作区哈希一致，见 `sha256.txt`。隔离快照修改只把 `CoverageTests_wtcore` 的测试翻译单元缩小为 `r42_parser_adapter_test.cpp`，并使实体 Boost 1.88 头优先；具体差异分别见 `focus.patch`、`root-boost.patch`。这不是完整 coverage 目标或最终整合树构建。

```sh
mkdir -p /tmp/wt-i03-r42-003-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r42-003-20261102/snapshot
# 在快照 src 目录分别应用 focus.patch 和 root-boost.patch。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r42-003-20261102/snapshot/src \
  -B /tmp/wt-i03-r42-003-20261102/build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r42-003-20261102/build --target CoverageTests_wtcore -j 6
sh 'docs/迭代计划/迭代03/实施跟踪/evidence/R42-003-20261102/run.sh'
```

配置、裁剪目标构建均 exit 0，详见 `configure.log`、`build.log`。目标依赖的 229 条编译命令均将实体 Boost 路径置于 `/opt/homebrew/include` 前，完整命令见 `build-commands.txt`；快照与构建目录未发现符号链接，见 `symlinks.txt`。首次独立运行的 manager 用例 exit 0，随后七个关联用例均使用独立进程、独立目录及 40 秒 alarm，`results.txt` 中均为 exit 0，XML 各为 1 test、0 failure、0 skip。

七项包括原验收的 `ManagerReleaseStopsRetainedSuccessfulAdapter`：释放前真实 fake connect 恰一次、假 provider 明确证明回调已排空、manager 清空后保留 adapter 的 run 失败且无额外 connect，重复 manager/adapter release 不重复释放；以及 `InitExtHandlesApiStates`、`BusyInitExtRetainsAndDeduplicatesTransferredApis` 和四个 release/remover 异常、manager 保留与延迟 remover 控制。各 fake 的回调排空证明仅是测试 seam，不代表真实 SDK 保证排空。脚本将每个进程退出码写入 `results.txt`，其自身退出码不代表每项均通过。

未运行 sanitizer、真实 provider、Windows/Linux、并发 run/release 或实际异步回调；provider release 后 API 指针清理顺序、重入、quarantine 生命周期和外部模块 pin 仍有原条目限制。本轮结果仅证明所列 fake 定向路径，不自动关闭整项或替代整体回归。
