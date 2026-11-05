# 迭代03 最终整体回归验收证据（2026-11-05）

按 `迭代计划.md` 阶段 4（整体回归验收）执行：重新冻结最终源码与构建输入，干净构建，逐例完整回归，并按适用范围执行 ASan+UBSan、LSan、TSan 与独立夹具套件。发现问题时按“归因修复 → 冻结 → 回归”循环，直到门槛满足；每轮证据与源码版本分开。本机为 macOS arm64，Windows/MSVC、Linux MSan、真实厂商与生产环境**未执行**，不计通过。

## 1. 冻结链与最终版本

| 冻结 | 时间 | 清单摘要 | 相对上一版 | 用途 |
|---|---|---|---|---|
| F | 16:14:44 | `6877360f…` | 第一轮后全部复修与缺口补齐 | 普通 6342/0 失败；ASan 暴露 3 例间歇失败（I03-NEW-136） |
| F2 | 17:07:01 | `5aaebd2a…` | 3 个测试文件 | 被 F3 取代 |
| F3 | 17:40:25 | `4de5b189…` | F2 + 3 个执行器析构（I03-NEW-137） | 全阶段；TSan 发现 I03-NEW-116 补充、138、139 |
| **F4** | **20:28:32** | **`e9f734487a6a49f706bad47e178f739c7b11badab0b054ef76c6237c1a24427f`** | 76 个夹具文件 + MQServer.cpp、TraderAdapter.{cpp,h}、T26 stub 与其测试 | **最终全量验收树** |
| **F5** | **22:38:51** | **`3f77fd17554bb47b1960fe7f0ed30ab044a1b5f74ee7b170b8a4d852f96a3ffa`** | **仅 `src/CTPLoader/TraderSpi.cpp`（I03-NEW-141）** | **定向复核：编入该文件的两个测试二进制 + 9 个相关夹具** |

- HEAD 均为 `ccaad010`，工作区未提交修改见 `meta/git-status.txt`，已跟踪文件二进制 diff 见 `meta/tracked.diff.gz`，逐文件哈希见 `meta/tree.sha256.gz`（F4，6717 个文件），`f5/diff-vs-F4.txt`。
- 每次冻结均以 `rsync -a --no-links` 复制 src 与 dist 到隔离目录，校验无符号链接，复制后与工作区 `diff -rq` 一致（`meta/post-copy-diff.txt` 为空）。构建与运行只使用冻结副本。
- F4 冻结树含 `src/default.profraw`（本会话测试运行产生、git 忽略、不参与编译）。
- 夹具运行期间一个套件脚本用不同版本 ninja 对 `final4/build` 执行 `-t deps`，删除了 `.ninja_log`；编译产物与其余文件未变（`fixtures/F4-integrity/`），不影响结果。

## 2. 构建

- 普通：`cmake -S <冻结>/src -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=/opt/homebrew/opt/boost/include`，Apple clang 17.0.0，`WT_WARNINGS_AS_ERRORS=ON`（缺省），configure=0、build=0、**0 警告**，45 个测试二进制（`meta/build-status.txt`、`meta/build-warnings.txt`、`meta/cmake-cache-key.txt`）。F5 普通全量构建同样 0 警告（`f5/build-warnings.txt`）。
- ASan+UBSan：Homebrew LLVM 22.1.8，`-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined`（alignment 按既有口径排除，见 I03-NEW-109）；TSan：`-fsanitize=thread`。两者用 `tools/llvm-implicit-usrlocal.cmake` 修正 `/usr/local/include`（Boost 1.81）排在 coverage Boost bridge 之前的问题（`tools/san-configure.sh`、`tools/final_pipeline.sh`）。
- 流水线：`tools/final_pipeline.sh <dest> freeze build run disabled asanbuild asanrun lsanrun tsanbuild tsanrun`，日志 `meta/pipeline.log`；F5 定向 `f5/pipeline.log`。

## 3. 运行结果（F4 全量；F5 定向替换两个 CTPLoader 二进制）

逐例独立进程、独立 cwd、600 s 超时；用例集合与模块预置同第一轮（`tools/run_full.sh`、`tools/run_regression.py.copy`）。

| 运行 | 用例 | 结果 | 目录 |
|---|---:|---|---|
| 普通 | 6342 | pass 6338、skipped 4 | `runs/run/` |
| 禁用用例诊断 | 163 | pass 53、fail 39、skipped 32、signal-11 30、timeout 4、其他 5（不计通过） | `runs/run-disabled/` |
| ASan+UBSan | 6342 | pass 6338、skipped 4，0 报告 | `runs/run-asan/` |
| LSan 普查 | 6342 | pass 5109、LSan 报告 1220、fail 9、skipped 4 | `runs/run-lsan/`（`lsan-triage.txt`） |
| TSan | 6342 | pass 6181、退出期 signal-6 100、线程泄漏 53、fail 3、timeout 1、skipped 4；**数据竞争 0** | `runs/run-tsan/`（`triage.txt`） |
| TSan 工具限制复核 | 104 | 插入 `dlclose` 空实现（`runs/run-tsan-nodlclose/nodlclose.c`）：pass 95、fail 9（8 例断言模块已卸载、1 例 r14 fork+exec） | `runs/run-tsan-nodlclose/` |
| F5 两个 CTPLoader 二进制 | 66 | 普通 66/66；ASan 66/66；TSan 65 + 1 例 `std::cerr` 标准流报告（I03-NEW-140） | `f5/run*/` |
| 独立夹具（113 个） | 113 | pass 91、not-applicable 13、fail 6、expected-red 2、blocked-env 1 | `fixtures/final-combined-summary.json` |

跳过 4 例：`R22PorterCapiTest.ConfiguredStartChildChecksStateMonitor`、`R19ExecChild.protocol`（仅由父用例拉起）、`R44ParserAdapterEdgesTest.RepeatedInitExtNeedsContract`（合同未定）、`UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`（I02-NEW-137，卡15）。

## 4. 归因

- **LSan**：71 个直接泄漏点（`runs/run-lsan/lsan-triage.txt`），主要为测试夹具未释放的合约/品种/会话与配置对象、按设计常驻的读端与 HotMgr 规则表、系统库一次性分配；涉泄漏的条目（I02-NEW-034/053/067/104/120、R42-001、I03-NEW-016/024/033/067 等）逐项核对其覆盖用例，未见归属被测产品代码的泄漏。产品侧执行器配置泄漏 I03-NEW-137 在 F3 修复（F→F3 `WTSVariant::createObject` 类由 322 例降至 221 例）。9 例 fail 为 LSan 改写 help 子进程退出码。
- **TSan**：退出期 SEGV/BUS 100 例及 `r85_uft_runner` 2 例、`r95_uft_parser` 超时为 TSan atexit/dlclose 工具限制（dlclose 空实现下通过，余 8 例为断言模块已卸载的用例，属预期）；`r14_quote_cli.valid_config_with_real_writer_returns_zero_after_signal` 为 TSan 插桩多线程进程 fork+exec 工具限制（同一 TSan 版 QuoteFactory 直接运行正常，`logs/tsan-qf-direct.txt`）；线程泄漏 53 例为内嵌 boost threadpool（第三方，退出期）。F3 的 MQServer、TraderAdapter 统计表、T26 stub 竞争在 F4 消失。
- **独立夹具**：34 个第一轮失败/受阻套件由 5 个代理处置（`fixtures-*/README.md`、`fixtures/fixfix-*.json`）：27 个 STALE_FIXTURE 已修正为当前合同并通过；7 个 SUPERSEDED（`i03_frequency`、`p04_cross_session_clock`、`r13_real_schedule`、`r13_sel_period_contract`、`r3_cancel_batch_cache`、`r3_split_reject`、`r7_twap_sell_sign`）逐一列出取代的 coverage 用例。未发现产品回归；F4 重跑另发现 CTPLoader 超时重复结束（I03-NEW-141），F5 修复后 9 个相关套件全部通过。
- **L00-004**：公开头编译自足检查在 F5 树复跑 13/13 exit 0（`l00-004-header-check/`）。

## 5. 本阶段修复（红/绿证据）

| 目录 | 内容 |
|---|---|
| `../20261105-final-sanitizer-fixes/` | 预扫与 I03-NEW-108/109（测试侧）/110/111/112/113/114/116 |
| `../20261105-final-gaps-slice{1..4}-result/` | 25 项本地缺口、I03-NEW-115/117/122/123/124/128–135/138、禁用用例处置 |
| `f2-fixes/` | I03-NEW-136（3 个间歇失败测试） |
| `f3-fixes/` | I03-NEW-137（执行器析构释放配置） |
| `f4-fixes/` | I03-NEW-116 补充（MQServer 第二处锁外判空）、I03-NEW-139（T26 stub） |
| `f5-fixes/` | I03-NEW-141（CTPLoader 超时唯一结束） |

## 6. 逐项结论

逐项结论由 `tools/build_final.py` 根据复核分类（`tools/overrides.json`、各 slice 复核结果）、最终运行结果与夹具结果计算，写入 `../../../执行跟踪.json` 各行 `最终验收` 字段，并汇总为 `逐项验收.json` 与 `../../../最终验收报告.md`。规则：
- 已验收：复核判定验收条件全部本机可验且已满足，覆盖用例在最终普通/ASan 回归通过、TSan 无未归因报告，依赖夹具通过或已被 coverage 取代。
- 本地验证通过：同上，但剩余外部条件。
- 阻塞：卡15 用户指示忽略或待用户裁定。
- 任何覆盖用例出现未归因的非通过即降为待验证（最终为 0 项）。
