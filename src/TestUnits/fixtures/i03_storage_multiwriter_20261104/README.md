# 迭代03 卡13：存储多 writer 发布协议（13a=A、13b=B、13c=A；2026-11-04）

用户裁定（2026-11-03）：13a=A prepare-then-publish + 失败回滚；13b=**B 多 writer 发布协议（第二个 writer 等待而不是被拒绝）**；13c=A 只有取得版本来源才迁移，未知格式隔离/fail-closed；13d 不在本次范围。

## 1. 设计

### 1.1 “多 writer”在各存储路径上的含义

| 路径 | 共享对象 | 多 writer 语义 | 发布点 |
|---|---|---|---|
| WtDataStorage RT 追加流（`rt/ticks|trans|orders|queue/<ex>/<code>.dmb`） | 同一 mmap 块文件 | N 个合作 writer 交错追加；每条记录完整出现一次，同一 writer 的记录保持自身顺序；不去重（两路相同行情会出现两次） | header `_size` 的一次对齐 32 位 release 存储 |
| WtDataStorage RT K 线（`rt/min1|min5`） | 同一 mmap 块文件 | 读改写最后一根 K 线或追加新 K 线在锁内串行；新 K 线先在未发布槽位完整准备再发布 | 同上 |
| WtDataStorage / AD tick 缓存（`cache.dmb` / `cache_tick.dmb`）及 AD K 线缓存（`cache_m1|m5|d1.dmb`） | 同一缓存文件，**各进程索引独立** | 锁内先同步其他 writer 的扩容映射、补扫其新增项再查索引；新合约项本地暂存，全部校验通过（AD 还须 LMDB 提交成功）后写槽位并发布 `_size` | 同上 |
| WtDataStorageAD LMDB（`ticks/<ex>/<code>`、`min1|min5|day/<ex>`） | LMDB 环境 | LMDB 自身以 `lock.mdb` 跨进程串行写事务；同键 put 幂等覆盖。协议只要求：**先提交 LMDB，再发布缓存/广播/外部 dumper** | `mdb_txn_commit` |

### 1.2 RT mmap 协议（`src/WtDataStorage/RTBlockPublish.hpp`，WtDataStorage 与 AD 共用）

1. **每文件发布锁**：`X.dmb` 旁的 `X.dmb.wlk`，`flock(LOCK_EX)` 阻塞获取（Windows 为 `LockFileEx`，未实机验证）。第二个 writer 等待而不是被拒绝；进程死亡时内核释放锁。锁只覆盖一次 prepare+publish（一条记录或一次结构性变更），不跨回调。
2. **prepare**：持锁后 ① 若日志显示上一持锁者死在结构性变更中，按物理文件长度校正 header（capacity ≤ 物理记录数，size ≤ capacity）；② 若已发布 capacity 超过本进程映射覆盖的记录数（其他 writer 已扩容），先重新映射；③ 满则扩容（扩展文件→映射校验→**最后**以 release 发布 capacity）；④ 把载荷写入第一个未发布槽位。
3. **publish**：对 `_size` 做一次 release 存储。崩溃在 ④ 与 publish 之间：槽位不可见，下一 writer 覆盖；崩溃在扩容中：文件已扩展但 capacity 未发布，下一次扩容复用该尾部。
4. **结构性日志与代号**：sidecar 前 40 字节为 `WTRTPUB` 日志（version=1、state、generation、owner_pid、target）。创建/打开修复/换日重置/扩容前置 `PREPARING` 并 generation+1，完成后 `IDLE`（`StructuralGuard` 保证存活进程的所有退出路径都复位）。新持锁者看到 `PREPARING` 即判定前任中途退出，执行校正并记 WARN。sidecar 内容未知（魔数/版本/状态不符）时 **fail-closed**（13c）拒写该块。
5. **新块创建**：完整初始化的文件映像写入同目录私有临时文件 `X.dmb.wtnew.<pid>` 后 rename 到位（持锁），崩溃只会留下“无块”或“完整块”，不会留下半初始化 header。
6. **读者可见性**（不取锁）：`visible = min(acquire(_size), acquire(_capacity), 本进程映射覆盖的记录数)`；当已发布 capacity 超过本进程映射（映射早于扩容）时先重新映射。`WtDataReader` 的 tick/逐笔/委托/队列/K 线实时读取改用该规则；`_size==0` 不再走 `_size-1` 下溢。
7. **不在协议内**：改动前编译的旧 writer/读者、非合作进程（不取锁直接写/截断）、断电与介质故障（无 fsync）、NFS 等网络文件系统、收盘作业 `transHisData/proc_loop`（重写/删除 RT 文件与缓存，须部署层指定唯一执行实例）、已发布最后一根 K 线/缓存项的原地更新对无锁读者仍可能读到撕裂（单 writer 时已有，未加 seqlock）。

### 1.3 AD 的 prepare-then-publish（R4-BUG-05）与 13c 隔离

- `writeTick` worker：缓存锁内准备缓存项（不发布）→ `commitTickToDb`（LMDB 提交）→ 成功才写缓存项/索引/`_size`，之后才调用外部 dumper、更新 K 线缓存与广播。提交失败：恢复预处理前的 tick（`action_time+200`、成交量差分等原地修改被撤销），缓存/广播/dumper 均不推进，同一 tick 可原样重试；同步模式 `writeTick` 返回 false，异步模式经新增 `tickPersistFailures()` 与 ERROR 日志观察。`get_t_db/get_k_db` 的目录创建改为 `error_code` 版本，不再在 worker 中抛异常。
- 13c=A：打开已有 LMDB 后只读探测首/末键：键长不等于当前 `LMDBHftKey`/`LMDBBarKey`，或 tick 库首键交易日为 0（P05-010 所述旧版零日期键），即**隔离**：不写、不迁移、记 ERROR，并记入 `_quarantined_dbs` 不再重复探测。只探首/末键（O(log n)），中间混入的异宽键不保证发现。
- R32-003/R100-003：v1 分钟键尾为 `uint32`，`bar.time > UINT32_MAX`（约 2033 年以后）时拒绝 LMDB 写入并记 ERROR，不再静默回绕；外部 dumper 仍收到完整 `WTSBarStruct`。V2 键迁移未接入。

### 1.4 ABI / 格式影响

- 磁盘格式不变（RT header、LMDB 键/值均未改）；新增 sidecar `*.wlk` 与临时 `*.wtnew.<pid>` 文件名。旧读者可继续读；旧 writer 不遵守协议，混跑不受保护（须部署层先停旧 writer）。
- `IDataWriter`（`src/Includes`）未改。`WtDataWriter`/`WtDataWriterAD` 是模块内部类，新增私有成员与 `WtDataWriterAD::tickPersistFailures()`；依赖其对象布局的外部代码须重编。
- 测试钩子结构 `p10_writer_test_hooks::Hooks` 末尾追加两个函数指针（仅 `WT_DATA_WRITER_TEST_HOOKS` 构建）。
- 性能：每次 RT 追加/缓存更新多一次 `open+flock+pread+close`（AD 每 tick 一次，WtDataStorage 每 tick 最多四次：缓存、tick 块、m1、m5）。未做基准，未缓存锁描述符。

## 2. 构建与运行

见第 4 节命令与 `evidence/`。WT_SRC_ROOT 选工作区（绿，`WT_GREEN=ON`）或改动前快照 `/tmp/wt-i03-storage-mw-20261104/baseline/src`（红，`WT_GREEN=OFF`，新 API/钩子用例不编译）。

## 3. 用例与红绿（macOS arm64，Homebrew LLVM 22.1.8，-j2；日志/XML 在 `evidence/`）

| 用例 | 覆盖条目 | 基线（红，改动前快照） | 工作区（绿） |
|---|---|---|---|
| `MwStorage.TwoProcessTransAppendNoLossNoTear`：两个 exec 的 writer 进程各追加 6000 笔成交到同一块（触发 3 次扩容） | R23-006 / 13b | exit 1：发布 11930/12000，410 条撕裂/混写记录 | 通过；日志 IDLE、generation≥4 |
| `MwStorage.TwoProcessTickCacheIndexAndSharedStream`：两进程各 24 个合约+1 个共享合约，共享 `cache.dmb` | 13b 缓存索引 | exit 1：缓存 50≠49 项，合约缺失 | 通过 |
| `MwStorage.ConcurrentReaderSeesOnlyCompletePublishedRecords`：独立读者进程按可见性规则轮询 | 读者可见性 | exit 1（读者见到撕裂/外来记录） | 通过 |
| `MwStorage.KillBetweenPrepareAndPublishLeavesSlotInvisible`：钩子停在写槽位后、发布前，父进程 kill -9 | 13a 崩溃 | 不适用（钩子为新增） | 通过：size 保持 50，下一 writer 复用槽位 |
| `MwStorage.KillInsideExpansionIsRecoveredByNextWriter`：扩展后、发布 capacity 前 kill -9 | 13a 崩溃恢复 | 不适用 | 通过：日志 PREPARING→下一 writer RECOVERED、复用尾部、原 2500 条完好 |
| `MwStorage.KillBeforeCacheEntryPublishDoesNotLeakIndex` | 缓存崩溃 | 不适用 | 通过 |
| `MwReaderVisibility.ClampsToOwnMapping…`（真实 `WtDataReader`） | R23-006 读端 | ASan SEGV | 通过 |
| `MwReaderVisibility.EmptyPublishedBlockReturnsNoSlice` | `_size-1` 下溢 | ASan BUS | 通过 |
| `MwAd.SyncDbFailureDoesNotPublishAndSameTickRetrySucceeds` | R4-BUG-05 | exit 1：抛 filesystem_error、缓存推进 | 通过 |
| `MwAd.AsyncDbFailureIsObservableAndNotPublished` | R4-BUG-05 异步 | 不适用（新计数 API） | 通过 |
| `MwAd.MinuteBarBeyondUint32KeyIsRefusedNotWrapped` | R32-003/R100-003 | exit 1：写入回绕键 | 通过 |
| `MwAd.UnknownKeyLayoutIsQuarantinedUntouched` / `LegacyZeroDateTickKeyIsQuarantined`（**合成**旧布局，非来源样本） | P05-010 / 13c | exit 1：混写新键（2 行） | 通过：隔离、库保持 1 行 |
| `MwAd.TwoProcessesShareTickCacheWithoutIndexCollision`（tick/m1/m5/d1 四个缓存，440 合约，含一次扩容） | 13b AD 缓存 | exit 1：tick 缓存 235≠440 | 通过 |
| 正控 `MinuteBarWithinUint32…`、`CurrentKeyLayoutDbKeepsWorking`、`OverlongExchangeRejectedBeforeSideEffects`（L04-004） | 兼容 | 通过 | 通过 |

重复：多进程用例 ASan 绿 ×5（storage 15/15、AD 5/5），基线红 ×5（storage 15/15 失败、AD 5/5 失败，`evidence/repeat/`）；plain 绿 storage×5 30/30、AD 两进程×10 10/10。

首次 plain（无 sanitizer）构建发现 AD m1/m5/d1 缓存创建未受协议保护：第二个进程 `create_new_file` 截断了对方已映射的文件，helper 以 SIGBUS(10) 退出；随后把三个 K 线缓存的创建/更新纳入同一协议，该用例 plain×10、ASan×3、TSan×3 均通过。

相关既有测试（逐用例独立进程，`evidence/regress/`）：
- WtDataStorage 158 例：基线 158/158；工作区首轮 156/158（`P10RtBlockTest` 两例白盒构造无文件的内存缓存，发布锁要求文件→修正为“无映射文件的缓存跳过跨进程锁”）后 158/158。
- AD 68 例：基线 67/68（`R122…UpdateTickCacheUninitialized` 两侧均 abort 134，既有）；工作区 65/68，新增失败 `R122WriterADDbOpenFailuresLogged`、`R132WriterADTickMapFullLogsPipeFailure` 均断言旧合同“LMDB 打开/写入失败仍缓存并广播 tick”，与 13a=A 直接冲突（现为不发布、`writeTick` 返回 false），**未修改这两条旧断言**，待整合时按裁定更新。
- 卡11 夹具：`i03_slice_unit` 6/6、`i03_slice_rdm` 9/9、`i03_slice_rdm_regress` 176/177（`Q03RdmKlineTest.day_tail_fragment_is_dropped` 与卡11 README 记录一致，两侧既有失败，即 I02-NEW-082 的旧日线尾碎片用例）。
- R23 旧映射探针（`r23_old_mapping/build.sh`，脚本末尾 `rg` 在 bash 子进程不可用 exit 127，改为直接运行四个用例）：两个原始反例仍红（原始映射不按可见性规则、非合作进程不取锁追加——均在协议外），两个正控通过。
- t26_fees_atomic：原 `run.sh` 反向打补丁失败（CTPLoader 已被他人后续改动，本工作未改 CTPLoader，哈希与改动前快照一致）；仅用当前源码构建 normal/asan 后 `valid` 例 exit 97（"No session configured"），属他人改动后夹具预期过时，非本次回归。

Sanitizer：ASan+UBSan（排除 alignment，同卡11）、TSan（0 告警）、plain 均跑；`detect_leaks=0`。产品 TU 以 `-Wall -Wextra -Werror` 编译通过（生产 CMake `WT_WARNINGS_AS_ERRORS=ON` 下 8 个存储 TU 编译成功；dylib 链接因本机 ld 不支持 `-no_warn_duplicate_libraries` 失败，环境问题，同卡11）。

## 4. 命令

```sh
B=/tmp/wt-i03-storage-mw-20261104; F=src/TestUnits/fixtures/i03_storage_multiwriter_20261104
# 改动前快照：rsync -a --no-links src/ $B/baseline/src/（HEAD ccaad010，哈希见 evidence/hashes.sha256）
cmake -S $B/baseline/src -B $B/core -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DWT_WARNINGS_AS_ERRORS=OFF
ninja -C $B/core -j2 WTSUtils WTSTools        # 两者不含本次改动
cmake -S $F -B $B/green-asan -G Ninja -DWT_LIB_DIR=$B/core/build_x64/Debug/libs -DWT_SAN=address -DWT_GREEN=ON ...
cmake -S $F -B $B/red-asan   -G Ninja -DWT_SRC_ROOT=$B/baseline/src -DWT_LIB_DIR=... -DWT_SAN=address -DWT_GREEN=OFF ...
# green-plain(WT_SAN=none) / green-tsan(WT_SAN=thread) 同理
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 perl -e 'alarm 600; exec @ARGV' $B/green-asan/mw_storage --gtest_output=xml:...
bash $F/evidence/run_cases.sh $B/green-asan/mw_regress_storage <out> 120   # 逐用例
```

## 5. 限制

仅本机 macOS/APFS 局部验证：未覆盖真实断电/介质故障（无 fsync）、Windows/MSVC（`LockFileEx` 路径未编译）、Linux、NFS/跨机共享、旧版 writer 与新 writer 混跑、真实行情负载性能、收盘作业多实例并发。P05-010 的隔离用例使用合成键布局，**不是**有来源的历史样本；探测只看首/末键。整体回归 NOT_RUN。
