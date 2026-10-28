# R14 manifest — 四个「整文件 0.00%」可执行体/工具模块（第七波 lane R14）

- 波次: 迭代01 第七波，路 14；派生自 `manifests/api-brief-R14.md`
- 交付用例（4 个新目录，**未改任何 CMakeLists**；`WT_MAIN_MODULES` 循环从磁盘派生目标，目录里有 .cpp 即成目标，需主控重新 configure）:

| 用例文件 | 启用 | DISABLED_ |
| --- | --- | --- |
| `src/TestUnits/coverage/m_QuoteFactory/r14_quotefactory_main_test.cpp` | 8 | 2 |
| `src/TestUnits/coverage/m_WtUftRunner/r14_wtuftrunner_main_test.cpp` | 12 | 3 |
| `src/TestUnits/coverage/m_WtLatencyUFT/r14_uftlatency_main_test.cpp` | 4 | 3 |
| `src/TestUnits/coverage/m_WtLatencyHFT/r14_hftlatency_main_test.cpp` | 4 | 3 |
| 合计 | **28** | **11** |

- 生产文件（只读，一行未改）: `src/WtUftRunner/WtUftRunner.cpp`(387 行/missed 285)、`src/WtUftRunner/main.cpp`(入口 TU)、
  `src/WtLatencyHFT/HftLatencyTool.cpp`(507/missed 219)、`src/WtLatencyUFT/UftLatencyTool.cpp`(328/missed 191)、
  `src/QuoteFactory/main.cpp`(331/missed 185)。四个模块目录各自的 `main.cpp` 一并编进对应二进制。
- 入口宏: 构建期由 `src/TestUnits/coverage/CMakeLists.txt` 的 `string(TOLOWER ${mod} _modlc)` 生成
  `-Dmain=wt_coverage_main_<模块小写>`；用例侧声明同名符号：
  `wt_coverage_main_quotefactory` / `wt_coverage_main_wtuftrunner` / `wt_coverage_main_wtlatencyuft` / `wt_coverage_main_wtlatencyhft`。
  注意后两个模块的 `main.cpp:25` 是 **`int main()` 无参形态**，改名后符号签名是 `int wt_coverage_main_wtlatencyuft()`，
  所以本轮**没有**按 (argc,argv) 去声明/调用它（调用形态也碰不到：里面是无条件 `getchar()`，见 BUG-R14-005）。
- 门禁（真实构建口径，未自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_m_LoaderRunner.dir/m_LoaderRunner/seam_loader_runner_test.cpp.o | tail -1`
  → 去掉前导 `/usr/bin/c++`、去掉 ninja 写入的字面双引号、截掉 ` -MT ...` 尾巴（含 `-MD/-MF/-o/-c`）
  → 追加 `-Dmain=wt_coverage_main_<模块小写> -fsyntax-only <用例>`；其余 flags（`-Wall -Wextra -Werror -std=gnu++17 -arch arm64 -fprofile-instr-generate -fcoverage-mapping` 等）一字未动。
  **结果 4/4 通过**（脚本 `/tmp/r14_gate.sh`，逐文件 `GATE_PASS`）。
  期间一次真实修复：`m_WtUftRunner` 用例缺 `#include "WtUftCore/WtHelper.h"` → `WtHelper` 未声明，补上后过。
- 未跑构建、未跑二进制、未跑 ctest、无 git 操作、未读挂钟做断言、未起线程、无 `#define private public`。

## 一、分支选取（为什么只有这些能启用）

四个模块全部带「加载配置 → 起适配器/引擎 → 主循环」的形态，可启用的只有能自行终止的早退支：

1. **命令行 `--help`**：`QuoteFactory/main.cpp:275`、`WtUftRunner/main.cpp:53` 都在实例化 runner 之前 `return 0`。
2. **日志/配置缺失早退**：`QuoteFactory/main.cpp:278~283 + 299~307`、`WtUftRunner.cpp:60~67`、
   `UftLatencyTool.cpp:198~209`、`HftLatencyTool.cpp:347~358`；`WTSCfgLoader.cpp:300` 的 `exists()` 判空让这条支不崩。
3. **自由函数 `getBinDir()`**（`QuoteFactory/main.cpp:50`、`WtUftRunner.cpp:22`）与
   **`initialize()` 的缺配置早退**（`QuoteFactory/main.cpp:96~105`，用 `WtHelper::get_module_dir()` 当可观测点）。
4. **`WtUftRunner::config()` 的「逐段早退」整链**：用例自己写一份最小 yaml（只含 `basefiles` 占位键 + 不存在的 `bspolicy`），
   于是 `env/data/share_domain/parsers/traders/strategies` 全部 `get()` 返回 NULL，
   一次跑通 `WtUftRunner.cpp:68~197 + 200~204 + 253~257 + 268~272` 而不碰共享内存、不 dlopen 厂商库、不起线程。
5. **LatencyTool 的分支决定权问题**：`init()` 读的是硬编码相对路径（见 BUG-R14-006），
   用例用 `ScopedEmptyCwd`（mkdir/chdir/rmdir，无 sleep、无线程）进到一个空的私有目录再退出，
   把「config.yaml 不存在」变成本用例可控且可证伪的前提，并断言 cwd 已还原。

一律不启用（写进各文件的 `DISABLED_` 与触发条件）：`run()` 主循环、`ShareManager::start_watching`、
`ParserAdapter/TraderAdapter::init/initExt`（dlopen + 接收线程）、`_engine.init(NULL,…)`、以及会 `getchar()` 的两个入口。

## 二、用例 → 生产文件:行段

### `m_QuoteFactory/r14_quotefactory_main_test.cpp`（8 启用 + 2 DISABLED_）

| 用例 | 生产位置 | 钉住的行为（期望值由本用例输入算出） |
| --- | --- | --- |
| `short_help_returns_zero_before_any_logging_init` | `main.cpp:264~276` | `-h` 在 `WTSLogger::init` 之前 `return 0` |
| `long_help_returns_zero` | `main.cpp:271,273,275~276` | `--help` 同上 |
| `help_wins_over_config_and_logcfg_words` | `main.cpp:268~276` | `-c/-l` 与 `-h` 并存时 help 优先，不访问文件 |
| `missing_config_path_returns_zero_via_explicit_c` | `main.cpp:278~283 + 299~307` | lParam 缺失走 `:282` 默认名，`:304` 判空后 `:307 return 0` |
| `both_c_and_l_given_still_returns_zero_when_files_absent` | `main.cpp:279~280 + 299~300 + 304~307` | 两个 `exists()` 的「真」侧 + `StdFile::exists` 的「假」侧 |
| `static_cache_returns_same_pointer_twice` | `main.cpp:50~61` | `:53` 的 static 缓存两次同指针（假分支） |
| `value_is_the_cwd_with_at_most_one_trailing_slash` | `main.cpp:55,57` | 值等于 `getcwd()` 或 `getcwd()+"/"`，二者恰有一个成立 |
| `missing_config_stamps_module_dir_then_returns` | `main.cpp:96~105` | `:98` 先写 module_dir，`:101~104` 早退；module_dir == getBinDir() |
| `DISABLED_existing_config_enters_parsers_run_and_endless_loop` | `main.cpp:310~327`（含 `:254/:260` 起线程、`:324` 10ms 死循环） | 不启用 |
| `DISABLED_bare_argv_depends_on_cwd_contents` | `main.cpp:302` 默认 `./dtcfg.yaml`（仓库 `dist/QuoteFactory/dtcfg.yaml` 真实存在） | 不启用：期望值无法由本用例输入决定 |

### `m_WtUftRunner/r14_wtuftrunner_main_test.cpp`（12 启用 + 3 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `short_help_returns_zero_before_constructing_runner` | `main.cpp:31~54` | `-h` 早于 `:65 WtUftRunner runner;` |
| `long_help_returns_zero` / `help_wins_over_config_and_logcfg_words` | `main.cpp:47~54` | 同上两条分支 |
| `static_cache_returns_same_pointer_twice` | `WtUftRunner.cpp:22~33` | `:25` 假分支，同指针 |
| `value_is_the_cwd_with_at_most_one_trailing_slash` | `WtUftRunner.cpp:27,29` | 与 `getcwd()` 至多差一个尾斜杠 |
| `ctor_dtor_roundtrip_registers_hooks_without_spawning_threads` | `WtUftRunner.cpp:37~51` | 构造只做 `install_signal_hooks`；`SignalHook.hpp` 全文无 `std::thread/pthread_create` |
| `init_with_missing_logcfg_stamps_inst_dir_with_binroot` | `WtUftRunner.cpp:53~58` | `:57` 无条件执行 ⇒ `WtHelper::getInstDir() == getBinDir()`（逐字相等） |
| `missing_file_returns_false` | `WtUftRunner.cpp:60~67` | `:63~66` 早退，`_config` 保持 NULL |
| `authored_config_without_basefiles_children_walks_the_early_exits` | `WtUftRunner.cpp:68~197` + `200~204` + `253~257` + `268~272` | 自造 yaml 决定每一步：`get()` 全 NULL ⇒ 逐段 return false，最终 `:197 return true` |
| `missing_then_authored_order_is_independent` | `WtUftRunner.cpp:60~67`→`:68~197` | 先失败后成功，顺序不影响结论（见 BUG-R14-009 的泄漏附注） |
| `table_matches_the_seven_declared_levels` | `WtUftRunner.cpp:375~383` | LOG_TAGS 七个字面量逐格核对 |
| `handleLogAppend_is_a_noop_for_every_declared_level` | `WtUftRunner.cpp:385~387` | LL_ALL..LL_NONE 全周期不抛（空实现） |
| `DISABLED_non_help_argv_reaches_engine_run_and_watchdog_thread` | `main.cpp:59~78` | 见 BUG-R14-002 |
| `DISABLED_run_false_loops_until_signal` | `WtUftRunner.cpp:351~373` | `:360 start_watching(2)` 起线程、`:362~365` 10ms 死循环 |
| `DISABLED_full_engine_config_needs_shm_and_vendor_libs` | `WtUftRunner.cpp:109~127,128~193,280~338` | 需真实 shm 与厂商库 |

### `m_WtLatencyUFT/r14_uftlatency_main_test.cpp`（4 启用 + 3 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `ctor_dtor_roundtrip_only_builds_member_managers` | `UftLatencyTool.cpp:189~196` | 空构造/空析构，不触引擎 |
| `init_without_config_yaml_returns_false_in_empty_cwd` | `UftLatencyTool.cpp:198~209` | `:200/:204` 双缺失 ⇒ `:208 return false` |
| `init_is_repeatable_and_still_false_without_config` | `UftLatencyTool.cpp:198~209` | 同输入两次同结论 |
| `empty_cwd_is_really_empty_and_restored` | （用例自身的 guard 前提） | `access("config.yaml")` 失败 + 退出后 cwd 逐字还原 |
| `DISABLED_entry_blocks_on_getchar` | `main.cpp:25~32` + `UftLatencyTool.cpp:28~34` | 见 BUG-R14-005 |
| `DISABLED_init_success_needs_real_config_and_engine` | `UftLatencyTool.cpp:211~259`（`:213` NULL 解引用 / `:254` 传 NULL 给 `_engine.init`） | 见 BUG-R14-003 |
| `DISABLED_run_starts_threads_and_busy_loop` | `UftLatencyTool.cpp:305~327` | `:317/:318/:320/:322` |

### `m_WtLatencyHFT/r14_hftlatency_main_test.cpp`（4 启用 + 3 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `ctor_dtor_roundtrip_only_builds_member_managers` | `HftLatencyTool.cpp:328~340` | 空构造/空析构 |
| `init_without_config_yaml_returns_false_in_empty_cwd` | `HftLatencyTool.cpp:347~358` | `:350/:353` 双缺失 ⇒ `:357 return false`（早于 `:362` 的 NULL 解引用） |
| `init_is_repeatable_and_still_false_without_config` | `HftLatencyTool.cpp:347~358` | 同输入两次同结论 |
| `empty_cwd_is_really_empty_and_restored` | （guard 前提，另加 `actpolicy.yaml`） | 三个硬编码相对路径都不存在 + cwd 还原 |
| `DISABLED_entry_blocks_on_getchar` | `main.cpp:25~32` + `HftLatencyTool.cpp:33~42` | 见 BUG-R14-005 |
| `DISABLED_init_success_dereferences_null_cfgbf_without_basefiles` | `HftLatencyTool.cpp:360~418` | `:362` 对 NULL `cfgBF` 调 `getBoolean` |
| `DISABLED_run_starts_threads_and_busy_loop` | `HftLatencyTool.cpp:484~506` | `:488 bind_core`、`:496~501`、`:503 catch(...)` 吞崩溃 |

## 三、实际进入的分支占比（用行数量出来）

「进入行数」按上表行段里**可执行语句**逐行数得（不含被跳过的 if 体、lambda 体、注释）；missed 用派单给的报表口径。

| 生产文件 | missed 基数 | 本轮启用用例进入 | 进入占比 | 剩余必须起引擎/真柜台才能进 |
| --- | --- | --- | --- | --- |
| `src/QuoteFactory/main.cpp` | 185 | ≈30（入口 8 + 日志/配置支 11 + getBinDir 5 + initialize 早退 6） | **≈16.2%** | ≈155 行（`:107~262` initialize 主体 + `:310~327` 主循环） |
| `src/WtUftRunner/WtUftRunner.cpp` | 285 | ≈56（getBinDir 5 + ctor/dtor 5 + init 4 + config 两条支 23 + 三个子 init 12 + LOG_TAGS 1 + handleLogAppend 2） | **≈19.6%** | ≈229 行（`:109~193` shm/适配器 + `:206~251` 策略装载 + `:280~338` + `:351~373`） |
| `src/WtUftRunner/main.cpp` | ≈30（入口 TU 全部 0.00%） | ≈8（`-h/--help` 链） | **≈27%** | ≈22 行（`:56~79`，含 BUG-R14-002 的空转 run） |
| `src/WtLatencyUFT/UftLatencyTool.cpp` | 191 | ≈11（ctor/dtor 4 + init 早退 7） | **≈5.8%** | ≈180 行（`:38~186` 文件内 TestParser/TestTrader/TestStrategy 只能由引擎回调驱动 + `:211~327`） |
| `src/WtLatencyHFT/HftLatencyTool.cpp` | 219 | ≈11（ctor/dtor 4 + init 早退 7） | **≈5.0%** | ≈208 行（`:89~325` 三个测试类 + `:360~478` + `:484~506`） |
| `src/WtLatencyUFT/main.cpp` / `src/WtLatencyHFT/main.cpp` | 各 ≈5 | 0 | **0%** | 全部（`getchar()` 阻塞，见 BUG-R14-005） |

四族合计：本轮可取样 ≈120 行 / missed 基数 880 行 ⇒ **≈13.6%**；其余 ≈760 行（≈86%）属于
「必须起引擎 / 真柜台 / 共享内存 / 会吞进程的 getchar」，其中至少 4 处（BUG-R14-002/003/004/005）是产品侧缺陷堵住的，
修掉之后同一批早退支才能扩到「配置齐备」的分支。

另注（不追）：`src/WtUftCore/WtUftEngine.cpp:209~213` 的 `return NULL;` 之后约 30 行不可达，前轮已登记。
