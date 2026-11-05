# BUG-R202-001 全天收盘标签本机定向验证

2026-11-02，在 macOS arm64 使用检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src`、`dist/common` 实体快照；该构建只把 `CoverageTests_wtcore` 的测试翻译单元裁剪为 R202 公共面测试及其独立探针，并把实体 Boost include 置于 Homebrew include 前。与原配置的快照差异见 `focus.patch` 和 `root-boost.patch`，这不是原完整目标的构建或整体回归。未修改产品代码。

快照和运行时工作区的 CTA/SEL/HFT ticker、相应 engine 和测试文件哈希一致；本轮后续工作区的 `WTSCfgLoader.cpp` 与快照不同（见 `snapshot-source-sha256.txt`、`current-source-sha256.txt`），因此不能将本次结果外推为当前完整源码树。只列关键源文件哈希，并非全部依赖闭包。

## 命令和结果

```sh
mkdir -p /tmp/wt-i03-r202-001-midnight/snapshot
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r202-001-midnight/snapshot
patch /tmp/wt-i03-r202-001-midnight/snapshot/src/TestUnits/coverage/CMakeLists.txt < focus.patch
# 对快照 src/CMakeLists.txt 添加 root-boost.patch 中的实体 Boost 优先路径。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S /tmp/wt-i03-r202-001-midnight/snapshot/src -B /tmp/wt-i03-r202-001-midnight/build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r202-001-midnight/build --target CoverageTests_wtcore -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/R202-001-midnight-20261102/run.py'
```

配置、裁剪目标编译和运行脚本均退出 0。生成的 1139 条编译命令均检查实体 Boost include 在通用 Homebrew include 之前；重点产品及测试翻译单元命令见 `build-commands.txt`。快照和构建目录符号链接扫描为空，见 `symlinks.txt`；没有创建符号链接。二进制哈希见 `binary-sha256.txt`。

三个全天收盘用例分别用合法 `23:59:59.999` 输入，断言 CTA、SEL、HFT 的分钟标签为 `2400`、回调各一次，CTA 同时断言交易日存储；三项均独立进程各 1/1、退出 0。三个对应的普通时段收盘正控也各 1/1、退出 0。六项均未跳过；`results.json` 和 `runs/` 中的日志、XML 保留具体结果。`run.py` 为每个进程设置 40 秒超时；普通时段测试依赖运行时墙钟，接近午夜可能自行跳过，本轮无跳过。

本次没有运行 TSan 或完整回归。此结果只验证当前测试构造中的收盘端，不证明下一交易日首 tick 不丢、日期推进、引擎时间归零、ticker 状态同步、回调重入或线程安全，也不证明其他平台。R202-001 整项仍未闭合。
