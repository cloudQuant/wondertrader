# R23-006 旧映射和扩容回滚定向取证

本次仅新增独立夹具 `src/TestUnits/fixtures/r23_old_mapping/cross_process_test.cpp`；没有修改产品 writer，也没有涉及此前被中止的 RDM 清缓存任务。三个探针使用真实 `WtDataWriter`、隔离目录与本机伪 sink，不访问真实行情或交易。目标是限定既有块格式缺少的同步合同，不把探针通过视为整项验收。

## 观察

1. `OldViewMustNotExposeRecordsBeyondItsMappedLength`：独立子进程持有按旧文件长度建立的只读 MAP_SHARED 映射，确认 header 为 capacity=size=2500 后通过 pipe 暂停；父进程使用真实 writer 扩容并写入第 2501 条，子进程再读原映射头。观察到 capacity=5000、size=2501，但旧映射逻辑长度 1280024 字节，小于按已发布 size 所需的 1280536 字节；测试不访问超界区域，安全断言失败、exit 1（`red.log`）。原映射中最后旧记录的 volume=3499，重新映射后可读新记录 volume=3500，均通过同一次探针的断言。这个受控交错说明仅在进入 reader 时检查一次 capacity 不足以保证后续使用的映射覆盖期间增长；没有证明任意真实部署必然触发，也没有宣称实时数据跨进程发布原子。
2. `RollbackMustNotTruncateAnotherProcessesAppend`：在短写 hook 中先模拟本进程部分追加，再用独立进程 append 标记，发生在 writer 检查文件尺寸并回滚之前。现有回滚将文件截断到原长，第二进程追加丢失；旧 header 和最后旧记录均未改变，目标断言失败、exit 1（`concurrent-rollback-red.log`）。这是一个没有外部排他协议时的受控并发反例；不代表产品已承诺多 writer。无法仅根据文件长度区分部分追加与其他进程的写入，不能将此数据损失归于一次真实磁盘故障。
3. `FailedRollbackKeepsPublishedCapacityAndAllowsRetry`：在隔离源副本的目标 `truncate_file` 前加入一次性故障钩子（见 `instrument.diff`），先注入 16 字节短写，再模拟回滚 truncate 失败。观察错误日志、旧 size/capacity 和最后旧 tick 保持、物理尾部残留；下一次真实重试扩容并写入下一 tick，1/1 PASS、exit 0（`rollback-failure-control.log`）。该注入只模拟失败返回，不证明真实 OS、断电、多个 writer 或外部截短下安全。

4. `FailedRemapPreservesOldProcessViewAndRetryCanRemap`：子进程先持有旧映射；父进程通过现有测试 hook 在文件已扩展后拒绝新映射，旧 header 和末条 tick 在子进程中仍正确。父进程再次写入后，子进程主动重新映射并读到新 tick，1/1 PASS、exit 0（`final-FailedRemapPreservesOldProcessViewAndRetryCanRemap.log`）。只验证该同步交错的 remap 失败与重试；旧映射上的并发使用风险仍由第一项保留。

## 构建和边界

2026-11-02 macOS arm64。独立实体复制 `src` 至 `/tmp/wt-i03-r23-old-mapping-20261102/src`，Boost/fmt/spdlog/GTest 使用 `/opt/homebrew/Cellar` 的实体目录，源码快照未建立符号链接。根目录 C++17 使用 Homebrew LLVM 22.1.8，产品 writer TU 与 test TU 编译时开启 `-fsanitize=address,undefined -fno-omit-frame-pointer`，链接复用 `/tmp/wt-i03-r41-drain/libs/libWTSTools.a`、`libWTSUtils.a`、Homebrew 预编 GTest/Boost 和系统库（未全部插桩）；`-undefined dynamic_lookup` 的最小链接不代表完整产品链接。首次链接遗漏覆盖率 runtime 失败，加入 `-fprofile-instr-generate` 后成功；快照注入首次因 BOM 位置编译失败，调整到 BOM 后成功，这些都是测试构建问题，未视为产品失败。未运行 LSan/TSan、Windows/Linux 或全量回归。

原映射反例首次以原样 writer TU（无测试 hook）构建并执行，`red.log` 来自该二进制。并发追加及回滚失败控制使用 `WT_DATA_WRITER_TEST_HOOKS` 编译，同一快照中仅插入 `instrument.diff`：当一次性 hook 未置位时，两个已观察的行为等价于当前 writer；hook 置位时模拟 `truncate_file` 失败。产品代码未引入 hook。每条使用 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`，由 Perl `alarm 35` 设置硬超时。

```sh
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
LLVM_PROFILE_FILE=/tmp/wt-i03-r23-old-mapping-20261102/replay-%p.profraw \
perl -e 'alarm 35; exec @ARGV or die $!' -- /tmp/wt-i03-r23-old-mapping-20261102/probe-rollback \
  --gtest_filter=R23OldMapping.FailedRollbackKeepsPublishedCapacityAndAllowsRetry
```

`hashes.txt` 记录工作区源/头、快照头与测试、插桩源和二进制；工作区 writer、头、TimeUtils 和 session 头的拷贝时内容与本次产品 TU一致，除 `instrument.diff` 所列的探针插桩；其余预编静态库与当前整合树一致性未证明。真实 reader 若要安全使用增长中的文件，需要已验证的版本化发布协议和 reader 对映射长度、记录数量的同步快照/重新映射保护；旧二进制、slice 生命周期及非合作多 writer 的协调还需定义。仅对 writer 写 header 增加内存栅栏，或只依赖读端一次容量检查，不能阻止上述检查后扩容的交错；禁止扩容也不能满足需求。

当前四项的隔离快照完整重建命令：

```sh
bash src/TestUnits/fixtures/r23_old_mapping/build.sh /tmp/wt-i03-r23-old-mapping-final-20261102
```

输出目录须不存在，脚本记录实际编译/链接、各探针退出码和预期失败文字。最终结果为两个预期失败、两个通过（`final-build-and-run.log` 与四个 `final-*.log`）；该脚本退出 0 表示红控按预期被复现，不代表 R23-006 通过。
