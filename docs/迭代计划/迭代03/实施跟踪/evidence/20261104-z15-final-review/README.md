# 卡15 Z 类 33 条最终复查（2026-11-04）

macOS arm64，HEAD `ccaad010`（工作区含大量未提交迭代改动）。本目录只记录本次复查的定向证据，**不是全量回归**，不改变 213 分母，不代表 Windows/MSVC、真实供应商或最终整合树。未修改任何跟踪文件。

## 快照与构建

- 08:5x 以 `rsync -a --no-links` 将工作区 `src/` 与 `dist/common/` 复制到 `/tmp/wt-i03-zreview-20261104/snap`（实体复制，`find -type l` 为 0）。快照含本次新增/修改的测试，产品文件为改前版本；哈希见 `hashes/snapshot-baseline-sha256.txt`。
- 构建脚本见 `scripts/`。默认配置（`-Werror` 开）构建目录 `/tmp/wt-i03-zreview-20261104/build`：porter、btporter、t_ParserShm exit 0；btcore、wtcore exit 1（见下「新发现 C」）。
- 本地诊断变通：`/tmp/wt-i03-zreview-20261104-btcore/build` 仅追加 `-Wno-error=sign-compare`（其余 `-Werror` 保持），btcore/wtcore/dtcore exit 0。不得提交该变通。
- NEW-030 全插桩：Apple clang 17 ASan 构建 exit 0，但二进制在 `main` 前挂起（`--gtest_list_tests` 20 s 被 SIGALRM，exit 142，见 `runs/new030_appleasan_*`）。改用 Homebrew LLVM 22（`-fsanitize=address -fno-omit-frame-pointer`，另加 `-include stdlib.h` 绕过 Homebrew fmt 在 LLVM 22 libc++ 下的 `malloc` 未声明，`WT_WARNINGS_AS_ERRORS=OFF`，均为本地诊断变通），目标 248 条编译命令全部带 `-fsanitize=address`（`logs/asanllvm-compile-count.txt`：248 / 0）。Homebrew 预编依赖（gtest/boost）与系统 libc++ 未插桩。
- 每例在 `/tmp/wt-i03-zreview-20261104/run/<label>` 独立 cwd 运行，`perl alarm` 硬限时，XML 是否存在以 `runs/*/result.xml` 为准（`scripts/runcase.sh`）。二进制哈希见 `hashes/binaries-sha256.txt`（为绿控后的二进制）。

## 产品修改与红绿

| 条目 | 文件 | 改前 sha256 | 改后 sha256 | 红控 | 绿控 |
|---|---|---|---|---|---|
| BUG-R211-001（子项：空监控器崩溃） | `src/WtCore/WtEngine.cpp`（仅 `init_riskmon`，`patches/WtEngine.patch`） | `a3eeae45…862c` | `8550cc68…9385` | `runs/r211_red`：新用例子进程 SIGSEGV，1/2 失败，exit 1 | `runs/r211_green` 2/2 exit 0；相关 `runs/r211_related`（R110 成功链、R41 两个风控分支）3/3 exit 0 |
| BUG-I02-NEW-093 | `src/WtBtCore/MatchEngine.cpp`（仅 `handle_tick` 无 sink 守卫，`patches/MatchEngine.patch`） | `89767481…b43f` | `6cb6c95d…18ff` | `runs/new093_red_v2`：两例子进程 SIGSEGV，2/3 失败，exit 1 | `runs/new093_green_v2` 3/3 exit 0；相关 `runs/new093_related_v2`（MatchEngineTest/R43/R20）55 例 0 失败、7 既有 DISABLED，exit 0 |

测试文件：`q01_wtcore3/r211_wtcore_rest_test.cpp`（新增 `R211Engine.UnknownRiskMonNameFromShippedFactoryIsRejectedWithoutCrash`，`2e9c625b…fcf` → `f1c901d4…52b4`）、新增 `l06_wtbtcore/z15_match_engine_unsinked_test.cpp`（`4d56bb1e…95fd`）、新增 `q06_btporter/z15_btrunner_config_lock_test.cpp`（`8ae2c327…e552`，仅补证，无产品修改）。

NEW-093 首版修复曾让 buy/sell 在无 sink 时拒单（`runs/new093_red|green|related`，已作废）：既有 `R20ExecMocker.CancelByIdDoesNotRestoreUndoneTwice` 依赖未初始化 ExecMocker 可下单而失败，说明“拒单”不是已定合同，故撤回该部分，只保留无争议的“不解引用 NULL、挂单保持到注册 sink 后的下一笔 tick”。未修改该既有断言。

## 只补证、不改产品的运行

- R9-001：`runs/r9_btporter` 3/3 exit 0（R205 成功后重复拒绝、preflight 可重试、新增工厂失败后锁定且新 runner 正控成功）。
- NEW-116：`runs/new116_shm` 3/3 exit 0。
- NEW-090：`runs/new090` 1/1 exit 0。
- NEW-030：`runs/new030_llvmasan_target`（迭代02 中止用例）1/1 exit 0；`runs/new030_llvmasan_suites` 27 例全部执行、XML 存在、无任何 AddressSanitizer 报告，5 例为未完成量记账断言失败（1 例为迭代02已知 Q04-002，4 例为工作区新增、HEAD 不存在的用例）。迭代02的混合插桩构建为红控原始证据，本次未重造混合构建。
- 现状失败（未改断言）：NEW-096、NEW-101、NEW-102、NEW-109、NEW-111、NEW-123×2、R42 冲突 1 例、NEW-026（8/9 夹具 SetUp 失败）、NEW-130 同进程 SIGSEGV（`runs/new130_lldb/lldb.log`：`WtEngine::get_commodity_info` WtEngine.cpp:132 `_base_data_mgr` 为空）。
- NEW-104 泄漏：`runs/new104_leaks_*`，`/usr/bin/leaks --atExit`：越界区间 3 leaks/224 B（含 WTSSessionInfo），times=1 10 leaks/832 B（含 WTSKlineSlice），成功重采样正控 0 leaks。

## 新发现（编号由主线程分配；若已占用顺延）

- A：TraderCTP `disconnect()` 无终帧时无界 join（T25-005 产品停止缺陷）。
- B：WtDtHelper `resample_bars` 三个失败返回路径泄漏 `WTSSessionInfo`/`WTSKlineSlice`。
- C：默认 `-Werror` 覆盖率构建中 gtest 模板 `-Wsign-compare` 报错（`p03…/r131_replayer_task_notifier_test.cpp:116`、`r141_replayer_block_dsb_test.cpp:107`、`n01…/r71_cta_ctx_test.cpp:353`，gtest 头经 `-I/opt/homebrew/include` 以非系统头引入），btcore/wtcore 默认构建 exit 1。
- D：R42-002 HFT 创建门禁后，`R124PorterExpHft.SubscribedTickAndTradeAreForwarded` 与 `ExecUnitStatusApiV2Fixture.StartupReentryScopeIsBoundToRunnerInstance` 以不存在的 trader 创建 HFT context 失败（测试前提过时）。
- E：initExt 外部 parser 在 runner 释放时按卡9隔离、不再收到 `EVENT_PARSER_RELEASE`，`ExecUnitStatusApiV2Fixture.ParserRelease*` 两例前提失效；需定义 initExt 停止通知合同。
- F：r145 夹具商品缺价格跳动/乘数，被修改后的 `WTSBaseDataMgr::loadCommodities` 拒绝，8/9 例在 SetUp 失败。
