# R100/R32 离线 staging：DBI 生命周期、并发与完成状态修正

本次修正仅限独立离线 staging，**未接入生产或发布；R100-003/R32-003 仍阻塞**。此记录取代相邻 `R100-R32-v2-staging-20261102` 中有关原 API 和当前测试数量的描述，旧证据保留历史背景。

## 接口与同步合同

- `Bindings` 是不可复制、调用方持有的对象。`prepareBindings` 必须在调用方独占两个环境期间执行；它串行打开并提交 V1 默认 DBI、两个目标命名 DBI，并对源与目标各执行过只读事务，确保仓内 LMDB 惰性 reader 初始化完成后才能发布绑定。未准备调用返回 `UnpreparedBindings`；同一对象重复/并发初始化被串行处理，只有一次成功。
- 环境所有者须确保同一个目标只创建一个绑定、所有目标访问经同一绑定协调，且直到全部调用与 callback 完成之前不关闭环境/DBI、不重新打开 DBI、不 resize。该约束不由组件对其他绑定、外部代码或其他进程强制；非合作访问不能视为安全。初始化失败在目标 DB 创建提交之后可能留下空命名 DB，但不会写数据或完成 manifest。
- `stageBatch` 不调用 `mdb_dbi_open/close`。同一绑定的目标短读与写事务由私有锁串行，锁与目标事务**不跨证据 callback**。每个 callback 返回后，以最多 batch 条为一组复核旧前缀，检查目标只读事务版本；提交前取得目标写锁并核对其事务号仍是读快照的下一版本，不符返回 `DestinationChanged`。此版本检查针对仓内 LMDB 的事务号实现，不是对其他数据库的通用承诺。
- 同线程 callback 直接调用 `stageBatch`/`prepareBindings` 返回 `ReentrantCall`。调用者仍应避免 callback 对任一参与的 source/destination 直接操作 LMDB、关闭对象或启动并等待不可控的递归工作。一个受控测试覆盖 callback 中另线程对同一绑定提交，外层检测版本改变并可重试；该场景不证明所有跨线程循环等待安全。源只读事务贯穿 callback，直接在同线程重入相同源的 LMDB 事务亦不属于支持的合同。
- 完成需同时满足源 cursor 达到 `MDB_NOTFOUND` 和同一快照的统计条数一致。仅在测试宏下受控少报/多报统计，两者均返回 `SourceInconsistent`，旧 staging 保持原样；这是构造的统计不一致，不是实际损坏数据库测试。没有测试宏的产品 TU 独立编译/运行，未引用测试 hook。

正常批量、证据、源码快照和数据不可发布等边界见原 staging 记录：外部必须先停写并排空、阻止旧程序、确认独立证据与真实数据完整性。源快照末次检查与目标提交间仍有窗口；组件不能证明门禁成立，且无法恢复 V1 已覆盖数据。I/C manifest 和独立环境不是访问控制，拥有目标环境访问权限的程序仍能直接读取 I 阶段数据，未完成数据不能对生产提供依赖外部隔离与发布协议。

## 本机验证

macOS arm64，使用 Homebrew LLVM 22.1.8；从仓库实体 LMDB `mdb.c`、`midl.c` 和专属测试直接编译，无真实交易或符号链接。执行：

```sh
bash src/TestUnits/fixtures/lmdb_bar_migration/run.sh /tmp/wt-i03-lmdb-binding-final-20261102
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/lmdb_bar_migration/benchmark.py /tmp/wt-i03-lmdb-binding-final-20261102
```

目标目录须事先不存在。普通和 ASan/UBSan（`detect_leaks=1`）各 **33/33**，命令及独立进程退出码在 `normal-results.json`、`asan-results.json`。保留先前 23 项真实 LMDB 故障/恢复场景，新增同一绑定并发初始化、未准备及重复初始化、同源同目标/不同目标并发、直接与跨线程重入、目标并发变更、源统计少报及多报。实际 LMDB C 源与测试 TU 均参与 sanitizer 编译；系统库和操作系统不受插桩。`no-test-hooks.log` 记录无测试宏编译运行；未定义符号列表没有测试 hook。

macOS 上未注解的真实 LMDB TSan 两个并发测试均报告 race（`tsan-raw-*.log`），首报在 LMDB 命名 semaphore 实际保护的 reader 计数。独立 POSIX named semaphore 测试的加锁但未注解版本同样报 race；对真实 semaphore 的成功 wait 后标注 acquire、post 前标注 release 后正控通过，无锁负控仍报 race（`tsan-sem-results.json`）。仅在测试构建的 LMDB C 源副本中对相同 semaphore 操作添加对应 TSan 同步注解后，**8/8** 定向退出 0；这不等于原始 TSan 通过，也不证明 LMDB 或所有依赖无竞争。`annotate_lmdb.py` 对原始源码精确匹配后生成临时副本，没有修改仓内 LMDB。

更早受控候选中，未预热 target reader 的 annotated TSan 报 `me_live_reader` 首次并发初始化，预热后但目标读写未按本绑定串行时又报 LMDB reader-slot 读写竞争；中间二进制重跑的 `before-warm.log`、`before-target-serialization.log` 保存了该过程，相关中间源码并非完整可重建快照。最终设计明确规避本组件内部这些并发访问，外部不合作访问和其他平台尚未验证。

## 性能限制

当前 callback 没有可验证的不可变独立证据快照身份，单靠源/目标 LMDB 事务号缓存“前缀已验证”不能发现外部证据后来变化；因此没有实现未经证明的快速路径。每次批次都重读并验证全部已提交前缀，复杂度约 `N²/(2B)`；此项为**上线阻塞**。

`benchmark-results.json` 为同一普通二进制、合成独立 LMDB、batch 1024 的单次迁移段计时（不含造数，可能受本机缓存影响）：1 万、5 万、10 万、25 万条分别调用证据回调 56,080、1,254,224、4,967,072、30,857,360 次；对应 21、664、1,941、12,761 ms。不是生产数据、跨进程/网络磁盘或真实证据存储的吞吐认证，不能外推任意规模。后续需定义可验证、可恢复的不可变证据快照和连续会话失效机制，完成目标数据/manifest 完整性检查及实际工作负载基准后，再考虑安全快速路径。

未验证真实 SDK、生产 reader/writer、Windows/Linux、真实断电/fsync 失败、非合作写者、跨进程回调生命周期或整合回归。真实磁盘满与强制电源中断也不能由 `MDB_MAP_FULL` 和 `_exit` 代替。
