# 缺陷记录 — R17_exefact（WtDiffMinImpactExeUnit 全量 + WtMinImpactExeUnit 残余 + WtExecMon 可达片）

复核说明：本次只静态核对源码、接口与测试前置条件，未运行测试。测试文件中的断言仅是可复核的测试源码，不视为运行结果；“源码确认”也不等同于运行时复现。

本轮打 `src/WtExeFact/WtDiffMinImpactExeUnit.cpp`(missed 260)、`.h`(9)、`WtMinImpactExeUnit.cpp` 残余(41)、
`WtOrdMon.h`(11)、`src/WtExecMon/WtSimpDataMgr.cpp`(125，仅可达片)、`WtExecPorter.cpp`(25)。
用例：`src/TestUnits/coverage/r7_exefact/r17_*.cpp`（3 个 .cpp，共 37 条 TEST，启用 30、DISABLED 7）。
前账对照：`bugs/R7_exefact.md`（BUG-R7-001~018）、`BUG-N05-001`（WtSimpDataMgr `_cur_*` 未初始化，本文件只补 `_s_info` 与判空漏写，不重复造编号）。

## BUG-R17-001

- 状态: 本地替身测试复现；真实柜台及晚到成交尚未验证
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:117-146（对照 src/WtExeFact/WtMinImpactExeUnit.cpp:163-171）
- 相关测试: r17_diff_minimpact_unit_test.cpp `cancel_ack_removes_terminal_order_from_expiry_monitor`；旧用例把重复撤单固化为通过条件，现已修正。
- 输入/前置条件: expire=1s；发单 id1 → 超时撤 → 撤单回报到齐（`_cancel_cnt` 归零）→ 补发 id2 → 再来一笔 tick
- 期望行为: 终态（已撤/已全成交）订单应像 MinImpact 一样从 `_orders_mon` 摘除，超时扫描不再碰它
- 实际行为: L128 的 `erase_order` 被人为注释掉（理由是要留给 on_trade 用），而 `on_trade`/`on_order`
  全路径都没有补回摘除 → `_orders_mon` 只增不减；下一轮 `check_orders`（L241-252）对已终结的 id1
  **再次按 id 撤单**。本地测试中 id1 撤单出现 2 次、总撤单 3 次，正确期望分别为 1 和 2；新单 id2 到期撤单的独立断言未失败。
- 复现步骤: 构建 `CoverageTests_exefact`，设置临时 `LLVM_PROFILE_FILE`，运行 `--gtest_filter=r17_diff_minimpact.cancel_ack_removes_terminal_order_from_expiry_monitor`；正确契约测试 FAIL。夹具为内存替身，未连接真实柜台。
- 影响范围: 终态单仍进入后续超时扫描，重复撤单请求可暂时增加 `_cancel_cnt` 并阻塞重算；长期监控表增长与柜台限频后果未做运行/柜台验证
- 备注: 功能性缺陷。不能简单在终态 `on_order` 无条件删除：当前 `on_trade` 依赖监控表判定本地订单；修复需保留成交回报关联（例如分开维护待成交回报引用与超时集合），再为撤单、全成、晚到成交分别验收。本次不改生产代码。

## BUG-R17-002

- 状态: 已确认源码缺陷（非法配置路径静态可达；本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:103-104（PriceModeNames[4] 定义在同目录 WtMinImpactExeUnit.cpp:27-33）；同形态还在 WtMinImpactExeUnit.cpp:143-144
- 相关测试: r17_diff_minimpact_unit_test.cpp `DISABLED_init_pricemode_3_reads_past_name_table`
- 输入/前置条件: 配置 `pricemode = 3`（或 ≤ -2）；init 对取值无任何校验（L97）
- 期望行为: 校验 `_price_mode ∈ [-1, 2]`，非法回退默认值并告警
- 实际行为: 日志实参 `PriceModeNames[_price_mode + 1]` 直接越界读下标 4；fmt 参数在调用点求值，
  宿主 writeLog 是 no-op 也挡不住。与 BUG-R7-010（WtStockMinImpactExeUnit.cpp:100/155）同形态，
  本轮登记另两个文件
- 复现步骤: DISABLED 用例描述 `pricemode=3` 的非法配置前置；本次未运行
- 影响范围: 一个数字笔误就让执行单元初始化阶段越界读（崩溃与否取决于相邻内存布局，UB）
- 备注: UB 类，保持 DISABLED；修复应与 R7-010 一并做（不实施）。

## BUG-R17-003

- 状态: 已确认源码缺陷（算式由源码直接确定；本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:432-436 与 448-449（对照 WtMinImpactExeUnit.cpp:502-506 与 518-519）
- 相关测试: r17_diff_minimpact_unit_test.cpp `mode1_buy_single_offset_sell_double_offset`
- 输入/前置条件: `pricemode=1`（对手价）、`offset=2`、priceTick=1.0；同一场景各发一笔买/卖
- 期望行为: 买卖两侧对 offset 的扣加口径一致，且与 MinImpact 一致（各一次）
- 实际行为: 买侧 `buyPx=ask(3510)` 只在 L448 加一次 → 3512；卖侧 L435 已经
  `sellPx = bid - tick*offset`（3498），L449 又统一 `sellPx -= tick*offset` → **3496，双重扣减**。
  同输入在 MinImpact 得 3498，两单元口径分裂
- 复现步骤: 测试源码包含 3512 / 3496 两条价格断言；本次未运行，方向与偏移事实由源码算式核对
- 影响范围: 对手价模式卖单系统性深让 2×offset 跳，冲击成本被放大
- 备注: 用例按实际行为断言（不放宽）；需业务确认哪侧是对的（不实施）。

## BUG-R17-004

- 状态: 已确认源码缺陷（空/未初始化 session 指针及无守卫由源码确认；本次未运行）
- 严重级别: Blocker
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:28-34（构造函数漏初始化 `_s_info` 及 `_cur_*` 家族，声明见 WtSimpDataMgr.h:170-183）
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_push_quote_without_session_info`
- 输入/前置条件: `_data_mgr.init()` 未成功获得有效交易时段信息后，runner 仍向数据管理器转发非空行情；或读到的 session 配置查找失败
- 期望行为: `handle_push_quote` 在调用 session 方法前检查 `_s_info`；`_cur_*` 初始化问题由 `BUG-N05-001` 单独跟踪
- 实际行为: ctor 只初始化 `_reader/_runner/_bars_cache/_rt_tick_map`；`_s_info` 未初始化，`initStore` 也可能把它设为 NULL，`handle_push_quote` 在 L153-L160 无判空直接调用 session 方法。L143 读取未初始化的 `_cur_date` 还构成独立未定义行为；其 `_cur_*` 成员问题已由 `BUG-N05-001` 登记，本条只保留 session 指针缺陷
- 复现步骤: `DISABLED_simpdata_push_quote_without_session_info` 描述了直接调用路径，但本次未运行；运行时是否由配置失败后的 parser 行情触发，仍需集成环境确认
- 影响范围: 若 session 缺失时仍有行情转入该管理器，则触发未定义行为或空指针解引用
- 备注: 修复建议是将 `_s_info` 明确置空、在时间转换前检查，并让 runner 在数据管理器初始化失败时 fail closed。不要把 `_cur_*` 的重复登记并入本条。

## BUG-R17-005

- 迭代02第二轮前状态（历史）: - 状态: 待验证（直接 NULL 输入的接口契约尚未明确；本次未运行）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:84-88
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_init_null_cfg_derefs`
- 输入/前置条件: `init(cfg, runner)` 传入 `cfg == NULL`
- 期望行为: 与 `initStore` L45-46 一样先判空返回 false
- 实际行为: L87 直接 `cfg->get("store")` —— 对 NULL 调成员函数会产生空指针解引用/未定义行为；
  判空写在了内层（initStore），外层漏了
- 复现步骤: DISABLED 用例构造直接 NULL 输入；本次未运行
- 影响范围: 直接违反非空配置前置条件的调用会崩；当前唯一生产调用 `WtExecRunner::initDataMgr` 在调用前已检查 `data` 节点是否为空，因此尚未证明存在生产调用路径
- 备注: 这是防御性接口缺口候选。若 `init` 的契约要求 NULL 返回 false，应补守卫；否则应撤回该产品缺陷结论。测试仅构造直接 NULL 输入，保持 DISABLED。

## BUG-R17-006

- 状态: 已撤回（测试直接调用未初始化对象；当前生产生命周期先设置 runner）
- 严重级别: Minor
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:95-103
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_simpdata_basedata_mgr_null_runner`
- 输入/前置条件: 对构造后尚未 `init` 的 manager 直接调用 `IDataReaderSink` 查询回调
- 期望行为: `_runner == NULL` 时返回 NULL
- 实际行为: 若绕过初始化直接调用，`_runner` 为无效值，调用会失败；但唯一生产初始化路径在 `WtSimpDataMgr::init` 开始处先赋 `_runner=runner`，再把 manager 传给 reader 的 `init`。源码未支持“reader 线程抢在 runner 就绪前回调”的描述
- 复现步骤: DISABLED 测试只覆盖新建对象直接调用，不代表生产回调顺序；本次未运行
- 影响范围: 未证明对满足初始化契约的路径有影响
- 备注: 不作为产品缺陷保留。若 reader 接口允许在 `init` 前回调，需由接口契约或具体 reader 实现另行证明。

## BUG-R17-007

- 状态: 已撤回（零挂单量场景的错误方向 oracle 未建立）
- 严重级别: Minor
- 模块/文件: WtExeFact / src/WtExeFact/WtDiffMinImpactExeUnit.cpp:394、src/WtExeFact/WtMinImpactExeUnit.cpp:464
- 相关测试: r17_diff_minimpact_unit_test.cpp `mode2_zero_prices_fall_back_to_preclose`（bidq+askq=200 时 mp=0 走 else 分支，为该算式唯一启用对照）
- 输入/前置条件: `pricemode=2` 且 tick 的 bidqty(0)+askqty(0) == 0（休市快照/空簿行情）
- 期望行为: 原记录要求分母为 0 时跳过失衡判断，但未找到该规则的策略/API 契约
- 实际行为: 在常见 IEEE 浮点环境下，双侧量为 0 时计算得到 NaN，比较 `mp > 0` 为假并稳定进入 bid 侧；这与当前控制流一致，尚无证据说明价向错误
- 复现步骤: 现有启用用例没有触发 0/0；原记录也明确承认结果一致，未提供可证伪的策略期望
- 影响范围: 可考虑增加除零保护提升可读性，但尚不能定性为产品缺陷
- 备注: 原缺陷结论撤回；如业务定义空簿时的自动价方向，再按该契约补充测试。

## BUG-R17-008（环境/接缝阻塞，非产品缺陷）

- 状态: 已合并（覆盖边界并入 BUG-R7-018；非产品缺陷）
- 严重级别: 不适用（覆盖边界）
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp:44-52(module 空分支)、61-81、130-161、187-232；src/WtExecMon/WtExecPorter.cpp:81-85、91-95
- 相关测试: r17_simpdata_porter_unit_test.cpp `DISABLED_porter_run_exec_spawns_threads`、`DISABLED_porter_release_exec_stops_uninited_logger`（后者的根因即 R7 的 DISABLED_release_stops_uninited_logger，WtExecRunner.cpp:401-404）
- 输入/前置条件: 见「实际行为」
- 期望行为: initStore 能用内存桩替换存储插件；run/release 有可注入的停止点
- 实际行为: 存储插件成功加载、真实 reader 回灌及基于真实配置的端到端路径确实需要受控集成环境，与 BUG-R7-018 的覆盖边界重复；但“run 必定起线程”“release 未初始化 logger 必然不安全”并未由这些 DISABLED 用例前置条件证明：空 runner 下 adapter 集合为空，`run()` 只调用空管理器。具体限制为：
  1. `initStore` 成功路径需加载 reader 插件（module 为空的默认分支 L49-55 会触达 dlopen）+ L79 会话查询；
  2. `handle_push_quote` 正常面（L135-161）与 `get_kline_slice` 合成面（L192-232，需 reader 回灌 K 线 +
     WTSDataFactory 合成）依赖上述成功；
  3. `run_exec` 在已装配 parser/trader adapter 时才会进入各 adapter 的 run 路径；空 runner 不等同于起线程。`release_exec` 的未初始化生命周期是否受支持未由接口承诺。
  因此本轮可记录真实 reader 集成依赖，但无法据现有前置条件给出 125 行的可覆盖上限。
- 复现步骤: 不适用
- 影响范围: WtExecMon 两文件的可覆盖上限
- 备注: 与 BUG-R7-018 合并记录为覆盖边界，不计为产品缺陷。注入式 reader/session seam 可扩展测试；本轮没有验证真实插件加载或全链路行情。
