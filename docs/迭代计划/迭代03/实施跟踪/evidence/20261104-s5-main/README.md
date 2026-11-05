# 2026-11-04 第五会话主线程复核记录

macOS arm64，HEAD `ccaad010`，工作区含大量未提交迭代改动。会话起点快照见 `../20261104-s5-start/`（status、diff sha256、src 修改/未跟踪文件 sha256）。本目录只记录主线程定向运行，**不是全量回归**，不改变 213 分母。

## 1. 默认 -Werror 覆盖率全目标增量构建

- 构建目录 `/tmp/wt-i03-main-cov-werror-20261104`（Debug、WT_ENABLE_COVERAGE=ON、WT_WARNINGS_AS_ERRORS=ON、Ninja、Boost_INCLUDE_DIR=/opt/homebrew/opt/boost/include）。
- `ninja -k 0 -j 6`：778 步，exit 0（12:51 开始，此时 12 个子任务尚未启动）。说明 I03-NEW-013/017 已修，且上次会话未登记的 WP1 切片5改动与 WtRtRunner 恢复命令可链接。

## 2. 上次会话未登记的 WP1 切片5复核（夹具）

- 切片5（09:06–09:36 修改，未登记）：`AccountTradingGate.h`、`ExecOwnerModes.h`、TraderAdapter `cancelVerified`/owner 路由/旧 ABI 非共享模式/`resumeAccountTrading`、`WtExecuterFactory` 宿主身份注入、6 个执行单元 `cancel_timeout_verified`、WtPorter `resume_account_trading`。
- 主线程新建非覆盖率 Debug 库 `/tmp/wt-i03-s5-lib`（WtCore WtDtCore WTSTools WTSUtils，exit 0），运行 `src/TestUnits/fixtures/i03_wp1_identity_gate_20261104/run.sh`：
  - current-asan（ASan+UBSan）26 例：25 过、1 跳过（ChildScenario 为子进程辅助），exit 0；
  - current-tsan 26 例：25 过、1 跳过，exit 0；
  - baseline2-asan（2026-11-04 补充裁定前的已接受状态）20 例：2 失败（`PausedAccountCancelsOnlyVerifiedIdentities`、`StalePositionSnapshotRowDoesNotOverwriteNewerTrade`），exit 1，为暂停期可核验撤单与持仓快照版本的红控。
- 结果与源码/二进制哈希见 `wp1-fixture/`。owner 路由等 6 例只在当前版本编译，无改前运行红控。
- 该包随后交由 wp1-ledger 子任务继续核对与补缺，本条不改变条目状态。

## 3. WP1 相关既有用例逐例隔离复跑

- 二进制：第 1 节构建的 `CoverageTests_wtcore`（`wp1-related/binary.sha256`）；清单 `wp1-related/tests.txt`（414 例，取自上次子任务）；脚本 `run1.sh`（每例独立 cwd、120 秒硬限、6 并发）。
- 结果 `wp1-related/per.txt`：373 过、41 失败，详见新增问题 I03-NEW-029。

## 4. I03-NEW-025 测试字面量修正

见 `sign-compare/`：改前副本、前后 sha256、以 `-I/opt/homebrew/include` 替换后的真实编译命令 `-fsyntax-only` 日志。改前 3 个 TU exit 1，改后 exit 0。运行结果待集成复跑。
