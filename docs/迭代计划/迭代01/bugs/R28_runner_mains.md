# R28_runner_mains（WtRunner / WtBtRunner 可执行体）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`，不降低 `-Werror`/告警门槛，
> 未证明的因果只写嫌疑位置。涉及用例见 `docs/迭代计划/迭代01/manifests/R28_runner_mains.md`。

## BUG-R28-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtRunner/WtRunner.cpp`（`WtRunner::~WtRunner` :45-48 及其按值持有的
  `WtCtaEngine _cta_engine` / `WtHftEngine _hft_engine` / `WtSelEngine _sel_engine`，声明见
  `src/WtRunner/WtRunner.h:151-166`）；崩溃点本身在 libWtCore 的引擎析构链里
- 相关测试: `src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp` →
  `r28_wtrunner_entry.DISABLED_stack_allocated_runner_is_sigkilled`；
  同目录 `r6_wtrunner_main_test.cpp` 原有 4 条（`ctor_dtor_roundtrip_*`、
  `init_with_missing_logcfg_*`、`missing_file_returns_false`、`config_error_path_*`）
  在改成堆构造之前全部 `CRASH(rc=137,sig=9)`
- 输入/前置条件: 在测试栈帧上构造再析构（`WtRunner runner;` 出作用域），
  或单独在栈上构造 `WtCtaEngine` / `WtSelEngine`。
  完全等价的对象改成 `new` / `std::unique_ptr` 持有则构造、析构、`init()`、`config()` 全程正常
  （实测 `r28_probe.c_heap_runner` rc0 vs `d_stack_runner_only` SIGKILL）。
- 期望行为: 同一类型的生命周期只取决于 ctor/dtor 本身，不因对象在栈上还是在堆上而改变进程存活。
- 实际行为: 进程被 **SIGKILL(9)** 终止，`[ RUN ]` 之后、`[ OK ]` 之前，约 0.1s；
  没有 core（`ulimit -c` = 0 且 `~/Library/Logs/DiagnosticReports` 无记录），
  peak RSS 只有 32MB、`sizeof(WtRunner)` = 5232 字节、`ulimit -s` = 8176KB。
  后果是乘法式的：clang 运行时不写计数器 → profraw 0 字节 → `coverage.sh` 丢弃该进程，
  该用例跑过的 `WtRunner.cpp` 一行都不进分母。这就是「m_WtRunner 有 12 条 TEST 但
  WtRunner.cpp 报 0.00%」的第一原因。
- 复现步骤:
  `cmake --build build-cov --target CoverageTests_m_WtRunner`，把任一启用用例的
  `std::unique_ptr<WtRunner> runner(new WtRunner());` 改回 `WtRunner runner;` 并把 `->` 改成 `.`，
  用 `case_runner.sh` 单跑，看 `$rc/*.rc` 首词是 `CRASH(rc=137,sig=9)`；
  或用 `os.fork()/os.waitpid()` 取真实信号（shell 的 `$?`=137 只到信号号，看不到「无 core」）。
- 影响范围: 凡是「在本进程内实例化 WtRunner/引擎」的测试写法都不可用；
  `src/WtRunner/main.cpp:100` 的 `WtRunner runner;` 正好就是这个形状，
  于是 `main.cpp:92/110/125` 三行在进程内永远取不到覆盖。
  另外三个兄弟可执行体（WtUftRunner/WtLatencyUFT/WtLatencyHFT）的同名入口用例只跑了 help 分支，
  一旦有人去驱动它们的非 help 分支会撞到同一面墙。
- 备注: **因果未证明，只登记嫌疑位置**。已排除的假设：栈溢出（对象 5KB / 栈 8MB）、
  OOM-jetsam（RSS 32MB）、代码签名（`codesign --verify` valid on disk 且 satisfies DR）、
  `_engine` 空指针（R6 原注释的猜测，实测死亡发生在 ctor/dtor 阶段，`config()` 都没调到）。
  两条嫌疑：① `CoverageTests_m_WtRunner` 同时链了 `libWtCore.a`（`WT_M_CORES`）和
  `WtPorter/WtDtPorter/WtBtPorter` 这些**内部又静态嵌了同一份 libWtCore.a** 的 dylib，
  libWtCore 的进程级 static 状态因此有两份（同 `bugs/l00_build_graph.md` BUG-L00-005 一族），
  栈上对象的析构链可能踩到「另一份」的状态；
  ② `-fprofile-instr-generate` 与 arm64 页保护的相互作用。
  需要能跑 `lldb` 追 stop reason 的环境（本机 `lldb --batch` 在此二进制上超过 2 分钟不返回）。
  本 lane 的绕行办法已经写进用例：一律 `std::unique_ptr` 堆上构造。

## BUG-R28-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Share/cppcli.hpp:1891-1901`（`asHelpParam` 的 help 处理体）与
  `:1570-1582`（缺参错误分支同样 `std::exit(0)`）；受害调用方
  `src/WtRunner/main.cpp:86-87`、`src/WtBtRunner/WtBtRunner.cpp:58-59`、
  以及 LoaderRunner/QuoteFactory/WtUftRunner/WtLatencyHFT/WtLatencyUFT 各自的同款分支
- 相关测试: `src/TestUnits/coverage/m_WtRunner/r28_runner_main_test.cpp` →
  `r28_wtrunner_entry.help_wins_even_when_c_and_l_are_present`（断言 help 时 `---logger initialized---`
  绝不能出现）；`r6_wtrunner_main_test.cpp` → 三条 `r6_wtrunner_entry.DISABLED_*help*`
  （改回 `EXPECT_EQ(call_entry(...), 0)` 的形式在 R28 之前是启用状态，判决是 NORUN）
- 输入/前置条件: 任一 WonderTrader 可执行体带 `-h` / `--help` 启动。
- 期望行为: `opt.parse()` 正常返回，调用方按自己写的 `if (hParam->exists()) return 0;` 收尾，
  库不做退出决定。
- 实际行为: `cppcli` 在 `parse()` 内部 `std::cout << buildHelpDoc(); std::exit(0);` ——
  调用方那行 `return 0` 是**死代码**，`lParam`/`cParam` 的取值也一次都不会执行。
- 复现步骤: 在 gtest 用例里调 `wt_coverage_main_wtrunner(2, {"x","-h"})`，
  进程以 0 退出但 stdout 里没有 `[  PASSED  ]`，`case_runner.sh` 记 `NORUN`。
- 影响范围: ① 嵌入式/被包装的调用方（把 WtRunner 当库用的场景）拿不到控制权，宿主被静默 exit；
  ② 覆盖率侧「参数校验/帮助」这条本该最安全的早退支路无法在进程内取样，
  只能像本 lane 一样 fork 子进程再收 stdout 断言。
- 备注: 不改产品代码。修复方向是把 help/错误的 `std::exit` 换成返回状态码，
  或至少给一个 `asHelpParam(/*bExit=*/false)` 开关。本 lane 用 fork + 管道把这段行为钉住了。

## BUG-R28-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtRunner/WtRunner.cpp:69`（`_config = WTSCfgLoader::load_from_file(filename);` 无
  try/catch）、`src/WtBtRunner/WtBtRunner.cpp:84`（同一形状）；抛出点在
  `src/WTSUtils/WTSCfgLoader.cpp` 的 `load_from_yaml` 分支
- 相关测试: `r28_runner_main_test.cpp` → `r28_wtrunner_config.unparsable_yaml_escapes_as_exception`
  （`EXPECT_THROW`，钉的是现状）与对照用例 `unparsable_json_returns_false_without_throwing`；
  `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` →
  `r28_btrunner_entry.bad_yaml_config_terminates_the_process`（断言子进程 SIGABRT）
- 输入/前置条件: 给一个语法不对的 YAML 配置（例：`basefiles: [ this is : not valid` 少一个 `]`）。
- 期望行为: 和「文件不存在」「后缀不认识」「空文件」三条一样，返回 false / -1 并给出可诊断日志
  —— `load_from_file` 对这三条都老老实实返回 NULL，`WtRunner.cpp:71-73` 也会 error + return false。
- 实际行为: yaml-cpp 异常从 `load_from_yaml` 一路穿透 `config()` / `main()`。
  `main.cpp` 侧没有 catch → `std::terminate` → **SIGABRT**，进程当场死；
  `WtRunner::config()` 被测试调用时异常直接抛给调用方。
  实测两条错误路径的健壮性差一个量级：同一份坏内容写成 `.json` 后缀就只是 `return NULL`。
- 复现步骤: 写一个 `r28wt_broken_cfg.yaml` 内容为 `basefiles: [ this is : not valid\n\t- {{{`，
  调 `runner->config("r28wt_broken_cfg.yaml")`，观察 `C++ exception with description
  "yaml-cpp: error at line 2, column 4: end of sequence flow not found"`。
- 影响范围: 实盘进程「配置文件少个括号」= 静默崩溃而不是报错退出，运维侧看不到原因；
  回测进程同理。覆盖率侧 `WtRunner.cpp:71-73` 的错误日志支对 YAML 输入永远取不到。
- 备注: 不阻塞本 lane（`EXPECT_THROW` 已把现状钉住，改好后这条会自然变成 FAIL 提醒）。
  修复方向：`WTSCfgLoader::load_from_yaml` 内部 catch 并返回 NULL，或在两个调用点包 try/catch。

## BUG-R28-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:160`（`main` 落尾无 `return`）× 
  `src/TestUnits/coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环（`-Dmain=wt_coverage_main_<模块>`）
- 相关测试: `src/TestUnits/coverage/m_WtBtRunner/r28_btrunner_main_test.cpp.blocked`
  （14 条 TEST，其中 4 条 `DISABLED_`；已用真实构建旗标 `-fsyntax-only` 验过零告警）
- 输入/前置条件: 建立 `CoverageTests_m_WtBtRunner` 目标 —— `WtBtRunner` **已经在**
  `WT_MAIN_MODULES` 列表里，只缺用例目录；目录一放上去目标就出现，然后编译失败。
- 期望行为: 产品 TU 在测试目标里换名后仍然编得过（或者构建表对这类 TU 有与
  SEL/UFT/HFT 工厂同款的处置），使 `src/WtBtRunner/WtBtRunner.cpp` 能进入覆盖率统计。
- 实际行为: `src/WtBtRunner/WtBtRunner.cpp:160:1: error: non-void function does not return a
  value in all control paths [-Werror,-Wreturn-type]`，`ninja` 停止。
  根因是名字而不是代码：C++ 规定 `main` 落尾等价 `return 0`，clang 因此对 `main` 豁免
  `-Wreturn-type`；`-Dmain=` 之后函数不再叫 `main`，豁免消失。
  生产构建同一份 TU 没问题（`src/CMakeLists.txt:265 ADD_SUBDIRECTORY(WtBtRunner)`，
  `build-cov/WtBtRunner/CMakeFiles/WtBtRunner.dir/WtBtRunner.cpp.o` 在），因为函数还叫 `main`。
  `WtBtRunner.cpp:159` 之后本来也没有 return（`int main` 却没有返回值语句），
  顺带还意味着这条路径每次回测跑完都靠「语言规定」而不是写明的 `return`。
- 复现步骤: `mkdir -p src/TestUnits/coverage/m_WtBtRunner && 放入任意 *.cpp &&
  cmake --build build-cov --target CoverageTests_m_WtBtRunner`。
- 影响范围: `src/WtBtRunner/WtBtRunner.cpp` 97 行保持 0.00%；
  并且因为 ninja 遇错即停，**放上去就挂会连累整个 `cmake --build build-cov` 全量构建**
  （`coverage.sh` 的构建步骤没有 `-k 0`），所以本 lane 把用例暂存成 `.cpp.blocked`
  让 glob 收不到、目标回到「不存在」，实测全量重建 exit 0。
- 备注: 三条出路都要主控拍板，本 lane 三条都无权走：
  ①产品侧给 `WtBtRunner.cpp:160` 补 `return 0;`（本轮不许改产品代码）；
  ②测试侧照 SEL/UFT/HFT 工厂那段先例，只对 `WtBtRunner/*.cpp` 这一个 TU
  加 `COMPILE_OPTIONS -Wno-error`（本轮不许改 `coverage/CMakeLists.txt`，也不许降告警门槛）；
  ③把 `WtBtRunner` 从 `WT_MAIN_MODULES` 摘掉并登记为「进程外采样」。
  放行任一方案后把 `.blocked` 改回 `.cpp` 即可，用例本身不需要再改。

## BUG-R28-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtCore/WtExecMgr.cpp:216-219`（`load_router_rules` 循环体）
- 相关测试: `r28_runner_main_test.cpp` →
  `r28_wtrunner_config.DISABLED_router_rule_without_executer_key_derefs_null`；
  正向对照 `r28_wtrunner_config.routers_section_reaches_load_router_rules`（补上 `executer` 键后 PASS）
- 输入/前置条件: `WtRunner::config()` 的配置里有 `routers` 数组，但某个条目没写 `executer` 键
  （例：`routers:\n-   strategy: x\n    symbol: y`）。
- 期望行为: 条目缺 `executer` 时跳过并记一条可诊断错误；`:216` 的 `item` 与 `:218` 的 `itemExec`
  都该有 NULL 判定。
- 实际行为: `itemExec = item->get("executer")` 返回 NULL，`:219` 无条件 `itemExec->isArray()` → 
  **SIGSEGV(11)**。实测 `case_runner.sh` 记 `CRASH(rc=139,sig=11)`。
- 复现步骤: 跑该 `DISABLED_` 用例，或在启用用例里把 routers 条目的 `executer:` 一行删掉。
- 影响范围: 路由规则是实盘「下单走哪个执行器」的开关，YAML 手改漏一个键就把进程打挂；
  `WtRunner.cpp:249-250`（executers 侧文件里的 routers）和 `:259-261`（顶层 routers）
  两个入口都会转发到这里。另外 `:217` 的 `straName` 也没有判空，
  `item->getCString("strategy")` 对缺键返回 `""`（`Includes/WTSVariant.hpp:346` 的 `defVal=""`），
  于是所有漏写 strategy 的规则会被合并到同一个空字符串键下，属于静默错误路由。
- 备注: 修复方向：`:216-219` 补 `if (item == NULL) continue;` + `if (itemExec == NULL) { log error; continue; }`。

## BUG-R28-006

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:98-150`（五个 `strcmp(mode, ...)` 分支）与
  `src/WtBtCore/HisDataReplayer.cpp:669`（`_listener->handle_init()`）；
  `_listener` 只在 `HisDataReplayer.h:804-807` 的 `register_sink` 里被赋值，初值 NULL 见 `:146`
- 相关测试: `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` →
  `r28_btrunner_entry.DISABLED_unknown_mocker_leaves_listener_null_and_segv_in_prepare`
- 输入/前置条件: `configbt.yaml` 的 `env.mocker` 写成 cta/hft/sel/exec/uft 之外的任意值（拼错即可）。
- 期望行为: 认不出 mocker 时给出「不支持的回测引擎 xxx」之类的错误并退出非 0，
  而不是带着一个从未注册的 listener 继续跑 `prepare()`。
- 实际行为: 五个分支全跳过 → `register_sink` 从没调用 → `_listener` 仍是 NULL →
  `:152 replayer.prepare()` 走到 `HisDataReplayer.cpp:669` 空指针 → SIGSEGV。
- 复现步骤: 见该 `DISABLED_` 用例（replayer 段与 env 段都给全，只有 mocker 值认不出来）。
- 影响范围: 独立回测进程 `WtBtRunner` 的配置拼写错误直接崩；
  这条也解释了为什么 `WtBtRunner.cpp:98-150`（约 57 行）在本轮只能全部留白 —— 
  想避开这个崩溃就必须走「mocker 认得」那条，而那需要真实工厂目录 + 真实历史数据。
- 备注: 与 `bugs/R6_porter_runner.md` BUG-R6-011（main 在 config 失败后照样 run）同族，不重复登记。

## BUG-R28-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtRunner/WtRunner.cpp:154`（`initEngine();`）、`:157`（`initDataMgr();`）、
  `:228`（`initEvtNotifier();`）—— 三处返回值被丢弃；对照 `:157-158` 的
  `if (!initActionPolicy()) return false;` 是同一个函数里唯一被检查的 init
- 相关测试: `r28_wtrunner_config.missing_env_section_stops_at_actionpolicy_but_returns_true`
  （断言「引擎根本没初始化成功，`config()` 仍然返回 true」这一现状）
- 输入/前置条件: 一份 basefiles/bspolicy 都齐、但**没有 `env` 段**的配置。
  `initEngine()` 在 `:368-370` 因 `cfg==NULL` 返回 false，`_engine` 保持未赋值。
- 期望行为: 三个 init 中任一个失败都应该让 `config()` 返回 false，或至少记一条 error，
  因为后续 `run()` 在 `:585` 无条件 `_engine->run()`。
- 实际行为: `config()` 一路走到 `:276 return true`，调用方无从知道 `_engine == NULL`；
  紧接着 `main.cpp:125 runner.run(false)` → `:585` 空指针调用。
  `initDataMgr()` 同理（`:418-420` 无 data 段返回 false 被丢），
  `initEvtNotifier()` 的返回值在整个函数里也没人看。
- 复现步骤: 跑该启用用例即可（不需要崩溃就能观察到 `EXPECT_TRUE(config(...))`）；
  要看到后果就照 `r28_wtrunner_entry.DISABLED_bare_argv_reaches_stack_runner_and_null_engine_run`。
- 影响范围: 「配置缺一整段」在实盘侧从可控失败变成稍后必崩的定时炸弹，
  而且 `run()` 外面套了 `catch(...)`（`:605-611`）也救不了空指针（SIGSEGV 不是 C++ 异常）。
- 备注: 修复方向：三处按 `initActionPolicy` 的写法补 `if (!initXxx()) return false;`
  或至少 `WTSLogger::error`。本 lane 用启用用例把现状钉住，改好后这条会 FAIL 提醒。

## BUG-R28-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtBtRunner/WtBtRunner.cpp:80`（`fmt::print("confiture {} not exists", filename);`）
- 相关测试: `m_WtBtRunner/r28_btrunner_main_test.cpp.blocked` →
  `r28_btrunner_entry.missing_config_prints_notice_and_returns_zero`
  （断言里直接匹配 `confiture {} not exists` 这个字面量，把现状钉住）
- 输入/前置条件: `WtBtRunner -c ./不存在的.yaml`。
- 期望行为: 一句拼写正确、以换行结尾的提示，例如 `config file {} not exists\n`。
- 实际行为: 三处叠加 —— ① 单词拼成 `confiture`；② 格式串没有 `\n`，
  而它前面刚打过 `---logger initialized---\n`，所以这行会紧贴 shell 提示符、看起来像没打完；
  ③ 这是 main 里唯一一条「配置找不到」的用户可见提示，却用 `fmt::print` 而不是
  `WTSLogger`（同函数 `:86` 的另一条错误 `WTSLogger::info` 用了日志），
  结果这条信息不会进日志文件。
- 复现步骤: `WtBtRunner -c ./nope.yaml`，或跑该用例看子进程 stdout 回收到的原文。
- 影响范围: 回测首次使用者的第一手排障线索；日志里查不到这条，脚本按行解析输出也会漏。
- 备注: 不改产品代码。用例行号是 `:74-78`（判空 + print + return 0），
  这三行本身是本轮 WtBtRunner 唯一「早退且返回码明确」的可覆盖支路，所以顺手把文案钉住。
