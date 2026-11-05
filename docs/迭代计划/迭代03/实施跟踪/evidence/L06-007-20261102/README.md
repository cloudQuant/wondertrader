# BUG-L06-007 手续费配置定向复验

2026-11-02，macOS arm64。取检查点 `ccaad010` 的 `src` 与 `dist/common` 实体快照，`HisDataReplayer.cpp/.h` 与 `l06_replayer_runtime_test.cpp` 在快照创建时与当前工作区相同。未修改工作区产品代码或共享构建文件。隔离快照中的 CMake 将 `CoverageTests_btcore` 的测试源裁剪到该测试翻译单元，并令实体 Boost 目录优先；精确差异见 `snapshot-cmake.patch`。这是裁剪目标而非完整测试目标。

```sh
mkdir -p /tmp/wt-i03-l06-fees-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-l06-fees-20261102/snapshot
# 在快照中应用 snapshot-cmake.patch。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-l06-fees-20261102/snapshot/src -B /tmp/wt-i03-l06-fees-20261102/build \
  -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake \
  --build /tmp/wt-i03-l06-fees-20261102/build --target CoverageTests_btcore -j 5
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python \
  'docs/迭代计划/迭代03/实施跟踪/evidence/L06-007-20261102/run.py'
```

配置与构建均退出 0，分别见 `configure.log`、`build.log`；实际依赖从此快照新编译。生成的 1139 条构建配置命令中，涉及 `/opt/homebrew/include` 的命令实体 Boost 路径均在其之前，见 `compile-order.txt` 与 `build-commands.txt`。快照与构建目录的 `find -type l` 无输出，本轮未创建符号链接。源码、夹具、数据和测试二进制哈希见 `sha256.txt`。

每个用例在独立进程和临时目录运行，超时 60 秒；结果和完整命令见 `results.json`，各自 XML 和日志同目录。3/3 退出 0、无失败或跳过：按额费率缺商品元数据时初始化拒绝；按额计算使用商品合约乘数；按量费率无需商品元数据仍能计算。

本轮只覆盖列出的本机快照和配置，未运行 sanitizer、其他平台、后续最终整合树或整体回归；系统及其他预编动态依赖未重建。上述结果不代替最终整体验收。
