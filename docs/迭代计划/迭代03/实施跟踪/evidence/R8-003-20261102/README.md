# BUG-R8-003：本机 reader 及无 reader 查询定向复验

2026-11-02，macOS arm64。原验收写“缺 data/store/plugin 时不报告 inited”，同一固定清单当前记录及既有测试则要求缺 data、无 store 为可初始化的 no-reader 模式；两者冲突，本记录只呈现行为，不以现状或测试裁定产品合同。本次没有修改工作区产品代码或原测试。

以 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 为实体快照，再覆盖本轮工作区的 `WtDtRunner.cpp/.h` 和 `WtDtServo.cpp/.h`（差异 `servo-overlay.patch`）；运行后核对四文件与工作区 SHA-256 一致。其他源码使用该检查点，不包含同工作区其他并行修改。隔离快照的 coverage CMake 只选 `r8_wtdtrunner_test.cpp`、`r45_servo_api_test.cpp` 和本目录 `r8_export_no_reader_probe.cpp`，根 CMake 额外将 Boost Cellar 实体头置首；并未改共享构建文件。精确编译命令见 `build-commands.txt`，164 条 C++ 编译命令检查实体 Boost 路径均早于 Homebrew include。快照和构建目录 `symlinks.txt` 为空，未创建符号链接。

本地从同一快照构建 `WtDataStorageAD` 实体动态模块，测试二进制与模块的哈希在 `sha256.txt`；模块导出 create/delete 符号见 `module-symbols.txt`。测试 `ConfiguredAvailableReaderCompletesInitializationWhenPresent` 首先检查模块存在、可 dlopen 且两符号存在；本轮 1/1 运行通过、没有 skip。其断言只确认插件可创建及 runner 初始化、clear_cache 不抛异常。生产路径调用的 `IRdmDtReader::init` 返回 void（`IDataReader::init` 也为 void），因此不能证明 reader 内部资源均已就绪、数据可实际读取或 provider/生产行为。

构建命令（首次构建及加入独立探针后增量构建均退出 0；日志分别为 `build.log`、`build-probe.log`）：

```sh
mkdir -p /tmp/wt-i03-r8-003-20261102/snapshot
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r8-003-20261102/snapshot
# 按 servo-overlay.patch 覆盖上述四个文件；将本目录 probe 复制到快照对应测试目录。
# 在隔离快照的 CMake 中筛选上述三个 TU，并将实体 Boost include 置首。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew cmake \
  -S /tmp/wt-i03-r8-003-20261102/snapshot/src -B /tmp/wt-i03-r8-003-20261102/build \
  -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin cmake --build /tmp/wt-i03-r8-003-20261102/build \
  --target CoverageTests_dtservo WtDataStorageAD -j 6
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python \
  'docs/迭代计划/迭代03/实施跟踪/evidence/R8-003-20261102/run.py'
```

`run.py` 在独立目录及进程运行七个关联测试、每例 30 秒超时，**7/7 PASS，0 skip**；逐条命令、退出码和 XML 在 `results.json`。其中四项分别验证缺 data 可初始化、显式不可用 store 拒绝初始化与查询、无 store 初始化及查询、实际模块可用正控；第五项覆盖未初始化的 C 导出七个查询。新增两项临时探针分别在已初始化无 store 和不可用 store 时，确认七个 C 导出查询返回 0 且不调用数据/数量回调。0 同样可表示没有数据，接口不提供可区分错误状态。

未运行其他测试或整体回归；没有 sanitizer、Windows/Linux、真实供应商、资源故障、完整整合树或部署验证。故本条仍有合同及验收缺口，不据此标记整项完成。
