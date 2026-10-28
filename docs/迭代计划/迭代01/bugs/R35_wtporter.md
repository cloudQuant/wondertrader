# R35_wtporter（WtPorter 实盘 C 导出层 / WtRtRunner）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。既有 BUG-Q06-001/002/004、BUG-R6-001~014、BUG-R13-001~006
> 覆盖的面本文件只引用、不重复登记。

## BUG-R35-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtPorter/WtRtRunner.h:841-849`（`_cta_engine/_hft_engine/_sel_engine` 按值成员）→ `src/WtCore/WtCtaEngine.cpp:35-39`（构造列表只初始化 `_tm_ticker`）、`src/WtCore/WtCtaEngine.cpp:41-47`（析构 `if (_cfg) _cfg->release();`，`_cfg` 声明于 `WtCtaEngine.h:92`，未进构造列表）
- 相关测试: `src/TestUnits/coverage/q06_porter/r35_porter_feed_init_test.cpp` → `R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody`；兄弟 lane 既有红 23 条（`CoverageTests_porter!R6PorterSilence/R6PorterReregister/R6PorterFactories/R6PorterLifecycle/R6PorterLogRoute/R6PorterPassthrough/R6PorterExtExecuter/R6PorterExtParserQuote` 全族，见 `manifests/R35_wtporter.md` 第五节，本 lane 未代改）
- 输入/前置条件: 在用例体（或任何调用方）里**栈构造** `WtRtRunner r;`，作用域结束触发 `~WtRtRunner` → 引擎成员析构。不需要调用任何方法，构造+析构即复现。
- 期望行为: 引擎构造列表把所有裸指针成员初始化到确定状态（`_cfg = NULL`），未 `init()` 的实例可安全析构。
- 实际行为: SIGSEGV。macOS crash report（`CoverageTests_porter-2026-10-28-012210.ips`）显示 `EXC_BAD_ACCESS, KERN_INVALID_ADDRESS at 0x10`，故障线程栈顶为 `TestBody → WtRtRunner::~WtRtRunner() → wtp::WtCtaEngine::~WtCtaEngine()`。对照证据：同批成员引擎 `WtHftEngine`/`WtSelEngine` 的构造列表**都**初始化了 `_cfg(NULL)`（`WtHftEngine.cpp:34`、`WtSelEngine.cpp:35`），唯独 `WtCtaEngine` 漏（`:35-39` 只有 `_tm_ticker`），而崩溃帧恰好在 `~WtCtaEngine` ⇒ 「`_cfg` 未初始化即被析构解引用」高度收敛；`WtEngine` 基类其余未初始化裸指针（`_hot_mgr/_data_mgr/_base_data_mgr` 等）是否同病未逐一证明，留作同族嫌疑。
- 复现步骤: 直接单跑该用例（lane_gate 会滤掉 DISABLED_，需手跑）：`build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_porter --gtest_filter='R35PorterRunnerLocal.DISABLED_stack_instance_destructor_crashes_after_testbody'` → 进程 SIGSEGV；打开 `DISABLED_` 前缀后单跑效果相同。
- 影响范围: ①`CoverageTests_porter` 整目标当前 24 条红里占 23 条，R6 lane 的 porter 侧成果全部不可用；②任何把 `WtRtRunner` 作为栈对象/非静态成员使用的宿主（二次封装、嵌入式调用）在离开作用域时随机崩溃；`getRunner()` 静态单例因构造前零初始化而侥幸不崩，掩盖了该缺陷。
- 备注: 修复方向（供评审，不在本迭代动手）：`WtCtaEngine/WtHftEngine/WtSelEngine` 构造列表补 `_cfg = NULL`（同族裸指针一并归零），或将析构改为先判有效性。测试侧规避手段是统一走 `getRunner()` 单例，本 lane 21 条启用用例即全部单例形状、0 崩溃。
