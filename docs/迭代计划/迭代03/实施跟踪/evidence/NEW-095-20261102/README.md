# BUG-I02-NEW-095：R180 / R210 夹具本机定向复验

2026-11-02，macOS arm64。检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 与 `dist/common` 导出到 `/tmp/wt-i03-new095-20261102/snapshot`；`HisDataReplayer.cpp` 和两个相关测试 TU 与当时工作区一致，但其他工作区文件已有检查点后的变更。本次没有更改产品源码、测试断言或工作区的 CMake 文件。

隔离快照的 CMake 将 btcore 用例限定为 R180、R210 两个 TU，并把实体 Boost 头目录置于 `/opt/homebrew/include` 之前，差异见 `root-cmake.patch` 和 `coverage-cmake.patch`。首次裁剪目标冷构建退出 1：R180 的 GTest 模板中 `int`/`unsigned` 比较被 `-Werror=sign-compare` 拒绝，完整日志保留在 `build.log`。经主代理确认后，仅在该**隔离测试 TU** 加 `-Wno-error=sign-compare`，不改变原断言和产品编译选项；重建裁剪目标退出 0，见 `build-test-warning-exception.log`。不能将其称为原完整目标构建通过。204 条目标编译命令中实体 Boost 目录均先于 Homebrew include，见 `build-commands.txt`；快照和构建目录未发现符号链接，见 `symlinks.txt`。

```sh
mkdir -p /tmp/wt-i03-new095-20261102/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-new095-20261102/snapshot
# 仅在快照中应用上面两个 CMake 差异。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-new095-20261102/snapshot/src -B /tmp/wt-i03-new095-20261102/build \
  -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-new095-20261102/build --target CoverageTests_btcore -j 5
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/NEW-095-20261102/run.py'
```

五个原验收单例各自独立进程均为 1/1 PASS、退出 0。关联组 `R180LoaderFixture.*:R180ReplayFixture.*:R210Fixture.*` 在独立进程中 **15 个实际执行：14 PASS、1 FAIL；另有 1 个 DISABLED 未执行**，进程退出 1。失败的 `R180His_CacheClearJoinsMultipleCodes` 在保留 `cache_clear_days=2` 时期望 loader 调用 2 次，实际为 1 次，关联 BUG-I02-NEW-096；禁用项是 `DISABLED_R180Exec_TwapSecondTickReleasesDanglingLastTick`。没有通过调整阈值或禁用测试掩盖失败。`run.py` 是收集器，脚本本身退出 0 不代表关联组通过；每个子进程退出码、过滤器、工作目录及 XML 计数见 `results.json`，原始输出与 XML 同目录保存。

二进制、真实同次构建 `libWtExeFact`、关键源与基础数据摘要见 `sha256.txt`。该结果仅适用于上述检查点本机及裁剪目标；未执行 sanitizer、其他平台、后续源码整合树或整体回归。不得据此关闭 NEW-096、禁用项或将关联组算作通过。
