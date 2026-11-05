# R103-002 已派发 tick 快照的局部验证

2026-11-02，macOS arm64。原始码订阅在 tick 回放期间改用实际派发时的自有快照；首次派发前或交易日变化后返回空。不同代码的未订阅历史查询保留旧行为；`prepare()` / `reset()` 同时清空原订阅与本地快照，`handle_init()` 重订阅的第二次完整回放已测试。快照映射使用互斥，锁不会跨用户回调。

前后复权历史仍采用旧路径：外部 loader / CSV 可提供独立于 raw 的数据，不能将原始快照冒充复权值。日线复权 bar 的合成回调会去除标准码后缀；此时原始码查询保留旧历史路径而不返回复权价格。两种路径的同刻或过滤可见性尚未解决，故不关闭本项。相关边界测试分别以原始价 10、复权价 20 区分来源。

临时错误候选红控仅在 `/tmp/wt-i03-r103-dispatched/buggy-adjusted.cpp` 中将合成源的 `adjustedSource` 固定为 false；`AdjustedSyntheticBarIsNotUsedAsRawTick` 以 exit 1 报出 raw 查询返回 20/22/19/21 而非 10，当前候选通过。该实验未修改产品源码。

## 局部 sanitizer 构建

```sh
sh src/TestUnits/fixtures/r103_dispatched_snapshot/run.sh /tmp/wt-r103-snapshot-asan
sh src/TestUnits/fixtures/r103_dispatched_snapshot/run.sh /tmp/wt-r103-snapshot-tsan tsan
```

输出目录须不存在；`WT_R103_BUILD` 可指向其他兼容的预先配置构建。脚本对当前 replayer 和测试翻译单元插桩，其他静态库复用 `/tmp/ctpm-i03-build`；它不是干净统一构建。实际命令产生的两个日志为 `/tmp/wt-r103-snapshot-final-asan.log` 和 `/tmp/wt-r103-snapshot-final-tsan.log`。ASan/UBSan 17/17；其中过滤混流和 bin 合成用例加载旧 `libWtDataStorage.dylib`，该库既有泄漏，整组关闭 LSan，不能计为泄漏通过。其余 15 项以 `ASAN_OPTIONS=detect_leaks=1` 另行运行通过：

```sh
DYLD_LIBRARY_PATH=/usr/local/lib:/opt/homebrew/lib ASAN_OPTIONS=detect_leaks=1:detect_container_overflow=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/wt-r103-snapshot-final-asan/prog '--gtest_filter=R103LastTickBoundaryTest.*-R103LastTickBoundaryTest.FilteredTickIsNotVisibleDuringOtherStreamCallback:R103LastTickBoundaryTest.SyntheticFallbackIsTheCurrentDispatchedSnapshot'
```

局部 TSan 并发查询用例 1/1；依赖未全部插桩，测试不保证所有调用与停止/销毁的线程安全。旧版的同刻次序、预热旧快照和过滤 tick 三个测试各失败；上一轮旧版红控证据见 `docs/迭代计划/迭代03/实施跟踪/evidence/R103-002-between-20261102/README.md`。

## 独立实体快照统一重编

为检验类布局变更，创建 `/tmp/wt-i03-r103-integrated/snapshot` 的 `src` 和 `dist/common` 实体快照，使用以下配置构建 `WtBtCore`、持有该类的 `WtBtPorter/WtBtRunner` 和 `WtDataStorage`，随后重编测试链接所需的 `WtDtCore` 与 `WtCore`。日志分别为 `config.log`、`build.log`、`build-core.log`；全部成功。快照不是原子采集，因此只能确认下面列出的关键源码哈希与当前文件一致。

```sh
work=/tmp/wt-i03-r103-integrated
mkdir -p "$work/snapshot/dist"
cp -R src "$work/snapshot/"
cp -R dist/common "$work/snapshot/dist/"
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake \
  -S "$work/snapshot/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=OFF -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-I/opt/homebrew/Cellar/boost/1.88.0/include
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake --build "$work/build" --target WtBtCore WtBtPorter WtDataStorage WtDtCore WtCore -j 6
```

以快照中最终测试源码、新编 `libWtBtCore.a` 及同一构建的静态库链接普通测试程序 `/tmp/wt-i03-r103-integrated/prog`；使用该构建的动态库运行 17/17，见 `test.log`。可使用以下脚本重新链接和运行（输出 `prog-review`）。该测试无 sanitizer；不能外推跨平台、真实 provider 或最终完整回归。

```sh
sh src/TestUnits/fixtures/r103_dispatched_snapshot/link-integrated.sh /tmp/wt-i03-r103-integrated
```

```text
21899ae8eec556d9bc7a2ce1eb89a78170db6c2ada05ed24e174074c55547e6f  src/WtBtCore/HisDataReplayer.cpp
a374f3dab992a09ccabdb433c883a18c791776178a930e5db204b266e749b5e5  src/WtBtCore/HisDataReplayer.h
1472eebf4275fc00aa0b2e29f0e7b5f29a0125706300256a530e73e91168d573  src/TestUnits/coverage/q02_btcore3/r103_replayer_last_tick_boundary_test.cpp
```

该类增加了状态，所有使用它的模块必须统一重编。全回放器时钟及订阅表、跨品种 session、动态订阅/停止/销毁并发、旧 ABI 二进制混用仍未验证。上述结果不是整项或迭代验收。
