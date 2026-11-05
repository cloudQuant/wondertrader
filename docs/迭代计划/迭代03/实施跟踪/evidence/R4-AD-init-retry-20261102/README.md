# AD writer 初始化失败后跨目录重试

本项仅修复**进入 init 时尚无缓存映射或任务线程的全新实例**。若首次初始化在后面的缓存文件检查或创建线程时失败，退出作用域前清除该次建立的 tick/m1/m5/day 映射、索引及借用 sink/base-data 指针；不会删除旧目录文件，也不会改动进入时已有的映射。再次初始化成功后不执行清理。并发 init 不受支持；已成功实例的重复 init、旧请求排空和线程生命周期未改变，不能据此声称重复初始化安全。

新测试使用公开 `init`、`getCurTick` 和 `writeTick`：A 目录的 tick/m1 有效、m5 身份字段未终止；B 目录的 tick/m1 数据不同。旧版先对 A 初始化失败，再对 B 返回成功，却读取 A 的价格 101 而非 B 的 202；之后行情更新也写入 A 的 m1 而不是 B，`red.log` 为 exit 1。修复后对应测试通过，并验证旧文件不再被改写。另一正控在已成功初始化 A 后使 B 的 header 无效，检查失败重试不会被新清理逻辑销毁原有映射；这不证明 `_base_dir` 等其他状态在重复初始化后仍安全。

`green.log` 为五个关联用例 5/5：跨目录失败重试、已映射对象失败重试保留原映射、tick/m1/m5/day 坏身份同目录修复后重试、合法旧缓存恢复。`related.log` 为先前日期/对齐及异步控制 21/21。macOS arm64，LLVM 22 局部 ASan/UBSan；LSan 关闭，其余预编译对象复用 `/tmp/wt-i03-ad-tick-alignment-20261102/build`，并非整仓干净构建。无真实交易和新建符号链接。`hashes.sha256` 包含修改前快照、工作树和二进制；`commands.json`、`green-commands.json` 是使用原 Ninja 参数提取后，重新编译该测试、当前 writer 并单独链接测试二进制的 argv，运行目录均为该隔离 build。直接重跑：

```sh
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TZ=UTC \
  /tmp/wt-i03-ad-init-review/green \
  '--gtest_filter=R4WriterADTest.FailedInitThenDifferentRootUsesOnlyNewRoot:R4WriterADTest.FailedReinitDoesNotDiscardPreviouslyMappedCache:R4WriterADTest.L03AlignedBarCacheRejectsMalformedIdentityAndRetries:R4WriterADTest.L03AlignedTickCacheRejectsUnterminatedIdentityAndRetries:R4WriterADTest.L03AlignedBarCachesRecoverSecondRecordAndFlushPreviousBars'
```

目录创建异常、已成功实例重入/并发 init、已接收异步任务的所有权、对外 dumper 的异步保留及落库/广播的失败原子性仍未闭合。writer 的内部 sink/base-data 由 host 借用；不能据这组测试外推 provider 停止或整个 R4-BUG-05 已修复。

## 落库、回调与返回值的未闭合反例

`bar_failure_probe.cpp` 是手动运行的故障探针，不加入正常测试集；`bar-commands.json` 为隔离构建命令，`bar-results.json` 和同名日志记录三个预期红控：

- `ManualDumperFailureDoesNotLosePreviousBar`：真实缓存和 LMDB 路径配合模拟 dumper，第一次回调返回 false；处理后续分钟时该 dumper 首次收到成功回调的 close 为 101，之前拒收的 100 没有再次送达。不能据此断言 LMDB 中同一 bar 丢失；它可能已经提交。多目标之间没有事务，整条行情重放可能重复此前已成功的回调或写入。
- `ManualDumperRetainsBorrowedBarAfterReturn`：受控模拟 dumper 在回调后保存并跨线程读取指针，ASan 报 stack-use-after-return（exit -6）。对齐变更将原映射中的 bar 改为对齐的局部副本；`IDataWriter.h` 的 `dumpHisBars` 未说明指针期限，`ExpDumper` 转发至外部回调。模拟者假定允许保留指针，并非真实插件会这样做或合同允许这样做的证据；需明确借用期限或设计带所有权和释放确认的版本化接口，不能无限保留内存。
- `ManualBarDirectoryFailureReportsAndCanRetry`：以普通文件占用真实 min1 目标目录，同步写入抛异常，异常前 tick 缓存已从 100 更新为 101；移除障碍后同一行情重试确实到达过 dumper。日志里的 accepted 初始值为测试哨兵，异常时调用未返回，不能解释为返回 true。此试验不证明所有副作用可回滚或重试无重复。

当前 `writeTick` 同步模式运行 task，内部早退仍可返回 true，异常可外抛；异步模式 true 表示入队，worker 后续异常只记录日志，没有逐任务完成结果。`DataManager::writeTick` 透传 bool；`ParserAdapter::handleQuote` 对 false 停止向指数处理器转发；`IndexWorkerMgr::push_tick` 忽略该返回值。仓库内搜索覆盖这些调用点，外部插件调用者未知。旧 bool 无法区分未接收、已完成、部分提交或结果不确定；把部分提交异常映射成 false 会改变下游行为，也不能保证外部安全重试。

预检可以发现已有的明显路径冲突，但不能防止检查后文件系统失败，也无法撤销此前的其他目标提交。未将预检当作事务，未添加异常转 false 或无界缓存。完整方案需要先确定各目标的完成确认与重放语义，再设计持久化的逐目标进度和幂等键、可恢复队列及能表达未知结果的版本化状态接口，兼容旧 ABI 并对现有调用方明确迁移策略；具体方案和验收尚未实现。
