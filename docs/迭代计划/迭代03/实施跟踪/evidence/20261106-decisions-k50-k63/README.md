# 用户逐项确认的裁定 K50–K63 实施证据（2026-11-06）

用户 2026-11-06 逐项确认了 K50–K53，以及原先按卡15 记“用户指示忽略”的 10 个阻塞条目的合同（K54–K63）。裁定原文见 `../../决策积压清单.md` 末节。本目录记录各项的实现、红绿对照和最终冻结树 G7 上的回归结果。

范围说明：
- 只在本机 macOS arm64 执行。
- Windows/MSVC、真实厂商 SDK/柜台、真实数据和生产环境仍未验证，不计通过。
- 迭代02 的未跟踪证据、Linux 容器验证按用户决定不在本次范围。

## 1. 冻结与构建

- 开发构建：`scratchpad/g6/build`，直接指向工作区源码，用于先红后绿。基线全量构建 0 失败、0 警告（Apple clang 17，Debug+coverage，-Werror）。
- 最终冻结 **G7**（全量）与 **G8**（定向，见第 4.2 节）：
  - 时间 2026-11-06T01:38:48+0800，HEAD `55cbf3325`，src+dist 共 6732 个文件。
  - 清单摘要 `07e2d89c8f8462d99b67d776d1e3353f5d4b6a8ce247bec19c928dfa314d8eb4`。
  - 用 `rsync -a --no-links` 复制，复制后与工作区 `diff -rq` 为 0 行，src/dist 中无符号链接。
  - 构建与回归只用冻结副本：`../20261105-final-acceptance/tools/final_pipeline.sh <g7> freeze build run disabled asanbuild asanrun tsanbuild tsanrun`。
- 回归结果见第 4 节（由 `g7-results.json` 汇总）。

## 2. 各裁定的实现与红绿对照

红态记录在 `dev-red/`，绿态记录在 `dev-green/`。两者都在开发构建上，以同一组测试源码运行：红态是改产品代码之前，绿态是改之后。汇总见 `dev-red-green-summary.json`。

| 裁定 | 条目 | 产品改动 | 用例 | 红 | 绿 |
|---|---|---|---|---|---|
| K50 | I03-NEW-089 | `AccountTradingGate::releaseContractClaim`：审计 `claim_released` 落盘后才释放；拒绝时记 `claim_release_rejected`。`TraderAdapter`/`TraderAdapterMgr::releaseManualClaim`：该合约有未终结的人工委托时拒绝。WtPorter C 接口 `release_manual_claim`。WtRunner 请求目录 `claim_release/`（`ManualClaimInbox.hpp`，配置项 `claim_release_folder`） | `I03K50ManualClaimRelease.*`（3）、`I03K50AccountGate.*`、`I03K50ManualClaimInbox.*`、`I03K50PorterReleaseClaim.*`、`I03K50WtRunnerClaimInbox.*` | 新功能：改前没有释放命令，manual 归属终身保留（K44 用例 `ManualEntryUnderRoutingClaimsTheContractExclusively` 证实执行器被拒）；另见第 3 节的变异对照 | 全部通过 |
| K51 | BUG-R18-011 子项 | `ParserShm`：读一格之前和之后都核对写端位置；落后时跳到最早未覆盖的位置，打 WARN 并累计丢弃数；只改读端，不改共享内存布局 | `I03K51ShmSlowConsumer.LappedConsumerDropsOldestWarnsAndDeliversContiguousTail`；跨进程对照 `I03S2ShmCrossProcess.*` | 改前读端交付乱序和重复的数据，且不打告警 | 通过；交付一段连续尾部，WARN 中写明丢弃条数 |
| K53 | I03-NEW-121 附带 | 产品不改（现状即合同） | 夹具 `ctpmini_api_init` 新增模式 `retry-after-missing-library`、`retry-after-null-config` | 变异副本（任何失败都锁死实例）下两个新模式以 53 失败，见 `k53/k53-red/` | 当前源码 12/12 通过，见 `k53/k53-green/` |
| K54 | BUG-R211-001 | `WtEngine::riskmon_config_valid()`（非虚，不改 `init` 的虚表）；active 风控加载失败时为 false，WtRunner 与 WtPorter 的 `initEngine` 据此失败 | `I03K54RiskmonFixture.*`（4）、`I03K54WtRunnerRiskmon.*`、`I03K54PorterRiskmon.*`；有效模块正控 `R110EngineRiskMon.*` | WtRunner/porter 层改前 config 成功（运行期失败）；引擎层新接口改前不存在（编译期） | 全部通过 |
| K55 | BUG-R60-006 | `ITraderApi::isConnected` 注释写明合同。TraderMocker 在登录结果送达时才就绪，logout/disconnect/release 后清零。XTP、YD、ATP、HuaX、HTS、DD、XTPXAlgo 的 logout 清除就绪状态 | `I03K55MockerIsConnected.*`（3）；`R214…DisconnectJoinsMatchThreadWithoutWorker`、`T6009…ConcurrentExternalReleaseAndRequests` 按新合同调整；XTP `login_success_then_recovery_and_logout`、YD `k55_logout_ends_readiness` | Mocker 3 例失败 | 通过。ATP/HuaX/HTS/DD/XTPXAlgo 没有厂商头文件，本机未编译（外部剩余） |
| K56 | BUG-R6-006 | `WtRtRunner::initDataMgr`：缺 `data` 段时报错并返回失败；`data: {}` 不配 store 仍合法 | `I03K56PorterMissingData.*`；缺 env 段的对照 `R46PorterConfig.MissingEnvMakesConfigFail` | 改前 config 成功（并暴露 I03-NEW-142 崩溃） | 通过 |
| K57 | BUG-R134-004 | 实盘 `feed_raw_ticks` 打 ERROR 写明“实盘不支持”和丢弃条数；头文件写明不支持 | `I03K57LiveTickLoader.*` | 报错文本不符合合同 | 通过 |
| K58 | BUG-R7-004 | TWAP/VWAP 清仓分支：净零锁仓先平可用多头，否则平可用空头；两边都不可用时记诊断，保留目标等待后续 tick。VWAP 清仓分支的单带平仓标志 | `r7_twap_clear_k58.*`（5）、`r7_vwap_clear_k58.*`（2）；`r7_vwap.clear_target_flattens_net_zero_locked_position` 增加平仓标志断言 | 5 例失败 | 11/11 通过 |
| K59 | BUG-I02-NEW-026 | `StateMonitor`：只有跨午夜的夜盘延续段看前一天是否交易日，其余时刻看当天（初始化日志与状态推进两处一致） | `I03K59WeekendTest.*`（4，固定日期、时间驱动） | **产品偏差**：夜盘品种周六 10:00、周六 22:00、周五节假日的 22:00 都被判为非假日 | 4/4 通过 |
| K60 | BUG-I02-NEW-059、I03-NEW-042 | 历史委托队列/委托明细/成交的右端点由 `lower_bound` 改为 `upper_bound`（闭区间） | `I03K60HftRangeEndpointTest.*`（3：历史、实时、全窗口对照）；r162/r74 相关断言按 K60 恢复 | 历史与实时不一致，2 例失败 | 3/3 通过 |
| K61 | BUG-I02-NEW-060 | 6 个 K 线读入口（WtDataReader、WtRdmDtReader ×2、AD ×3）拒绝 m1/m5/day 以外的周期，打 ERROR | `I03K61KlinePeriodTest.*`（4）、`I03K61AdKlinePeriodTest.*`（2）；`R193LoaderUnknownPeriodIsRejected`（由旧的“回退为日线”断言改写） | 4 例失败 | 通过 |
| K62 | BUG-I02-NEW-090 | 产品不改 | 新增 `R200His_BarReplayWithOnlyEmptyTickListDispatchesNothing` | — | 通过。行覆盖 `k62/HisDataReplayer.cov.txt`：`enqueue` 执行 3 次、游标初始化 2 次、循环体 0 次，空列表路径得到确证 |
| K63 | BUG-I02-NEW-137 | 产品不改；`CLAUDE.md` 记录例外 | `UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed` 以 `WT_ALLOW_SYMLINK_TESTS=1` 单独运行 | 不设变量时按策略跳过（`dev-green/k63-skip`） | 通过（`dev-green/k63-run`）。符号链接只建在系统临时目录的独立子目录里，用后删除；运行后临时目录无遗留链接，仓库 src/dist 中无符号链接 |
| — | I03-NEW-142（新发现） | CTA/HFT/SEL 引擎 `run()` 在 `env.product` 缺失时拒绝启动（原为空指针崩溃） | `I03New142Fixture.*`（6，含 3 个正控） | 缺 product 的 3 例 SIGSEGV | 6/6 通过 |

## 3. 变异对照与说明

- **K53**：在 `ParserCTPMini.cpp` 副本中，让“空配置”和“动态库打不开”两种失败都锁死实例（`k53/k53-red/ParserCTPMini.mutated.cpp`），以 `WT_K53_FORCE=1` 运行夹具。两个新模式以 53 失败。其余模式与夹具的基线预期不符，是因为夹具传入替代源码时套用的是另一组基线预期，与本项无关。
- **K50**：在 G7 树的独立副本（`scratchpad/k50mut`）上做了两种变异，都只改副本、改后用 `cmp` 确认已恢复（`k50-mutations/`）：
  - M1：忽略在途人工委托。只有 `LiveManualOrderBlocksTheRelease` 失败。第一次尝试因 -Werror 报变量未使用而编译失败，加 `(void)` 后重跑。
  - M2：先释放、忽略审计。`ReleaseWithoutDurableAuditKeepsTheClaim` 与 `I03K50AccountGate.*` 失败。
- 开发过程中自查到两处问题，均已在冻结前修正：
  - 我的 Python 编辑把若干 CRLF 文件改成了 LF。已按 HEAD 逐行恢复原换行符，保证每个文件的 diff 只有实际改动的行。
  - porter 报错文本中的字面量 `{}` 被 fmt 当成占位符，已改为转义。

## 4. 冻结树回归结果

### 4.1 G7 全量

G7 冻结树 `07e2d89c…`。构建与运行都只用冻结副本，逐例独立进程、独立 cwd，单例超时 600 s。用例集合与模块预置同 F4（`../20261105-final-acceptance/tools/run_full.sh`）。

| 运行 | 结果 | 说明 |
|---|---|---|
| 普通构建 | configure=0、build=0、0 警告 | Apple clang 17，-Werror |
| 普通逐例 | 6394 例：pass 6383、fail 7、skipped 4 | 7 例都是断言旧合同的既有用例，见下 |
| 禁用用例诊断 | 163 例，分布同 F4 | 不计通过 |
| ASan+UBSan | 6394 例：pass 6383、fail 7、skipped 4 | 失败的就是同样 7 例，0 个 sanitizer 报告 |
| TSan | 6394 例：pass 6225、退出期 signal-6 100、sanitizer 54、fail 10、timeout 1、skipped 4 | **产品数据竞争 0**，分类见 `g7/tsan-triage.txt`；详见下方 |

TSan 结果的构成：
- 退出期 SEGV/BUS 100 例：atexit/dlclose 工具限制，同 F4。
- 线程泄漏 53 例：内嵌的 boost threadpool，同 F4。
- 1 例 `std::cerr` 的 `ios_base::width`：即 I03-NEW-140，标准流，不构成数据竞争。
- fail 10 例：上述 7 例旧合同断言，加上 F4 已证实的 r85×2、r14 两类工具限制（3 例）。
- timeout 1 例：r95，同 F4。

普通与 ASan 下失败的 7 例，原因都是断言了被新裁定改变的旧行为：
- K60（右端点不包含）：`R100RdmHisHftExclusiveEndBoundaries`、`R70RdmHisHftFilesBuildAndReject`，以及 `R74RdmHftHotTest` 的 3 例。
- K56（缺 data 照常启动）：`R82RunnerEdges.ConfigFailureArmsAcrossSections`。
- K61：`R133WtDataManagerTest.realtime_day_and_second_buckets_are_cached_and_reported`。由此发现 I03-NEW-143：dtservo 的秒线订阅以 `KP_Tick` 向读端取种子，K61 之后订阅建不起来。修正为空缓存起步（产品），并把该用例按地址比较的检查改成按时间线判断（测试）。

处理方式：
- 前 6 例按 K60/K56 改断言；r100 改名为 `R100RdmHisHftClosedEndBoundaries`；r162 中 ccaad010 跟随旧语义的 `stime + 1` 恢复为起止相等。
- 第 7 例按上面的方式修正。
- 这些改动做成冻结 G8。

### 4.2 G8 定向

G8 冻结树 `049a3d9f3086d20915c3dedb77988533e6f062dbcabba9ab9b4beb4672e80551`，HEAD 同 G7，与 G7 只差 7 个文件（`g8/diff-vs-G7.txt`）：
- `WtDtServo/WtDataManager.cpp`
- r100、r70、r74、r82、r133、r162 共 6 个测试文件

| 运行 | 范围 | 结果 |
|---|---|---|
| 普通全量构建 | 全部 | 0 警告 |
| 普通逐例 | 编入改动文件的 CoverageTests、CoverageTests_porter、CoverageTests_dtservo，共 1790 例 | pass 1789、skipped 1（`R22PorterCapiTest.ConfiguredStartChildChecksStateMonitor`，同 F4），0 失败 |
| ASan+UBSan 全量构建 + 逐例 | 同上 1790 例 | pass 1789、skipped 1，0 报告 |
| TSan | CoverageTests_dtservo 129 例 | pass 127；2 例 PASSED 后退出期 DEADLYSIGNAL（`Q05ServoWtDataManagerTest.InitStoreFactoryReturningNullReturnsFalse`、`R45ParserAdapterFakeTest.ReleaseRetainsModuleContextWhileV2DrainIsPending`，与 F4/G7 相同的工具限制），0 数据竞争 |
| TSan 补跑 | r100/r70/r74/r162 共 11 例，r82 1 例 | 全部通过，0 个 TSan 警告（`g8/tsan-extra/`） |
| K63 | 设 `WT_ALLOW_SYMLINK_TESTS=1` 运行 `UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed` | 通过，临时目录无遗留链接（`g8/k63/`） |
| K53 夹具 | `ctpmini_api_init`，对当前源码 | 12/12 模式 exit 0（`g8/k53/`） |

### 4.3 合并口径

最终结论的口径：
- CoverageTests、CoverageTests_porter、CoverageTests_dtservo 三个二进制以 G8 为准，其余二进制以 G7 为准。
- 合并后，普通与 ASan 两轮都没有非通过用例（跳过的 4 例同 F4）。
- 各裁定相关用例在三轮中都通过，计数见 `decision-tallies.json`，由 `g7_update_tracking.py` 生成，生成时若有非通过即报错退出。
- TSan 无产品数据竞争。

## 5. 文档写入

由 `g7_update_docs.py` 写入 `执行跟踪.json` 与 `逐项验收.json`：
- 10 个原阻塞条目改为新结论（已验收 7、本地验证通过 3），汇总改为已验收 148、本地验证通过 65、阻塞 0、待验证 0。
- BUG-R18-011 的备注追加 K51 的实施情况。

手工同步的文档：
- `新增问题.md`：I03-NEW-089、121、042 更新状态，新登记 142、143。
- `决策积压清单.md`、`遗留问题清单.md`、`最终验收报告.md`、`迭代计划.md`、`执行记录.md`。

修改前的跟踪文件副本保存在 scratch（`执行跟踪.before-g7.json`）。
