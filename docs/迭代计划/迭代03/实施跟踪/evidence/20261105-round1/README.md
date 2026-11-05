# 迭代03 第一轮集中回归（2026-11-05）

本轮按 `迭代计划.md` 阶段 2 执行：冻结源码与构建输入，干净构建，逐例运行全部 TestUnits/CoverageTests 用例。本轮只记录问题，没有修改冻结快照。这**不是**整体回归验收：复修和最终整体回归尚未进行，`执行跟踪.json` 的 `全量回归` 仍为 NOT_RUN。

## 冻结
- 时间：2026-11-05T11:37:34+0800（`meta/frozen_at.txt`）。HEAD `ccaad010e9169f610aded4d8dc0f5647f70a5cff`（`meta/HEAD.txt`）。工作区未提交修改见 `meta/git-status.txt`，已跟踪文件的二进制 diff 见 `meta/tracked.diff.gz`。
- 快照：`src/` 与 `dist/` 用 `rsync -a --no-links` 复制到隔离目录。共 6649 个文件，无符号链接。逐文件 SHA-256 见 `meta/tree.sha256`，清单摘要 `d442e3e6218006343ca85577df7ec6c6f8ba6381d2d7ff95311bddc1ce2ff59a`（`meta/tree.sha256.digest`）。复制完成后与工作区逐文件比较，`diff -rq` 无差异。
- 构建与运行只使用该隔离副本，冻结后工作区的任何修改都不进入本轮。
- 回归工具及外部平台清单的副本和哈希见 `meta/tools.sha256`。

## 干净构建
- 全新目录：`cmake -S <快照>/src -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`；Apple clang 17，`WT_WARNINGS_AS_ERRORS=ON`（缺省）。
- configure=0，build=0（`meta/build-status.txt`），产出 45 个测试二进制。
- 编译告警 2 处，各出现两次，均为产品 `WtSelStraFact/WtStraDtSel.cpp` 的 `-Wunused-variable`（`curPx`、`commInfo`）。该模块处于放宽集合内，不中断构建（`meta/build-warnings.txt`），既有问题，登记 I03-NEW-107。

## 全量逐例运行（`run/`）
- 每例独立进程、独立 cwd，8 路并发，单例超时 600 s。参数与 int13 相同：m_WtRunner 预置模块，排除 1 例需真实工厂的用例。
- 时间：11:45:35 至 11:50:53，运行器退出码 0。
- **结果：共 6116 例，通过 6099，失败 0，信号 0，超时 0，跳过 17，禁用 224（未执行，不计通过），列举失败 0。**
- 外部平台跳过（2026-11-05 用户指示）：9 项，即 8 条外部条目加 P04-011 的真实 provider 部分，在 `summary.json` 的 `external_platform_skipped` 中单列，不计通过。

### 17 例跳过归因（`meta/skipped-reasons.json`；int13 中是同样 17 例，属长期存在）

| 类别 | 用例 | 处理 |
|---|---|---|
| 子进程用例（只由父用例拉起） | `R22PorterCapiTest.ConfiguredStartChildChecksStateMonitor`、`R19ExecChild.protocol` | 设计如此 |
| 运行时变量未提供 | `R66BtPorterNotifier.*` 1 例、`S5R20RunnerNotifierRelease.*` 3 例（需 `WT_R66_FAKE_MQ_DIR`），`R42HftTraderBinding.*` 2 例（需 `R42_HFT_PLUGIN`） | 见下文补充运行；回归工具未提供这些变量，登记 I03-NEW-106 |
| 跳过原因不实 | `R143PorterBars.*` 3 例：提示“libWtDataStorage.dylib not found”，模块实际存在。lldb 证实是 `WtRtRunner::config()` 返回 false：夹具基础文件缺 holiday，按卡1 被拒（`probes/`） | I03-NEW-104 |
| 写死路径 | `R15MgrTest.*` 3 例写死 `/Users/…/wondertrader/build-cov/…/libWtIndexFactory.dylib`；`P04ReplayCsvTest.failedBinaryRawBarsLoadCanBeRetriedSafely` 只在仓库相对 `build-cov`/`build_x64` 下找模块 | I03-NEW-105，隔离构建中从未运行 |
| 合同未定 | `R44ParserAdapterEdgesTest.RepeatedInitExtNeedsContract`（重复 initExt 语义未定义） | 按现状保留跳过 |
| 无符号链接约束 | `UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed` | I02-NEW-137，卡15 用户指示忽略 |

### 补充运行：同一冻结构建，只提供运行时变量（`supplement-env/`）
- 设置 `WT_R66_FAKE_MQ_DIR=<构建>/TestUnits/coverage/r160_fake_mq` 和 `R42_HFT_PLUGIN=<构建>/bin/libWtHftStraFact.dylib`，运行上述 6 例：**5 通过、1 失败**。
- 失败的是 `R42HftTraderBinding.KnownTraderStillPublishesBoundContext`：`WtRunner::initHftStrategies()` 返回 false。lldb 断在 `WtRunner.cpp:1180`（`probes/r42-init-return-false.lldb.log`），即 `SimpleHft::init` 拒绝参数。原因是本迭代 BUG-WSN06-002 给 SimpleHft 加了参数 schema 校验，必填项为 `code`，而该用例传入空 params。用例一直因缺变量被跳过，过期未被发现。登记 I03-NEW-103，在复修阶段修改用例参数。

### 禁用用例
- 共 224 例（`DISABLED_` 前缀），均未执行，不计通过。各二进制的名单见 `run/manifest.json` 的 `disabled_names`；较多的二进制为 btcore 30、CoverageTests 23、dtcore 18、adstorage 16、wtcore 15、porter 13、exefact 12、uftcore 11。

## 本轮未运行
- 独立夹具套件：`src/TestUnits/fixtures/` 下有 112 个目录，其中 63 个带 run.sh/run.py，53 个在 README 中写明了运行命令（`meta/fixture-suites.json`）。它们各自编译测试，接口各异：需要指定构建目录、prepare 快照、conda python 或旧的 /tmp 构建，部分包含刻意的红控基线。本轮没有统一运行，留到复修阶段按各 README 逐个对冻结树运行。
- sanitizer、资源与并发验收属于整体回归阶段，本轮未做。上一段已用 LLVM 22 ASan+UBSan 跑过 btcore/btporter 逐例（见 `../20261105-s5-main-r20-zstd/`），但不是本冻结快照。
- Windows/Linux MSan/真实厂商与生产环境：外部平台项，平台跳过。

## 第一轮问题清单（待复修）
- I03-NEW-103：R42 用例参数与 WSN06-002 的 schema 不符，补充运行时失败。
- I03-NEW-104：R143PorterBars 夹具缺日历，`config()` 失败，被误报为模块缺失而跳过。
- I03-NEW-105：R15MgrTest、P04ReplayCsvTest 写死或使用仓库相对的模块路径，隔离构建中不运行。
- I03-NEW-106：回归工具没有提供 `WT_R66_FAKE_MQ_DIR`/`R42_HFT_PLUGIN`，6 例在全量运行中从未执行。
- I03-NEW-107：`WtStraDtSel.cpp` 两处未使用变量告警（放宽集合内）。
