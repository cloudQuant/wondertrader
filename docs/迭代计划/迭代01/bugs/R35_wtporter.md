# R35_wtporter（WtPorter 实盘 C 导出层 / WtRtRunner）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。既有 BUG-Q06-001/002/004、BUG-R6-001~014、BUG-R13-001~006
> 覆盖的面本文件只引用、不重复登记。本文件唯一候选与 BUG-R144-003 是同一根因，作为复现证据补充，不重复计数。

## BUG-R35-001（与 BUG-R144-003 重复）

- 状态: 待审核
- 严重级别: Minor（未初始化的非静态 `WtCtaEngine` 析构风险；常规 C 导出路径使用静态 runner）
- 模块/文件: `src/WtPorter/WtRtRunner.h:841-849`（`_cta_engine/_hft_engine/_sel_engine` 按值成员）→ `src/WtCore/WtCtaEngine.cpp:35-39`（构造列表只初始化 `_tm_ticker`）、`src/WtCore/WtCtaEngine.cpp:41-47`（析构 `if (_cfg) _cfg->release();`，`_cfg` 声明于 `WtCtaEngine.h:92`，未进构造列表）
- 相关测试: `src/TestUnits/coverage/q06_porter/r35_porter_feed_init_test.cpp` → `R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody`；兄弟 lane 既有红 23 条（`CoverageTests_porter!R6PorterSilence/R6PorterReregister/R6PorterFactories/R6PorterLifecycle/R6PorterLogRoute/R6PorterPassthrough/R6PorterExtExecuter/R6PorterExtParserQuote` 全族，见 `manifests/R35_wtporter.md` 第五节，本 lane 未代改）
- 输入/前置条件: 栈构造非静态 `WtRtRunner r;`，不调用 `config()`，作用域结束触发 `~WtRtRunner` 与按值引擎成员析构。常规 C 导出入口通过 `WtPorter.cpp:34-37` 的函数静态 `WtRtRunner`，不具有相同的自动存储期前置条件。
- 期望行为: 引擎构造列表把所有裸指针成员初始化到确定状态（`_cfg = NULL`），未 `init()` 的实例可安全析构。
- 实际行为与证据等级: 缺陷本身有源码证据：`WtCtaEngine` 构造函数只初始化 `_tm_ticker`，析构在 `release()` 后读取 `_cfg`（`WtCtaEngine.cpp:34-47`；声明在 `WtCtaEngine.h:92`）；HFT/SEL 构造函数已初始化各自 `_cfg`（`WtHftEngine.cpp:32-35`、`WtSelEngine.cpp:33-36`）。本次对现有覆盖率二进制单跑禁用用例（见下）实际得到进程退出码 139，证明该对象形状会崩，但退出码本身不独立证明唯一故障指针就是 `_cfg`。manifest 记录的 2026-10-28 crash report 栈帧作为既有证据引用；本次未重新读取该 `.ips`。基类 `_hot_mgr/_data_mgr/_base_data_mgr` 等成员是否影响此析构未在本条证明。
- 复现步骤与实测: `ulimit -c 0` 后执行 `LLVM_PROFILE_FILE=/tmp/codex-r35-%p.profraw build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_porter --gtest_filter=R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody --gtest_also_run_disabled_tests`，本次实测进程 exit 139。profile 写入 `/tmp`。
- 影响范围: 非静态 `WtRtRunner` 持有的未初始化 CTA 引擎在析构时有未定义行为，当前复现表现为崩溃。常规 C 导出路径走静态 runner；其静态存储零初始化会掩盖该前置条件，因此现有证据不支持将该问题描述为常规实盘启动的 Blocker。CoverageTests 的红数是测试目标影响，不等同于生产影响范围。
- 备注: 与既有 BUG-R144-003（`WtCtaEngine` 未 init 即析构读未初始化 `_cfg`）同一缺陷，不另立案；R155:57 与 R36:202 也要求沿用该编号。最小修复为只在 `WtCtaEngine` 构造函数初始化 `_cfg(NULL)`；HFT/SEL 已初始化 `_cfg`，无需重复改。基类其它指针须逐一结合其析构/释放路径审计后再决定是否归零。
