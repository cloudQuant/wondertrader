# BUG-R58-003 定向复验

建立独立源目录 `/tmp/wt-i03-r58-src` 和构建目录 `/tmp/wt-i03-r58-build`；配置命令：

```sh
/opt/homebrew/bin/cmake -S /tmp/wt-i03-r58-src/src -B /tmp/wt-i03-r58-build -G Ninja -DCMAKE_MAKE_PROGRAM=/Users/yunjinqi/opt/anaconda3/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWT_COV_P07_FIXTURE_ROOT=/tmp/wt-i03-r58-src
/Users/yunjinqi/opt/anaconda3/bin/ninja -C /tmp/wt-i03-r58-build CoverageTests_btcore -j 4
```

构建结束退出 0。分别在独立目录运行 `CoverageTests_btcore --gtest_filter=R43ReplayerInitBasefilesTest.<case> --gtest_output=xml:<path>`：`optionalBasefilesAllLoaded`、`configuredSessionLoadFailuresFailInit`、`configuredCommodityLoadFailuresFailInit`、`adjFactorFileParsed`、`badAdjFactorFileDoesNotBreakInit` 均 PASS（5/5）。XML 和输出按用例命名保存在本目录；二进制与关键源码哈希见 `sha256.txt`。

这是隔离快照的本机结果：测试 Boost bridge 是实体目录，且构建目录未发现符号链接；但产品编译命令中有既有系统 `/opt/homebrew/include/boost` 符号链接，不能称全部依赖均经实体目录编译。构建后工作区已有其他并行修改，快照与当时工作区差异见 `snapshot-hashes.json`，包括 WTSCollection.hpp 等；不能将本次结果说成当前完整工作树验证。其他平台和最终集成/全量回归未运行，因此本项继续 IN_PROGRESS。
