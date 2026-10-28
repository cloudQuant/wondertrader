# 缺陷记录 — R17_exefact（WtDiffMinImpactExeUnit 全量 + WtMinImpactExeUnit 残余 + WtExecMon 可达片）

本轮打 `src/WtExeFact/WtDiffMinImpactExeUnit.cpp`(missed 260)、`.h`(9)、`WtMinImpactExeUnit.cpp` 残余(41)、
`WtOrdMon.h`(11)、`src/WtExecMon/WtSimpDataMgr.cpp`(125，仅可达片)、`WtExecPorter.cpp`(25)。
用例：`src/TestUnits/coverage/r7_exefact/r17_*.cpp`（3 个 .cpp，共 37 条 TEST，启用 30、DISABLED 7）。
前账对照：`bugs/R7_exefact.md`（BUG-R7-001~018）、`BUG-N05-001`（WtSimpDataMgr `_cur_*` 未初始化，本文件只补 `_s_info` 与判空漏写，不重复造编号）。

## BUG-R17-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:117-146（对照 src/WtExeFact/WtMinImpactExeUnit.cpp:163-171）
- 相关测试: r17_diff_minimpact_unit_test.cpp `cancelled_order_stays_monitored_and_recancels`
- 输入/前置条件: expire=1s；发单 id1 → 超时撤 → 撤单回报到齐（`_cancel_cnt` 归零）→ 补发 id2 → 再来一笔 tick
- 期望行为: 终态（已撤/已全成交）订单应像 MinImpact 一样从 `_orders_mon` 摘除，超时扫描不再碰它
- 实际行为: L128 的 `erase_order` 被人为注释掉（理由是要留给 on_trade 用），而 `on_trade`/`on_order`
  全路径都没有补回摘除 → `_orders_mon` 只增不减；下一轮 `check_orders`（L241-252）对已终结的 id1
  **再次按 id 撤单**。用例实测 `byidCancels` 里 id1 出现 2 次（期望 1 次），且每轮僵尸单都推高
  `_cancel_cnt`，把 `do_calc` 挡在 L318-322 之外
- 复现步骤: 启用该用例即见 `zombieHits==2`
- 影响范围: 真实柜台对重复撤终态订单只会回「无法找到该笔订单」，但本地 `_cancel_cnt` 与监控表持续膨胀，
  执行单元在长跑中逐渐发不出新单（cancel_cnt 被终态单消耗）；撤单频率超限还有交易所封通道风险
- 备注: 功能性缺陷；建议在 `on_order` 的 `leftover==0 || isCanceled` 终态处摘除订单、`on_trade` 里改判
  其它状态表（不实施）。

## BUG-R17-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:103-104（PriceModeNames[4] 定义在同目录 WtMinImpactExeUnit.cpp:27-33）；同形态还在 WtMinImpactExeUnit.cpp:143-144
- 相关测试: r17_diff_minimpact_unit_test.cpp `DISABLED_init_pricemode_3_reads_past_name_table`
- 输入/前置条件: 配置 `pricemode = 3`（或 ≤ -2）；init 对取值无任何校验（L97）
- 期望行为: 校验 `_price_mode ∈ [-1, 2]`，非法回退默认值并告警
- 实际行为: 日志实参 `PriceModeNames[_price_mode + 1]` 直接越界读下标 4；fmt 参数在调用点求值，
  宿主 writeLog 是 no-op 也挡不住。与 BUG-R7-010（WtStockMinImpactExeUnit.cpp:100/155）同形态，
  本轮登记另两个文件
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 一个数字笔误就让执行单元初始化阶段越界读（崩溃与否取决于相邻内存布局，UB）
- 备注: UB 类，保持 DISABLED；修复应与 R7-010 一并做（不实施）。

## BUG-R17-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:432-436 与 448-449（对照 WtMinImpactExeUnit.cpp:502-506 与 518-519）
- 相关测试: r17_diff_minimpact_unit_test.cpp `mode1_buy_single_offset_sell_double_offset`
- 输入/前置条件: `pricemode=1`（对手价）、`offset=2`、priceTick=1.0；同一场景各发一笔买/卖
- 期望行为: 买卖两侧对 offset 的扣加口径一致，且与 MinImpact 一致（各一次）
- 实际行为: 买侧 `buyPx=ask(3510)` 只在 L448 加一次 → 3512；卖侧 L435 已经
  `sellPx = bid - tick*offset`（3498），L449 又统一 `sellPx -= tick*offset` → **3496，双重扣减**。
  同输入在 MinImpact 得 3498，两单元口径分裂
- 复现步骤: 该用例两条价格断言（3512 / 3496）即证据，期望值按实际代码算得
- 影响范围: 对手价模式卖单系统性深让 2×offset 跳，冲击成本被放大
- 备注: 用例按实际行为断言（不放宽）；需业务确认哪侧是对的（不实施）。

## BUG-R17-004

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:28-34（构造函数漏初始化 `_s_info` 及 `_cur_*` 家族，声明见 WtSimpDataMgr.h:170-183）
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_push_quote_without_session_info`
- 输入/前置条件: 未经 `initStore` 成功（或读线程抢在 runner 就绪前回调）时收到任意一笔非空 tick
- 期望行为: `handle_push_quote` 对 `_s_info` 判空；`_cur_date/_cur_act_time` 等构造即置 0
- 实际行为: ctor 只初始化了 `_reader/_runner/_bars_cache/_rt_tick_map` 四个指针；
  L143 拿不确定的 `_cur_date` 做新旧 tick 比较（比较结果不可预期），L153 无条件
  `_s_info->timeToMinutes(...)` 对不确定指针解引用 → 崩溃或更糟的静默错值。
  `_cur_*` 的未初始化在 `BUG-N05-001` 已登记，本条补登 `_s_info` 同一漏洞根源与可达路径
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: exec 监控进程行情接入早期（store 未就绪/配置失败仍在跑时）随时崩
- 备注: 崩溃类，保持 DISABLED；与 N05-001 同修：ctor 补全成员就地初始化 + L153 前判空（不实施）。

## BUG-R17-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:84-88
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_init_null_cfg_derefs`
- 输入/前置条件: `init(cfg, runner)` 传入 `cfg == NULL`
- 期望行为: 与 `initStore` L45-46 一样先判空返回 false
- 实际行为: L87 直接 `cfg->get("store")` —— 对 NULL 调成员函数解引用 `_type/_value` → SEGV；
  判空写在了内层（initStore），外层漏了
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 上层任何「配置段缺失传 NULL」的路径直接崩进程
- 备注: 崩溃类，保持 DISABLED（不实施）。

## BUG-R17-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:95-103
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_basedata_mgr_null_runner`
- 输入/前置条件: mgr 构造后未经 `init(cfg, runner)`（`_runner=NULL`）即被回调 `get_basedata_mgr()/get_hot_mgr()`
- 期望行为: `_runner == NULL` 时返回 NULL
- 实际行为: 直接 `_runner->get_bd_mgr()` 对 NULL 调成员（inline 取成员偏移）→ 解引用崩溃。
  正常时序由 WtExecRunner 保证，但 IDataReaderSink 回调方（存储线程）不受该时序约束
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 契约性判空缺失；实际风险窗口窄（读线程抢跑）
- 备注: 保持 DISABLED（不实施）。

## BUG-R17-007

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:394、src/WtExeFact/WtMinImpactExeUnit.cpp:464
- 相关测试: r17_diff_minimpact_unit_test.cpp `mode2_zero_prices_fall_back_to_preclose`（bidq+askq=200 时 mp=0 走 else 分支，为该算式唯一启用对照）
- 输入/前置条件: `pricemode=2` 且 tick 的 bidqty(0)+askqty(0) == 0（休市快照/空簿行情）
- 期望行为: 分母为 0 时跳过失衡判断，按文档默认一侧取价并告警
- 实际行为: `mp = 0*1.0/0` → NaN；`NaN > 0` 为假 → 静默走 bidprice 侧。IEEE 下不崩，但方向选择失去语义，
  且两个文件复制同一算式，宽严无法单点修复
- 复现步骤: 构造双侧挂单量 0 的 tick（本轮用例未直接触发 0/0，因双侧同价兜底后结果一致；登记备查）
- 影响范围: 空簿时刻的自动价方向不可预期；属可读性/防御缺失
- 备注: 不阻塞覆盖率（不实施）。

## BUG-R17-008（环境/接缝阻塞，非产品缺陷）

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:44-52(module 空分支)、61-81、130-161、187-232；src/WtExecMon/WtExecPorter.cpp:81-85、91-95
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_porter_run_exec_spawns_threads`、`DISABLED_porter_release_exec_stops_uninited_logger`（后者的根因即 R7 的 DISABLED_release_stops_uninited_logger，WtExecRunner.cpp:401-404）
- 输入/前置条件: 见「实际行为」
- 期望行为: initStore 能用内存桩替换存储插件；run/release 有可注入的停止点
- 实际行为: 三个面不可达，与 BUG-R7-018 同根源：
  1. `initStore` 成功路径需 `DLLHelper::load_library` 真实拉起 WtDataStorage 插件（module 为空的默认分支
     L49-50 即触达 dlopen，属进程级副作用，用例侧刻意只测加载失败分支）+ L79 `_runner` 真实会话查询；
  2. `handle_push_quote` 正常面（L135-161）与 `get_kline_slice` 合成面（L192-232，需 reader 回灌 K 线 +
     WTSDataFactory 合成）依赖上述成功；
  3. `run_exec/release_exec` 起线程 / 停未初始化 logger，硬规则 2 禁止。
  估算 WtSimpDataMgr.cpp 125 行 missed 中仅 ~35 行可达（本轮已全部打到），~90 行阻塞。
- 复现步骤: 不适用
- 影响范围: WtExecMon 两文件的可覆盖上限
- 备注: 需要 owner 决策（同 BUG-R7-018）：注入式 reader/时钟桩或受控配置目录的一次性 config() 冒烟。
