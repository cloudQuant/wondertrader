# 缺陷记录 — r7_exefact（WtExeFact VWAP/TWAP/股票执行单元 + WtExecMon 运行器）

本轮只打 5 个文件：`src/WtExeFact/WtStockVWapExeUnit.cpp`(missed 444)、`WtStockMinImpactExeUnit.cpp`(409)、
`WtVWapExeUnit.cpp`(372)、`WtTWapExeUnit.cpp`(325)、`src/WtExecMon/WtExecRunner.cpp`(323, 0.00%)。
用例：`src/TestUnits/coverage/r7_exefact/`（5 个 .cpp，共 112 条 TEST，启用 101 条、DISABLED 11 条）。

## BUG-R7-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtVWapExeUnit.cpp L120+L123；src/WtExeFact/WtStockVWapExeUnit.cpp L192+L198
- 相关测试: r7_vwap_unit_test.cpp 文件头说明 + 全部 `boot`/`prime` 用例（依赖 tail_secs=0 才能判定）
- 输入/前置条件: 任何 `init(cfg)`，`total_times>=1`
- 期望行为: `_fire_span = (_total_secs - _tail_secs) / _total_times` 应在 `_total_secs = calTmSecs(begin,end)` 之后计算
- 实际行为: 两个文件都先算 `_fire_span`（此时 `_total_secs` 还是构造函数的 0），再赋 `_total_secs`；
  `uint32_t` 相减下溢成 ~4.29e9，`_fire_span` 变成天文数字或截断后的无意义值，
  于是 `on_tick` 的间隔闸门（WtVWapExeUnit.cpp:272 / WtStockVWapExeUnit.cpp:439）
  「防止开盘前集中下单把通道封掉」的机制实际失效（tail_secs=0 时恒真，其它值时恒假）
- 复现步骤: `init` 后读日志 L122 打出的「执行时限」为 0（`_total_secs` 赋值前打印），可佐证顺序颠倒
- 影响范围: VWAP 期货/股票两条链的发单节奏；TWAP 顺序是对的（L162 先于 L166），可作对照
- 备注: 测试侧只能用 `tail_secs=0` 把 `_fire_span` 钉成 0 来获得可判定性，属于放宽环境的临时手段。

## BUG-R7-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:120、WtTWapExeUnit.cpp:166、WtStockVWapExeUnit.cpp:192
- 相关测试: r7_vwap_unit_test.cpp `DISABLED_init_zero_total_times_divide_by_zero`；r7_twap_unit_test.cpp `DISABLED_init_zero_total_times_divide_by_zero`
- 输入/前置条件: 配置 `total_times = 0`（缺省即 0：`cfg->getUInt32("total_times")` 对不存在的键返回 0，且无范围校验）
- 期望行为: init 校验 `total_times >= 1`，非法时写日志并回退默认值
- 实际行为: 整数除零 → SIGFPE，进程当场崩溃；崩溃发生在 `init()` 内，早于任何交易动作
- 复现步骤: 启用该 DISABLED 用例即可（`mkCfg(0, ...)` / `mkTCfg(0, ...)`）
- 影响范围: 一份笔误的执行器配置就能让 WtExecMon 进程启动即崩
- 备注: 崩溃类，用例保持 DISABLED；建议除零保护（不实施）。

## BUG-R7-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:372、WtStockVWapExeUnit.cpp:578（配对的 init 早退在 L128-131 / L228-231）
- 相关测试: r7_vwap_unit_test.cpp `DISABLED_short_distribution_file_out_of_range_read`；
  启用侧对照 `init_missing_distribution_file_returns_early`
- 输入/前置条件: 分布文件 `Vwap_<品种名>.txt` 不存在（`VwapAim` 为空）或行数 < 241；随后收到一笔 tick
- 期望行为: `do_calc` 在取分桶前校验 `VwapAim.size()`，不足则不发单
- 实际行为: `double InminsTm = calTmStamp(...)`（值域 0..240）直接 `VwapAim[InminsTm]`，
  用 double 作下标且无任何边界检查 → 越界读/UB。init 发现文件缺失只是 `return`（L131/L231），
  单元照常进入发单路径
- 复现步骤: 启用该 DISABLED 用例（分布文件只写 10 格，tick 落在下标 30）
- 影响范围: 品种漏配分布文件时，VWAP 单元首次成交即越界；且 `_Vwap_vol` 会拿到垃圾值
- 备注: 崩溃/UB 类，保持 DISABLED。

## BUG-R7-004

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:348-362、WtTWapExeUnit.cpp:634-653、WtStockVWapExeUnit.cpp:554-568
- 相关测试: r7_vwap_unit_test.cpp `clear_target_dbl_max_exits_without_order`、r7_twap_unit_test.cpp `clear_target_with_flat_position_does_nothing`
- 输入/前置条件: `set_position(code, DBL_MAX)`（清仓标记），实仓 0
- 期望行为: 清仓路径按多头余量 `newVol = -min(lPos, _order_lots)` 强制出一笔平单
- 实际行为: 三处都有 `if (decimal::eq(diffQty, 0)) return;` 的前置早退（L319/L586/L515），
  而 `curPos = realPos`、`diffQty = newVol - realPos`，所以到达 `if (decimal::eq(curPos, newVol))` 时
  该条件恒为假 → 整个清仓分支（含 `getPosition(code,true,1)` 调用与 "Clearing process triggered" 日志）不可达
- 复现步骤: 见上述两条启用用例：断言 DBL_MAX 目标下既无买也无卖
- 影响范围: 每个文件约 12~19 行死代码（合计 ~45 行）永远无法覆盖，且清仓兜底逻辑实际不存在
- 备注: 覆盖率天花板类；建议把完成/清仓判定移到 diffQty 早退之前（不实施）。

## BUG-R7-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:517-519（判空在 L547）
- 相关测试: r7_stock_vwap_unit_test.cpp `DISABLED_channel_ready_before_first_tick_crashes`
- 输入/前置条件: `init` → `set_position(非0目标)` → `on_channel_ready()`（此时还没有任何 tick）
- 期望行为: 与其它三个单元一致，先判 `_last_tick == NULL` 再解引用
- 实际行为: `if (decimal::ge(_start_price, 0)) _start_price = _last_tick->price();` 位于 L547 判空之前，
  `_last_tick` 为 NULL → 段错误。通道先于行情就绪是实盘常见时序（断线重连、行情慢于柜台）
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 股票 VWAP 单元启动早期崩溃；另外 `ge(_start_price, 0)` 对 0 也成立，
  使得该行每次计算都覆写开盘基准价，「补一次」的语义实际变成「每次都补」
- 备注: 崩溃类，保持 DISABLED；本轮所有股票 VWAP 用例都被迫按 init→set_position→on_tick→on_channel_ready 排序。

## BUG-R7-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / WtTWapExeUnit.cpp:508/734/738、WtVWapExeUnit.cpp:408/412/470、WtStockVWapExeUnit.cpp:633/637/714
- 相关测试: r7_twap_unit_test.cpp `DISABLED_null_commodity_info_crashes_price_offset_branch`
- 输入/前置条件: `ctx->getCommodityInfo()` 返回 NULL（品种表未加载/代码写错）
- 期望行为: init 既已允许 `_comm_info == NULL`（L136-139 等显式判空），发单路径也应判空
- 实际行为: do_calc/fire_at_once 无条件 `_comm_info->getPriceTick()` → 空指针解引用
- 复现步骤: 启用该 DISABLED 用例（`ctx.comm = NULL` + offset≠0）
- 影响范围: 基础数据缺失时执行单元崩溃而非报警退出
- 备注: 崩溃类，保持 DISABLED。

## BUG-R7-007

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:530、542、595-609
- 相关测试: r7_stock_vwap_unit_test.cpp `vwap_bucket_distribution_does_not_affect_fired_qty`
- 输入/前置条件: 两个完全相同的场景，只把分布文件 `Vwap_*.txt` 的每格值从 25 换成 100000
- 期望行为: 分片量随 VWAP 预测分布变化（L582 `_Vwap_vol = aimQty - curPos`、L591 取该值）
- 实际行为: 能走到 L595 说明 `undone == 0`（L542 已拦），而 L530 的 `isBuy` 是**由 undone 决定的**，
  恒为 false → 恒走「卖」分支；且 L609 无条件 `curQty = min(vailyPos, _min_open_lots)` 覆盖了
  L601/L607 的结果 → 最终成交量与分桶值完全无关。用例断言两次 `buys[0].qty` 相等（都是 100）
- 复现步骤: 直接跑该用例；另 L595-598 的「买入取整」分支因 isBuy 恒假而成为死代码
- 影响范围: 股票 VWAP 的算法核心（按量分片）实际不生效，等同于固定每笔 min_open_lots；
  该文件 444 行未覆盖里有相当一部分是不可达或等效不可达路径
- 备注: 功能性缺陷，建议把 L595 的方向判断改为按 `diffQty`（与 L617 一致）（不实施）。

## BUG-R7-008

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:609、659-665
- 相关测试: `normal_stock_clamps_min_open_lots_down_to_100`（减仓目标发出买单）、
  `flat_position_buy_target_emits_zero_qty_sell`（建仓目标发出 0 量卖单）、
  `clear_all_position_filters_code`、`sell_price_floored_to_lower_limit`
- 输入/前置条件: 实仓 1000、目标 500（要减 500）；以及实仓 0、目标 1000（要买 1000）
- 期望行为: 前者发出 sell 500（或按分片），后者发出 buy
- 实际行为: `curQty = min(vailyPos, _min_open_lots)` 恒为非负 → `if (curQty > 0)` 用**量的符号**决定方向，
  于是减仓目标发出 buy 100；空仓建仓目标 curQty=0 → 落入 else 发出 `sell(code, px, 0)` 的 0 量卖单。
  价格方向却按 `diffQty` 决定（L617），出现「卖单用了买方价格、买单用了卖方价」的错位
- 复现步骤: 上述四条启用用例
- 影响范围: 真实柜台会直接拒单（0 量）或反向加仓，属资金安全风险
- 备注: 与 BUG-R7-007 同一处代码，两个表现，分开登记便于评审分别定级。

## BUG-R7-009

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / WtStockVWapExeUnit.cpp:201、WtStockMinImpactExeUnit.cpp:122、WtStockVWapExeUnit.h:364、WtStockMinImpactExeUnit.h:328
- 相关测试: `r7_stock_vwap_unit_test.cpp DISABLED_two_segment_stock_code_out_of_range`、
  `r7_stock_minimpact_unit_test.cpp DISABLED_two_segment_stock_code_out_of_range`
- 输入/前置条件: 标准化合约代码按 WonderTrader 常规形态 `SHSE.600000`（2 段）
- 期望行为: 按段数取值前先校验 `split(...).size() >= 3`，或直接取最后一段
- 实际行为: `std::stoi(StrUtil::split(stdCode, ".")[2])` 对 2 段代码是 vector 越界（未定义），
  段非数字时 `std::stoi` 抛 `std::invalid_argument` 且 init 不捕获
- 复现步骤: 启用任一 DISABLED 用例
- 影响范围: 本轮所有股票执行单元用例只能使用自造的 3 段代码 `SHSE.STK.600000` 才能跑通 init，
  即真实代码形态下这两个单元的 init 不可用
- 备注: UB/异常类，保持 DISABLED；同时登记「代码格式契约只在实现里隐式约定」的可维护性问题。

## BUG-R7-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtStockMinImpactExeUnit.cpp:100、155
- 相关测试: r7_stock_minimpact_unit_test.cpp `DISABLED_illegal_pricemode_reads_out_of_range_name_table`
- 输入/前置条件: `pricemode` 配 3（或 ≤ -2）
- 期望行为: 校验 `_price_mode ∈ [-1, 2]`，非法回退默认值并告警
- 实际行为: 日志里 `PriceModeNames[_price_mode + 1]` 直接做下标，3 → 下标 4 越界读（同一表达式其它 8 个占位符都会先求值，崩溃点在 init 内）
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 一个数字写错就让执行器初始化阶段越界读
- 备注: 另注意 do_calc 的价格模式判定（L641-673）对非法值不落到任何分支，会带着 buyPx=sellPx=0 走 L676-680 的 preclose 兜底，两处宽严不一致。

## BUG-R7-011

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtExecRunner.cpp:71-72
- 相关测试: r7_exec_runner_test.cpp `DISABLED_config_without_basefiles_section_derefs_null`
- 输入/前置条件: 合法的 JSON 配置内容（如 `{}`）但缺少 `basefiles` 段
- 期望行为: `cfgBF == NULL` 时给出明确错误并 `return false`
- 实际行为: `_config->get("basefiles")` 返回 NULL 后立刻 `cfgBF->get("session")` → 空指针解引用；
  对比同函数后面 L78-92 的 `cfgItem` 是判过空的，说明这里是漏写
- 复现步骤: 启用该 DISABLED 用例
- 影响范围: 配置少写一段就让 exec 监控进程在 config 阶段崩溃（`exec_*` 入口之后第一步）
- 备注: 崩溃类，保持 DISABLED。

## BUG-R7-012

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:170/388、WtTWapExeUnit.cpp:219/711、WtStockVWapExeUnit.cpp:289/613
- 相关测试: r7_vwap_unit_test.cpp `cancel_reports_decrement_cancel_cnt` 等价场景（本文件里 `cancel_reports_decrement_cancel_cnt` 在 r7_vwap_unit_test.cpp，断言止于 `cancelcnt -> 1`）
- 输入/前置条件: 尚未发生过一次「完整发单」的 do_calc，就收到 `on_order(..., isCanceled=true)` 且 `_cancel_cnt` 归零
- 期望行为: `_this_target` 应有确定初始值（0 或未定义即不重发）
- 实际行为: `_this_target` 只在 do_calc 走到 L388/L711/L613 时才赋值，构造函数未初始化；
  on_order 的 `!decimal::eq(realPos, _this_target)` 读的是不确定值，随后 `fire_at_once(max(_min_open_lots, _this_target - realPos))`
  可能带着 NaN/巨大值发单
- 复现步骤: 用例侧通过「先完整发一单再撤」来规避；直接对新建单元喂撤单回报即不可断言
- 影响范围: 撤单重发的数量不可预期；同时构成覆盖率天花板（该分支无法写可证伪断言）
- 备注: 建议构造函数就地初始化（不实施）。

## BUG-R7-013

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:118-119（成员 `_min_open_lots` 构造函数未初始化）、WtTWapExeUnit.cpp:158-159、WtStockVWapExeUnit.cpp:188-189
- 相关测试: 本轮所有用例都显式配 `minopenlots` 以规避
- 输入/前置条件: 配置里没有 `minopenlots` 键
- 期望行为: 缺省值 1.0（或与 `_min_hands` 一致的品种最小量）
- 实际行为: 只有 `if (cfg->has("minopenlots"))` 才赋值，`_min_open_lots` 在构造函数初始化列表里缺席
  （WtVWapExeUnit.cpp:18-36、WtTWapExeUnit.cpp:~30-49 同样如此），后续 `max(_Vwap_vol, _min_open_lots)` 读不确定值
- 复现步骤: 去掉 `minopenlots` 后跑任一发单用例，数量随栈上残留变化
- 影响范围: 缺参配置下发单量不可复现；本轮无法为该分支写可证伪断言
- 备注: 建议头文件就地初始化（不实施）。

## BUG-R7-014

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:401 与 463、WtTWapExeUnit.cpp:724 与 498、WtStockVWapExeUnit.cpp:626 与 705
- 相关测试: r7_vwap_unit_test.cpp `fire_at_once_price_mode_1_takes_ask_price`、
  r7_twap_unit_test.cpp `fire_at_once_mode_1_and_2_pricing`、
  r7_twap_unit_test.cpp `fire_at_once_unknown_price_mode_has_no_preclose_fallback`
- 输入/前置条件: 同一笔买入，分别经 do_calc 与 fire_at_once 出单
- 期望行为: 「最优价 / 对手价」在两条出单路径上含义一致；fire_at_once 也应有零价兜底
- 实际行为: do_calc 的 `price_mode==1` 买入取 **bid**，fire_at_once 的 `price_mode==1` 买入取 **ask**
  （WtVWapExeUnit.cpp:401 vs :463；TWAP 两处一致为 bid，StockVWap 两处一致为 ask，三个文件三种口径）；
  另外 TWAP/WtVWap 的 do_calc 有 `if (eq(targetPx,0)) → preclose` 兜底（L741-742 / L416-417），
  fire_at_once 没有，非法 price_mode 时直接按 0 价 + `priceTick*_cancel_times` 报出 0.01
- 复现步骤: 见三条启用用例的期望值算式
- 影响范围: 撤单重发的价格档位与正常分片不一致（对「加点重发」的预期被打断），极端情况报出无意义价
- 备注: 不阻塞覆盖率（已按实际行为断言），但需业务确认哪一条是对的。

## BUG-R7-015

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:445、WtTWapExeUnit.cpp:769、WtStockVWapExeUnit.cpp:670
- 相关测试: r7_vwap_unit_test.cpp / r7_twap_unit_test.cpp / r7_stock_vwap_unit_test.cpp 文件头的引用计数说明
- 输入/前置条件: do_calc 成功发单
- 期望行为: 与 fire_at_once（L452/L475/L686 先 `retain()` 再 `release()`）对称，do_calc 解引用 `_last_tick` 不需要额外释放
- 实际行为: do_calc 只做 `WTSTickData* curTick = _last_tick; ... curTick->release();`，没有配对的 retain
  → 每成功发一次单就把单元自己持有的那一份引用吃掉，引用计数归零时 `_last_tick` 仍指向该对象，
  下一次 on_tick/析构再 release 就是二次释放（对象回池后被别的 tick 复用，跨用例污染）
- 复现步骤: 用例侧通过「发过单的 tick 不再由用例 release」来避免踩坑（见三个文件头注释）
- 影响范围: 长时间运行的 VWAP/TWAP 单元行情对象生命周期；也是本轮断言设计的直接约束
- 备注: 建议去掉未配对的 release 或补 retain（不实施）。

## BUG-R7-016

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / WtStockVWapExeUnit.h:373-388、WtStockMinImpactExeUnit.h:332-347（`get_minOrderQty`）
- 相关测试: r7_stock_vwap_unit_test.cpp `mkSComm` 的额外 retain 注释；`init_clamps_min_order_for_*`
- 输入/前置条件: 任何一次 `init`（两个股票单元都调用 `get_minOrderQty(stdCode)`）
- 期望行为: 只读取上下文提供的品种信息，不改变其引用计数；判空要在使用之前
- 实际行为: 先 `WTSCommodityInfo* comm_info = _ctx->getCommodityInfo(...)` 再
  `strcmp(comm_info->getProduct(), cbondStr)`（**未判空就解引用**），末尾又
  `if (comm_info) comm_info->release();` —— 对借来的指针做一次多余 release，
  把 WtBaseDataMgr 里共享的品种对象引用计数打漏
- 复现步骤: 用例里 `create` 后补一次 `retain()` 才能安全收尾（已在 r7_stock_vwap / r7_stock_minimpact 落实）
- 影响范围: 品种信息可能被提前销毁；后续任何使用该代码的单元拿到悬空指针
- 备注: 建议去掉 release 并把判空前置（不实施）。

## BUG-R7-017

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtStockMinImpactExeUnit.cpp:393-407
- 相关测试: r7_stock_minimpact_unit_test.cpp `successful_entrust_also_recalculates`
- 输入/前置条件: 收到 `bSuccess == true` 的委托回报
- 期望行为: 只有委托失败才需要重算（注释 L405 写的就是「委托不成功，重新处理」）
- 实际行为: `do_calc()` 在 `if (!bSuccess)` 块**之外**（L406），成功回报同样触发一次重算；
  配上默认 `span=0`，同一笔 tick 内会重复下出第二/第三笔单
- 复现步骤: 该用例断言 `buys.size()` 从 1 变成 2 再变成 3
- 影响范围: 委托回报风暴时的重复报单风险（超买/超卖）
- 备注: 建议把 do_calc 移入 if（不实施）。

## BUG-R7-018（环境/接缝阻塞，非产品缺陷）

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:259-275、WtTWapExeUnit.cpp:388-419、WtStockVWapExeUnit.cpp:421-443、WtStockMinImpactExeUnit.cpp:328-366；WtExecMon / WtExecRunner.cpp:61-178、220、342、354-369、401-404
- 相关测试: 无（本轮无法为这些面写出可证伪断言）
- 输入/前置条件: 见「实际行为」
- 期望行为: 单元应提供一个可注入的「当前时间」，或把超时撤单判定抽成可单测的纯函数
- 实际行为: 两类面被硬规则 2 挡住：
  1. **挂钟依赖**：`on_tick` 非首笔分支的超时撤单闭包（`check_orders(expire, TimeUtils::getLocalTimeNow(), ...)`）
     需要真实时间流逝 ≥ expire 秒才会进入回调体；订单入监控器用的也是同一个挂钟，
     单测里 elapsed≈0 → 回调恒不触发。受影响行：WtVWap 263-270(8)、TWAP 397-410(14)、
     StockVWap 428-435(8)、StockMinImpact 335-346(12) + `_cancel_map` 超限强撤 350-366(17)，
     合计约 59 行不可回收（不改产品代码的前提下）。
  2. **进程级副作用**：`WtExecRunner::config()` 的完整成功路径要
     `_bd_mgr.loadSessions/loadCommodities/loadContracts/loadHolidays` 读真实基础数据文件，
     再 `initDataMgr()`（`WtSimpDataMgr::init` 会按配置建行情存储/共享内存）、
     `initParsers/initTraders`（new ParserAdapter/TraderAdapter，随后 `run()` 会起收发线程）、
     `initExecuters` + `addExeFactories`（`dlopen` executer 插件目录，与 WtCtaStraFact/WtHelper
     存在 BUG-L00-003 的同名符号冲突）。禁止线程/共享内存/真实 socket ⇒ 该文件只能拿到
     「拒绝分支 + NULL 查询」这一小片。预计可回收约 35~45 行 / 323 行（其余 ~278 行为阻塞面）。
- 复现步骤: 不适用
- 影响范围: 本轮 5 个文件的可覆盖上限
- 备注: 需要 owner 决策：要么给单元加时间注入口（同一批 `on_tick` 已有 `_sess_info`/ticktime 可控，
  只差 now 的来源），要么允许在受控临时目录里跑一次带真实配置的 `config()`。
  WtSimpDataMgr 的 `_cur_*` 未初始化已由 BUG-N05-001 登记，本轮不重复计条目，只在
  r7_exec_runner_test.cpp 里避免对 `get_real_time()/get_trading_day()` 做数值断言。
