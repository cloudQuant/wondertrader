# BUG-I02-NEW-091 回放夹具定向复验

2026-11-02，本机 macOS arm64。使用检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 和 `dist/common` 实体快照，不读取并发修改中的产品源码。没有修改工作区产品代码和共享构建文件。

在隔离快照中将 `src/TestUnits/coverage/CMakeLists.txt` 的 btcore 测试来源缩为 `p03_btcore_mockers2/r131_uft_mocker_edges_test.cpp+p03_btcore_mockers2/r161_btcore_edges_test.cpp`；根 `src/CMakeLists.txt` 中在 Boost 查找前增加 `include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)`，保证实体 Boost 优先。这是裁剪测试目标，而非完整 btcore 测试目标。全部 204 条编译命令中，涉及 `/opt/homebrew/include` 者均将实体 Boost 目录排在其前；命令见 `build-commands.txt`。快照与构建目录 `find -type l` 无结果，见 `symlinks.txt`。

```sh
mkdir -p /tmp/wt-i03-new091-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-new091-20261102/snapshot
git archive ccaad010 dist/common | tar -xf - -C /tmp/wt-i03-new091-20261102/snapshot
# 仅在快照中作上述两个 CMake 改动。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-new091-20261102/snapshot/src -B /tmp/wt-i03-new091-20261102/build \
  -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-new091-20261102/build --target CoverageTests_btcore -j 5
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-091-20261102/run.py'
```

配置及构建均退出 0，分别见 `configure.log`、`build.log`；本次目标链接及产品依赖在该快照中新建。关键源码和测试二进制的 SHA-256 见 `sha256.txt`。四个原验收单例各 1/1，关联组 `R131UftOrder.*:R161ReplayFixture.*` 10/10，均退出 0、无失败和跳过；各组独立进程、独立工作目录和 60 秒超时，具体命令与 XML 汇总见 `results.json`、同目录日志和 XML。

这是检查点快照的本机局部结果；未覆盖之后的 R132 变更、当前最终整合树、完整回归、其他平台或真实 provider。系统动态库为本机版本，本次未执行 sanitizer。不得把旧快照或本次局部通过外推为整体项目验收。
