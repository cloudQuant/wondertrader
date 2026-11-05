# R100/R32：证据快照一致性与迁移规模复核

2026-11-02，本机 macOS arm64。本次没有修改 staging 产品代码或生产读写；仅在 `migration_test.cpp` 增加两个证据撤销场景，并将批间撤销正控接入该夹具的定向脚本。没有引入未经证明的快速路径。R100-003/R32-003 仍阻塞。

`evidence-revoked-between-batches` 先以合法证据提交一条，再令该条证据撤销；现有前缀重查返回 `MissingEvidence`，保留已提交一条和 `I` 状态，证据恢复后正常完成。普通与 ASan/UBSan 均通过。

`evidence-revoked-during-batch` 为**手动 known-red 探针，不属于定向通过总数**：在一个四条批次中，先给第一条提供合法证据，读取第二条时撤销第一条证据，其余记录仍各自提供合法证据；此后重查第一条会明确拒绝。当前 API 没有快照代次/撤销通知，也不会重新查询已读取的第一条；普通与 ASan 均返回 `Complete` 并提交 4 条与 `C`，再次校验虽返回 `MissingEvidence`，已提交的 `C` 仍存在，断言“不发布撤销后证据”分别退出 1，见 `*-revocation-red.log`。这证明任意可变 callback 的批内一致性没有得到保证；这个受控假 callback 不证明真实证据系统实际发生撤销，无法判断其来源是否可信。

本机对最终测试源的原有 33 例加批间撤销控制，普通、ASan/UBSan 各 **34/34**。TSan 原始并发 0/2（两个进程均报告命名 semaphore 锁内 race），测试专用 semaphore 注解后 8/8；独立 semaphore 正负控制保留，不能把注解结果说成原始 TSan 通过。完整退出码在 `*-results.json`；`source-before.sha256` 和 `source-after.sha256` 一致，目标仅实体文件，未创建符号链接。

```sh
bash src/TestUnits/fixtures/lmdb_bar_migration/run.sh /tmp/wt-i03-migration-evidence-final3-20261102
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/benchmark.py /tmp/wt-i03-migration-evidence-final2-20261102
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/wt-i03-migration-evidence-final3-20261102/asan-test evidence-revoked-during-batch \
  /tmp/wt-i03-migration-evidence-final3-20261102/asan-revocation-red
```

上述输出目录必须事先不存在；最后一条为预期退出 1 的手动红控，重复执行要更换红控目录。仓库 LMDB C 与测试 TU 用 Homebrew LLVM22 编译，系统依赖未全部插桩；真实磁盘故障、硬断电、Windows/Linux 未验证。

`final2` 合成基准批次为 1024，1 万、5 万、10 万、25 万条分别调用证据回调 56,080、1,254,224、4,967,072、30,857,360 次；单次热缓存用时分别 27、750、3033、19648 ms。`final3` 对测试源码的唯一后续修改是增加该手动红控的复核输出，不改变基准模式或产品源码。构造数据不计入时间；这是本机单次合成观察，不是生产吞吐或任意规模验证。数据见 `benchmark-results.json`。

`Bindings` 只借用裸 `MDB_env*`；若外部直接关闭环境或 DBI，既无失效位也无法安全检测悬空指针。现有合同要求所有者在调用完成之前保活并禁止外部关闭/resize，不能将它当作已验证的强所有权。详见 [候选协议](candidate-protocol.md)。
