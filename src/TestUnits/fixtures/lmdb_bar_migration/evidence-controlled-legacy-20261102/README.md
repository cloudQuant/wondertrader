# 受控证据接口的历史场景定向回归

2026-11-02，macOS arm64。新测试包含但不修改 `migration_test.cpp`；以受控证据装载和无 callback 的 `stageBatch` 调用重建可适用场景。旧 34 场景中 **29 场景有对应验证**，另有 1 场景验证旧 callback 被直接拒绝、callback 不被执行、staging 和 metadata 不被写入。运行命令：

```sh
bash src/TestUnits/fixtures/lmdb_bar_migration/run_controlled_regression.sh /tmp/wt-i03-controlled-legacy-r4
```

输出目录必须不存在。普通构建 30/30；LLVM 22.1.8 ASan/UBSan 构建 30/30，`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`，`UBSAN_OPTIONS=halt_on_error=1`；实际编译命令、逐项退出码和输入前后 SHA256 随本记录保存。崩溃子进程通过 `_exit` 直接退出，不能据此声明该子进程泄漏检查通过。完整逐项日志与临时实体 LMDB 数据在上述 `/tmp` 目录，可能被系统清理。fixture 是离线模拟数据，未接实际交易，未创建符号链接。系统依赖不一定全部插桩。

其中下列不能视为历史测试的原样等价：

* `badkey`、`badvalue`：受控证据 API 不接受格式长度错误的 key/value，因此该行先因证据缺失被拒绝，返回 `MissingEvidence`，不是原 callback 版本的 `InvalidRecord`；同时断言既无已提交 staging 也无 metadata。
* `concurrent-same`：同一 binding 现在在证据锁内串行，两次按序均可提交一个 batch；不再断言旧 callback 版本的 `DestinationChanged`。`concurrent-different` 验证另一个 binding 的迁移在首个目标暂停时仍可完成一个批次，均只覆盖受控 fake。
* `concurrent-prepare` 是违反初始化独占前置条件的受控负例，仅验证该次运行的同一 binding 准入，不证明任意环境或其他 binding 的并发初始化安全。
* `missing`、`identity`、`time` 在第一条记录注入失败，断言无提交及修复证据后的成功重试；批中途缺证后保留先前 batch 的语义由 `interrupted-batch` 另测。
* `map-full`、`map-full-partial` 使用 900 条证据，将目标空间填满以诱发 `MDB_MAP_FULL`，分别核查空状态和已有一个已提交 batch 时无部分新提交，再在没有在途操作时扩容并重试。填充通过单独测试表直接写目标，属于受控故障注入，不证明不合作的生产写入安全。

原 34 场景中 **5 项没有直接迁移，不计通过**：`exception`、`source-change`（callback 执行期间改源）、`reentrant`、`reentrant-thread`、`destination-change`（callback 执行期间改目标）。原回调不再执行，不能再用它注入异常、递归或事务期间事件。旧红控及测试源码继续保留；当前 30 项结果不能证明这些旧时序行为。另一个旧已知失败的批内撤销红控原本不属于旧 34 项，也不能用原 callback 在新接口中复现；新接口受控并发撤销的测试见 `../evidence-capability-20261102/README.md`。

中间 R2/R3 测试中 4 条记录的 `map-full-partial` 未达到断言条件：即使先前填充测试写入报告 `MDB_MAP_FULL`，其余 3 条 staging 小记录仍可能全部提交，因此该两轮普通和 sanitizer 均为失败，日志保存在本目录。最终 R4 将这两项恢复到 900 条数据、按提交计数核查并重新运行全组；不得将此前失败称为通过。

本次未执行 TSan。上一组真实 TSan 原始及注解版均报告 LMDB reader slot 竞争，仍为未解决失败；源证据真实性、非合作进程、单 binding 所有权、旧二进制停写门禁、真实断电、跨平台及 O(N²/B) 规模问题仍未验收，`Complete` 不是发布许可。该候选未接入生产 reader/writer，不构成整项验收。
