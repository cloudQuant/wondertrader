# lane N01 — WtCore 前半（交易路径）疑似缺陷

> 状态一律「待审核」；本迭代不修改 `src/**`。本轮复核依据当前源码、接口签名和测试源文件；没有运行测试或实盘。`DISABLED_` 用例只表示代码中有该案例，不等于本轮已复现。

## BUG-N01-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtCore/ActionPolicyMgr.cpp:47-124`（`init` 未强制要求 default）、`:133-160`（`getActionRules`）
- 相关测试: `N01ActionPolicy.DISABLED_empty_rules_map_crashes_get_action_rules`
- 输入/前置条件: `init()` 成功加载了不含 `default` 的规则表，随后查询未映射到现存规则组的品种。源码没有在 `init()` 校验 default，因此此配置不会因缺 default 被拒绝。
- 期望行为: 缺少回退规则时确定性报错并拒绝初始化或拒绝该次交易；不能依赖无效迭代器。
- 实际行为: `getActionRules` 查询品种映射组，未命中时再查 `default`，最后只做 `assert(it != _rules.end())` 并返回 `it->second`。断言开启时会终止；`NDEBUG` 下访问 `end()` 是未定义行为，不能据此断言必然 SIGSEGV。
- 证据边界: `init()` 对配置组逐项装载后直接返回 true，没有 default 校验；`DISABLED_empty_rules_map_crashes_get_action_rules` 的测试源确实用 `{"not_default": ...}` 并调用 accessor，但本轮未运行。测试用例的前置条件有效，尚无运行时复现证据。
- 影响范围: 仅当缺 default 且 accessor 查询落到缺失组时触发；不是“任意配置手误”都会影响交易通道。
- 修复方向: 优先在 `init()` 检查必须的 default 并返回失败；若设计允许缺省，则 accessor 必须提供确定性结果，且确认空规则组在买卖循环中不发单，或由调用方显式 fail-closed。

## BUG-N01-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/ParserAdapter.cpp:30-38（构造函数）、ParserAdapter.cpp:45-83（`initExt`）、ParserAdapter.cpp:259（`handleQuote`）
- 相关测试: `N01ParserAdapter.DISABLED_uninitialized_check_time_and_null_bd_mgr_crash`（注释点名）
- 输入/前置条件: 通过 `initExt()`（外部接入解析器）初始化后推送行情。
- 期望行为: `_check_time` 在构造和所有初始化路径上都有确定值。
- 实际行为: 构造函数未初始化 `_check_time`；`init()` 在 `src/WtCore/ParserAdapter.cpp:101` 设置它，但 `initExt()` 未设置，成功订阅后的 `handleQuote` 在 `:260` 读取不确定值。`_hot_mgr` 并非 initExt 漏设：`init()` 与 `initExt()` 分别在 `:92`、`:53` 赋值（参数默认值为 NULL）。
- 证据边界: 代码静态确认 `initExt()` 路径读取未初始化 bool；丢帧结果和 sanitizer 报告未在本轮运行验证。现有 `DISABLED_uninitialized_check_time_and_null_bd_mgr_crash` 没有把测试里的 `FakeBaseDataMgr` 传给 adapter，直接 `handleQuote` 时先在 `:250` 解引用空 `_bd_mgr`，所以它不能证明 `_check_time` 的行为，也没有覆盖 `initExt()`。
- 影响范围: `initExt()` 成功、行情带有效合约信息并进入时间检查时，读取未初始化 bool；不应把 `_hot_mgr` 列为同一缺陷。
- 修复方向: 构造函数初始化 `_check_time(false)`，并由 `initExt()` 明确采用默认值或接收配置；独立测试需传入有效 `bdMgr`、使用已附合约的行情，并触发 `handleQuote` 到达时间检查。

## BUG-N01-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/TraderAdapter.cpp:392-402（`getRiskParams`），配合 TraderAdapter.cpp:168-172（`init` 的 riskmon 解析）
- 相关测试: `N01TraderAdapter.risk_gates_pass_when_riskmon_disabled`（只覆盖风控关闭短路）；`N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map` 的测试体没有触发风控查询，不能证明本项。
- 输入/前置条件: `riskmon.active = true`，但 `riskmon.policy` 里没有任何键名为 `default` 的品种规则。
- 期望行为: 风控启用却没有适用规则时，明确拒绝初始化或拒绝交易；不能返回无效地址。
- 实际行为: `init()` 在 `src/WtCore/TraderAdapter.cpp:147-151` 仅警告缺 default 后继续；`getRiskParams` 在品种未命中后直接返回 `&it->second`，若 default 不存在则 `it == end()`，形成未定义行为。`checkOrderLimits/checkCancelLimits` 的 `riskPara == NULL` 分支无法由当前实现安全到达。
- 复现条件（静态推导）: 风控已启用，策略表只含指定品种，对不匹配该键的标准代码调用风控查询；本轮未运行该配置。
- 影响范围: 只覆盖启用风控且品种和 default 均未命中的配置。该条与 `p11_wtcore_traderadapter.md` BUG-P11-006、`R181.md` 的合并复核为同一源代码位置，不重复计数。
- 修复方向: 不应直接返回 NULL，因为两个调用点当前把 NULL 当作“允许通过”（`return true`）。应在初始化时拒绝缺少 default 的活动风控配置，或定义明确的默认拒绝策略；修复后补匹配品种、default 回退和缺 default 三种测试。
- 证据边界: 源码静态确认；N01 对应 DISABLED 用例是空壳，P11 禁用组中也先调用会触发 BUG-P11-001 的连接成功分支，故那些禁用测试并未独立复现本项。本轮未运行测试。

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
- 修复方向: 在读取 `memberNames()` 前判定 `cfgPolicy`；活动风控缺 `policy` 时记录错误并让 `init()` 返回 false，避免以空规则继续装配。
- 证据边界: 源码静态确认该配置会在 `init()` 阶段解引用空指针；本轮未运行缺段配置测试。

## BUG-N01-005

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Major（前置条件待验证）
- 模块/文件: src/WtCore/TraderAdapter.cpp:1898（`onPushOrder` 撤单统计）、TraderAdapter.cpp:2172（`onPushTrade`）
- 相关测试: `N01TraderAdapter.DISABLED_push_report_before_run_dereferences_null_stat_map`
- 输入/前置条件: 直接调用回报处理器时 `_stat_map == NULL`，且输入进入撤单统计或成交统计分支。
- 期望行为: 回报处理器在依赖统计表前校验生命周期，或保证统计表在注册 SPI 前就绪。
- 实际行为: `onPushOrder`（`:1898`）与 `onPushTrade`（`:2172`）无条件访问 `_stat_map`。但 `run()` 在 `:409-414` 先创建统计表，再 `registerSpi(this)` 并 `connect()`；`init()`/`initExt()` 本身不注册 SPI。因此本候选所称“柜台在 run 前回报”的正常 API 路径未获证明，当前可确认的是直接调用 handler 会触发空指针风险。
- 证据边界: 源码静态确认访问顺序；N01 DISABLED 测试函数体只有局部变量，没有回报调用。P11 禁用测试后续有 `onPushTrade` 调用，但此前先调用 `handleEvent(WTE_Connect, 0)`，会先命中 BUG-P11-001；没有独立运行证据。
- 影响范围: 若 API 违反注册顺序，或其他代码绕过 SPI 直接调用，可能崩溃；不据此认定正常柜台回报路径会在 run 前进入。
- 修复方向: 保持“先建统计表、后注册 SPI”的顺序；如 handler 也支持外部直接调用，则加统一的空表早退/初始化。`onPushOrder` 的 `getCommInfo()` 未判空是另一输入条件，需独立判断基础数据契约，不与 `_stat_map` 合并计数。


- 迭代02复核: TEST。源码确实要求 _stat_map 存在，但 run 在 registerSpi/connect 之前先创建 map，因此候选所说的正常柜台回报早于 run 与正常注册顺序相反。N01 对应 disabled test 体只有局部变量，完全没有构造/投递回报；本测试主张作为生产 bug 的现有证据不成立。没有证明外部 API 会越过注册顺序回调。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-N01-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtCore/ParserAdapter.cpp:302-308（`handleOrderQueue`）、:325-331（`handleOrderDetail`）、:348-354（`handleTransaction`）
- 相关测试: `N01ParserAdapter.DISABLED_uninitialized_check_time_and_null_bd_mgr_crash`（只调用 `handleQuote`，未调用本项三个 Level2 handler）
- 输入/前置条件: 未初始化的 adapter 被测试或外部代码直接调用任一 Level2 handler；或 SPI 回调传入空 payload。`initExt()` 在 `api->init(NULL)` 成功时会于 `:63` 解引用 `_bd_mgr`；若 API init 失败则只记录日志、仍返回 true，缺少 manager 的 adapter 可能继续被 `run()`。
- 期望行为: 初始化时拒绝必需依赖为空；SPI 回调边界对空 payload 有明确定义并安全处理。
- 实际行为: 三个 handler 在取字段和 `_bd_mgr->getContract(...)` 前都没有判空。此处静态确认了防御缺口，但未证明解析器 API 会在未注册/未初始化时回调，接口也没有在签名旁说明 nullability。
- 证据边界: 现有 DISABLED 测试只构造未初始化 adapter 并调用 `handleQuote`，既未调用 Level2 handler，也未把准备好的 `FakeBaseDataMgr` 接入 adapter；不能作为本条复现证据。`R18_wtdtcore.md` 记录的是 `src/WtDtCore/ParserAdapter.cpp`，属另一个组件的相似缺口，不是本路径重复登记。
- 影响范围: 直接越过初始化生命周期调用 handler 或传入空数据时可能崩溃；正常生产回调前提待解析器 API 合同和具体插件验证。
- 修复方向: `init/initExt` 对必需 base-data manager 为空及 API 初始化失败都返回失败；三个 SPI handler 对空数据指针早退。只有确认 API 允许无 manager 收数后，才在 handler 中增加 `_bd_mgr` 可选分支。

## BUG-N01-007

- 状态: 已合并（迭代02：主编号 BUG-R42-002）
- 严重级别: Major
- 模块/文件: src/WtCore/HftStraBaseCtx.cpp:211-259（`stra_buy`）、:262-323（`stra_sell`）
- 相关测试: 无（`_engine`/`_trader` 均需真实引擎，属跳过目标）
- 输入/前置条件: 策略调用 `stra_buy/stra_sell` 时 `_trader == NULL`（例如 sel/hft 混合部署里未绑定交易通道的上下文）。
- 期望行为: HFT 上下文未绑定交易适配器时，创建失败或交易调用明确失败，不能读取未初始化成员。
- 实际行为: 构造函数 `src/WtCore/HftStraBaseCtx.cpp:41-48` 未初始化 `_trader`；`stra_buy/stra_sell` 在部分分支只把 `_trader` 放进风控检查条件，随后仍调用 `buy/sell`，标准代码分支也会解引用 `_trader`。这与 `R42.md` BUG-R42-002 的成员漏初始化、`R13_wtporter.md` BUG-R13-001 的 runner 绑定失败后仍返回 context 是同一故障链，不应另计。
- 证据边界: 静态源码证据；本轮未运行 HFT 或 WtPorter 用例。`stra_sell` 的 `getCommodity()` 返回 NULL 后仍调 `canShort()` 是另一个基础数据前置条件，尚无有效运行输入证明，不并入 `_trader` 缺陷。
- 修复方向: 按 R42/R13 主条目统一处理：构造时初始化 `_trader = NULL`；runner 绑定失败时拒绝创建或返回失败句柄；所有下单入口在使用前统一早退。对品种查询结果另行定义和测试 null 处理。


- 迭代02复核: DUPLICATE。候选描述的未初始化 _trader 与无守卫交易调用链，已由 R42-002 主记录覆盖；R13-001 提供 runner 绑定失败后仍返 context 的另一个触发链，但它明确指出成员漏初始化与 R42-002 同根因。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-N01-008

- 状态: 已合并（BUG-R215-003）
- 严重级别: Minor（关联记录，不另计）
- 模块/文件: `src/WtCore/TraderAdapter.h:368`（inline 声明）、`src/WtCore/TraderAdapter.cpp:598-607`（唯一函数体）
- 相关测试: N01 的 `risk_gates_pass_when_riskmon_disabled` 未调用此函数；测试注释引用 BUG-N01-004 是编号错误，应为本条。R202/R215 有同一接口记录。
- 输入/前置条件: 任何外部 TU（含 gtest）调用 `TraderAdapter::isTradeEnabled`。
- 期望行为: 声明与定义位置一致：要么 `inline` 定义写在头文件，要么去掉 `inline`。
- 实际行为: 调用方包含头文件时看不到 inline 函数体；R202 测试源对该告警作了 Clang pragma 抑制，并把直接调用放在 DISABLED 测试中。源码可确认声明/定义错位，但不能仅凭此断言所有外部调用都会 SIGSEGV 或不可链接。
- 证据边界: N01 本身没有编译或运行直接调用；R202/R215 有相关记录但本轮未重新验证它们的构建产物和链接结果。
- 影响范围: 跨 TU 调用的编译告警/符号可见性依工具链和链接方式而异；这是 `R202.md` BUG-R202-003（R215 已合并复核为 BUG-R215-003）的重复关联项，不重复计数。
- 修复方向: 若定义留在 `.cpp`，从头文件声明移除 `inline`；若保留 `inline`，把定义放入调用方可见的头文件。用未屏蔽告警的外部 TU 编译并链接验证。
