# BUG-R65-002：持仓不完整批次本机定向复验

2026-11-02，macOS arm64，Apple clang 17。使用检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 的 `src` 和 `dist/common` 实体快照；本轮未修改产品和测试。执行时工作区中的 `TraderCTPOpt.cpp/.h` 和测试 TU 与检查点一致，但工作区其他源码（包括 WTSCfgLoader）有后续变更，本结果不是最终整合树证据。源码及二进制哈希见 `hashes.txt`。

两个测试分别以独立进程、隔离工作目录和 30 秒 alarm 运行，均退出 0、各 1/1 PASS、0 skip：

- `position_query_unknown_rows_mark_batch_incomplete_and_next_batch_is_clean`：未知非末行及未知末行使本批不发布部分快照，清除聚合状态；下一批只含本批合约。
- `position_query_error_response_discards_partial_batch_and_release_resets_incomplete`：错误响应丢弃部分聚合；`release()` 清空不完整状态。

stdout 和 XML 分别见 `unknown.log/xml`、`error.log/xml`。测试通过 `R61OptProbe::harden()` 构建受控对象，直接调用 fake SPI 回调；未调用真实 CTPOpt 或模拟网络服务。

## 构建与运行

```sh
mkdir -p /tmp/wt-i03-r65-002-20261102/snapshot /tmp/wt-i03-r65-002-20261102/run
git archive ccaad010 src dist/common | tar -xf - -C /tmp/wt-i03-r65-002-20261102/snapshot
# 对快照应用本目录 cmake-root.patch 和 cmake-coverage.patch。
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r65-002-20261102/snapshot/src -B /tmp/wt-i03-r65-002-20261102/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r65-002-20261102/build --target CoverageTests_t_TraderCTPOpt -j 6
cd /tmp/wt-i03-r65-002-20261102/run
LLVM_PROFILE_FILE=/tmp/wt-i03-r65-002-20261102/unknown.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r65-002-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderCTPOpt '--gtest_filter=R61_TraderCTPOpt.position_query_unknown_rows_mark_batch_incomplete_and_next_batch_is_clean' --gtest_output=xml:/tmp/wt-i03-r65-002-20261102/unknown.xml
LLVM_PROFILE_FILE=/tmp/wt-i03-r65-002-20261102/error.profraw perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r65-002-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderCTPOpt '--gtest_filter=R61_TraderCTPOpt.position_query_error_response_discards_partial_batch_and_release_resets_incomplete' --gtest_output=xml:/tmp/wt-i03-r65-002-20261102/error.xml
```

配置退出 0。首次裁剪构建因该测试 TU 中其他既有断言的 GTest `-Wsign-compare` 被 `-Werror` 拒绝，退出 1，见 `build.log`。最终构建仅在隔离快照对该测试 TU 使用 `-Wno-error=sign-compare`，并将目标测试源限定为该 TU；退出 0，见 `build-final.log`。因此不能称原完整目标构建通过。`compile-commands-target.json` 记录三个相关 TU 的命令；实体 Boost 头路径优先于 Homebrew 入口。快照与构建目录的链接扫描结果 `symlinks.txt` 为空；没有创建符号链接。

## 未验证事项

这两项只证明构造的同步 fake 回调路径。同步请求返回非零后是否仍有迟到回调、请求 ID 是否可靠回显、跨批交错、真实 SDK/provider 的时序与排空合同、跨平台、sanitizer 和后续最终整合树均未验证。本条不能据此作为整体完成或整体验收。
