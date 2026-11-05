# BUG-R14-003 四入口 basefiles 定向复验

2026-11-02 macOS arm64，使用本地检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 导出的 `src` 实体快照。四个入口及四份关联测试在运行时与当前工作区源码一致；源码、四个测试二进制 SHA-256 见 `sha256.txt`。没有修改产品代码和共享 CMake。

各测试 TU 只保留其 `r14_*_main_test.cpp`（隔离快照的改动见 `focus-cmake.patch`），其他产品和库依赖按现有 CMake 构建，因此这不是完整测试目标。为保证实体 Boost 头优先，在隔离快照根 CMake 中添加实体 Cellar include 目录，见 `root-cmake.patch`。Ninja 生成的 213 条编译命令均将 `/opt/homebrew/Cellar/boost/1.88.0/include` 放在 `/opt/homebrew/include` 之前，完整命令见 `build-commands.txt`；快照及新构建目录 `find -type l` 为空，见 `symlinks.txt`。

```sh
mkdir -p /tmp/wt-i03-r14-003-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r14-003-20261102/snapshot
# 在该快照中应用 focus-cmake.patch 和 root-cmake.patch。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r14-003-20261102/snapshot/src \
  -B /tmp/wt-i03-r14-003-20261102/build-physical -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r14-003-20261102/build-physical \
  --target CoverageTests_m_QuoteFactory CoverageTests_m_WtUftRunner CoverageTests_m_WtLatencyHFT CoverageTests_m_WtLatencyUFT -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/R14-003-20261102/run.py'
```

最终 configure 和裁剪目标构建各 exit 0，日志为 `configure-physical.log`、`build-physical.log`。第一次构建时检查发现 `/opt/homebrew/include` 中的 Boost 链接优先于实体路径，主动中断，exit 130，保留 `configure.log`、`build.log`；该次产物未用于测试。没有通过仅构建推断测试通过。

每个关联测试以独立工作目录、独立进程和 30 秒超时运行，`run.py` exit 0：**14/14 测试通过，0 跳过**。每例命令、工作目录、退出码和 XML 摘要见 `results.json`，原始日志和 XML 同目录。QuoteFactory、UFT runner、HFT latency、UFT latency 各覆盖缺失和非 object 拒绝及 object 对照；其中 UFT runner object 对照仅证明越过 basefiles 校验并到达缺 env 的检查，不证明完整 runner 启动。QuoteFactory 的失败用例调用被重命名但同源码的入口，成功对照调用初始化函数；未进入真实 parser 主循环或连接 provider，其他两个 latency 对照仅初始化而未调用 run。

这是检查点快照、本机裁剪测试目标的局部结果，不是当前整合树完整构建、全部项目回归、Windows/Linux 或真实供应商验收。此次没有运行 sanitizer，外部动态库和工具链由本机提供；QuoteFactory 真正 parser/主循环依旧未验证。
