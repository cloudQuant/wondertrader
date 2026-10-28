# lane N01 — WtCore 前半（交易路径）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。

## BUG-N01-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtCore/ActionPolicyMgr.cpp:146-161（`ActionPolicyMgr::getActionRules`）
- 相关测试: `N01ActionPolicy.DISABLED_empty_rules_map_crashes_get_action_rules`
- 输入/前置条件: `init()` 的配置文件里没有任何名为 `default` 的规则组（或 `init()` 直接失败/未调用），随后 `getActionRules(任意 pid)`。
- 期望行为: 找不到规则组时返回空 `ActionRuleGroup` 并打错误日志，调用方（TraderAdapter 开平校验）据此拒绝下单。
- 实际行为: 代码只有 `assert(it != _rules.end());` 然后无条件 `return it->second;`。Release/NDEBUG 下 assert 被摘除，解引用 `std::map::end()` 是未定义行为，实际表现为段错误或读到任意内存。
- 复现步骤: 构造 `{"not_default": {"order":[{"action":"open","limit":1}]}}`，`init` 后调用 `getActionRules("SHFE.rb")`。
- 影响范围: 动作策略表为空/拼错组名时整个交易通道进程可崩；配置手误即触发。
- 备注: 阻塞「非法配置」这一整类分支的可测性，故测试保留但置为 `DISABLED_`。建议修复方向：`it == _rules.end()` 时返回一个静态空组。

## BUG-N01-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/ParserAdapter.cpp:30-38（构造函数）、ParserAdapter.cpp:45-83（`initExt`）、ParserAdapter.cpp:259（`handleQuote`）
- 相关测试: `N01ParserAdapter.DISABLED_uninitialized_check_time_and_null_bd_mgr_crash`（注释点名）
- 输入/前置条件: 通过 `initExt()`（外部接入解析器）初始化后推送行情。
- 期望行为: `_check_time` 与 `_hot_mgr` 在所有构造路径上都有确定初值。
- 实际行为: 构造初始化列表只写了 `_parser_api/_remover/_stopped/_bd_mgr/_stub/_cfg`，`_check_time` 与 `_hot_mgr` 未初始化；`initExt()` 也不设置 `_check_time`（只有 `init()` 会 `cfg->getBoolean("check_time")`）。`handleQuote` 里 `if (_check_time)` 读的是不确定值 —— 同一条 tick 可能被判成脏时间戳丢弃，也可能放行。
- 复现步骤: 对 `initExt` 初始化过的 ParserAdapter 连续推送同一批 tick，观察是否偶发丢帧。
- 影响范围: 外部行情接入路径的丢帧不确定性；ASan/MSan 下会直接报 use-of-uninitialized-value。
- 备注: 不阻塞覆盖率目标。建议修复方向：构造函数补 `, _check_time(false), _hot_mgr(NULL)`。

## BUG-N01-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtCore/TraderAdapter.cpp:392-402（`getRiskParams`），配合 TraderAdapter.cpp:168-172（`init` 的 riskmon 解析）
- 相关测试: `N01TraderAdapter.risk_gates_pass_when_riskmon_disabled`（只覆盖了风控关闭分支）、`N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map`
- 输入/前置条件: `riskmon.active = true`，但 `riskmon.policy` 里没有任何键名为 `default` 的品种规则。
- 期望行为: 没有 `default` 规则时应回退为「该合约不受限」或拒绝启动，而不是返回悬空引用。
- 实际行为: `init` 只在缺 default 时打一条 WARN 就继续；`getRiskParams` 先 `find(pid)` 未命中后 `it = _risk_params_map.find("default"); return &it->second;` —— `it == end()` 时对 `end()` 取 `&second`，越界引用。随后 `checkOrderLimits/checkCancelLimits` 会读到垃圾的边界值。
- 复现步骤: 配置 `{"riskmon":{"active":true,"policy":{"SHFE.rb":{"order_total_limits":100}}}}`，对非 SHFE.rb 合约下单。
- 影响范围: 电光/高频下单的风控判定不可信，极端情况下读到垃圾边界值导致误封单或风控完全失效。
- 备注: 建议修复方向：未命中 default 时返回 NULL（调用点已写了 `if (riskPara == NULL) return true;`）。

## BUG-N01-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/TraderAdapter.cpp:129-135（`init` 的 `cfgPolicy`）
- 相关测试: 无（需要 `riskmon.active=true` 的完整配置，属跳过目标）
- 输入/前置条件: `{"riskmon": {"active": true}}`，即启用了风控但漏写 `policy` 段。
- 期望行为: 记录错误并按「无风控规则」继续，或拒绝初始化。
- 实际行为: `WTSVariant* cfgPolicy = cfgRisk->get("policy");` 未判空即 `cfgPolicy->memberNames()`，对 NULL 调用非静态成员函数 —— 空指针解引用。
- 复现步骤: 用漏写 policy 的 broker 配置启动实盘。
- 影响范围: 配置手误即导致交易进程在启动阶段崩溃。
- 备注: 建议修复方向：`if (cfgPolicy == NULL) { warn; } else { ... }`。

## BUG-N01-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: src/WtCore/TraderAdapter.cpp:1898（`onPushOrder` 撤单统计）、TraderAdapter.cpp:2172（`onPushTrade`）
- 相关测试: `N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map`
- 输入/前置条件: `_stat_map` 只在 `run()` 里创建（`TraderAdapter.cpp:404-407`），而 `run()` 在 `_trader_api == NULL` 时提前返回；任何在此之后、`run()` 之前到达的订单/成交回报都会命中。
- 期望行为: 统计容器为空时先创建或跳过统计。
- 实际行为: 两处都无条件 `(WTSTradeStateInfo*)_stat_map->get(...)`，`_stat_map` 为 NULL 时段错误。同一缺陷类的第二个入口是 `onPushOrder` 里 `commInfo->getCategoty()`（`cInfo->getCommInfo()` 无判空）。
- 复现步骤: 在 `initExt()` 之后、`run()` 之前调用 `onPushOrder`（订单状态 `WOS_Canceled`）。
- 影响范围: 交易通道登录时序异常时进程崩溃；同时使 onPushOrder/onPushTrade 无法在无真实通道的单测里覆盖。
- 备注: 需要真实 `WTSOrderInfo/WTSTradeInfo`（含合约信息）才能构造，与真实回报等价，故只保留一个 `DISABLED_` 示例。

## BUG-N01-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/ParserAdapter.cpp:302-308（`handleOrderQueue`）、:325-331（`handleOrderDetail`）、:348-354（`handleTransaction`）
- 相关测试: `N01ParserAdapter.DISABLED_uninitialized_check_time_and_null_bd_mgr_crash`
- 输入/前置条件: 未调用 `init/initExt`（`_bd_mgr == NULL`，`_filters` 为空），推送日期字段合法的 level2 数据。
- 期望行为: 与 `handleQuote` 一样先判 `quote == NULL`，并在 `_bd_mgr == NULL` 时丢弃数据。
- 实际行为: 三个函数都没有 `_bd_mgr` 判空，直接 `_bd_mgr->getContract(...)`；同时对入参数据指针本身也没有 NULL 校验（`handleQuote` 有）。末尾的 `_stub` 反倒做了判空，说明「可选」与「必需」的边界判断不一致。
- 复现步骤: `ParserAdapter adapter; WTSOrdQueData* d = ...; adapter.handleOrderQueue(d);`
- 影响范围: level2 解析器在配置加载顺序异常时段错误。
- 备注: 建议修复方向：入口统一加 `if (_bd_mgr == NULL) return;`。

## BUG-N01-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/HftStraBaseCtx.cpp:211-259（`stra_buy`）、:262-323（`stra_sell`）
- 相关测试: 无（`_engine`/`_trader` 均需真实引擎，属跳过目标）
- 输入/前置条件: 策略调用 `stra_buy/stra_sell` 时 `_trader == NULL`（例如 sel/hft 混合部署里未绑定交易通道的上下文）。
- 期望行为: 判空之后要么安全返回空 `OrderIDs`，要么明确报错，不应「判了空又照用」。
- 实际行为: 两个函数都是 `if (_trader && !_trader->checkOrderLimits(...)) { ... return OrderIDs(); }` 紧接 `auto ids = _trader->buy(...) / _trader->sell(...)`。即 `_trader == NULL` 时前半个条件被短路跳过，后一行仍然解引用 NULL。同一函数里 `stra_sell` 还在 `commInfo->canShort()` 前没有对 `getCommodity()` 返回值判空（合约存在但品种未登记时段错误）。
- 复现步骤: `HftStraBaseCtx` 子类不调用 `setTrader()` 直接 `stra_buy("SHFE.rb2510", 3500, 1, "t")`。
- 影响范围: 未绑定通道的 HFT 上下文下单即崩；下单前置校验（可平空/涨跌停/手数）分支难以在单测里隔离。
- 备注: 建议修复方向：统一 `if (_trader == NULL) return OrderIDs();` 前置早退。

## BUG-N01-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtCore/TraderAdapter.h:368（`inline bool isTradeEnabled(const char* stdCode) const;`）
- 相关测试: `N01TraderAdapter.risk_gates_pass_when_riskmon_disabled`（因该缺陷而移除了对 `isTradeEnabled` 的直接调用）
- 输入/前置条件: 任何外部 TU（含 gtest）调用 `TraderAdapter::isTradeEnabled`。
- 期望行为: 声明与定义位置一致：要么 `inline` 定义写在头文件，要么去掉 `inline`。
- 实际行为: 头文件按 `inline` 声明，函数体却在 `TraderAdapter.cpp:598`，在 `-Wall -Wextra -Werror` 下触发 `-Wundefined-inline` 编译错误，测试代码只能不调用它。
- 复现步骤: 编译本轮 `n01_trader_adapter_test.cpp` 时保留 `adapter.isTradeEnabled("X")` 即报错（首轮门禁实测）。
- 影响范围: 该函数在当前形式下不可被 WtCore 之外的 TU 使用（WtCore 是 STATIC 库，符号本来可直接链接），并使 `isTradeEnabled` 的可测性下降。
- 备注: 建议修复方向：把头文件里的 `inline` 去掉，或把头文件里的声明改成内联定义。
