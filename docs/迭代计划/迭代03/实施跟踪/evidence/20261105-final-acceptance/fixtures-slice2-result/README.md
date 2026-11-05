# 最终验收：失败夹具套件处置（slice2-result）

日期 2026-11-05，macOS arm64。任务说明：`SCRATCH/fixtures/FIXFIX-PROMPT.md`（SCRATCH=`/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad`）。本目录只由 slice2-result 写入。

## 结论

7 个套件全部归为 **STALE_FIXTURE**，已修复夹具并在“F3 + 夹具修改”的树上重跑通过；**无 PRODUCT 类、无 SUPERSEDED 类**。逐套件机器可读结果：`fixfix-slice2-result.json`（与 `SCRATCH/fixtures/fixfix-slice2-result.json` 相同）。

| 套件 | 分类 | 根因 | 修改 | 重跑结论 | 条目 |
|---|---|---|---|---|---|
| i03_dt_parser_host_20261104s5 | STALE_FIXTURE | `run.sh` 普通变体在 macOS `/bin/bash` 3.2 下 `"${extra[@]}"` 于 `set -u` 报 unbound variable，配置前中止 | `run.sh` 改为 `${extra[@]+"${extra[@]}"}` | pass（plain 6/6、thread 6/6；baseline 红控需改前副本，跳过） | BUG-R123-003、BUG-R18-006、BUG-R18-012、BUG-R8-006、I03-NEW-044、I03-NEW-047 |
| i03_dt_writer_lifecycle_20261104s5 | STALE_FIXTURE | TSan 变体 `Lifecycle.RealWriterReleaseIsBoundedWhenCallbackBlocks` 的 gtest 通过但进程退出报 thread leak：按卡9 9a 隔离的 writer 连同线程保留到进程退出，属设计（主代理已接受），夹具未标注 | `run_cases.sh`：仅 TSan 变体、仅该例关闭 `report_thread_leaks` 并在 results 行尾标注 `tsan_thread_leak=expected-by-design(card9-9a;report_thread_leaks=0)`；竞争等其他 TSan 报告照常计入，显式 `S5_TSAN_THREAD_LEAKS` 覆盖该清单 | pass（normal/asan/tsan 各 18/18，异常 0） | BUG-P10-008、BUG-R18-002、BUG-R23-001 |
| r23_retry_wait | STALE_FIXTURE | `build.sh` 内嵌插桩脚本的锚点仍为 `fmtutil::format("{}rt/min1/", _base_dir)`；I03-NEW-039（本段，2026-11-05）已改为 `fmt::format`，assert 失败 | `build.sh` 锚点改为当前文本，hook 位置与 baseline 还原方式不变 | pass（baseline shutdown exit 5 为预期红，其余 0；300 秒长测未跑） | BUG-R23-001 |
| i03_csv_block_type_20261103 | STALE_FIXTURE | `EmptyDirectoryAndIgnoredFilter` 期望空目录/不匹配过滤器为成功且过滤器被忽略；BUG-L05-002（2026-11-04）合同为 `WT_CSV_DUMP_NO_INPUT` 且过滤器生效 | `check.cpp`：改为 `EmptyDirectoryAndUnmatchedFilterAreNoInput`，断言 NO_INPUT 且不写文件，并显式断言整串通配、大小写敏感的接受/拒绝 | pass（10/10，导出符号齐全） | BUG-L05-002 |
| l05_csv_failure_signal | STALE_FIXTURE | `LargeWriteFailureWithLogger` 的 1000 条相同记录压缩比超过 K37=B（2026-11-04）解压预算 1000:1，写前即被跳过，观察不到“写入失败” | `probe.cpp`：`source()` 多条记录逐条变化（单条不变），断言不改 | pass（green 全部；KnownRed 子集按设计 exit 1） | BUG-L05-002 |
| i03_csv_status_20261103 | STALE_FIXTURE | CMake 无 `BASELINE_SOURCE` 即 FATAL_ERROR；改前 `WtDtHelper.cpp` 快照（SHA-256 `676a86c2…`）已不存在，git 历史与证据目录中均无该哈希；`FlushOrWriteFailure` 的 1000 条相同记录同样会触发 K37=B 预算 | `CMakeLists.txt`：`BASELINE_SOURCE` 可选，缺省时不构建 `csv_baseline` 与改前字节对比 `NormalMatchesPriorBytes`（not-applicable，提供基线时照旧构建）；`check.cpp`：原字节对比用例改为 `NormalAndLegacyWriteIdenticalBytes`（三种调用字节一致，新增 CSV 内容断言），`source()` 多条逐条变化 | pass（14/14，导出符号齐全；改前字节对比 not-applicable） | BUG-L05-002 |
| l04_contract_import | STALE_FIXTURE | `ValidStorageWidthIdentityPersistsAndReloads` 的合约只有 `rules.session`、无 pricetick/volscale 且未加载会话，违反当前商品就绪合同（i03_commodity_ready），`loadContracts` 整文件拒绝 | `roundtrip.cpp`：先 `loadSessions`（ALLDAY），rules 补 `pricetick:1, volscale:1`（与 `l03_contract.storage_width_*` 同数据），其余断言不变 | pass（normal 与 `L04_SANITIZE=1` 均通过） | BUG-L04-004 |

条目 ID 来源：r23、csv_block_type、l05 见 `执行跟踪.json`；l04 见 `执行跟踪.json` 与夹具 README；其余取自各夹具 README 引用（csv_status 按其内容归入 BUG-L05-002 的 CSV 导出状态范围）。

## 与主代理指示的对应

- r23_retry_wait：确如所料，是本段 I03-NEW-039 改了写端清理路径的格式化写法，夹具的文本锚点失配；写端接口本身未变。
- i03_dt_writer_lifecycle：按“设计如此”在夹具中如实标注，只对该一例关闭线程泄漏报告，其余 TSan 报告不受影响；结果行带标注，可追溯。
- I03-NEW-118（长路径下 `fmtutil::format` 512 字节截断导致 terminate）在 writer_lifecycle 中仍会出现，属已登记产品问题；夹具未改动该行为，运行器继续把构建与运行放在短路径。

## 夹具运行器改动

`SCRATCH/fixtures/run_fixtures.py` 中 `i03_csv_status_20261103` 的处理函数原为无条件 `raise Blocked`；改为与 `i03_csv_block_type_20261103` 同样的方式：镜像中替换静态库路径、CMake 配置构建（LLVM）、运行 `csv_status`（ASan/UBSan 环境）、检查导出符号，并注明改前字节对比 not-applicable。只改该函数；本段改动的 diff 在 `logs/run_fixtures.py.slice2-result.diff`（同期其他代理对该文件的改动不在其中）。改前副本 `SCRATCH/fixtures/run_fixtures.py.bak-slice2-result-*`。

## 重跑

- 树：`rsync -a --no-links` 工作区 `src`、`dist` 到 `SCRATCH/slice2-result-fxtree/`；与 F3（`SCRATCH/final3/tree`）相比，`fixtures/` 之外无差异，`fixtures/` 内为本段 7 个套件与其他代理的套件改动。
- 命令：`/usr/bin/python3 -I SCRATCH/fixtures/run_fixtures.py --tree SCRATCH/slice2-result-fxtree --build SCRATCH/final3/build --out SCRATCH/slice2-result-fxrun --repo-git <repo>/.git --head ccaad010e9169f610aded4d8dc0f5647f70a5cff --annotations SCRATCH/fixtures/empty-annotations.json --only <7 个套件> --parallel 2 --jobs 2`，exit 0。
- 结果：`logs/fxrun-summary.json`、`logs/fxrun-console.txt`、`logs/<suite>-result.json`；完整构建与运行日志在 `SCRATCH/slice2-result-fxrun/<suite>/logs/`。
- 各夹具 README 的“最终验收复核”段落在运行之后追加（只改 .md）；运行所用的其余夹具文件与仓库现内容逐一比对一致。
- 改前/改后副本：`before/`、`after/`；哈希 `files.sha256`。

## 限制

未修改产品代码和迭代跟踪文档；没有创建符号链接，也没有把产品源码复制进夹具目录。各套件的 baseline/改前红控，凡依赖已不存在的改前源码的，均为 not-applicable（parser_host、writer_lifecycle 的 baseline 模式，以及 csv_status 的改前字节对比）。r23 的 300 秒墙钟长测、Windows、真实存储故障均未运行。
