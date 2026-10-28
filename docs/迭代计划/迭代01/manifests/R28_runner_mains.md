# R28 覆盖率补写 manifest —— 两个「可执行体 main」目标

- lane: **R28**
- 范围: `src/WtRunner/WtRunner.cpp` + `src/WtRunner/main.cpp`（目标 `CoverageTests_m_WtRunner`）、
  `src/WtBtRunner/WtBtRunner.cpp`（目标 `CoverageTests_m_WtBtRunner`）
- 用例目录: `src/TestUnits/coverage/m_WtRunner/`、`src/TestUnits/coverage/m_WtBtRunner/`
- 未改: `src/TestUnits/coverage/CMakeLists.txt`、任何 `src/**` 产品代码、任何告警/`-Werror` 门槛

---

## 第一部分：诊断（为什么 m_WtRunner 已有 12 条 TEST，WtRunner.cpp 却是 491/491 = 0.00%）

结论先行：**三个独立原因叠加，缺一个都不会是「整文件 0%」**。

### 1.1 目标确实建立了，而且二进制在（所以「目标没建」不是原因）

```
$ cmake --build build-cov --target CoverageTests_m_WtRunner
[1/5] Building CXX object TestUnits/coverage/CMakeFiles/CoverageTests_m_WtRunner.dir/m_WtRunner/r6_wtrunner_main_test.cpp.o
[2/5] Building CXX object .../CoverageTests_m_WtRunner.dir/__/__/WtRunner/main.cpp.o
[3/5] Building CXX object .../CoverageTests_m_WtRunner.dir/__/__/WtRunner/WtRunner.cpp.o
[4/5] Linking CXX executable build_x64/Debug/bin/TestUnits/CoverageTests_m_WtRunner

$ ls -l build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtRunner
-rwxr-xr-x@ 1 yunjinqi staff 6790792 Oct 27 23:43 ...
```

生产 TU 确实被编进了这个二进制（对象路径含 `/__/__/`，符合 `coverage_report.py:find_objects()`
对「测试目标里的生产 TU」的收录条件，不是被排除名单吃掉）。

`nm` 确认重命名入口在二进制里、而且是 C++ mangled（`-Dmain=` 改的是标识符，不是链接语言）：

```
$ nm build-cov/.../CoverageTests_m_WtRunner | grep -i coverage_main
000000010005711c T __Z25wt_coverage_main_wtrunneriPPc
```

`WT_MAIN_MODULES` 实测内容（`src/TestUnits/coverage/CMakeLists.txt`）：
`WtRunner WtBtRunner WtUftRunner LoaderRunner QuoteFactory WtLatencyHFT WtLatencyUFT`
—— **WtBtRunner 在列**，我没有改这张表。

### 1.2 真正的第一原因：12 条里有 4 条把宿主进程打成 SIGKILL，profraw 是 0 字节

```
$ grep -n 'wt_coverage_main_wtrunner' src/TestUnits/coverage/m_WtRunner/*.cpp
r6_wtrunner_main_test.cpp:29:int wt_coverage_main_wtrunner(int argc, char* argv[]);
r6_wtrunner_main_test.cpp:33:	return wt_coverage_main_wtrunner(...);   # 只有 call_entry 一处调用点
```

也就是说 12 条里只有 3 条 `*_help_*` 真的调了入口，其余 9 条调的是 `getBinDir()` /
`WtRunner` 成员函数。而 R6 的 4 条成员函数用例一律写成 `WtRunner runner;`（测试栈帧上构造）。
逐条单跑（`fork`+`waitpid` 取真实信号，不用 shell 的 137 猜）：

```
r6_wtrunner_entry.short_help_returns_zero_before_touching_runner   -> rc0（但判决 NORUN，见 1.3）
r6_wtrunner_entry.long_help_returns_zero                           -> rc0（同上）
r6_wtrunner_entry.help_wins_over_config_and_logcfg_words           -> rc0（同上）
r6_wtrunner_binroot.getBinDir_is_cwd_and_stable_across_calls        -> rc0
r6_wtrunner_init.ctor_dtor_roundtrip_routes_declared_levels_only    -> signal 9 SIGKILL
r6_wtrunner_init.init_with_missing_logcfg_returns_and_stamps_inst_dir-> signal 9 SIGKILL
r6_wtrunner_config.missing_file_returns_false                       -> signal 9 SIGKILL
r6_wtrunner_config.log_tag_table_matches_the_seven_declared_levels   -> rc0
r6_wtrunner_config.config_error_path_leaves_object_reusable         -> signal 9 SIGKILL
```

SIGKILL 的三个「不是别的东西」的旁证（都实测过）：

| 观察 | 值 |
| --- | --- |
| 死亡时刻 | `[ RUN ]` 之后、`[ OK ]` 之前，约 0.1s |
| peak RSS | 32MB（`/usr/bin/time -l`）→ 不是 jetsam/OOM |
| `sizeof(WtRunner)` | 5232 字节，`ulimit -s` = 8176KB → 不是栈溢出 |
| core / `~/Library/Logs/DiagnosticReports` | 都没有 → 不是普通 SIGSEGV/SIGABRT |

定位到「栈 vs 堆」这一层（临时探针，跑完已删）：

```
r28_probe.c_heap_runner   （new WtRunner(); delete p）      -> rc0 OK
r28_probe.d_stack_runner  （WtRunner runner;）              -> SIGKILL(9)
r28_probe2.b_stack_with_markers: 打印 "before stack ctor" "inside stack scope"
                                  不打印 "after stack dtor"  -> 死在作用域退出的析构里
r28_probe3.a_hft_engine_heap / b_hft_engine_stack            -> 都 OK
r28_probe3.d_sel_engine_stack（WtSelEngine 栈上）            -> ctor 完成、dtor 死
r28_probe3.e_cta_engine_stack_twice（WtCtaEngine 栈上）      -> ctor 完成、dtor 死
```

后果是**乘法式的**：`docs/迭代计划/迭代01/tools/coverage.sh` 会把 0 字节 profraw 直接丢弃
（`kept=$kept dropped_empty=$dropped`），所以这 4 条不但自己不算 PASS，
它们跑过的 `WtRunner.cpp` 函数体一行都没进分母。剩下的 `getBinDir`/`LOG_TAGS` 两条
只碰一个函数和一张全局数据表，量级上不足以离开 0。

### 1.3 第二原因：`-h/--help` 那 3 条的判决是 NORUN，不是 PASS

`opt.parse()` 内部就 `std::exit(0)`（`src/Share/cppcli.hpp:1897-1900`），
所以 `wt_coverage_main_wtrunner` 从不返回，gtest 的 `[  PASSED  ]` 永远不写出来：

```
$ case_runner.sh ... r6_wtrunner_entry.short_help_returns_zero_before_touching_runner
NORUN r6_wtrunner_entry.short_help_returns_zero_before_touching_runner
```

`WtRunner/main.cpp:86-87` 的 `if (hParam->exists()) return 0;` 是**死代码**（见 BUG-R28-002）。
讽刺的是覆盖率反而落了：`exit()` 会触发运行时写 profraw，所以 `main.cpp` 有 86.96%，
只是用例判决不绿。

### 1.4 第三原因：报表快照早于二进制

`docs/迭代计划/迭代01/reports/coverage-transport-only-20261027T1438Z.txt`（落盘 22:59:43）里
`491/491 0.00% src/WtRunner/WtRunner.cpp`，而 `CoverageTests_m_WtRunner` 的链接时间是 23:43、
`WtRunner.cpp.o` 是 23:12 —— 那份报表根本没有跑过这个二进制。
所以「12 条用例没贡献一行」= **1.2 的 SIGKILL 让 WtRunner.cpp 归零** ＋ **1.4 让报表连这个二进制都没读**，
1.3 只是让 help 那 3 条也没能算绿。

### 1.5 修复动作与验证

把 R6 那 4 条的 `WtRunner runner;` 改成 `std::unique_ptr<WtRunner>`（堆），
3 条 help 用例转 `DISABLED_`（真实覆盖由我的 fork 子进程用例钉住）。改完立刻见效：

```
# 只有 4 条堆驱动用例的最小验证（诊断阶段，非最终数字）
src/WtRunner/WtRunner.cpp   Lines=491  Missed=456  Cover=7.13%
src/WtRunner/main.cpp       Lines=23   Missed=3    Cover=86.96%
```

最终数字见文末。

### 1.6 `CoverageTests_m_WtBtRunner`：目标根本没建立，而且建不起来

`src/TestUnits/coverage/m_WtBtRunner/` 此前**不存在**，而 `WT_MAIN_MODULES` 的循环是
`if(NOT mod_tus OR NOT mod_tests) continue()` —— 没有用例目录就静默不建目标。
我建目录放了用例之后，目标出现了，但**编译被测 TU 就失败**：

```
$ cmake --build build-cov --target CoverageTests_m_WtBtRunner
[1/6] Building CXX object .../CoverageTests_m_WtBtRunner.dir/m_WtBtRunner/r28_btrunner_main_test.cpp.o
[2/6] Building CXX object .../CoverageTests_m_WtBtRunner.dir/__/__/WtBtRunner/WtBtRunner.cpp.o
FAILED: ...
src/WtBtRunner/WtBtRunner.cpp:160:1: error: non-void function does not return a value in all control paths [-Werror,-Wreturn-type]
ninja: build stopped: subcommand failed.
```

根因是**改名机制与产品代码的相互作用**：`WtBtRunner.cpp` 的 `main()` 走到
`:159 WTSLogger::stop();` 就落到函数尾（没有 `return`）。C++ 规定 `main` 落尾等价于
`return 0`，clang 因此对 `main` 豁免 `-Wreturn-type`；一旦 `-Dmain=wt_coverage_main_wtbtrunner`，
这个函数就不再叫 `main`，豁免消失，`-Werror` 直接判死。
生产侧同样编这一份 TU（`src/CMakeLists.txt:265 ADD_SUBDIRECTORY(WtBtRunner)`，
对象 `build-cov/WtBtRunner/CMakeFiles/WtBtRunner.dir/WtBtRunner.cpp.o` 在），函数叫 `main` 所以过得去。

我三条出路都被本轮规则堵死：不能改产品代码（加 `return 0;`）、不能改
`src/TestUnits/coverage/CMakeLists.txt`（照 SEL/UFT/HFT 工厂那段先例给这一个 TU 加
`COMPILE_OPTIONS -Wno-error`）、不能降低告警门槛。**所以我没有留一个会让共享构建树挂掉的文件**：
用例暂存为 `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked`（不匹配 `*.cpp` glob，
目标回到「不存在」，`cmake --build build-cov` 全量重建实测 exit 0）。
主控按上面任一方案放行后，把这个文件改回 `.cpp` 即可直接跑（14 条 TEST，其中 4 条 DISABLED；
用真实构建旗标 `-fsyntax-only` 验过零告警）。

---

## 第二部分：逐用例 → 生产文件行段

`CoverageTests_m_WtRunner`，启用 52 条 / DISABLED 13 条。
执行方式：`-P 1` 串行（main 类用例会往共享 cwd 写夹具与 `Logs/`、`portfolio/`，
并行会互相覆盖；见第四节的复现命令）。

### 2.1 入口（`src/WtRunner/main.cpp:51-127`，5 条，全部 fork 子进程）

| 用例 | 生产行段 |
| --- | --- |
| `r28_wtrunner_entry.long_help_prints_usage_doc_in_child` | main.cpp:44-55（`Option` 构造、三条 `opt(...)`、`asHelpParam`、`parse()`），cppcli 的 `buildHelpDoc()` |
| `r28_wtrunner_entry.short_help_prints_usage_doc_in_child` | 同上，短选项拼写路径 |
| `r28_wtrunner_entry.help_wins_even_when_c_and_l_are_present` | main.cpp:71-84（`-c`/`-l` 两条 Rule 注册）+ help 抢占 |
| `r28_wtrunner_entry.help_first_word_still_exits_zero` | main.cpp:84 `opt.parse()` |
| `r28_wtrunner_entry.help_line_shape_is_typed_from_opt_impl` | 列宽折行与 `MUST-ENTER[`/`DEFAULT->[` 表头 |

这 5 条把 `main.cpp` 从 0 推到 **20/23**；剩 `87`（`return 0`，死代码）、
`92`（`lParam->get<std::string>()`）、`110`（`cParam->get<std::string>()`）
分列在 help 之后与栈上 `WtRunner runner;`（:100）之后，进程内取不到。

### 2.2 `getBinDir()`（WtRunner.cpp:26-37，2 条）

| 用例 | 行段 |
| --- | --- |
| `r28_wtrunner_binroot.second_call_hits_cached_branch` | :28-31 两个分支（首调进 `if(empty)`，二调走缓存） |
| `r28_wtrunner_binroot.value_matches_process_cwd` | :30 `boost::filesystem::initial_path` + :32 `standardisePath` |

### 2.3 `init()`（WtRunner.cpp:53-65，3 条）

| 用例 | 行段 |
| --- | --- |
| `init.missing_logcfg_takes_warn_branch_and_stamps_inst_dir` | :55 `WTSLogger::init` → :57 `setInstDir` → :59 真 → :60 `warn` |
| `init.present_logcfg_skips_warn_and_creates_log_dir` | :59 假分支（不进 warn）；断言 `Logs/` 目录这个副作用 |
| `init.repeat_init_is_idempotent_on_inst_dir` | :53-65 整段第二遍 |

### 2.4 `config()` 早退与错误路径（WtRunner.cpp:67-74，5 条）

| 用例 | 行段 |
| --- | --- |
| `config.missing_file_returns_false` | :69 → :71 真 → :72 error → :73 `return false` |
| `config.empty_file_returns_false` | `WTSCfgLoader::load_from_file` 的 `content.empty()` 出口回到 :71-73 |
| `config.extensionless_or_unknown_suffix_returns_false` | 后缀白名单之外 return NULL → :71-73 |
| `config.unparsable_json_returns_false_without_throwing` | rapidjson 不抛 → :71-73 |
| `config.unparsable_yaml_escapes_as_exception` | :69 无 try/catch，yaml-cpp 异常穿透（BUG-R28-003） |

### 2.5 `config()` basefiles 段（WtRunner.cpp:77-152，6 条）

| 用例 | 行段 |
| --- | --- |
| `config.full_cta_config_reaches_true_and_writes_portfolio_files` | :77-152 主干 + `WtEngine::init_outputs()` 的 `portfolio/trades.csv`、`closes.csv` 副作用断言 |
| `config.basefiles_session_key_absent_skips_load_sessions` | :78 假分支 |
| `config.contract_section_as_array_loads_each_entry` | :85-93 commodity 数组、:95-107 contract 数组、:113-115 second |
| `config.basefiles_rules_section_loads_custom_hot_rules` | :142-151 `rules` + `loadCustomRules` + info |
| `config.empty_action_policy_group_still_passes_the_gate` | :77-115 精简版（无 holiday/hot）+ `ActionPolicyMgr::init` 空表 |
| `config.second_config_call_rereads_from_new_path` | :67-73 与 :77-152 交替，验证对象可复用 |

### 2.6 `config()` 引擎与后续段（WtRunner.cpp:154-277 + init* 私有函数，17 条）

| 用例 | 行段 |
| --- | --- |
| `config.missing_env_section_stops_at_actionpolicy_but_returns_true` | initEngine():368-370 `cfg==NULL`；initDataMgr():418-420 data 缺 |
| `config.missing_bspolicy_makes_config_return_false_early` | :157-158 `if(!initActionPolicy()) return false;` |
| `config.hft_env_routes_to_hft_engine_and_skips_executers` | initEngine():390-396 `_hft_engine`；:227 `if(!_is_hft)` 假；:263-266 `initHftStrategies` |
| `config.sel_env_routes_to_sel_engine` | initEngine():381-384 `_is_sel` + :397-400 `_sel_engine` |
| `config.env_name_empty_defaults_to_cta_branch` | initEngine():375 `strlen(name)==0` |
| `config.unknown_env_name_falls_through_to_hft_else` | initEngine():390-394 兜底 else（拼错也归 HFT） |
| `config.parsers_inline_array_with_inactive_entry_loads_zero` | :188-191 数组分支 + initParsers():434-437 `!active` continue + :456 |
| `config.traders_inline_array_with_inactive_entry_loads_zero` | :219-222 + initTraders():543-546 continue + :556 |
| `config.parsers_side_file_loads_and_reports_failure` | :166-178 字符串分支 + initParsers():425-427 `cfgParser==NULL` |
| `config.parsers_side_file_unloadable_logs_error` | :179-182 `Loading parser config {} failed` |
| `config.traders_side_file_with_inactive_array_loads_zero` | :196-208 字符串分支 + initTraders 数组体 |
| `config.traders_side_file_non_array_reports_loading_traders_failed` | :209 `Loading traders failed` + initTraders():553-554 |
| `config.traders_side_file_unloadable_logs_error` | :210-213 |
| `config.executers_side_file_with_inactive_array_loads_zero` | :231-241 + :248-250 `routers` 转发 + initExecuters():470-472 |
| `config.executers_side_file_non_array_reports_loading_executers_failed` | :246 + initExecuters():463-464 |
| `config.executers_side_file_unloadable_logs_error` | :242-245 |
| `config.exec_inline_array_inactive_and_router_absent` | :228-258 精简 + :263-266 initCtaStrategies():279-283 |

### 2.7 策略/通知器/日志回调（WtRunner.cpp:263-362、614-637，6 条）

| 用例 | 行段 |
| --- | --- |
| `config.strategies_object_without_cta_array_returns_false_from_engine_side` | initCtaStrategies():279-284 |
| `config.strategies_cta_inactive_entry_is_skipped` | initCtaStrategies():288-296 `!active` continue + :312 |
| `config.notifier_section_absent_returns_false_from_init_evt_notifier` | initEvtNotifier():632-634 |
| `config.notifier_inactive_entry_short_circuits_evt_notifier_init` | initEvtNotifier():635-637（`EventNotifier::init` 首行 `!active` 即返回，不 dlopen、不碰端口） |
| `config.notifier_active_entry_fails_to_load_mq_module_and_returns` | initEvtNotifier():635-637 + `EventNotifier.cpp:63-79` 的模块路径回落与加载失败 |
| `config.routers_section_reaches_load_router_rules` | config():259-261 + `WtExecMgr.cpp:210-238` 两条 executer 分支 |
| `logappend.all_seven_declared_levels_route_to_notifier` | :614-627 全 7 级 |
| `logappend.tag_table_content_matches_wtrunner_definition` | :614-622 表内容 |

（`config.missing_file_returns_false` 等 3 条 R6 用例改堆驱动后仍在，合计启用 52 条。）

### 2.8 `r6_wtrunner_main_test.cpp` 的改动（同目录、非新增文件）

| 改动 | 原因 |
| --- | --- |
| 4 条 `WtRunner runner;` → `std::unique_ptr<WtRunner>` | 1.2 的 SIGKILL；改完这 4 条从 CRASH(sig=9) 变 PASS |
| 3 条 `*_help_*` → `DISABLED_*` | 1.3 的 NORUN（cppcli `std::exit(0)`），保留为「为什么不能在本进程里调」的记录 |
| 文件头与 D0 段注释更正 | R6 原文断言 help 分支 `return 0` 可达、失败是段错误，两处都被实测推翻 |

---

## 第三部分：留白面（原因 + 行数量级 + 需要的接缝）

`WtRunner.cpp` 还剩 **163/491 行**（`llvm-cov report` 口径），聚成 6 块：

| 行段 | 行数量级 | 为什么进不去 | 进主循环需要的接缝 |
| --- | --- | --- | --- |
| `:132` | 1 | 自定义主力规则要真的命中某个合约的 `getFullCode()` | 需要一份「三段式分月码 + customRule」同时命中的 hots/rules 夹具（本轮已知坑里 `SHFE.rb.2510` 那种）；纯配置可达，属夹具工作量而非接缝缺失 |
| `:302-310`、`:321-362` | 约 51 | `initCtaStrategies`/`initHftStrategies` 的 `active:true` 体。`CtaStrategyMgr::createStrategy` 要先从 `WtHelper::getCWD()+"cta/"` dlopen 到工厂；目录空则返回 NULL，紧接着 `:307 stra->self()` 空指针（实测 DISABLED 用例 K6） | 接缝 1：工厂目录可注入（`addXxxFactories(path)` 已经是公开入口，缺的是「测试能把 `libWtCtaStraFact.dylib` 拷进用例 run 目录」的约定或一个 setter）。给了之后这 51 行能拆成 20 行（工厂加载失败/strategy 不存在）+ 31 行（真策略 init） |
| `:440-455` | 约 15 | `initParsers` 的 `active:true` 体 → `ParserAdapter::init` 按 `module` dlopen parser 插件 | 接缝 2：一个假 parser 插件目录（`libParserUDP.dylib` 在 `build-cov/build_x64/Debug/bin/`，拷进 run 目录 + `module: ParserUDP` 就能进 `:122-137`，但仍需要行情端口才有意义） |
| `:476-544` | 约 63 | `initExecuters` 的 local/diff/dist 三个体，都要 `_exe_factory.loadFactories(cwd+"executer/")` 后拿到真工厂 | 接缝 3：同上，`libWtExeFact.dylib` 注入 `executer/`。注意 `WtLocalExecuter::init` 还要 `trader` 键，缺失只走 error 日志（`:490-504` 这段可先吃下） |
| `:563-571` | 约 8 | `initTraders` 的 `active:true` 体 → `TraderAdapter::init` dlopen trader 插件 + 起连接线程 | 接缝 4：假 trader 插件 + 「只 init 不 connect」的入口拆分。`TraderMocker` 是候选，但 `init` 里就会起线程，需要 CASE_TIMEOUT 外的显式 release 步骤 |
| `:579-612` | 约 28 | `run()` 整体：`:581-583` `_parsers.run()/_traders.run()` 起线程，`:585 _engine->run()`，`:591-596 install_signal_hooks`（改全局信号表 + 注册退出回调），`:599-603 while(!_to_exit) sleep(10ms)` 永不返回 | 接缝 5（最关键）：`run()` 需要一个「跑到第一个事件就退出」的开关。现有 `_to_exit` 是 private，只有 `install_signal_hooks` 的退出回调能置位；也就是说**只能靠发信号**（SIGINT/SIGTERM）来退出主循环。给了 `void stop()`/`std::atomic_bool` 外部可见的接缝，或者把主循环改成带超时的 `run_for(ms)`，`run()` 的 28 行 + `main.cpp:92/110/125` 就都能进。本轮没有硬闯：发真信号会连带打死同一进程里 gtest 的崩溃判定，且 `install_signal_hooks` 会覆盖 case_runner 依赖的信号语义 |

**`main.cpp` 剩余 3 行（:87/:92/:110）**：:87 是 cppcli `std::exit(0)` 造成的死代码，
:92/:110 在栈上 `WtRunner runner;`（:100）之后 —— 1.2 的 SIGKILL 直接封死。
要拿这 3 行，要么给 `WtRunner` 一个能堆上构造的入口（`int wt_main(int, char**, WtRunner*)` 这种），
要么由主控改用进程外采样（直接跑 `build-cov/build_x64/Debug/bin/WtRunner/WtRunner` 可执行体）。

**`WtBtRunner.cpp` 97/97 行全为留白**，因为目标建不起来（1.6）。放行后按暂存文件的预期：
可覆盖 `:43-55`（help 三 Rule）、`:57-72`（lParam/cParam 两支）、`:74-81`（E1 `return 0`）、
`:84-88`（E2 `return -1`）约 **35~40 行**；`:91-159` 约 **57 行**需要下面这些接缝：

- `:91-93` `HisDataReplayer replayer;` + `replayer.init(cfg->get("replayer"))` —— replayer 段缺失即空 variant 解引用；
- `:95-96` `cfgEnv->getCString("mocker")` —— env 段缺失即空指针；
- `:98-150` 五个 mocker 分支 —— 需要 `cta/`、`hft/`、`sel/`、`exec/`、`uft/` 工厂目录 + 真实历史数据目录；
  而且 `main` 只调 `replayer.init(cfg)` 一个实参，`_bt_loader` 保持 NULL（`HisDataReplayer.cpp:147`）；
- **mocker 拼错这条路必然是段错误**：五个分支全跳过 → `register_sink`（`HisDataReplayer.h:804-807`）
  从没被调用 → `_listener` 仍是 NULL（`:146`）→ `:152 replayer.prepare()` 走到
  `HisDataReplayer.cpp:669 _listener->handle_init()`（BUG-R28-006）；
- `:157 getchar()` 阻塞等 stdin，子进程里可以靠 `/dev/null` 立刻 EOF，
  但 `:159 WTSLogger::stop()` 会停掉日志线程，同进程后续用例的日志会哑。

---

## 第四部分：门禁复现命令与判决

```
ROOT=/Users/yunjinqi/Documents/new_projects/wondertrader
cmake --build build-cov --target CoverageTests_m_WtRunner          # 退出码 0
BIN="$ROOT/build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtRunner"   # 存在，绝对路径
T=/tmp/r28; rm -rf $T; mkdir -p $T/prof $T/rc $T/run
"$BIN" --gtest_list_tests 2>/dev/null | awk '/^[^ ]/{s=$0; sub(/\.$/,"",s); next} NF{gsub(/^[ \t]+/,""); print s "." $0}' | grep -v '\.DISABLED_' > $T/cases.txt
# enabled=52
cat $T/cases.txt | xargs -P 1 -I'@' -n 1 /bin/sh "$ROOT/docs/迭代计划/迭代01/tools/case_runner.sh" "$BIN" "$T/prof" "$T/run" "$T/rc" 60 '@' r28
```

判决（`$T/rc/*.rc` 首词统计）：

```
52 PASS    0 FAIL    0 CRASH    0 HANG    0 RUNDIR
profraw=57  0 字节=0        # 57 = 52 个父进程 + 5 个 fork 子进程（入口用例）
cmake --build build-cov -j 8   # 全量重建 exit 0，没给其他 lane 留坏掉的 target
```

- 用 `-P 1` 而不是 `-P 2`：main/config 类用例会往共享 cwd 写 `r28wt_*.yaml` 夹具，
  并由 `WtEngine::init_outputs()` 往 `portfolio/`、由 logcfg 往 `Logs/` 写产物；
  `portfolio/` 与 `Logs/` 是全进程共享路径，并行会互相截断（实测把 `TEST_JOBS` 调到 2 时
  `full_cta_config_reaches_true_and_writes_portfolio_files` 的副作用断言会偶发不过）。
- 子进程计数文件不是免费的：第一版把名字写成 cwd 相对路径，导致 `main.cpp` 覆盖凭空是 0
  （`coverage.sh` 只 `find $PROF_DIR`）。修法是读 `LLVM_PROFILE_FILE`、自己展开 `%p`
  再接用例专属后缀，见 `child_profpath()`。

覆盖率数字不在这里报（按规则只认主控的统一度量）；本 lane 的自测口径只用于确认「计数确实落到了
`src/WtRunner/WtRunner.cpp` 与 `src/WtRunner/main.cpp`」。
