# p06_uftcore2 待审核缺陷（lane p06 / WtUftCore 剩余部分）

> 本文件只记录，不修改 `src/**`。状态一律「待审核」。
> 与 `docs/迭代计划/迭代01/bugs/n03_uftcore_index.md`（BUG-N03-001…008）同一缺陷类的条目只做引用，不重复立案。

## BUG-P06-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:35-51`、`src/WtUftCore/WtUftEngine.h:133-134,154`
- 相关测试: `src/TestUnits/coverage/p06_uftcore2/p06_uft_engine_test.cpp` → `DISABLED_p06_uft_engine.uninitialized_and_malformed_stdcode_class` 第 (1) 段
- 输入/前置条件: `WtUftEngine e;` 之后、`e.init(...)` 之前调用任意查询接口（`get_contract_info` / `get_commodity_info` / `get_session_info` / `get_cur_price` / `get_last_tick`）
- 期望行为: 构造函数把 `_base_data_mgr`、`_data_mgr`、`_dependent` 一并初始化为 `NULL/false`，或在未 init 时安全返回
- 实际行为: 初始化列表只有 `_tm_ticker/_cfg/_notifier`；`_base_data_mgr`、`_data_mgr`、`_dependent` 是未初始化标量/指针，未 init 即读就是野指针解引用
- 复现步骤: 取消 `DISABLED_` 前缀后运行 `CoverageTests_uftcore`，第 (1) 段立刻崩溃
- 影响范围: 所有「先 new 引擎、后 init」的装配路径（WtLatencyUFT/WtUftCore 调用方）；也让引擎的查询接口无法在单测里脱离 init 使用
- 备注: 与 BUG-N03-005（TraderAdapter 未初始化成员）同一缺陷类，修复方向是补齐构造函数初始化列表；不实施。本波启用用例已改为「init 之后再查」，不阻塞覆盖率目标

## BUG-P06-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:118-168`（`get_commodity_info` / `get_contract_info` / `get_session_info(isCode=true)`）
- 相关测试: 同上 DISABLED 用例第 (2) 段
- 输入/前置条件: 传入不含 `.` 的合约代码，例如 `get_contract_info("600000")`
- 期望行为: 校验 `StrUtil::split` 结果长度，不足两段时返回 `NULL`（或按交易所补全）
- 实际行为: 直接取 `ay[1].c_str()` / `ay[0].c_str()`，`StringVector` 只有 1 个元素时越界读
- 复现步骤: DISABLED 用例第 (2) 段
- 影响范围: 策略侧任何漏写交易所前缀的 `stra_get_*` 调用；越界读可能表现为随机崩溃或脏数据
- 备注: 阻塞该分支的覆盖（无法写启用断言）；建议修复方向为统一的 `extractStdCode` 校验

## BUG-P06-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:161-167`
- 相关测试: 同上 DISABLED 用例第 (3) 段
- 输入/前置条件: `get_session_info("SSE.600000", true)`，合约存在但 `cInfo->getCommInfo()` 为 `NULL`
- 期望行为: 与 `get_commodity_info` 一致，先判空再取 `getSessionInfo()`
- 实际行为: `commInfo->getSessionInfo()` 未判空，直接解引用
- 复现步骤: DISABLED 用例第 (3) 段
- 影响范围: 基础数据未完整挂接（合约/品种分离装载）时的启动早期查询
- 备注: 不阻塞覆盖率目标（该分支已收进 DISABLED）

## BUG-P06-004

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/UftStraContext.cpp:43-50`、`src/WtUftCore/UftStraContext.h`（`_trader` 成员）
- 相关测试: `src/TestUnits/coverage/p06_uftcore2/p06_uft_context_test.cpp` → `DISABLED_p06_uft_context.uninitialized_trader_and_param_class` 第 (1)(2) 段
- 输入/前置条件: `UftStraContext ctx(&engine, "x");` 后未调用 `setTrader()`，直接 `stra_get_position` / `stra_cancel` / `stra_get_undone`；或 `setTrader()` 挂一个从未 `init()` 的 `TraderAdapter`
- 期望行为: 构造函数把 `_trader` 初始化为 `NULL`，下单/查询接口判空后安全返回
- 实际行为: 初始化列表只有 `IUftStraCtx(name)/_engine/_tradingday/_strategy`，`_trader` 未初始化；`stra_get_position` 等 8 个接口无判空地解引用
- 复现步骤: DISABLED 用例第 (1)(2) 段（第 (2) 段同时触发 BUG-N03-005 的适配器空表路径）
- 影响范围: UFT 策略在 `on_init` 阶段（引擎尚未注入交易适配器）读取持仓/撤单是常见写法，属真实崩溃面
- 备注: 与 BUG-N03-005 同一类但不同文件，独立立案便于分别修复；建议 `_trader = NULL` + 入口判空

## BUG-P06-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftEngine.cpp:179-183`（`get_tick_slice`）、`209-243`（`get_kline_slice`）
- 相关测试: `p06_uft_engine.data_mgr_delegation_and_price`（启用，断言恒为 NULL）
- 输入/前置条件: 任意 `get_kline_slice(sid, code, period, count, times, etime)` 调用
- 期望行为: 写入 `_bar_sub_map[合约-周期-倍数]` 订阅表并向数据管理器取切片
- 实际行为: 函数第一行 `return NULL;`，其后 30 余行（含订阅登记、`KP_Minute5/KP_Minute1/KP_DAY` 周期换算）永久不可达；连带 `on_bar`（`584-603`）的订阅表永远是空表，K 线回调不可能触发
- 复现步骤: `p06_uft_engine.bar_and_l2_push_dispatch` 里 `on_bar` 分发的空表分支即为此证据
- 影响范围: UFT 策略的 `on_bar` 回调链路整体失效；`WtUftEngine.cpp` 约 40 行无法被任何测试覆盖
- 备注: **阻塞覆盖率目标**（死代码），需产品侧确认是「有意关闭 K 线」还是遗留调试语句；本波未改源码

## BUG-P06-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/WtUftTicker.cpp:54-61`（`init`）、`68-166`（`on_tick`）
- 相关测试: `p06_uft_ticker.init_and_forward_before_thread_start`（启用，只覆盖 `_thrd == NULL` 转发支）、`p06_uft_ticker.null_engine_guard_on_forward`
- 输入/前置条件: `WtUftRtTicker t(nullptr); t.init("ALLDAY");` 或 `init()` 时基础数据管理器取不到时段（`_s_info == NULL`）后再走 `_thrd != NULL` 的行情分支
- 期望行为: `init` 与 `on_tick` 对 `_engine`、`_s_info` 一致判空；取不到时段时应拒绝初始化或降级
- 实际行为: `init` 不判 `_engine`（`on_tick` 判）；`_s_info` 在 `on_tick` 的 `timeToMinutes/minuteToTime/isLastOfSection` 与 `run()` 线程体里均无判空；`on_tick` 里 `if(_engine)` 分支内又无条件调用 `_s_info->...`（第 99-109 行在判空之前）
- 复现步骤: 该分支只能由 `run()` 起线程后进入，本波按禁令不调 `run()`，故以源码审阅定级
- 影响范围: 交易时段配置缺失（session id 写错）时 UFT 引擎启动后首个 tick 即崩
- 备注: 属排除清单（需要 `run()` 线程）；修复方向是 `init` 里 `_s_info == NULL` 直接报错返回
