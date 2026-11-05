# 迭代03 待验证项无链接重编与定向复跑（2026-11-03 至 11-04）

范围：BUG-R14-004、BUG-R58-003、BUG-R10-005、BUG-L05-002、BUG-T4-002、BUG-I02-NEW-019、BUG-I02-NEW-020、BUG-I02-NEW-053。仅做验证：未修改任何产品源码、测试源码或台账文件；本目录是唯一写入仓库的内容。macOS arm64，HEAD `ccaad010e9169f610aded4d8dc0f5647f70a5cff`，工作区带大量未提交并行修改。**本次只跑与各行直接相关的用例，不是整体回归；fake/模拟器结果不覆盖真实柜台、Windows 或生产。**

## 1. 构建环境与无链接约束

- 构建目录（全新空目录）：`/tmp/wt-i03-reverify-20261103/build`（Apple clang，覆盖率插桩）与 `/tmp/wt-i03-reverify-20261103/build-asan`（Homebrew LLVM 22.1.8，`-fsanitize=address,undefined -fno-omit-frame-pointer`，同时带覆盖率插桩）。
- Boost：把 `/opt/homebrew/Cellar/boost/1.88.0/include/boost` 复制到 `/tmp/wt-i03-reverify-20261103/boost-include/boost`，`diff -rq` 与源一致，`find -type l` 为 0。
- 第一次构建的依赖文件显示产品静态库仍通过 `/opt/homebrew/include/boost`（符号链接）解析 Boost：`CMAKE_CXX_FLAGS` 的 `-I` 被排在目标自身 `-I/opt/homebrew/include` 之后（见 `build/b3-build-homebrew-boost-order.log`）。因此改用编译器 launcher（`scripts/boost-first-launcher.sh`，LLVM 用 `scripts/boost-first-launcher-llvm.sh`，后者另加 `-include stdlib.h`，否则 Homebrew fmt 在 libc++22 下缺 malloc/free，见 `build/asan1-build-fail-stdlib.log`）把实体 Boost 目录放到搜索首位，并整树重编。最终 `ninja -t deps` 中经 `/opt/homebrew/include/boost` 或 `/usr/local/include/boost`（1.81）解析的 Boost 头为 0（`build/nonphysical-boost-dep-count.txt`）；构建、sanitizer 构建、fixture 目录及 Boost 副本 `find -type l` 为 0（`build/symlink-count.txt`）；覆盖率 bridge `cov_boost_include/boost` 为实体目录。其他第三方头/库（fmt、spdlog、gtest、zstd、rapidjson）仍来自 Homebrew 或 `/usr/local`，未声称全依赖为实体路径。
- 配置命令（普通树）：

```sh
cmake -S src -B /tmp/wt-i03-reverify-20261103/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DBoost_INCLUDE_DIR=/tmp/wt-i03-reverify-20261103/boost-include \
  -DCMAKE_CXX_FLAGS=-I/tmp/wt-i03-reverify-20261103/boost-include -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# 随后：-DWT_WARNINGS_AS_ERRORS=OFF（原因见下）、-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=scripts/boost-first-launcher.sh
```

  sanitizer 树同上，编译器换为 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang{,++}`，C/CXX/EXE/SHARED/MODULE 链接标志加 `-fsanitize=address,undefined`，launcher 为 `boost-first-launcher-llvm.sh`。每次构建由 `scripts/phase_build.sh <tag> <dir> <targets>` 执行（构建前后对 `src/` 与 `dist/common` 全量 sha256）。
- **-Werror 破损（新发现，非本 8 项范围）**：按仓库默认 `WT_WARNINGS_AS_ERRORS=ON`（APPLE），当前工作区 `src/WTSTools/WTSDataFactory.cpp:1334` 未使用参数 `bIncludeOpen` 致编译失败，exit 1（`build/build1-werror-fail.log`）。本次仅作为本地临时手段设 `-DWT_WARNINGS_AS_ERRORS=OFF` 继续；同时存在的警告（`WtStraDtSel.cpp` 两个未用变量、`unresolved_report_journal_test.cpp` range-loop 复制）见 `build/b6-build.log`。在默认策略下当前树不能编译通过。
- **主目标 CoverageTests 不可用（新发现）**：已提交文件 `src/TestUnits/coverage/p08_btcore_mockers3/r20_mq_fixture/notifier_check.cpp` 自带 `int main`，被主目标 `GLOB_RECURSE` 收入并取代 gtest_main（`round2/logs/CoverageTests-main-symbol.txt`：`main at notifier_check.cpp:17`）；主目标运行任何过滤器都无输出地 exit 2。L05 的 in-tree lane（`l05_dt_servo_porter`，属主目标）因此无法复跑。另外第一轮主目标还因当时并行修改中的 `p09_rdm_window_range_test.cpp` 与 `WTSDataDef.hpp` 不一致编译失败（`build/b4-build.log`、`b5-build.log`），第二轮已可链接。

## 2. 两轮运行与树的对应关系（请勿混用）

| 轮次 | 二进制 | 构建前快照 | 对应源码状态 |
|---|---|---|---|
| 第一轮普通（`round1/`，`results-normal.json`） | b5 增量构建 | 2026-11-03 21:51 | 构建期仅 `ParserXeleSkt` 变化（不在依赖内）。**早于**其后 `WTSBaseDataMgr.cpp`、`WTSDataDef.hpp`、`r130_misc_tail_test.cpp`、`WtDiffExecuter.cpp`、`WTSDataFactory.cpp`、`r90/r170` 测试的修改。`WTSContractInfo.hpp`、`WTSCollection.hpp`、`WtEngine.{h,cpp}`、`CtaMocker/SelMocker.cpp`、`HisDataReplayer`、`TraderAdapter` 的修改发生在 b3/b4 期间，已包含在 b5 中。 |
| 第一轮 sanitizer（`round1/results-asan*.json`） | asan2 | 2026-11-03 约 22:3x | 构建期间 `WTSDataDef.hpp`、`WTSBaseDataMgr.cpp`、`r130` 变化且在依赖内，**混合快照**，仅作参考。 |
| 第一轮 Femas 手工构建（`round1/femas/`） | 手工编译 | 约 22:2x | TraderFemas 源及测试至最终未变；所含公共头早于 `WTSDataDef.hpp` 修改。 |
| 第一轮 L05 fixture | 第一轮库 | — | 链接 b5 的 WTSTools/WTSUtils，早于 BaseDataMgr/WTSDataDef 修改。 |
| **第二轮普通（`round2/results-r2.json`）** | b6 增量（-j2） | 2026-11-04 02:23 | 构建期 `WTSDataFactory.cpp` 与 `r90/r170` 测试变化；最终 `ninja -n` 显示无待重编项（`build/ninja-uptodate-final.txt`），即二进制与 final 快照一致。 |
| **第二轮 sanitizer（`round2/results-asan-r2.json`）** | asan3 | 2026-11-04 02:32 | 构建与运行期间依赖无漂移，`ninja -n` 为 0。 |
| 第二轮 L05 fixture | b6 的 WTSTools/WTSUtils 重链接 | — | 与 final 一致。 |

final 快照 2026-11-04 02:35:03（`final.sha256`）。起始到最终有 54 个非 fixture 源文件变化（`src-start.sha256` 对比）。关键文件逐阶段哈希见 `hashes.txt`；二进制哈希见 `binary-hashes.txt`。最终结论以第二轮为准；它代表 02:35 的工作区，而不代表其后任何并行修改或最终整合树。

运行方式：`scripts/run_cases.py <cases.json> <results.json>`，每例单独进程，cwd/TMPDIR 为 `/tmp/wt-i03-reverify-20261103/run/<id>/<case>`，`perl -e 'alarm 300; exec @ARGV'` 限时，`--gtest_filter=<case> --gtest_output=xml:...`，`LLVM_PROFILE_FILE` 指向该运行目录。sanitizer 运行统一 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`LSAN_OPTIONS=exitcode=23:report_objects=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，未用 suppression。LLVM22 LSan 正控 `round1/femas/leak-control.log` 确实报告泄漏，空程序 smoke exit 0，工具在本机有效。

## 3. 逐项结果

### BUG-R14-004（QuoteFactory / WtBtRunner 入口）
- 用例：原证据 13 例（QuoteFactory 8、WtBtRunner 5），目标 `CoverageTests_m_QuoteFactory`、`CoverageTests_m_WtBtRunner`，依赖 QuoteFactory/WtBtRunner/WtDataStorage 一并重编。
- 第一轮与第二轮均为 **12/13**：`r14_wtbtrunner_cli.selTaskDefaultsAllowRunWhenTemplatesAreOmitted` 失败（exit 1），子进程输出 `Preparing historical data replayer failed`，退出码 255，期望 0（`round2/logs/R14-004-r2--r14_wtbtrunner_cli.selTaskDefaultsAllowRunWhenTemplatesAreOmitted.log`）。其余 12 例（含真实 writer 就绪后 SIGINT）通过。
- 判断：`HisDataReplayer.cpp/.h` 在本次 b4 构建期间被并行修改（相对 HEAD +777/−319 行，`prepare()` 新增区间/监听/费用/日期映射门禁），失败指向 `prepare()` 返回 false；具体是哪一道门禁未确证（lldb 定位未取得输出）。这是相对 2026-11-02 证据的回归，不能视为通过。

### BUG-R58-003（回放器 basefiles 失败传播）
- 用例：原证据 7 例 `R43ReplayerInitBasefilesTest.*`，目标 `CoverageTests_btcore`。
- 两轮均为 **6/7**：`badAdjFactorFileDoesNotBreakInit` 在 `r43_replayer_init_basefiles_test.cpp:428` `replayer.prepare()` 返回 false（`init` 本身成功）。必需 session/commodity 文件缺失/损坏、commodity 数组、诊断路径、有效配置等 6 例通过。
- 与 R14 同源：可选 adjfactor 损坏后 init 仍成功（本项核心合同成立），但随后的 prepare/run 收尾被当前并行 HisDataReplayer 修改阻断。

### BUG-R10-005（EventNotifier 必需导出）
- 用例：原证据 4 例，目标 `CoverageTests_wtcore`（含 CMake 内建的三个 fake MQ 模块）。
- 普通：两轮 **4/4**，exit 0。
- ASan/UBSan（第二轮）：4/4 断言通过，无 ASan/UBSan 错误；两例缺导出用例 LSan exit 23（缺 register 150 字节、缺 create 140 字节，均为 strdup）。泄漏大小精确等于该运行的 `LLVM_PROFILE_FILE` 路径长度 +1，栈位于已卸载模块；同一二进制取消 `LLVM_PROFILE_FILE` 后两例 exit 0（`R10-005-asan-noprofile-r2`）。强烈指向被 dlopen/dlclose 的覆盖率插桩模块中 profile 运行时，而非 notifier 产品代码，但根因未确证。

### BUG-L05-002（CSV 导出/存储写失败）
- in-tree lane：主目标 CoverageTests 因上述 `notifier_check.cpp` main 冲突 exit 2、无输出，**未能运行**（`round2/logs/L05-002-r2--lane_DumpBars_DumpTicks.log` 为空）。
- fixture（以本次新构建 WTSTools/WTSUtils 替换原 `/tmp/wt-i03-r41-drain` 预编库，实体 Boost，LLVM22；CMake 改动仅为路径替换，见 `scripts/*.cmake.diff`）：`i03_csv_status_20261103` ASan/UBSan **14/14**（金样由保存的改前源码重新生成），`i03_csv_block_type_20261103` UBSan **10/10**；两轮一致。`nm` 确认四个导出 `dump_bars/dump_ticks/_v2` 存在。fixture 的 `.d` 文件中 Boost 全部来自实体副本。LSan 关闭（`detect_leaks=0`，沿用 fixture 定义）。
- 剩余事项中 strFilter 未生效、空目录返回成功、未知版本/标识负向分支、旧 void 接口无返回、事务发布、真实磁盘故障、Windows 均未因此改变。

### BUG-T4-002 / BUG-I02-NEW-019（TraderFemas）
- 普通：5 例定向两轮 **5/5**；补充运行整个 `CoverageTests_t_TraderFemas`（66 例，3 disabled）**63 通过 / 0 失败**（原记录 48 PASS/2 FAIL）。原证据追加的 2 例（`femas_extra.cpp`，手工编译）**2/2**。
- ASan/UBSan/LSan（in-tree 第二轮，及手工 LLVM22 构建）：持仓两例、`make_order_info` 两例 exit 0，无错误无泄漏；`order_query_and_push_callbacks` 及追加例 `isolated_query_push_same_user_order_id` 断言通过但 LSan exit 23，56 字节泄漏，栈为 `TraderFemas::OnRspQryOrder` 创建的 `m_ayOrders` WTSArray 及其内部 vector。代码检查：`m_ayOrders/m_ayTrades/m_ayPosDetail` 仅 `clear()`，析构函数为空、`release()` 也不释放容器——**产品端容器泄漏（新发现）**，不在测试夹具。
- 因此：T4-002 持仓路径在本机 fake 下 sanitizer 干净；NEW-019 所在订单查询路径有真实产品泄漏。

### BUG-I02-NEW-020（TraderMocker 撤单引用 / UDP 对齐）
- 普通：两轮 **3/3**。
- 全插桩 ASan/UBSan（整个 t_TraderMocker 目标及其依赖重编插桩，比原证据范围更大）：3/3 断言通过，无 use-after-free、无 UBSan 未对齐报告；两例撤单用例 LSan exit 23，232 字节，栈为 `TraderMocker::orderInsert` 中创建的 `_awaits`（`OrderCache`/WTSHashMap）。代码检查：析构只释放 `_orders/_trades/_ticks`，**`_awaits` 从未释放（新发现，模拟器产品泄漏）**。UDP 对齐例 exit 0。
- 对象池同址复用、关闭生命周期仍未证明。

### BUG-I02-NEW-053（P11/R59 测试夹具引用平衡）
- 普通：组合 4 例 **4/4**、单例 5/5（含未知合约 `R59Misc.push_trade_without_contract_is_ignored`）。
- 全插桩 ASan/UBSan/LSan（TraderAdapter 与整个 wtcore 目标重编插桩，比原证据“只插桩 3 个 TU”更完整）：组合设 profile 4/4 断言通过但 exit 23（84 字节）；取消 `LLVM_PROFILE_FILE` 同二进制 4/4 exit 0；单例 `init_rejects_overlong_ids_and_keeps_empty_id_accepted` 设 profile exit 23（137 字节）/ 取消后 exit 0；其余单例 exit 0。泄漏大小与 profile 路径长度 +1 精确吻合，栈为 strdup 于已卸载模块——复现了原证据现象，仍未确证根因。

## 4. 文件
- `round1/`、`round2/`：各例 `logs/<id>--<case>.log`、`xml/`、`cases*.json`、`results*.json`（含 argv、cwd、退出码、XML 计数）、`run*.log`；`round1/femas/` 为手工 Femas 构建与运行。
- `build/`：配置/构建日志、-Werror 失败、Boost 次序问题、符号链接与 Boost 来源计数、`ninja -n` 结果。
- `scripts/`：运行器、构建脚本、launcher、fixture CMake 路径替换 diff。
- `hashes.txt`、`final.sha256`、`src-start.sha256`、`binary-hashes.txt`。
