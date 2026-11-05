# R103-002 间隔定位与三个未闭合红控（2026-11-02）

## 结果与边界

仅实现局部修复：查询在两条 tick 之间时回退到前一条；查找只在 `min(_count, _items.size())` 范围内执行。首前和尾后仍返回空，精确命中仍返回首条同刻记录。尾后为本项选定工程合同，不能据此推断所有调用者均希望空值；空列表、首前、精确、间隔、历史查询后精确及尾后共 6 项定向通过。尾部无效元素缺少从公开接口注入的动态用例，只有边界代码静态检查。

三项红控保留为未解决：同刻第二笔派发时查询返回第一笔；回放前预热的查询快照与回放快照不同时返回旧值；非法秒数 tick 被回放过滤后，订单明细回调仍可查询到该 tick。`_cursor` 会跨过过滤记录，不能将它简单解释成已派发水位。该项为未闭合，完整回归未运行。

## 构建来源和哈希

本机 macOS arm64，Homebrew clang 22.1.8，代码根 `/Users/yunjinqi/Documents/new_projects/wondertrader`，本轮编译输出 `/tmp/wt-i03-r103-between`。`build.sh` 已归档于本目录；当前代码的产品 TU 和测试 TU 重新编译并插桩，依赖复用下列旧构建，**不是干净全树构建**，旧静态库及动态库未保证全量插桩，也未验证与当前树一致。完整 build 输出为 `build-filtered.log`，含链接器 DWARF 警告。

SHA-256：

```text
b4342cf8378229ac03540eed3e61d41c7a64da6b7b45f45d260f2cf8fa4fe943  src/WtBtCore/HisDataReplayer.cpp
09bf928b902b80ea00ac90799610b980153c8e9f16c21f91965f9be7c31c6e5b  src/TestUnits/coverage/q02_btcore3/r103_replayer_last_tick_boundary_test.cpp
233e9e7fb2b4d825fea3e3da927d82e89a757fb92c7c8bb56801c897805de58e  baseline/HisDataReplayer.cpp
41fcc87acf90a15166728fc6dfa14a0b3db3f042182ef361b7cd6bc944035cf8  build.sh
90249769bc09e9be98c47096de424e5792e20a6e3a4609bcdec23a41524c71bc  prog-baseline
3765795e68cf1fba89bcf467cabefbf7e17588cf0066f70ae6470c57cc4e1cc5  prog-final
674ad5d19a8f2638811576cb91e20d04d785004371b3c6abae03ecd60a061f46  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWtBtCore.a
c5d3589188ea20c5b038f12a704a91a91c35fa349d60fece8aacbed29fe0167b  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWtDtCore.a
918e560417e2098d8240fd416ff87c8da09dcff530d1ccb3d9c23f9948a9d3c8  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWtCore.a
ccc59a8e9b4c63ff0a8d8dd5c9ceda7b302cfe614a8f6e23c33c6bf577fa9940  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSTools.a
0c5a671b94b80a3a62ed8dfacbd9a7c5fd2a4f420c045bf5dccff67aedc3fdc8  /tmp/ctpm-i03-build/build_x64/Debug/libs/libWTSUtils.a
df94fc0b310d86c77a8a67919515f8135b834e00f0132e5df7eeb4117928063b  /tmp/ctpm-i03-build/build_x64/Debug/bin/libWtDataStorage.dylib
```

baseline 是本轮前工作树源码，其哈希恰好等于 `ccaad010e9169f610aded4d8dc0f5647f70a5cff:src/WtBtCore/HisDataReplayer.cpp`；这不表示其他链接库来自同一提交。重建需上述本机路径、依赖及 Boost/gtest 库仍存在：在仓库根目录运行下面命令会覆盖 `/tmp/wt-i03-r103-between` 下的本轮构建产物；若需保留它们应先另存。先核验当前源码和依赖哈希，若已漂移则结果不再对应本证据。

```bash
mkdir -p /tmp/wt-i03-r103-between/baseline
git show ccaad010e9169f610aded4d8dc0f5647f70a5cff:src/WtBtCore/HisDataReplayer.cpp > /tmp/wt-i03-r103-between/baseline/HisDataReplayer.cpp
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/R103-002-between-20261102/build.sh'
```

## 运行命令

均从仓库根目录运行。先设置本次环境：

```bash
export DYLD_LIBRARY_PATH=/usr/local/lib:/opt/homebrew/lib
export ASAN_OPTIONS=detect_leaks=1:detect_container_overflow=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

局部 6/6，exit 0，未见 sanitizer 诊断，日志 `final-focused.log`：

```bash
/tmp/wt-i03-r103-between/prog-final '--gtest_filter=R103LastTickBoundaryTest.*-R103LastTickBoundaryTest.SameTimestampQueriesFollowDispatchedOrder:R103LastTickBoundaryTest.ReplayDoesNotUseStaleHistorySnapshot:R103LastTickBoundaryTest.FilteredTickIsNotVisibleDuringOtherStreamCallback'
```

间隔旧版红控，exit 1，日志 `final-red-interval.log`；final 的同项包含在上述 6/6：

```bash
/tmp/wt-i03-r103-between/prog-baseline '--gtest_filter=R103LastTickBoundaryTest.BetweenTicksReturnsPreviousTick'
```

同刻第二笔红控，exit 1，查询返回 10 而非 11，日志 `final-red-order.log`：

```bash
/tmp/wt-i03-r103-between/prog-final '--gtest_filter=R103LastTickBoundaryTest.SameTimestampQueriesFollowDispatchedOrder'
```

不同快照红控，exit 1，查询返回 100 而非 10，日志 `final-red-snapshot.log`：

```bash
/tmp/wt-i03-r103-between/prog-final '--gtest_filter=R103LastTickBoundaryTest.ReplayDoesNotUseStaleHistorySnapshot'
```

过滤 tick 混流红控，exit 1，订单明细回调确实发生，但查到无效 tick；必须加动态库目录，否则测试因库缺失而没有触发回调。日志 `filtered.log` 另含 LSan 报告：旧 `libWtDataStorage.dylib` 的 `createBtDtReader` 路径泄漏 128 字节；该项不能作为 sanitizer 通过证据。

```bash
DYLD_LIBRARY_PATH=/tmp/ctpm-i03-build/build_x64/Debug/bin:/usr/local/lib:/opt/homebrew/lib /tmp/wt-i03-r103-between/prog-final '--gtest_filter=R103LastTickBoundaryTest.FilteredTickIsNotVisibleDuringOtherStreamCallback'
```
