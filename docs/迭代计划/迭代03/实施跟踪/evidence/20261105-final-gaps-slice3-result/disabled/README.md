# 第 3 段 · DISABLED_ 用例逐例处置（disabled/）

- 任务：对 `SCRATCH/disabled-inventory.json` 的 224 个 `DISABLED_` 用例逐例分类并如实处置；规则按 FIX-PROMPT.md。本小节不修改迭代跟踪文档（执行跟踪.json、执行记录.md、新增问题.md、决策积压清单.md、迭代计划.md）。
- 日期：2026-11-05；仓库 HEAD 见 `HEAD.txt`（`ccaad010`，工作树含大量他人未提交改动）。
- 普通构建：`SCRATCH/slice3-result-build`（仓库源码，Debug，`WT_ENABLE_COVERAGE=ON`，`WT_WARNINGS_AS_ERRORS=ON`，Ninja，`-j 3`）。本小节所有 20 次运行都在这一构建上完成；没有使用 scratch 副本树，也没有构建 sanitizer 树。
- 诊断输入：协调方的 ASan+UBSan 预扫（`SCRATCH/pre/run-disabled-asan/results.jsonl`，日志 `SCRATCH/pre/run-disabled-asan/cwd/<二进制>/<用例>/log.txt`）。表中“诊断”列即该预扫状态，它不是普通构建结果。
- 分类依据：用例体与禁用注释；迭代02 `缺陷裁定总表.json`（PRODUCT/TEST/NOT_BUG/DUPLICATE/UNDECIDED）；迭代01 bugs 记录；213 项清单与 `执行跟踪.json` 状态；`新增问题.md`；决策卡（4a、9c、10、14c、15、K02、K27、4b 等）。

## 方法

1. 逐例阅读用例体、禁用注释、预扫日志首个报错点，按五类判定：ENABLE / KEEP_DEFECT / KEEP_EXTERNAL / KEEP_CONTRACT / KEEP_TOOL。
2. 判定原则：
   - 只有当前通过、断言（或无断言用例所隐含的“不崩溃”）与现行合同一致、不依赖真实厂商/网络/端口/长时运行，且普通构建中单例 20 次稳定，才 ENABLE。
   - 用例前置被迭代02 裁定为无效（TEST/NOT_BUG），或依赖未声明可空的必需依赖、未登记请求号、人为 NULL 回调参数等，即使现在已不崩溃，也归 KEEP_CONTRACT，不把未约定行为固化为回归。
   - 只有 `GTEST_SKIP()`、`EXPECT_TRUE(true)`、`SUCCEED()`、空函数体、恒失败占位（`FAIL()`/`EXPECT_FALSE(true)`）的用例，启用后不提供可证伪断言，归 KEEP_CONTRACT（按 skip 理由属外部环境的归 KEEP_EXTERNAL）。
   - 断言旧缺陷行为（“必须崩”“返回成功”）的用例归 KEEP_CONTRACT。
3. ENABLE 候选先预跑：`<bin> --gtest_also_run_disabled_tests --gtest_filter=<原名> --gtest_repeat=20`（pre）。用 `lock.py slice3-result DISABLED-disposition <文件>` 登记锁之后再去掉前缀。行尾保持 LF（45 个文件均为 LF），断言不改。失效的禁用注释改成“迭代03 最终验收复核后启用（原禁用：<编号/原因>）”。然后重编，以新名运行 `--gtest_repeat=20`（post）。
4. 依赖进程级单例的用例（WTSLogger 一次性 init/stop、porter 进程单例），同进程 repeat 本身不可行，同套件已启用用例也一样。这类用例改为 20 个独立进程各跑 1 次（preproc/postproc），并在表中注明。
5. 证据：`logs/<phase>-main-<序号>.log/.xml`；`runs.jsonl` 记录每次运行的命令、rc、OK/失败/跳过计数；`before.sha256`/`after.sha256`/`enable.patch` 记录源码改动。运行 cwd 在 `SCRATCH/s3dis/run/`，运行后已删除 profraw。

## 计数（224 例）

| 分类 | 数量 | 说明 |
|---|---:|---|
| ENABLE（已落地） | 57 | 已去掉 DISABLED_ 前缀；普通构建中新名用例 56 例同进程 `--gtest_repeat=20` 均为 20/20；例 2 用 20 个独立进程验证，20/20。例 195 在锁转交后于 followup/ 落地 |
| ENABLE（改写） | 1 | 例 173：I03-NEW-131 修复后改写为启用的合同用例（followup/，改前 SIGSEGV，改后 20/20） |
| KEEP_DEFECT | 4 | 例 17、144、147、148 |
| KEEP_EXTERNAL | 23 | 真实厂商库/API、真实端口/组播/UDP、真实 shm 段、长时运行或永久轮询 |
| KEEP_CONTRACT | 139 | 前置无效（迭代02 TEST/NOT_BUG）、合同未定、断言旧缺陷行为、占位/无可证伪断言、设计性子进程探针；例 160 在 I03-NEW-130 修复后转入此类（由合法前置新用例取代） |
| KEEP_TOOL | 0 | 唯一的打包布局 alignment 报告（例 22，I03-NEW-109 类）同时需要 sleep 300 s，按 KEEP_EXTERNAL 处理 |

- 本表初版（followup 前）为 ENABLE 56 / 未落地 1 / KEEP_DEFECT 6 / KEEP_CONTRACT 138；followup 处置了例 160、173、195，详见 [../followup/README.md](../followup/README.md)。

- KEEP_DEFECT 与 213 项的关系：
  - 例 144、147：BUG-Q06-002，已并入 213 项 BUG-R6-006（状态：阻塞）。
  - 例 17：I03-NEW-083（新增问题）。
  - 例 148：BUG-Q06-004/R46-001，不在 213 项中，也无 I03-NEW 登记（见“发现”）。
  - （followup 前另有例 160 = BUG-R105-004、例 173 = BUG-T25-003，已分别登记为 I03-NEW-130/131 并修复，见 ../followup/。）
- 普通构建复现 KEEP_DEFECT（pre，`--gtest_also_run_disabled_tests`）：
  - 例 17：ASSERT 失败 20/20。
  - 例 144、147、148：首轮即 SIGSEGV（rc=-11）。（例 160、173 修复前同为 SIGSEGV，修复后见 followup/。）

## 源码改动

- 45 个测试源文件，共 56 处 `TEST(...)` 名称去掉 `DISABLED_`（例 150、183、192、194、202、203 是去掉套件名前缀；套件仍保留其他禁用用例的，只改本例的套件名）。另外替换失效的禁用注释。断言、用例体和产品代码均未改动；`enable.patch` 中除 TEST 行和注释外没有其他差异。（本节的 patch/哈希只覆盖初版 56 例；例 195 去前缀、例 160 注释、例 173 改写及两处产品修复见 ../followup/ 的 `followup.patch`、`before.sha256`、`after.sha256`。）
- 锁：`lock.py slice3-result DISABLED-disposition <44 个文件>` 返回 ok；`m_WtRunner/r28_runner_main_test.cpp` 在第 2 阶段已由本段锁定（I03-NEW-077）。
- 哈希：`before.sha256`（改前快照）、`after.sha256`、`enable.patch`。
- 构建：`build/build3-post-edit.log` 与 `build/build4-porter-post-edit.log` 均为 `EXIT=0`（-Werror），改名没有造成重名或告警。

## 运行说明

- `runs.jsonl` 每行一次运行，含 phase（pre/preproc/post/postproc）、rc、OK/失败/跳过计数、日志和命令。同名日志被后一次运行覆盖时，以 `runs.jsonl` 中的多行记录为准。
- porter 四例（138、140、141、143）和 wtcore 五例在 15:12 的构建（build2）中做过第一次 pre。当时 porter 有三种表现：
  - 例 138：mutex EINVAL，20/20 失败。
  - 例 143：夹具 config=false，20/20 失败。
  - 例 140、141：第 2 轮 terminate。

  原因是 S3-NEW-2：产品镜像与测试 TU 编译时的头文件版本不一致。15:16–15:35 全量增量重编（build3）后重做 pre，四例均 20/20；去前缀并重编（build4）后，post 均 20/20。当时的栈见 `logs/s3new2-lldb-15h14.txt`，符号绑定关系见 `logs/s3new2-porter-symbol-binding.txt`。
- 例 2 的同进程 repeat 对照：`logs/post-main-002.log`（第 2 轮 SIGSEGV）。同套件已启用用例的对照：`logs/s3new1-r56-RootInfoFiltersDebug-repeat3.log`、`logs/s3new1-r56-suite-same-process.log`。
- m_WtRunner 两例按惯例把 bin 下的 dylib 复制到 cwd 后运行，运行后已删除。

## 未覆盖与需外部执行

- 本段没有构建 sanitizer 树。新启用的 57 例（含例 195）与 followup 的改写/新增用例需要在最终 ASan+UBSan(+LSan) 回归中逐例重跑。协调方预扫中这些用例的禁用名版本均为 pass，但当时的源码是预扫树快照，不等于当前树。
- 只做了 macOS arm64 本机 fake 验证；Windows/MSVC、真实厂商 SDK 和整体回归均未执行。
- KEEP_EXTERNAL 23 例需要真实环境，本段没有执行。

## 发现（主代理已分配编号：S3-NEW-1 → I03-NEW-126，S3-NEW-2 → I03-NEW-125，例 17 → I03-NEW-127；下文保留临时编号便于对照日志文件名）

- **S3-NEW-1 = I03-NEW-126（测试隔离，R56Logger）**：`R56Logger` 套件依赖进程级 WTSLogger 的一次性 init/stop。在普通构建中同进程运行 `R56Logger.*`，第 2 例（`InitReleasesConfigAndKeepsRetainedPatternAfterMissingRoot`）断言失败，第 3 例 SIGSEGV；已启用的 `RootInfoFiltersDebug` 用 `--gtest_repeat=3` 运行，第 2 轮即 SIGSEGV。只有逐例独立进程能通过。本例 2 与它们同型，因此用 20 个独立进程验证（`logs/preproc-main-002.log`、`logs/postproc-main-002.log`）。
- **S3-NEW-2 = I03-NEW-125（测试链接集，CoverageTests_porter）**：`CoverageTests_porter` 运行时装载 `bin/WtPorter/libWtPorter.dylib`、`libWtExecMon.dylib` 等产品镜像，WtCore 符号跨镜像绑定。lldb 栈显示 `WtRtRunner::createExtExecuter` → `WtCtaEngine::findExecuter` → **libWtExecMon.dylib** 中的 `WtExecuterMgr::find_executer`，并抛出 `std::system_error: mutex lock failed: Invalid argument`；`i03s5_fresh_configured_porter("sel")` 经 **libWtPorter.dylib** 中的 `WtSelEngine::init`/`WtEngine::init` 配置后返回 false。触发点：WtEngine.h（15:05）与 WtExecMgr.h（15:08）被他人改动，dylib 与测试 TU 编译时用的头文件版本不一致，跨镜像调用就落到布局不同的对象上。这与 I03-NEW-055（uftcore，第 2 阶段已隔离）同类。只要镜像与测试编译时的源码不同步，porter 用例的结果就不可信。
- **S3-NEW-2 的后果（已消失，仅作记录）**：在 15:12 的构建中，已启用的 `R13PorterSelExport` 6 例和本段第 2 阶段新增的 `S3R13SelPublicSchedule` 2 例逐例运行，均在夹具配置处失败（`i03s5_fresh_configured_porter("sel")` 返回 false）。15:35 全量增量重编后两者都通过（`R13PorterSelExport.sel_log_text_passes_all_levels_without_touching_state` 1/1，`S3R13SelPublicSchedule.*` 2/2，`Q06PorterWrapper.*` 5/5）。由此可见，只要 porter 产品镜像与测试 TU 不同步，就会出现这种假失败。建议参照 I03-NEW-055 隔离 CoverageTests_porter 的链接集，或在 porter 回归前强制重编其依赖镜像。
- **登记缺口（不是新问题，请主代理决定是否登记）**：
  - （已登记为 I03-NEW-131，修复见 ../followup/）BUG-T25-003（迭代02 UNDECIDED）：缓存过内容后，结算单 NULL 末帧仍会空指针。卡14c 要求在没有 SDK 契约时空响应 fail-closed，当前不满足。该项既不在 213 项中，也没有 I03-NEW 登记。
  - （已登记为 I03-NEW-130，修复见 ../followup/）BUG-R105-004（迭代02 PRODUCT，T4-010 已并入）：`ParserCTPMini::IsErrorRspInfo` 仍恒返回 false。不在 213 项中，也没有 I03-NEW 登记。
  - BUG-Q06-004 / BUG-R46-001（迭代02 PRODUCT）：`WtRtRunner::feedAdjFactors` 不检查 `_feeder_fcts`，在未注册 feeder 时调用会跳到空函数指针。不在 213 项中，也没有 I03-NEW 登记。
  - （已登记为 I03-NEW-127，按布局限制登记）I03-NEW-083 已改为 fail-closed（超长代码时 create 返回 NULL）。结果是 63 字节合法 stdCode 的历史 tick 读取返回 NULL，I03-NEW-039 的端到端目标仍未达成，需要裁定 >31 字节代码是否属于合法输入。
- **注释编号错配**：例 82/83 注释写的 BUG-R18-005/006，在 `迭代01/bugs/R18_wtdtcore.md` 中实际对应 BUG-R18-008/009。其中 BUG-R18-006 在 213 项中是另一项（ParserAdapter::initExt），与这两例无关。例 103 注释写的 BUG-R17-007，在登记中是另一问题（零挂单量，已撤回）。例 212 注释写的 BUG-R71-001 在任何登记中都找不到。以上注释均未改动（这些用例保持禁用）。
- **磁盘写满事件**：约 14:5x，数据卷只剩 122 MiB（ENOSPC）。我删除了自己 scratch 目录 `slice3-result-run/` 下第 2 阶段逐例运行留下的 17 GB profraw 和 6 GB 复制的 dylib（汇总 JSON 已在证据目录中），释放后剩余 33 GB 以上。同一时段其他代理的构建或运行可能遇到 ENOSPC，建议各自复核那段时间的产物。

## 与 213 项的交叉核对

- 213 项中已判“本地验证通过”的条目，没有一个在本批禁用用例里仍复现：
  - BUG-T25-005（例 175）启用后 20/20 通过。
  - BUG-T26-007（例 152）现已不崩溃；保持禁用是因为夹具前置被裁定无效。
  - BUG-P08-006（例 53）被首个 NULL tick UB 遮蔽，未触达。
  - BUG-WSN06-004（例 77）的 UAF 来自 init 失败后继续调用，加上夹具的切片所有权，不是该项缺口。
  - BUG-I02-NEW-016（例 219）的 WTE_Connect 空 `_cfg` 已不崩溃，首个 UB 换成了人为插入的 NULL 资金元素。
- 213 项中仍复现的：BUG-R6-006（状态阻塞）——例 144、147 的未就绪引擎直取入口。
- 其他关联：
  - BUG-R31-002（待验证）：例 223 启用（断言与卡4b=A 一致）；例 224 断言旧的类型双关行为，保持禁用。
  - BUG-R8-003（待验证）：例 99、100 启用，断言与“无 store 合同”的两种裁定方向都不冲突。
  - BUG-T4-015（待验证）：例 156 已不再 exit，但断言无合同依据。

## 逐例分类表

说明：二进制列中 CT = CoverageTests。“诊断”列是协调方 ASan+UBSan 预扫状态。pre/preproc 是改前以禁用名运行，post/postproc 是改后以新名运行。崩溃用例的 rc 为信号值，只跑了第 1 轮。

| # | 二进制 | 用例（清单原名） | 分类 | 理由 | 证据 |
|---|---|---|---|---|---|
| 1 | CT | `l01_objectpool.DISABLED_destroy_null_is_undefined_and_rejected` | KEEP_CONTRACT | destroy(nullptr) 的接口容忍语义未定义（注释明确待定），ASan/UBSan 下仍解引用空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 2 | CT | `R56Logger.DISABLED_LogByCatPrefixKeepsFormattedPayload` | ENABLE | BUG-R56-002 已修，断言为正确口径（前缀 + 格式化正文）；进程级 WTSLogger 一次性 init/stop，与同套件已启用用例相同，同进程 repeat 第 2 轮 SIGSEGV（S3-NEW-1），按 20 个独立进程验证 | 诊断 pass；pre/main 1/20 OK rc=-11 `logs/pre-main-002.log`；preproc/main 20/20 OK rc=0 `logs/preproc-main-002.log`；postproc/main 20/20 OK rc=0 `logs/postproc-main-002.log`；post/main 1/20 OK rc=-11 `logs/post-main-002.log` |
| 3 | CT | `l03_contract.DISABLED_get_long_margin_ratio_without_comm_info` | KEEP_CONTRACT | 迭代02 裁定 BUG-L03-005 为 NOT_BUG：用例构造未挂接 commodity 的未就绪对象并查询，无此查询合同；ASan/UBSan 下仍空指针（WTSContractInfo.hpp:361） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 4 | CT | `P09RdmCacheTest.DISABLED_p09_underflow_inputs_from_range_indexers` | ENABLE | range 索引器下溢（BUG-P09-005/BUG-P05-007，PRODUCT）已修；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-004.log`；post/main 20/20 OK rc=0 `logs/post-main-004.log` |
| 5 | CT | `P12Extract.DISABLED_OutOfBoundsClass` | ENABLE | extractKlineData(seconds=0) 除零（BUG-P12-002，PRODUCT）已修；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-005.log`；post/main 20/20 OK rc=0 `logs/post-main-005.log` |
| 6 | CT | `P12TickFactory.DISABLED_CrashClassNotExecuted` | KEEP_CONTRACT | BUG-P12-001 已撤回：只能经 protected helper 人工构造时段外 tick，公开入口 updateKlineData 先拒绝；首个 UB 落在该非公开路径（WTSDataFactory.cpp:208） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 7 | CT | `R100RdmCrossDayTest.DISABLED_R100RdmHisHftValidCrossDateFiles` | ENABLE | 合法 CMP_V2 跨日历史文件读取 queue/detail/trans 各 1 条；原“未运行/V2 头指针风险”已排除（BUG-R175-002 已修） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-007.log`；post/main 20/20 OK rc=0 `logs/post-main-007.log` |
| 8 | CT | `R100RdmCrossDayTest.DISABLED_R100RdmHisHftEmptyCmpV2Payloads` | ENABLE | 空 CMP_V2 载荷返回 NULL，断言与现行读端合同一致 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-008.log`；post/main 20/20 OK rc=0 `logs/post-main-008.log` |
| 9 | CT | `R100RdmCrossDayTest.DISABLED_R100RdmQueueDetailTransCachesAreIndependent` | ENABLE | RDM 三类缓存互不遮挡的合法文件回归（R1 §2） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-009.log`；post/main 20/20 OK rc=0 `logs/post-main-009.log` |
| 10 | CT | `R112RdmResidualTest.DISABLED_R112RdmQueueDetailTransCachesAreIndependent` | ENABLE | 同 9（R112 RDM 侧） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-010.log`；post/main 20/20 OK rc=0 `logs/post-main-010.log` |
| 11 | CT | `R112ReaderResidualTest.DISABLED_R112ReaderQueueDetailTransCachesAreIndependent` | ENABLE | 同 9（R112 WtDataReader 侧）；历史 signal 9 未再出现 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-011.log`；post/main 20/20 OK rc=0 `logs/post-main-011.log` |
| 12 | CT | `R162ReaderEdgeTest.DISABLED_R162RdmReaderHisTransBlocksWithMixedActionDatesFromFile` | ENABLE | BUG-R175-002（PRODUCT）V2 装载头指针失效已修，生产格式文件 2 条混合日期成交读出 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-012.log`；post/main 20/20 OK rc=0 `logs/post-main-012.log` |
| 13 | CT | `R203StorageResidualTest.DISABLED_R203ReaderQueueDetailTransCacheContract` | ENABLE | 正确契约候选（先读 queue 不阻止 detail/trans 首次装载）现通过 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-013.log`；post/main 20/20 OK rc=0 `logs/post-main-013.log` |
| 14 | CT | `R203StorageResidualTest.DISABLED_R203RdmQueueDetailTransCacheContract` | ENABLE | 同 13（RDM 侧） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-014.log`；post/main 20/20 OK rc=0 `logs/post-main-014.log` |
| 15 | CT | `R74ReaderResidualTest.DISABLED_R74ReaderQueueCacheHidesSameDayDetailAndTrans` | ENABLE | BUG-R74-001（主记录 BUG-R132-002，PRODUCT）已修 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-015.log`；post/main 20/20 OK rc=0 `logs/post-main-015.log` |
| 16 | CT | `R94RdmDayBackoffTest.DISABLED_R94RdmRangeBeforeFirstBarUnderflowsCacheCursor` | ENABLE | BUG-R94-001（主记录 BUG-P05-007）已修；右界早于首根的游标不再回绕 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-016.log`；post/main 20/20 OK rc=0 `logs/post-main-016.log` |
| 17 | CT | `S5StorageReaderTest.DISABLED_LongestLegalStdCodeHistoryTickKeyStaysExact` | KEEP_DEFECT | I03-NEW-083（新增问题，待复核）现按 fail-closed 实施（超长代码 create 返回 NULL）：普通构建中 63 字节合法 stdCode 的历史 tick 读取返回 NULL（ASSERT 失败 20/20），预扫 ASan 构建为堆破坏；I03-NEW-039 端到端功能仍不可用 | 诊断 sanitizer-ubsan(noxml-exit-1)；pre/main 0/20 OK rc=1 `logs/pre-main-017.log` |
| 18 | CT | `R22PorterCapiTest.DISABLED_write_log_with_null_category_hits_strlen_null` | KEEP_CONTRACT | write_log 传 NULL 分类：迭代02 裁定 BUG-L05-005 为 TEST（接口只承诺空串表示默认分类，NULL 超出合同）；注释所指 BUG-R22-005 为格式串问题，与本例输入不同 | 诊断 sanitizer-asan(noxml-exit-1) |
| 19 | CT | `R22PorterCapiTest.DISABLED_capi_start_without_initialization_spawns_lifetime_monitor` | KEEP_CONTRACT | 只 SUCCEED，无可观察断言；故意起进程级监控线程（注释说明保持禁用） | 诊断 pass |
| 20 | CT | `R22RunnerInitTest.DISABLED_start_sync_branch_never_returns` | KEEP_EXTERNAL | start(false) 同步分支按设计阻塞至信号，90s 超时（长时间运行/需外部信号） | 诊断 timeout |
| 21 | CT | `R22RunnerInitTest.DISABLED_start_async_branch_spawns_state_monitor_thread` | KEEP_CONTRACT | 同 19：只 SUCCEED，起状态监控线程 | 诊断 pass |
| 22 | CT | `R23WriterTest.DISABLED_ClearCacheCommandMaySleepThreeHundredSeconds` | KEEP_EXTERNAL | 清缓存失败重试 sleep 300s（长时间），需可控失败目录；另 UBSan alignment(I03-NEW-109) | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 23 | CT | `R23WriterTest.DISABLED_SecondInitOnSameWriterTerminatesProcess` | KEEP_CONTRACT | 断言重复 init 成功，与卡9c（重复 init 拒绝）冲突；现返回 false | 诊断 fail |
| 24 | CT_adstorage | `R4WriterADTest.DISABLED_TickDbPathOccupiedByRegularFileThrowsOutOfWriteTick` | KEEP_CONTRACT | R4-BUG-05 不再 terminate，writeTick 返回 false（fail-closed）；用例断言写入成功，属旧前提 | 诊断 fail |
| 25 | CT_adstorage | `R122ReaderADTest.DISABLED_R122ReaderADKlineArrayTwoReturnsWrongTail` | KEEP_CONTRACT | 注释明确该 oracle 不触发增量回源/环形回绕，启用会误称覆盖；回绕已由同文件启用用例覆盖 | 诊断 pass |
| 26 | CT_adstorage | `R132ReaderADCacheTest.DISABLED_R132ReaderADRtBarAheadRechecksLmdb` | ENABLE | BUG-R144-002 已修：RT bar 超前时再读 LMDB | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-026.log`；post/main 20/20 OK rc=0 `logs/post-main-026.log` |
| 27 | CT_adstorage | `R162AdReaderTest.DISABLED_R162WriterADMergedDayBarVolumeIsDoubleCounted` | ENABLE | BUG-R162-002 已修：同日第二笔 tick 合并日线量额为 1+1=2 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-027.log`；post/main 20/20 OK rc=0 `logs/post-main-027.log` |
| 28 | CT_adstorage | `R172AdTest.DISABLED_R172RdmADTickRangeStartsAtFirstTick` | ENABLE | BUG-R32-001（PRODUCT）已修：首 tick 不再被末笔覆写 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-028.log`；post/main 20/20 OK rc=0 `logs/post-main-028.log` |
| 29 | CT_adstorage | `R172AdTest.DISABLED_R172ReaderADRtCacheBarPromotedToLmdb` | ENABLE | BUG-R144-002 已修 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-029.log`；post/main 20/20 OK rc=0 `logs/post-main-029.log` |
| 30 | CT_adstorage | `R172AdTest.DISABLED_R172ReaderADRtCacheBarAheadOfEtime` | ENABLE | BUG-R144-002 已修 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-030.log`；post/main 20/20 OK rc=0 `logs/post-main-030.log` |
| 31 | CT_adstorage | `R203AdResidualTest.DISABLED_R203RdmCrossDateTickRangeKeepsAllTicks` | ENABLE | BUG-R215-002/BUG-R32-001 已修：跨日三笔全部返回 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-031.log`；post/main 20/20 OK rc=0 `logs/post-main-031.log` |
| 32 | CT_adstorage | `R212AdReaderTest.DISABLED_R212AdReaderRtBarAheadOfEtimeRefreshesLmdbOnly` | ENABLE | BUG-R212-001 已修：RT bar 晚于 etime 只刷新 LMDB | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-032.log`；post/main 20/20 OK rc=0 `logs/post-main-032.log` |
| 33 | CT_adstorage | `R21ReaderADTest.DISABLED_RdmInitWithNullSinkContractPending` | KEEP_CONTRACT | GTEST_SKIP：NULL sink 合同未定义 | 诊断 skipped |
| 34 | CT_adstorage | `R21ReaderADTest.DISABLED_KlineReadWithoutAnyBarCrashesOnEmptyVectorSubscript` | ENABLE | R21-BUG-06 已修：空库 K 线读取不再对空 vector 取址 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-034.log`；post/main 20/20 OK rc=0 `logs/post-main-034.log` |
| 35 | CT_adstorage | `R21ReaderADTest.DISABLED_EtimeOnExistingTickNeedsBoundaryContract` | KEEP_CONTRACT | GTEST_SKIP：etime 等值包含规则未定 | 诊断 skipped |
| 36 | CT_adstorage | `R21ReaderADTest.DISABLED_OrderDetailRangeSupportNeedsContract` | KEEP_CONTRACT | GTEST_SKIP：AD reader 是否支持逐笔委托未定 | 诊断 skipped |
| 37 | CT_adstorage | `R21ReaderADTest.DISABLED_OrderQueueRangeSupportNeedsContract` | KEEP_CONTRACT | GTEST_SKIP：委托队列未定 | 诊断 skipped |
| 38 | CT_adstorage | `R21ReaderADTest.DISABLED_TransactionRangeSupportNeedsContract` | KEEP_CONTRACT | GTEST_SKIP：逐笔成交未定 | 诊断 skipped |
| 39 | CT_adstorage | `R4ReaderADTest.DISABLED_AdvancingEtimeAfterTwoHandlesCrashesOnNullDb` | KEEP_CONTRACT | 以无条件 FAIL() 结尾的崩溃占位（R4-BUG-02 已不再崩溃）；需改写正向断言才能启用 | 诊断 fail |
| 40 | CT_btcore | `CtaMockerStateTest.DISABLED_emptyStateCallsCrashOrHang` | KEEP_CONTRACT | 首个 UB 为直接以 NULL tick 调 handle_tick（UBSan CtaMocker.cpp:1417）；回放宿主不会传 NULL tick，空状态调用序列不在合同内 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 41 | CT_btcore | `ExecMockerTest.DISABLED_matchSinkCallbacksCrashWithoutExecuterUnit` | KEEP_CONTRACT | 无行情、无执行单元时直接调用 IMatchSink::handle_order（UBSan ExecMocker.cpp:1023）；BUG-N04-001 修复后失败 init 不再登记 sink，正常回放不可达 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 42 | CT_btcore | `P03CtaTradePath.DISABLED_CalcFeeByAmountCrashesWhenCommodityMissing` | KEEP_CONTRACT | 崩溃占位以无条件 FAIL() 结束；calc_fee 已不再崩溃（BUG-P03-001 已由卡13d 费用改造覆盖），需改写正向断言 | 诊断 fail |
| 43 | CT_btcore | `R171ReplayFixture.DISABLED_R171Sel_ReplayDoneWithoutScheduleDividesByZero` | KEEP_CONTRACT | BUG-R171-001 已不再除零（到达 FAIL），占位以无条件 FAIL() 结束 | 诊断 fail |
| 44 | CT_btcore | `R180LoaderFixture.DISABLED_R180Exec_TwapSecondTickReleasesDanglingLastTick` | KEEP_CONTRACT | 占位以无条件 FAIL() 结束（BUG-R180-001），且 FAIL 后析构 libc++abi terminating，原因未判定 | 诊断 signal-6 |
| 45 | CT_btcore | `R72HftUftPaths.DISABLED_UftEnumPositionWithoutStrategyDerefsNull` | KEEP_CONTRACT | BUG-R72-001 已不再解引用（到达 FAIL），占位以无条件 FAIL() 结束 | 诊断 fail |
| 46 | CT_btcore | `R92MockerResidual.DISABLED_HftMarketOrderFirstTickDerefsErasedOrder` | KEEP_CONTRACT | BUG-R92-001 已不再崩溃（到达 FAIL），占位以无条件 FAIL() 结束 | 诊断 fail |
| 47 | CT_btcore | `P04ReplayTermTest.DISABLED_taskFrequencyAndOutOfRangeReplayCrash` | KEEP_CONTRACT | etime=0 现被回放配置校验拒绝（init 返回 false），用例以 init 成功为前提，属旧前提 | 诊断 fail |
| 48 | CT_btcore | `P07MinuteReplayTest.DISABLED_emptyMinuteCsvHitsUnsubscriptedBars` | ENABLE | BUG-P04-004/005（PRODUCT）已修：表头-only 分钟 CSV 回放不越界；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-048.log`；post/main 20/20 OK rc=0 `logs/post-main-048.log` |
| 49 | CT_btcore | `P07TaskScheduleTest.DISABLED_unknownSessionTemplateCrashes` | ENABLE | 未知 session 模板现由入口安全处理（卡1 入口前置校验）；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-049.log`；post/main 20/20 OK rc=0 `logs/post-main-049.log` |
| 50 | CT_btcore | `P07TickLoaderTest.DISABLED_mismatchedTickDateHangsReplayHftDatas` | KEEP_CONTRACT | 注释明确未构造串日挂死输入，仅归档；启用不能证明所称路径 | 诊断 pass |
| 51 | CT_btcore | `P08HftOrderCancel.DISABLED_BlockingHookAndNullDerefFamily` | KEEP_CONTRACT | 单线程调用 step_tick/_cond_calc 需配对线程的 hook，必然阻塞（90s 超时），调用序列不符合 hook 合同 | 诊断 timeout |
| 52 | CT_btcore | `P08UftDispatchT1.DISABLED_TickPointerAndSessionDerefFamily` | KEEP_CONTRACT | 向 on_tick/handle_tick 传 NULL tick、未装策略调用 session 回调（UBSan UftMocker.cpp:374），宿主不会传 NULL tick，输入合同未定义 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 53 | CT_btcore | `P08UftOrderPosition.DISABLED_NullStrategyAndNullCommDerefFamily` | KEEP_CONTRACT | 首个 UB 为 update_dyn_profit(NULL tick)（UBSan UftMocker.cpp:1133），NULL tick/空策略属人工输入；可独立判断的未知代码平仓风险已拆为 BUG-P08-006（213，本地验证通过），本例被首个 UB 遮蔽，不构成该项缺口 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 54 | CT_btcore | `R191MockerFixture.DISABLED_R191Uft_PluginFactoryDestructorCrashes` | ENABLE | BUG-R191-001（主记录 BUG-R64-002，PRODUCT）已修：真实本地 WtUftStraFact 插件加载与析构 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-054.log`；post/main 20/20 OK rc=0 `logs/post-main-054.log` |
| 55 | CT_btcore | `R201MockerFixture.DISABLED_R201UftMocker_ShortFrozenIsNotReleasedOnSessionBegin` | KEEP_CONTRACT | BUG-R201-001 已并入 BUG-P08-002，迭代02 裁定 TEST：在 TM_LongT1 下直接注入空头成交，绕过交易模式约束，空头 T+1 跨日语义未定 | 诊断 fail |
| 56 | CT_btcore | `R20EventNotifier.DISABLED_InitActiveLoadsMqModuleAndOpensServer` | KEEP_EXTERNAL | 需拉起真实 MQ 服务（socket），GTEST_SKIP | 诊断 skipped |
| 57 | CT_btcore | `R20EventNotifier.DISABLED_InitNullConfigDerefsAndCrashes` | KEEP_CONTRACT | 只有 GTEST_SKIP 占位，无断言 | 诊断 skipped |
| 58 | CT_btcore | `R20ExecMocker.DISABLED_TickSwapReleasesPreviousTick` | KEEP_CONTRACT | 注释列明的启用条件之二（夹具为 matcher 提供 no-op ExecuteUnit）未满足；未 init 的 ExecMocker 直接驱动 handle_tick/下单不在回放合同内（BUG-N04-001 修复后失败 init 不进入回放）；现虽通过，不据此固化 | 诊断 pass |
| 59 | CT_btcore | `R20ExecMocker.DISABLED_BuyAfterTickAccumulatesUndone` | KEEP_CONTRACT | 注释列明的启用条件之二（夹具为 matcher 提供 no-op ExecuteUnit）未满足；未 init 的 ExecMocker 直接驱动 handle_tick/下单不在回放合同内（BUG-N04-001 修复后失败 init 不进入回放）；现虽通过，不据此固化 | 诊断 pass |
| 60 | CT_btcore | `R20ExecMocker.DISABLED_SellDecrementsUndone` | KEEP_CONTRACT | 注释列明的启用条件之二（夹具为 matcher 提供 no-op ExecuteUnit）未满足；未 init 的 ExecMocker 直接驱动 handle_tick/下单不在回放合同内（BUG-N04-001 修复后失败 init 不进入回放）；现虽通过，不据此固化 | 诊断 pass |
| 61 | CT_btcore | `R20ExecMocker.DISABLED_CancelByCodeSkipsUnfiredOrders` | KEEP_CONTRACT | 注释列明的启用条件之二（夹具为 matcher 提供 no-op ExecuteUnit）未满足；未 init 的 ExecMocker 直接驱动 handle_tick/下单不在回放合同内（BUG-N04-001 修复后失败 init 不进入回放）；现虽通过，不据此固化 | 诊断 pass |
| 62 | CT_btcore | `R20ExecMocker.DISABLED_SessionEndClearsMatcherAndUndone` | KEEP_CONTRACT | 注释列明的启用条件之二（夹具为 matcher 提供 no-op ExecuteUnit）未满足；未 init 的 ExecMocker 直接驱动 handle_tick/下单不在回放合同内（BUG-N04-001 修复后失败 init 不进入回放）；现虽通过，不据此固化 | 诊断 pass |
| 63 | CT_btcore | `R20ExecMocker.DISABLED_ExecUnitNullDerefFamily` | KEEP_CONTRACT | 只有 GTEST_SKIP 占位（_exec_unit NULL 族），无断言 | 诊断 skipped |
| 64 | CT_btcore | `R20ExecMocker.DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick` | KEEP_CONTRACT | 只有 GTEST_SKIP 占位（fire_orders _sink NULL），无断言 | 诊断 skipped |
| 65 | CT_btcore | `R20HisDataMgr.DISABLED_InitCrashesWhenReaderNullAfterLoadFailure` | KEEP_CONTRACT | 只有 GTEST_SKIP 占位（HisDataMgr NULL reader），无断言 | 诊断 skipped |
| 66 | CT_btcore | `Q02CtaDumpTest.DISABLED_NullReplayerDerefAndBlockingHookFamily` | KEEP_CONTRACT | NULL replayer 夹具调用依赖回放器的路径（UBSan CtaMocker.cpp:1834），并含须线程配对的阻塞 hook；该路径以回放器存在为前提 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 67 | CT_btcore | `Q02CtaIntegrationTest.DISABLED_RealTickAndBlockingHookFamily` | KEEP_CONTRACT | 阻塞 hook 族（90s 超时），且 T+1 口径注释标“待裁定” | 诊断 timeout |
| 68 | CT_btcore | `Q02FactoryTest.DISABLED_RealModuleLoadAndSelDivZeroFamily` | ENABLE | SelMocker 除零已修；NULL replayer 为同目录启用用例的既有夹具形态；断言不存在模块时 init 返回 false | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-068.log`；post/main 20/20 OK rc=0 `logs/post-main-068.log` |
| 69 | CT_btcore | `R80TaskBars.DISABLED_DailyTaskSessionEndContractPending` | KEEP_CONTRACT | GTEST_SKIP：任务触发与日线闭合合同未定 | 诊断 skipped |
| 70 | CT_btporter | `Q06BtPorterCapi.DISABLED_BacktestLifecycleNeedsStorageAndThreads` | KEEP_CONTRACT | 空占位 EXPECT_TRUE(true)，注释说明真实路径需存储/线程；启用会虚增通过数 | 诊断 pass |
| 71 | CT_btporter | `Q06BtRunner.DISABLED_BacktestRunPathsNeedStorage` | KEEP_CONTRACT | 空占位 EXPECT_TRUE(true) | 诊断 pass |
| 72 | CT_btporter | `R205BtPorterLevel2.DISABLED_QueueMissPoisonsTransCacheAndHidesLaterTransFile` | ENABLE | BUG-R205-003 已修：ordque 缺失不再污染 trans 缓存，后写文件读出 1 条 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-072.log`；post/main 20/20 OK rc=0 `logs/post-main-072.log` |
| 73 | CT_btporter | `R69BtPorterLifecycle.DISABLED_RunBacktestNeedsConfiguredReplayer` | KEEP_CONTRACT | 无断言；进程单例 run_backtest 现于未配置时记错误返回，但若前序用例已配置单例会真实回放，只能作进程级取样 | 诊断 pass |
| 74 | CT_btporter | `R69BtPorterLifecycle.DISABLED_ReleaseBacktestStopsGlobalLogger` | KEEP_CONTRACT | release_backtest 会永久停止全局日志（WTSLogger::stop），属进程级取样，无断言 | 诊断 pass |
| 75 | CT_btporter | `R9WiredMockerCapi.DISABLED_OrderAndSlicePathsNeedLoadedStorage` | KEEP_CONTRACT | 空占位 EXPECT_TRUE(true) | 诊断 pass |
| 76 | CT_ctafact | `n06_cta_fact.DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS` | KEEP_CONTRACT | 向工厂直接传 NULL 策略名（ASan strcmp NULL）；runner 已在边界拒绝缺名（WSN06-001），工厂 NULL 入参合同未定义 | 诊断 sanitizer-asan(noxml-exit-1) |
| 77 | CT_ctafact | `n06_cta_ctx.DISABLED_ZERO_PERIOD_DAYS_DIVIDE_CLASS` | KEEP_CONTRACT | BUG-WSN06-004（213，本地验证通过）后 TwoMa init 拒绝 0 周期，用例忽略 init 失败继续 on_schedule；ASan UAF 为 TwoMa release 的 K 线切片被 DualThrust 在同一 mock ctx 上再读（WtStraDualThrust.cpp:283），属调用序列越界与夹具切片所有权问题，不构成该项缺口 | 诊断 sanitizer-asan(noxml-exit-1) |
| 78 | CT_dtcore | `R145StateMonitorRunTest.DISABLED_SecondStopAfterRunThrows` | KEEP_CONTRACT | run 后重复 stop：迭代02 裁定同类 BUG-R154-003 为 NOT_BUG（未承诺的手工重复调用）；无断言，启用会把未约定行为固化为回归 | 诊断 pass |
| 79 | CT_dtcore | `R154UdpCasterTest.DISABLED_NullManagersAreAnInvalidDependencyProbe` | KEEP_CONTRACT | GTEST_SKIP：空 manager 依赖无合同 | 诊断 skipped |
| 80 | CT_dtcore | `R154UdpCasterTest.DISABLED_DestructorAfterActiveStartCrashes` | KEEP_EXTERNAL | active UDPCaster 需打开本地 UDP socket/端口（BUG-R164-001 现已不崩） | 诊断 pass |
| 81 | CT_dtcore | `R154UdpCasterTest.DISABLED_SecondStopAfterStartThrows` | KEEP_EXTERNAL | 同 80，需本地 UDP 端口（BUG-R154-003 现已不抛） | 诊断 pass |
| 82 | CT_dtcore | `R18IdxWorkerMgrTest.DISABLED_HandleQuoteCrashesWhenContractInfoMissing` | KEEP_CONTRACT | 注释编号错配（实为 BUG-R18-008）：迭代01 复核唯一调用方 ParserAdapter 先确认合约再分发，无合约 tick 直接喂入不对应现行调用链；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 83 | CT_dtcore | `R18IdxWorkerMgrTest.DISABLED_SubTicksWithoutDotCrashesOnIndex` | KEEP_CONTRACT | 注释编号错配（实为 BUG-R18-009；213 中 BUG-R18-006 为 ParserAdapter::initExt，与本例无关）：内置调用方先归一为 EXCHG.code，无点号代码不对应产品路径；ASan 仍越界 | 诊断 sanitizer-asan(noxml-exit-1) |
| 84 | CT_dtcore | `R18ParserAdapterTest.DISABLED_HandleQuoteCrashesWhenDataManagerIsNull` | KEEP_CONTRACT | WtDtCore ParserAdapter 以 NULL 数据管理器构造后推行情（UBSan ParserAdapter.cpp:702）；该依赖不可省略，NULL 依赖合同未定义 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 85 | CT_dtcore | `R18ShmCasterTest.DISABLED_InitWithActiveTrueNeedsRealMappedFile` | ENABLE | ShmCaster active=true 用临时目录本地映射文件（无网络/端口），断言合法路径 init 成功 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-085.log`；post/main 20/20 OK rc=0 `logs/post-main-085.log` |
| 86 | CT_dtcore | `R44DataManagerEdgesTest.DISABLED_NullBaseDataManagerIsAnInvalidDependencyProbe` | KEEP_CONTRACT | GTEST_SKIP：NULL base data manager 不受支持 | 诊断 skipped |
| 87 | CT_dtcore | `R44IndexWorkerFreshTest.DISABLED_InitCrashesWithNullConfig` | KEEP_CONTRACT | 迭代02 裁定 BUG-R44-006 为 TEST：init 需要配置对象，NULL 非合法配置；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 88 | CT_dtcore | `R44IndexWorkerFreshTest.DISABLED_FreshPushTickCrashesOnUninitializedDataManager` | KEEP_CONTRACT | 迭代02 裁定 BUG-R44-007 为 NOT_BUG：违反 worker 初始化顺序的直接调用；ASan 仍 SEGV | 诊断 sanitizer-asan(noxml-exit-1) |
| 89 | CT_dtcore | `R44ParserAdapterEdgesTest.DISABLED_RunAfterReleaseUsesFreedApi` | KEEP_CONTRACT | BUG-R44-004 已修：release 后 run() 返回 false；用例断言 run() 为 true（旧缺陷行为） | 诊断 fail |
| 90 | CT_dtcore | `R44StateMonitorLifecycleTest.DISABLED_ReinitializeSemanticsNeedContract` | KEEP_CONTRACT | GTEST_SKIP：重复 initialize 语义未定 | 诊断 skipped |
| 91 | CT_dtcore | `R44StateMonitorLifecycleTest.DISABLED_InitializeCrashesWithNullBdMgr` | KEEP_CONTRACT | 迭代02 裁定 BUG-R44-002 为 TEST：bdMgr 为必需依赖，无可空合同（现已不崩，但不据此固化 NULL 依赖行为） | 诊断 pass |
| 92 | CT_dtcore | `R5StateMonitorBdtTest.DISABLED_StateMachineAdvanceNeedsInjectableClockSeam` | KEEP_CONTRACT | 需真实时钟线程且 initialize 前提在现行配置校验下失败（ASSERT 失败），注释说明缺可注入时钟接缝 | 诊断 fail |
| 93 | CT_dtcore | `R5UDPCaster.DISABLED_AddMRecverWithValidAddressNeedsRealSocket` | KEEP_EXTERNAL | 合法地址 addMRecver 真实 bind 端口并加入组播 | 诊断 pass |
| 94 | CT_dtcore | `R68UdpCasterInitTest.DISABLED_InitCrashesWhenConfigIsNull` | KEEP_CONTRACT | 迭代02 裁定 BUG-R68-001 为 TEST：cfg 为必需指针，无可空合同；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 95 | CT_dtcore | `R84bStateMonitorPositiveOffsetTest.DISABLED_NullSessionIdContractUnproven` | KEEP_CONTRACT | GTEST_SKIP：nullptr sid 不受支持 | 诊断 skipped |
| 96 | CT_dtservo | `R102WtDataManagerTest.DISABLED_InitStoreWithoutPluginDerefsNullReader` | ENABLE | BUG-Q05-001（PRODUCT）已修：插件缺失 fail-closed，不再解引用 NULL reader；无显式断言 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-096.log`；post/main 20/20 OK rc=0 `logs/post-main-096.log` |
| 97 | CT_dtservo | `R45ParserAdapterFakeTest.DISABLED_ReleaseTwiceContractPending` | KEEP_CONTRACT | GTEST_SKIP：重复 release 无公开合同 | 诊断 skipped |
| 98 | CT_dtservo | `R45RunnerExtraTest.DISABLED_EmptyStoreSinglePeriodRangeReadSegfaults` | ENABLE | BUG-R45-003（主记录 BUG-P05-002，PRODUCT）已修：空库单周期区间读返回 NULL | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-098.log`；post/main 20/20 OK rc=0 `logs/post-main-098.log` |
| 99 | CT_dtservo | `R8WtDataManagerSinksTest.DISABLED_InitStoreWithDefaultModuleNameDerefsNullReader` | ENABLE | 默认模块名加载失败 fail-closed（init 返回 false），与 BUG-Q05-001 合同一致；不涉 R8-003 的“无 store”裁定 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-099.log`；post/main 20/20 OK rc=0 `logs/post-main-099.log` |
| 100 | CT_dtservo | `R8WtDataManagerSinksTest.DISABLED_TickSliceDelegationWithoutReaderSegfaults` | ENABLE | 无 reader 时委托入口返回 NULL；只断言查询结果，不涉 R8-003 init 结果裁定 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-100.log`；post/main 20/20 OK rc=0 `logs/post-main-100.log` |
| 101 | CT_exefact | `r17_mon.DISABLED_simpdata_init_null_cfg_derefs` | KEEP_CONTRACT | 占位断言 EXPECT_FALSE(true) 恒失败（BUG-R17-006 已不再 SEGV） | 诊断 fail |
| 102 | CT_exefact | `r17_mon.DISABLED_simpdata_push_quote_without_session_info` | KEEP_CONTRACT | 占位断言 EXPECT_TRUE(false) 恒失败（BUG-R17-005 已不再崩溃） | 诊断 fail |
| 103 | CT_exefact | `r17_mon.DISABLED_simpdata_basedata_mgr_null_runner` | KEEP_CONTRACT | 注释 BUG-R17-007 与登记不符（登记的 R17-007 为零挂单量且已撤回）；未 init 的 mgr 不会被读线程回调（reader 在 initStore 后创建），同 BUG-R17-005 NOT_BUG；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 104 | CT_exefact | `r17_mon.DISABLED_porter_run_exec_spawns_threads` | KEEP_CONTRACT | run_exec 起收发/共享内存线程（进程级），仅 EXPECT_NO_THROW，无可观察断言 | 诊断 pass |
| 105 | CT_exefact | `r17_mon.DISABLED_porter_release_exec_stops_uninited_logger` | KEEP_CONTRACT | release_exec 触发 WTSLogger::stop（进程级关停），仅 EXPECT_NO_THROW，无可观察断言 | 诊断 pass |
| 106 | CT_exefact | `r7_execrunner.DISABLED_release_stops_uninited_logger` | KEEP_CONTRACT | 同 105：release 关停全局日志，仅 SUCCEED | 诊断 pass |
| 107 | CT_exefact | `r7_execrunner.DISABLED_add_exe_factories_dlopens_plugins` | KEEP_CONTRACT | 仅 SUCCEED；dlopen 插件目录存在同名符号风险（BUG-L00-003），无可观察断言 | 诊断 pass |
| 108 | CT_exefact | `r24_runner.DISABLED_get_sess_info_with_empty_code_crashes_code_helper` | ENABLE | BUG-R24-001 已修：空串代码 get_sess_info 返回 NULL | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-108.log`；post/main 20/20 OK rc=0 `logs/post-main-108.log` |
| 109 | CT_exefact | `r24_runner.DISABLED_release_after_full_config_stops_logger` | KEEP_CONTRACT | 前提 config 在现行校验下失败（ASSERT 失败），且 release 会关停全局日志，仅 SUCCEED | 诊断 fail |
| 110 | CT_exefact | `r24_stkvw.DISABLED_time_buckets_use_cumulative_aim_candidates` | KEEP_CONTRACT | 注释：候选预期“仍待业务确认”且会暴露 BUG-R7-007；期望 1 实际 0 | 诊断 fail |
| 111 | CT_exefact | `r24_stkvw.DISABLED_fire_at_once_zero_qty_and_sell_paths_need_safe_fixture` | KEEP_CONTRACT | GTEST_SKIP：无安全公开夹具 | 诊断 skipped |
| 112 | CT_exefact | `r24_stkvw.DISABLED_afternoon_bucket_index_out_of_range_read` | ENABLE | BUG-R7-003（PRODUCT）已修：map_action_time 校验分桶下标；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-112.log`；post/main 20/20 OK rc=0 `logs/post-main-112.log` |
| 113 | CT_m_QuoteFactory | `r104_quote_initialize.DISABLED_config_without_basefiles_derefs_null` | ENABLE | 缺 basefiles 的 initialize 子进程正常返回（BUG-R104-001 同族 R6-002 已修） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-113.log`；post/main 20/20 OK rc=0 `logs/post-main-113.log` |
| 114 | CT_m_QuoteFactory | `r104_quote_initialize.DISABLED_state_monitor_branch_starts_threads` | KEEP_CONTRACT | 空占位 SUCCEED，注释说明需状态机线程接缝 | 诊断 pass |
| 115 | CT_m_QuoteFactory | `r14_quote_entry.DISABLED_existing_config_enters_parsers_run_and_endless_loop` | KEEP_EXTERNAL | 需可加载真实 parser 的 dtcfg.yaml 与 shm 目录，可能永久轮询 | 诊断 fail |
| 116 | CT_m_QuoteFactory | `r14_quote_entry.DISABLED_bare_argv_with_existing_config_enters_runner` | KEEP_EXTERNAL | 同 115 | 诊断 fail |
| 117 | CT_m_WtLatencyHFT | `r14_lazy_hft_tool.DISABLED_entry_blocks_on_getchar` | KEEP_CONTRACT | 空占位 EXPECT_TRUE(true)（入口内部 getchar 阻塞） | 诊断 pass |
| 118 | CT_m_WtLatencyHFT | `r14_lazy_hft_tool.DISABLED_run_starts_threads_and_busy_loop` | KEEP_CONTRACT | 仅 EXPECT_TRUE(true)；run 起线程/忙等，无可观察断言 | 诊断 pass |
| 119 | CT_m_WtLatencyUFT | `r14_lazy_uft_tool.DISABLED_entry_blocks_on_getchar` | KEEP_CONTRACT | 空占位 EXPECT_TRUE(true) | 诊断 pass |
| 120 | CT_m_WtLatencyUFT | `r14_lazy_uft_tool.DISABLED_init_success_needs_real_config_and_engine` | KEEP_EXTERNAL | 需真实 config.yaml + env 段与引擎 | 诊断 fail |
| 121 | CT_m_WtLatencyUFT | `r14_lazy_uft_tool.DISABLED_run_starts_threads_and_busy_loop` | KEEP_CONTRACT | 仅 EXPECT_TRUE(true)；run 起线程/忙等 | 诊断 pass |
| 122 | CT_m_WtRunner | `r104_wtrunner_config.DISABLED_run_starts_parsers_traders_engine_and_polls_forever` | KEEP_CONTRACT | 空占位 SUCCEED（run 永久轮询） | 诊断 pass |
| 123 | CT_m_WtRunner | `r28_wtrunner_entry.DISABLED_stack_allocated_runner_is_sigkilled` | KEEP_CONTRACT | 断言缺失配置文件时 config 成功，与现行合同（返回 false）冲突 | 诊断 fail |
| 124 | CT_m_WtRunner | `r28_wtrunner_entry.DISABLED_bare_argv_reaches_stack_runner_and_null_engine_run` | KEEP_CONTRACT | 断言无配置的裸 argv 入口返回 0，与现行合同（配置失败返回非零）冲突 | 诊断 fail |
| 125 | CT_m_WtRunner | `r28_wtrunner_entry.DISABLED_unknown_long_option_segv` | ENABLE | K2：未知长选项入口返回非零，不再 SIGSEGV；同进程 repeat 20 稳定 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-125.log`；post/main 20/20 OK rc=0 `logs/post-main-125.log` |
| 126 | CT_m_WtRunner | `r28_wtrunner_config.DISABLED_valid_config_without_basefiles_derefs_null_section` | ENABLE | K3/BUG-R6-002（PRODUCT）已修：缺 basefiles 的 config 返回 false | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-126.log`；post/main 20/20 OK rc=0 `logs/post-main-126.log` |
| 127 | CT_m_WtRunner | `r28_wtrunner_strategies.DISABLED_active_cta_entry_needs_real_factory_dir` | KEEP_CONTRACT | 断言无 cta/ 工厂目录时 config 成功；现行合同为策略创建失败即配置失败，需工厂目录注入接缝 | 诊断 fail |
| 128 | CT_m_WtRunner | `r6_wtrunner_entry.DISABLED_short_help_returns_zero_before_touching_runner` | KEEP_CONTRACT | -h 在 cppcli 内 exit(0)，进程内调用直接终止 gtest 宿主（noxml exit 0），无法进程内断言 | 诊断 noxml-exit-0 |
| 129 | CT_m_WtRunner | `r6_wtrunner_entry.DISABLED_long_help_returns_zero` | KEEP_CONTRACT | 同 128（--help） | 诊断 noxml-exit-0 |
| 130 | CT_m_WtRunner | `r6_wtrunner_entry.DISABLED_help_wins_over_config_and_logcfg_words` | KEEP_CONTRACT | 同 128 | 诊断 noxml-exit-0 |
| 131 | CT_m_WtRunner | `r6_wtrunner_config.DISABLED_empty_object_config_derefs_null_basefiles` | KEEP_CONTRACT | 夹具从未写入 ./r6_empty_cfg.yaml，实际走缺文件分支，所称空对象前提不成立；空对象配置面已由 r28_wtrunner_config 用例（本表 126）覆盖 | 诊断 pass |
| 132 | CT_m_WtUftRunner | `r14_uft_config.DISABLED_full_engine_config_needs_shm_and_vendor_libs` | KEEP_EXTERNAL | 需真实 shm 与柜台库的全量配置 | 诊断 fail |
| 133 | CT_m_WtUftRunner | `r14_uft_runner.DISABLED_run_false_loops_until_signal` | KEEP_CONTRACT | 仅 EXPECT_TRUE(true)，无可观察断言 | 诊断 pass |
| 134 | CT_m_WtUftRunner | `r95_uft_strategy_child.DISABLED_probe` | KEEP_CONTRACT | 设计性禁用：子进程探针入口，由父用例带环境变量 spawn；直接运行缺环境变量即失败 | 诊断 fail |
| 135 | CT_m_WtUftRunner | `r85_uft_runner_child.DISABLED_log_capture_probe` | KEEP_CONTRACT | 设计性禁用：子进程日志捕获探针，同 134 | 诊断 fail |
| 136 | CT_mqshare | `n06_mq_capi.DISABLED_NULL_URL_CRASH_CLASS` | KEEP_CONTRACT | create_server/create_client 传 NULL url：C 接口未声明可空，未见登记缺陷；ASan 仍在 nanomsg strlen SEGV | 诊断 sanitizer-asan(noxml-exit-1) |
| 137 | CT_porter | `Q06PorterCapi.DISABLED_UninitializedEngineGettersCrash` | KEEP_CONTRACT | 空占位 (void)0，无产品调用与断言 | 诊断 pass |
| 138 | CT_porter | `Q06PorterWrapper.DISABLED_ExtParserCreationNeedsEngine` | ENABLE | BUG-R6-003（PRODUCT）已修：未 config 时 createExtParser/createExtExecuter 安全拒绝；无显式断言；验证前须保证 porter 镜像与测试同源（S3-NEW-2） | 诊断 pass；pre/main 0/20 OK rc=1 `logs/pre-main-138.log`；pre/main 20/20 OK rc=0 `logs/pre-main-138.log`；post/main 20/20 OK rc=0 `logs/post-main-138.log` |
| 139 | CT_porter | `Q06PorterRunner.DISABLED_BlockingAndNullEnginePaths` | KEEP_CONTRACT | 仅 getEngine + EXPECT_TRUE(true) 占位；gtest.cc:2588 夹具冲突失败 | 诊断 fail |
| 140 | CT_porter | `R13PorterCtaExport.DISABLED_position_mutation_entries_deref_uninitialized_engine` | ENABLE | 卡4a：未 config 引擎上 cta 仓位变更入口安全无操作；无显式断言 | 诊断 pass；pre/main 1/20 OK rc=-6 `logs/pre-main-140.log`；pre/main 20/20 OK rc=0 `logs/pre-main-140.log`；post/main 20/20 OK rc=0 `logs/post-main-140.log` |
| 141 | CT_porter | `R13PorterCtaExport.DISABLED_history_slice_entries_need_a_data_manager_seam` | ENABLE | 卡4a：未 config 时 cta_get_bars 返回 0 | 诊断 pass；pre/main 1/20 OK rc=-6 `logs/pre-main-141.log`；pre/main 20/20 OK rc=0 `logs/pre-main-141.log`；post/main 20/20 OK rc=0 `logs/post-main-141.log` |
| 142 | CT_porter | `R13ExpContextBlanks.DISABLED_init_and_session_end_and_hft_trader_paths_need_output_and_trader_seams` | KEEP_CONTRACT | getCtaContext(0) 为 NULL 时整体无操作，等同占位 | 诊断 pass |
| 143 | CT_porter | `R13PorterSelExport.DISABLED_sel_mutation_and_subscription_need_engine_seam` | ENABLE | 卡4a：未 config 时 sel_set_position/sel_sub_ticks 安全无操作；无显式断言 | 诊断 pass；pre/main 0/20 OK rc=1 `logs/pre-main-143.log`；pre/main 20/20 OK rc=0 `logs/pre-main-143.log`；post/main 20/20 OK rc=0 `logs/post-main-143.log` |
| 144 | CT_porter | `R13PorterEngineSeam.DISABLED_engine_direct_getters_deref_null_engine` | KEEP_DEFECT | BUG-Q06-002 已并入 213 项 BUG-R6-006（状态阻塞）：cta/sel/hft get_price 等直取入口仍无条件解引用未就绪引擎（UBSan WtPorter.cpp:1022）；卡4a 要求未配置对象返回明确未就绪 | 诊断 sanitizer-ubsan(noxml-exit-1)；pre/main 0/20 OK rc=-11 `logs/pre-main-144.log`；pre/main 0/20 OK rc=-11 `logs/pre-main-144.log` |
| 145 | CT_porter | `R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody` | KEEP_CONTRACT | 栈上 WtRtRunner::init 把自身登记为进程级日志 handler 并改写全局 Inst/Generate 目录，析构后留下悬挂 handler；属进程级取样，不能在共享进程启用 | 诊断 pass |
| 146 | CT_porter | `R6PorterNullArgs.DISABLED_ctx_on_tick_null_tickdata_derefs` | KEEP_CONTRACT | 迭代02 裁定 BUG-R6-005 为 NOT_BUG（无合法生产调用方传 NULL tick）；且用例以 FAIL() 断言“必须崩” | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 147 | CT_porter | `R6PorterRawCode.DISABLED_get_raw_stdcode_without_engine_crashes` | KEEP_DEFECT | BUG-Q06-002 所列 get_raw_stdcode 入口，已并入 213 项 BUG-R6-006（阻塞）：无引擎时仍空指针（UBSan WtRtRunner.cpp:1086） | 诊断 sanitizer-ubsan(noxml-exit-1)；pre/main 0/20 OK rc=-11 `logs/pre-main-147.log`；pre/main 0/20 OK rc=-11 `logs/pre-main-147.log` |
| 148 | CT_porter | `R6PorterFeed.DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage` | KEEP_DEFECT | BUG-Q06-004（R6-009 合并指向，迭代02 主记录 BUG-R46-001 PRODUCT；不在 213，亦无 I03-NEW）：feedAdjFactors 未检查 _feeder_fcts，未注册 feeder 时调用空函数指针（ASan SEGV） | 诊断 sanitizer-asan(noxml-exit-1)；pre/main 0/20 OK rc=-11 `logs/pre-main-148.log`；pre/main 0/20 OK rc=-11 `logs/pre-main-148.log` |
| 149 | CT_porter | `R6PorterRunRelease.DISABLED_run_without_engine_and_release_global_logger_stop` | KEEP_CONTRACT | run 无引擎 + release 关停全局日志（进程级），无断言 | 诊断 pass |
| 150 | CT_riskmon | `DISABLED_n05_simpdatamgr.push_quote_without_session_dereferences_null` | ENABLE | BUG-N05-002（并入 BUG-R17-004，PRODUCT）已修：handle_push_quote 显式判 _s_info | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-150.log`；post/main 20/20 OK rc=0 `logs/post-main-150.log` |
| 151 | CT_selfthft | `n06_sel_uft_hft.DISABLED_NULL_NAME_AND_NULL_CFG_CRASH_CLASS` | KEEP_CONTRACT | 迭代02 裁定 BUG-WSN06-001：直接 createStrategy(NULL) 不代表正常 runner 参数（runner 已校验名称）；首个 SEGV 即在此（WtSelStraFact.cpp:72），后续 init(NULL) 部分（WSN06-002，213 待验证）未被执行，不构成证据 | 诊断 sanitizer-asan(noxml-exit-1) |
| 152 | CT_t_CTPLoader | `DISABLED_t26_ctploader_dump_crash.fees_dump_of_unknown_margin_code_dereferences_end_iterator` | KEEP_CONTRACT | 迭代02 裁定该夹具（ZZ99 无合约行）为无效前置；且以未登记请求号 79 直接调回调，与卡14 请求号登记合同不符（I03-NEW-093 同类） | 诊断 pass |
| 153 | CT_t_CTPLoader | `DISABLED_t6_ctploader_spi_crash.on_front_connected_dereferences_null_user_api` | KEEP_CONTRACT | pUserApi 未创建时直接调用 SDK 驱动的 OnFrontConnected；run 先创建 API 再 Init，不在 SDK 生命周期内（同 BUG-T4-004 NOT_BUG 口径）；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 154 | CT_t_CTPLoader | `DISABLED_t6_ctploader_spi_exit.instrument_last_response_calls_exit_zero` | KEEP_CONTRACT | 无断言；K02=B 后插件不再 exit，且以未登记请求号直接调回调（卡14），用例前提失效；末包终态由启用的 t26 完成态用例覆盖 | 诊断 pass |
| 155 | CT_t_CTPLoader | `DISABLED_t6_ctploader_run_net.complete_config_reaches_vendor_api_init` | KEEP_EXTERNAL | run() 走到厂商 CreateFtdcTraderApi/Init 并连前置（90s 超时） | 诊断 timeout |
| 156 | CT_t_CTPOptLoader | `CTPOptLoaderTest.DISABLED_last_instrument_packet_exits_the_process` | KEEP_CONTRACT | BUG-T4-015 已不 exit；断言最后一包后 _contracts 为空，与现行行为（保留合约）不符且无合同依据 | 诊断 fail |
| 157 | CT_t_CTPOptLoader | `CTPOptLoaderDisabled.DISABLED_complete_config_reaches_dlopen_and_null_creator` | KEEP_EXTERNAL | 结果取决于本机是否存在厂商 soptthosttraderapi 库；存在时会创建真实 API 并连前置；已有 T4-014 fake 模块用例覆盖 | 诊断 pass |
| 158 | CT_t_CTPOptLoader | `CTPOptLoaderDisabled.DISABLED_front_connected_and_login_success_require_vendor_api` | KEEP_CONTRACT | 无 API 实例时强制调用登录成功回调（同 BUG-T4-004 NOT_BUG 口径，SDK 成功回调以有效实例为前提）；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 159 | CT_t_ParserCTP | `ParserCTPTest.DISABLED_rsp_user_login_last_packet_dereferences_null_api` | KEEP_CONTRACT | 同 158：m_pUserAPI 未创建时强制 OnRspUserLogin 末包（UBSan ParserCTP.cpp:305） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 160 | CT_t_ParserCTPMini | `ParserCTPMiniDisabled.DISABLED_login_success_branch_dereferences_null_api` | KEEP_CONTRACT | BUG-T4-010 已并入 BUG-R105-004 = I03-NEW-130，谓词已修（followup/）；本例前置为未 connect、m_pUserAPI 为空的 parser 收到登录应答，不在 SDK 生命周期内（BUG-T4-004 NOT_BUG 口径），由合法前置（注入 API 会话）的 s3_new130_ctpmini_rsp_error_test.cpp 5 例取代 | 诊断 sanitizer-ubsan(noxml-exit-1)；pre/main 0/20 OK rc=-11 `logs/pre-main-160.log` |
| 161 | CT_t_ParserCTPMini | `ParserCTPMiniDisabled.DISABLED_callback_before_register_spi_uses_wild_sink` | ENABLE | BUG-T4-008 已修：构造置空 m_sink/m_pBaseDataMgr（卡4a），回调与 ReqUserLogin 判空 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-161.log`；post/main 20/20 OK rc=0 `logs/post-main-161.log` |
| 162 | CT_t_ParserCTPMini | `ParserCTPMiniDisabled.DISABLED_init_dlopens_vendor_module` | KEEP_EXTERNAL | init dlopen 厂商行情库；本机缺库时 fail-closed，存在时创建真实 API | 诊断 pass |
| 163 | CT_t_ParserCTPOpt | `T2ParserCTPOptTest.DISABLED_init_and_login_rsp_dereference_missing_ctpopt_library` | KEEP_CONTRACT | GTEST_SKIP：本机无 CTPOpt 厂商库 | 诊断 skipped |
| 164 | CT_t_ParserFemas | `femas_parser.DISABLED_login_rsp_last_true_calls_null_api` | KEEP_CONTRACT | GTEST_SKIP：厂商驱动回调无 API | 诊断 skipped |
| 165 | CT_t_ParserShm | `R76ParserShmTest.DISABLED_NullBaseDataMgrContractPending` | KEEP_CONTRACT | GTEST_SKIP：NULL base data mgr 无合同 | 诊断 skipped |
| 166 | CT_t_ParserShm | `ParserShmNoSpi.DISABLED_subscribe_without_register_spi_uses_uninit_bd_mgr` | ENABLE | ParserShm::subscribe 显式判 _bd_mgr；未 registerSpi 时安全返回（卡4a） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-166.log`；post/main 20/20 OK rc=0 `logs/post-main-166.log` |
| 167 | CT_t_ParserShm | `ParserShmReceiveLoop.DISABLED_receive_path_needs_a_real_mapped_file` | KEEP_EXTERNAL | connect 起线程并等待真实映射文件（sleep 循环），需真实共享内存段 | 诊断 fail |
| 168 | CT_t_ParserUDP | `DISABLED_t6_parserudp_net.connect_binds_ports_and_starts_io_thread` | KEEP_EXTERNAL | connect 绑定真实 UDP 端口并起 io 线程 | 诊断 pass |
| 169 | CT_t_ParserXTP | `XtpLoginF.DISABLED_init_dereferences_null_creator_when_vendor_dylib_is_absent` | KEEP_EXTERNAL | 断言 init 成功需真实 XTP 厂商 dylib（本机缺库现已 fail-closed 返回 false，T1-XTP-01 不再崩） | 诊断 fail |
| 170 | CT_t_ParserXTP | `XtpLoginF.DISABLED_posted_login_handlers_deref_sink_nulled_after_post` | ENABLE | ParserXTP 登录失败 post 的 handler 判 sink；fake API + 本地 pump | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-170.log`；post/main 20/20 OK rc=0 `logs/post-main-170.log` |
| 171 | CT_t_ParserXTP | `XtpData27.DISABLED_state_type_support_scope_is_not_confirmed` | KEEP_CONTRACT | GTEST_SKIP：XTP_TBT_STATE 支持范围未确认 | 诊断 skipped |
| 172 | CT_t_ParserXeleSkt | `T2ParserXeleSktTest.DISABLED_connect_joins_multicast_and_starts_parser_thread` | KEEP_EXTERNAL | GTEST_SKIP：需真实组播环境 | 诊断 skipped |
| 173 | CT_t_TraderCTP | `T25TraderCTPTest.DISABLED_settlement_info_last_row_without_payload_crashes` | ENABLE（改写） | BUG-T25-003 = I03-NEW-131 已按卡14c 修复（followup/）；原用例无断言，改写为启用合同用例 settlement_info_null_last_row_ends_query_with_cached_content（改前 SIGSEGV，改后 20/20），另增空响应与错误末帧两例 | 诊断 sanitizer-ubsan(noxml-exit-1)；pre/main 0/20 OK rc=-11 `logs/pre-main-173.log` |
| 174 | CT_t_TraderCTP | `T25TraderCTPTest.DISABLED_rsp_error_with_null_sink_crashes` | KEEP_CONTRACT | OnRspError NULL sink 已不崩；用例以 ADD_FAILURE 断言“必须崩”，属旧缺陷行为断言 | 诊断 fail |
| 175 | CT_t_TraderCTP | `T25TraderCTPTest.DISABLED_disconnect_hangs_when_no_response_clears_in_query` | ENABLE | BUG-T25-005（213，本地验证通过）：无终帧时 disconnect 有界停止；fake creator，本地线程 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-175.log`；post/main 20/20 OK rc=0 `logs/post-main-175.log` |
| 176 | CT_t_TraderCTP | `T25TraderCTPTest.DISABLED_plugin_created_trader_has_indeterminate_sink` | ENABLE | BUG-T25-001 已修：构造置空 m_sink/m_bdMgr，无 API 时 orderInsert 返回 -1 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-176.log`；post/main 20/20 OK rc=0 `logs/post-main-176.log` |
| 177 | CT_t_TraderCTP | `T2TraderCTPTest.DISABLED_connect_starts_worker_thread_and_needs_real_front` | KEEP_EXTERNAL | 目标路径 connect 需真实 CreateFtdcTraderApi/前置；用例体只剩 disconnect 桩，启用会误报覆盖 | 诊断 pass |
| 178 | CT_t_TraderCTPMini | `ctpmini.DISABLED_disconnect_joins_worker_thread` | KEEP_CONTRACT | GTEST_SKIP：缺线程 shim | 诊断 skipped |
| 179 | CT_t_TraderCTPMini | `ctpmini.DISABLED_order_insert_positive_path_needs_api` | KEEP_CONTRACT | GTEST_SKIP：需 API 实例 | 诊断 skipped |
| 180 | CT_t_TraderCTPMini | `ctpmini.DISABLED_login_and_rsp_user_login_needs_api` | KEEP_CONTRACT | GTEST_SKIP：需 API 实例 | 诊断 skipped |
| 181 | CT_t_TraderCTPMini | `ctpmini.DISABLED_qry_account_last_without_record_crashes` | KEEP_CONTRACT | GTEST_SKIP 占位（无复现体）；m_ayFunds 空 clear 需另核 | 诊断 skipped |
| 182 | CT_t_TraderCTPOpt | `DISABLED_TraderCTPOpt.login_success_and_connect_require_real_ctp_lib` | KEEP_EXTERNAL | 登录成功分支与 connect 需真实 CTP 期权库（UBSan TraderCTPOpt.cpp:1303 空 API） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 183 | CT_t_TraderCTPOpt | `DISABLED_TraderCTPOpt.order_action_rsp_null_rspinfo_derefs_errormsg` | ENABLE | OnRspOrderAction 空 pRspInfo 只在错误分支解引用，空信息安全；无显式断言 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-183.log`；post/main 20/20 OK rc=0 `logs/post-main-183.log` |
| 184 | CT_t_TraderDumper | `dumper.DISABLED_run_once_loops_forever_and_run_threaded_spawns_worker` | KEEP_CONTRACT | GTEST_SKIP：run(true) 无限轮询/run(false) 起线程，缺停止接缝 | 诊断 skipped |
| 185 | CT_t_TraderDumper | `trader_adapter.DISABLED_on_rsp_trades_always_derefs_null_api` | KEEP_CONTRACT | GTEST_SKIP 占位 | 诊断 skipped |
| 186 | CT_t_TraderDumper | `trader_adapter.DISABLED_login_and_connect_success_paths_deref_null_api` | KEEP_CONTRACT | GTEST_SKIP 占位 | 诊断 skipped |
| 187 | CT_t_TraderFemas | `TraderFemasTest.DISABLED_user_login_success_branch_needs_vendor_api` | KEEP_EXTERNAL | 登录成功分支需真实 CUstpFtdcTraderApi（sentinel 指针上调用，UBSan） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 188 | CT_t_TraderFemas | `TraderFemasDisabled.DISABLED_query_account_before_connect_dereferences_null_strand` | KEEP_CONTRACT | 注释“待验证”：人工注入 sentinel API + WS_ALLREADY 绕过生命周期前置，可达性未确认 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 189 | CT_t_TraderFemas | `TraderFemasDisabled.DISABLED_init_dlopens_vendor_module` | KEEP_EXTERNAL | 需宿主安装真实 USTPtraderapiAF 厂商模块 | 诊断 fail |
| 190 | CT_t_TraderMocker | `R113MockerLoginRetry.DISABLED_ReentrantLoginConnectContractPending` | KEEP_CONTRACT | GTEST_SKIP：worker 活动时重复 login/connect 无公开合同 | 诊断 skipped |
| 191 | CT_t_TraderMocker | `R60MockerTest.DISABLED_OpenOrderSuccessMessageContractUnproven` | KEEP_CONTRACT | GTEST_SKIP：成功开仓 state_msg 文案无合同 | 诊断 skipped |
| 192 | CT_t_TraderMocker | `DISABLED_t6_thread_paths.login_spawns_match_thread_and_sleeps` | ENABLE | login 起本地撮合线程、release join，无端口；原禁用为 lane“不起线程”约束而非合同 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-192.log`；post/main 20/20 OK rc=0 `logs/post-main-192.log` |
| 193 | CT_t_TraderMocker | `DISABLED_t6_thread_paths.connect_binds_real_udp_socket_and_runs_io_context` | KEEP_EXTERNAL | connect 真实 open/bind UDP 端口 19827 并起 io 线程 | 诊断 pass |
| 194 | CT_t_TraderMocker | `DISABLED_t6_hang_paths.split_volume_with_zero_bounds_spins_forever` | ENABLE | splitVolumeWithRng 对 minQty==0/空区间直接返回空，死循环已消除 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-194.log`；post/main 20/20 OK rc=0 `logs/post-main-194.log` |
| 195 | CT_t_TraderXTP | `TraderXTPTest.DISABLED_connect_calls_null_creator_and_writes_flow_dirs` | ENABLE | connect 无 creator 时记错误返回（fail-closed）；锁由 slice4-result 转交后去前缀（followup/），新名 20/20；无显式断言，作不崩溃回归 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-195.log` |
| 196 | CT_t_TraderXTP | `TraderXTPTest.DISABLED_query_apis_dereference_null_api_without_guard` | KEEP_CONTRACT | _api 为 NULL 时 queryAccount 现返回 -1（fail-closed）；用例断言 0（成功），与现行合同冲突 | 诊断 fail |
| 197 | CT_t_TraderYD | `TraderYDTest.DISABLED_login_success_branch_needs_real_api` | KEEP_EXTERNAL | notifyLogin 成功分支需真实 YD API（UBSan TraderYD.cpp:548） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 198 | CT_t_TraderYD | `TraderYDTest.DISABLED_order_action_with_valid_entrustid_dereferences_null_api` | KEEP_CONTRACT | api=NULL 时 orderAction 现返回 -1；用例断言 0（成功），与 fail-closed 合同冲突 | 诊断 fail |
| 199 | CT_uftcore | `DISABLED_n03_trader_adapter.uninitialized_deref_class` | KEEP_CONTRACT | 迭代02 裁定 BUG-N03-005/N03-007 为 TEST：未 init/run 的适配器与裸代码均违反公开前置；UBSan 仍空指针（TraderAdapter.cpp:1674） | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 200 | CT_uftcore | `p06_uft_context.DISABLED_uninitialized_trader_and_param_class` | KEEP_CONTRACT | 迭代02 裁定 BUG-P06-004 为 TEST：未 setTrader 的上下文与未 init 的适配器无执行合同；UBSan 仍报错 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 201 | CT_uftcore | `p06_uft_engine.DISABLED_uninitialized_and_malformed_stdcode_class` | KEEP_CONTRACT | 迭代02 裁定 BUG-P06-001/P06-003 为 TEST：引擎 init 前查询与非标准代码不在合同内；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 202 | CT_uftcore | `DISABLED_R54UftCtx.tick_without_contract_info_crashes_with_local_position` | ENABLE | BUG-R54-002（PRODUCT）后上下文对无 ContractInfo 的 tick 安全；有持仓前置断言 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-202.log`；post/main 20/20 OK rc=0 `logs/post-main-202.log` |
| 203 | CT_uftcore | `DISABLED_q04_trader_buysell.missing_default_action_group` | ENABLE | 缺 default 动作组不再解引用 end()（BUG-N01-001 同源，PRODUCT）；无显式断言 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-203.log`；post/main 20/20 OK rc=0 `logs/post-main-203.log` |
| 204 | CT_uftcore | `DISABLED_q04_trader_entrust.bare_code_with_initialized_api_probe` | KEEP_CONTRACT | 迭代02 裁定 BUG-N03-007 为 TEST：公开方法参数约定为 stdCode，裸代码违反前置（现已不崩，不固化） | 诊断 pass |
| 205 | CT_uftcore | `DISABLED_q04_trader_push.null_base_datamgr_and_no_statmap_class` | KEEP_CONTRACT | 第二部分向 initExt 传 nullptr 基础数据管理器：必需依赖人为置空（迭代02 BUG-R175-003 TEST 口径） | 诊断 pass |
| 206 | CT_uftcore | `DISABLED_q04_trader_reply.uninitialized_and_null_deref_class` | KEEP_CONTRACT | 同 199：未 initExt/未 run 的适配器（BUG-N03-005、BUG-P06-001 TEST）；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 207 | CT_uftcore | `DISABLED_q04_uftctx_local.uninitialized_trader_and_shared_param_class` | KEEP_CONTRACT | 同 200（BUG-P06-004 TEST）；ASan 仍 SEGV | 诊断 sanitizer-asan(noxml-exit-1) |
| 208 | CT_uftcore | `R123UftEngine.DISABLED_get_kline_slice_never_reaches_subscription` | KEEP_CONTRACT | BUG-R123-001 已按迭代01 复核不计缺陷：IUftStraCtx 将 K 线切片标注为暂未实现，启用测试断言返回 NULL；本例期望非空与现行接口冲突 | 诊断 fail |
| 209 | CT_uftcore | `R163UftTrader.DISABLED_NullApiIsAnInvalidDependencyProbe` | KEEP_CONTRACT | GTEST_SKIP：nullptr ITraderApi 不在合同内 | 诊断 skipped |
| 210 | CT_wtcore | `N01ParserAdapter.DISABLED_null_bd_mgr_level2_callback_crash` | KEEP_CONTRACT | 未 init 的 ParserAdapter（NULL _bd_mgr）：迭代02 BUG-N01-002/N01-006 认定该前置无效；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 211 | CT_wtcore | `N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map` | KEEP_CONTRACT | 空占位：只构造未使用的 adapter2，无产品路径与断言 | 诊断 pass |
| 212 | CT_wtcore | `R71DtMgrTest.DISABLED_InitNullCfgCrashes` | KEEP_CONTRACT | BUG-R71-001 未见任何登记；NULL cfg 为必需配置人为置空，同类已裁 TEST（R68-001、R44-006）；UBSan 仍空指针 | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 213 | CT_wtcore | `N02TickerDisabled.DISABLED_UnverifiedSelHftPreInitPaths` | KEEP_CONTRACT | NULL 引擎 + NULL tick 构造/调用，名称即标注未验证，无合同 | 诊断 pass |
| 214 | CT_wtcore | `R101TraderReport.DISABLED_NetShortCloseOverDeductsNewPosition` | ENABLE | BUG-R101-001 已修：净头寸平空超量反手开多，今仓 2 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-214.log`；post/main 20/20 OK rc=0 `logs/post-main-214.log` |
| 215 | CT_wtcore | `R73DtMgrTest.DISABLED_EmptyStoreTickSliceCrashesInReader` | KEEP_CONTRACT | BUG-R73-001 已不崩；空库非复权路径返回 reader 的非 NULL 切片，用例断言 NULL，空结果返回形式（NULL 或空切片）无合同 | 诊断 fail |
| 216 | CT_wtcore | `R73HftCtxTest.DISABLED_SellUnknownProductDereferencesNullCommodity` | ENABLE | BUG-R73-002 已修：未知品种 stra_sell 返回空（卡6 未知代码拒绝） | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-216.log`；post/main 20/20 OK rc=0 `logs/post-main-216.log` |
| 217 | CT_wtcore | `P02ExecuterCrashClass.DISABLED_ChannelReadyWithoutTraderAndBadPolicyConfigs` | KEEP_CONTRACT | 空函数体占位（仅登记缺陷类），无产品调用 | 诊断 pass |
| 218 | CT_wtcore | `P02EngineCrashClass.DISABLED_NullHotMgrAndUnseededBaseDataPaths` | KEEP_CONTRACT | 空函数体占位（仅登记缺陷类），无产品调用 | 诊断 pass |
| 219 | CT_wtcore | `P11CrashClass.DISABLED_report_paths_that_dereference_null_before_run` | KEEP_CONTRACT | 首个 UB 为资金数组中人为插入的 NULL 元素（saveData，TraderAdapter.cpp:1341）：迭代02 裁定 BUG-P11-002 为 TEST | 诊断 sanitizer-ubsan(noxml-exit-1) |
| 220 | CT_wtcore | `Q01SelPositionTest.DISABLED_SelCrashClass_null_history_mgr` | KEEP_CONTRACT | 裸引擎 _data_mgr=NULL 的人工注入状态：迭代02 BUG-P02-004 认定空 manager 注入非正常 Runner 路径（现已不崩，不固化） | 诊断 pass |
| 221 | CT_wtcore | `R192SelEngine.DISABLED_HfqFlagBitsScaleVolumeTurnoverAndInterest` | ENABLE | BUG-R192-001（主记录 BUG-P02-003，PRODUCT）已修：后复权标志位缩放量/额/持仓 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-221.log`；post/main 20/20 OK rc=0 `logs/post-main-221.log` |
| 222 | CT_wtcore | `R211SelEngine.DISABLED_ExpiredTaskTerminatesBeforeSchedule` | ENABLE | BUG-R192-002/003 已修；日任务 931 于 930 分钟闭合时触发 1 次，与卡 K27=A 一致 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-222.log`；post/main 20/20 OK rc=0 `logs/post-main-222.log` |
| 223 | CT_wtcore | `R211HftStrategyMgr.DISABLED_CtaPluginTypePunCrashesOnRegistration` | ENABLE | BUG-R31-002（213，待验证）按卡4b=A：HFT 管理器跳过 CTA 版本化插件，loadFactories 返回 true；与启用的 hft_refuses_cta_plugin_before_creator 一致 | 诊断 pass；pre/main 20/20 OK rc=0 `logs/pre-main-223.log`；post/main 20/20 OK rc=0 `logs/post-main-223.log` |
| 224 | CT_wtcore | `r31_stra_mgr.DISABLED_hft_loads_cta_plugin_via_shared_symbol_name` | KEEP_CONTRACT | 断言 HFT 管理器能用 CTA 插件创建策略（类型双关旧行为）；现行为返回 NULL 与修复方向一致 | 诊断 fail |
