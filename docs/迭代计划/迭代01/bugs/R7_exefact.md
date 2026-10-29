# 缺陷记录 — r7_exefact（WtExeFact VWAP/TWAP/股票执行单元 + WtExecMon 运行器）

本轮只打 5 个文件：`src/WtExeFact/WtStockVWapExeUnit.cpp`(missed 444)、`WtStockMinImpactExeUnit.cpp`(409)、
`WtVWapExeUnit.cpp`(372)、`WtTWapExeUnit.cpp`(325)、`src/WtExecMon/WtExecRunner.cpp`(323, 0.00%)。
用例：`src/TestUnits/coverage/r7_exefact/`（5 个 .cpp，共 112 条 TEST，启用 101 条、DISABLED 11 条）。

复核说明：本次只静态核对源码、接口与测试前置条件，未运行测试。测试文件中的断言仅是可复核的测试源码，不视为运行结果；“源码确认”也不等同于运行时复现。

## BUG-R7-001

- 状态: 已确认源码缺陷（赋值顺序静态确认；本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtVWapExeUnit.cpp L120+L123；src/WtExeFact/WtStockVWapExeUnit.cpp L192+L198
- 相关测试: r7_vwap_unit_test.cpp 文件头说明 + 全部 `boot`/`prime` 用例（依赖 tail_secs=0 才能判定）
- 输入/前置条件: 任何 `init(cfg)`，`total_times>=1`
- 期望行为: `_fire_span = (_total_secs - _tail_secs) / _total_times` 应在 `_total_secs = calTmSecs(begin,end)` 之后计算
- 实际行为: 两个文件都先算 `_fire_span`（此时 `_total_secs` 是构造函数设置的 0），再赋 `_total_secs`；`tail_secs==0` 时所得间隔为 0，间隔闸门恒放行；`tail_secs>0` 时无符号减法下溢，间隔成为极大值，通常会长期拦截发单。实际时长影响由源码算式可推导，未做运行测量
- 复现步骤: 源码赋值顺序直接证明；初始化日志打印在赋 `_total_secs` 前，不能单独作为运行证据
- 影响范围: VWAP 期货/股票两条链的发单节奏；TWAP 顺序是对的（L162 先于 L166），可作对照
- 备注: 修复方向：先计算 `_total_secs`，再校验 `tail_secs <= _total_secs` 并计算间隔；启用测试用非零尾部时间断言派生值。本次未改生产代码。

## BUG-R7-002

- 状态: 已确认源码缺陷（零分母配置可达；本次未运行）
- 严重级别: Blocker
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:120、WtTWapExeUnit.cpp:166、WtStockVWapExeUnit.cpp:192
- 相关测试: r7_vwap_unit_test.cpp `DISABLED_init_zero_total_times_divide_by_zero`；r7_twap_unit_test.cpp `DISABLED_init_zero_total_times_divide_by_zero`
- 输入/前置条件: 配置 `total_times = 0`（缺省即 0：`cfg->getUInt32("total_times")` 对不存在的键返回 0，且无范围校验）
- 期望行为: 计算间隔前校验 `total_times >= 1`；非法配置需要明确诊断和失败关闭行为。当前 `init` 返回 void，不能单凭此接口断定应回退到某个默认次数。
- 实际行为: `_fire_span` 计算对 `total_times==0` 作整数除法，构成未定义行为；常见平台会触发 SIGFPE。具体信号未在本轮运行验证
- 复现步骤: DISABLED 用例给出 `mkCfg(0, ...)` / `mkTCfg(0, ...)` 前置；本次未运行
- 影响范围: 缺失或为零的执行次数会在上述三个执行单元初始化时触发未定义行为；是否异常退出、静默产生错误间隔或影响进程启动取决于平台、编译方式和实际加载路径，本次尚未证明真实进程启动结果。
- 备注: 建议在初始化时拒绝 `total_times==0` 并提供诊断；用例保持 DISABLED，直到通过子进程/信号隔离方式验证。

## BUG-R7-003

- 状态: 已确认源码缺陷（缺分布校验及 `operator[]` 边界保护；本次未运行）
- 严重级别: Blocker
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:372、WtStockVWapExeUnit.cpp:578（配对的 init 早退在 L128-131 / L228-231）
- 相关测试: r7_vwap_unit_test.cpp `DISABLED_short_distribution_file_out_of_range_read`；
  启用侧对照 `init_missing_distribution_file_returns_early`
- 输入/前置条件: 分布文件 `Vwap_<品种名>.txt` 不存在（`VwapAim` 为空）或行数 < 241；随后收到一笔 tick
- 期望行为: `do_calc` 在取分桶前校验 `VwapAim.size()`，不足则不发单
- 实际行为: `double InminsTm = calTmStamp(...)`（值域 0..240）直接 `VwapAim[InminsTm]`，
  用 double 作下标且无任何边界检查 → 越界读/UB。init 发现文件缺失只是 `return`（L131/L231），
  单元照常进入发单路径
- 复现步骤: DISABLED 用例描述 10 格分布与第 30 格访问；本次未运行
- 影响范围: 品种漏配分布文件时，VWAP 单元首次成交即越界；且 `_Vwap_vol` 会拿到垃圾值
- 备注: `double` 下标会隐式转为整数；缺失/短文件后仍可进入下单路径。建议 init 明确失败并在访问前校验长度与下标；尚无运行时崩溃证据。

## BUG-R7-004

- 状态: 待验证（分支不可达由源码确认；业务影响仅覆盖净持仓为零但多头子仓仍非零的场景，本次未运行）
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:348-362、WtTWapExeUnit.cpp:634-653、WtStockVWapExeUnit.cpp:554-568
- 相关测试: r7_vwap_unit_test.cpp `clear_target_dbl_max_exits_without_order`、r7_twap_unit_test.cpp `clear_target_with_flat_position_does_nothing`
- 输入/前置条件: 清仓标记被映射为真实目标 0，净仓位已为 0，但 `getPosition(code,true,1)` 仍返回非零多头子仓
- 期望行为: 若产品需要清理残余多头子仓，应执行该单元中明确编写的 `newVol = -min(lPos, _order_lots)` 兜底
- 实际行为: 三处都先在 `diffQty == 0` 时返回，后续 `curPos == newVol` 清仓分支不可达。静态不可达确定；该多头子仓场景是否在目标账户模式中有效、是否属于产品承诺尚未验证
- 复现步骤: 现有启用用例仅证明平仓标记+空仓不发单，不能证明多头子仓的清仓需求
- 影响范围: 代码中为净仓位为零时保留的兜底逻辑无法执行；“全部清仓功能普遍失效”的原结论过宽
- 备注: 需用明确的持仓语义和 fake position 组合测试确认后再定缺陷。源码分支可在完成判定之前处理，或删除无效分支。

## BUG-R7-005

- 状态: 本机替身隔离复现 signal 11；真实通道及修复后的生命周期未验证
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:517-519（判空在 L547）
- 相关测试: `r7_stock_vwap_unit_test.cpp` 的 `channel_ready_before_first_tick_returns_without_orders` 和 `channel_ready_after_first_tick_returns_without_orders`。
- 输入/前置条件: `init` → `set_position(非0目标)` → `on_channel_ready()`（此时还没有任何 tick）
- 期望行为: 与其它三个单元一致，先判 `_last_tick == NULL` 再解引用
- 实际行为: `if (decimal::ge(_start_price, 0)) _start_price = _last_tick->price();` 位于 L547 判空之前；在目标和持仓不等、其他早退条件不触发且默认 `_start_price` 为 0 时，空 `_last_tick` 被解引用。`WtLocalExecuter` 和 `WtDiffExecuter` 在通道已就绪时可在执行单元初始化后直接通知就绪，没有强制要求先收到行情。
- 复现步骤: 在本地 fake 上用非零目标触发无行情的就绪回调，父进程收到调用前标记后子进程 signal 11；已有行情且目标为 10 股的无下单控制 PASS。源码支持空指针归因，但本次未采集崩溃栈。
- 影响范围: 通道先就绪、目标与持仓不同且到达上述分支的股票 VWAP 单元；不据此推断所有非零目标都会到达该分支，也不声称真实柜台已复现。
- 修复方向: 在任何 `_last_tick` 访问之前检查行情是否存在，无行情时保留待执行目标、记录原因并等待有效 tick；恢复行情后以隔离 BUG-R7-015 的方式验证正常计算与生命周期。
- 备注: `_start_price` 后续没有读取点，因此“每次覆写开盘基准价”的附加影响不成立。对照目标小于最小手数并停在完成判断，不覆盖下单路径；临时额外引用用于隔离现有商品对象所有权缺陷。

## BUG-R7-006

- 状态: 已确认源码缺陷（空品种信息未被一致拒绝/保护；本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / WtTWapExeUnit.cpp:508/734/738、WtVWapExeUnit.cpp:408/412/470、WtStockVWapExeUnit.cpp:633/637/714
- 相关测试: r7_twap_unit_test.cpp `DISABLED_null_commodity_info_crashes_price_offset_branch`
- 输入/前置条件: `ctx->getCommodityInfo()` 返回 NULL（品种表未加载/代码写错）
- 期望行为: 缺少 commodity info 时，在 init 明确失败或在使用前退出并记录诊断
- 实际行为: TWAP 在价格偏移路径解引用空 `_comm_info`（DISABLED 测试只覆盖此类路径）；VWAP/StockVWAP 还会在 init 的 `getName`、`get_minOrderQty` 或 `getTradingMode` 等更早位置解引用。故原记录的单一“do_calc/fire_at_once”崩溃点描述不完整，但未处理空品种信息的问题成立
- 复现步骤: TWAP 的 DISABLED 用例构造 `ctx.comm = NULL` 和非零 offset；测试未运行。其它类需单独补前置，不应由该用例代证
- 影响范围: 基础数据缺失时执行单元崩溃而非报警退出
- 备注: 建议对品种信息作统一的初始化失败处理；运行时影响和其它单元的触发位置尚未独立验证。

## BUG-R7-007

- 状态: 本机替身测试复现；真实柜台及分片业务合同待验证
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:530、542、595-609
- 相关测试: `r7_stock_vwap_unit_test.cpp` 的 `vwap_bucket_distribution_changes_planned_buy_qty`、`flat_position_buy_target_requires_a_buy`、`odd_lot_clear_target_sells_within_available_position`、`clear_all_position_filters_code` 等
- 输入/前置条件: 两个完全相同的增仓场景，只把分布文件 `Vwap_*.txt` 的每格累计目标从 1300 换成 2500；另外单独覆盖空仓建仓、碎股清仓和普通清仓。
- 期望行为: 分片量随 VWAP 预测分布变化（L582 `_Vwap_vol = aimQty - curPos`、L591 取该值）
- 实际行为: 到达 L595 意味着 `undone==0`，但 L530 的 `isBuy` 由 `undone` 得出，必为 false；随后 L609 又无条件用 `min(vailyPos, _min_open_lots)` 覆盖此前的分桶量。L617 后按 `diffQty` 重设方向，然而实际下单在 L659-665 又按被覆盖后的 `curQty` 符号选择 buy/sell。因此买入目标可能发 0 量卖单，减仓目标可能发买单，分桶量也不控制数量
- 复现步骤: 修正 `SFakeCtx` 的可用/总仓参数语义和商品构造参数后，构建 `CoverageTests_exefact` 并逐例运行。本地分桶对照 FAIL：两组分别发 100、100，未随累计目标变大；空仓建仓没有买单、碎股和普通清仓没有预期卖单，正确方向断言 FAIL。另修正 R24 分布文件名及对象生命周期后，`r24_stkvw.price_modes_preserve_sell_direction` 的两种价格模式均在总仓/可用仓 500、目标 200 时发买单而非卖单，定向 FAIL；用例暂时保留调用者 tick 引用以隔离 BUG-R7-015，不能证明正常引用生命周期安全。仅验证本地假执行上下文，不证明真实交易所行为或最终分片公式。
- 影响范围: Stock VWAP 的分片数量与下单方向控制存在同源错误；原 BUG-R7-008 登记的反向/零量表现并入本条
- 备注: 修复时保留 `diffQty` 的方向符号，并根据可卖仓量限制绝对数量；在业务确认分片公式后分别验收买入、减仓、零仓和碎股场景。

## BUG-R7-008

- 状态: 已合并（与 BUG-R7-007 同一控制流缺陷；不单独计数）
- 严重级别: Blocker
- 模块/文件: WtExeFact / src/WtExeFact/WtStockVWapExeUnit.cpp:609、659-665
- 相关测试: 当前正确契约用例见 BUG-R7-007；旧 `normal_stock_clamps_min_open_lots_down_to_100` 已改为增仓对照，不再作为减仓证据。
- 输入/前置条件: 实仓 1000、目标 500（要减 500）；以及实仓 0、目标 1000（要买 1000）
- 期望行为: 前者发出 sell 500（或按分片），后者发出 buy
- 实际行为: `curQty = min(vailyPos, _min_open_lots)` 恒为非负 → `if (curQty > 0)` 用**量的符号**决定方向，
  于是减仓目标发出 buy 100；空仓建仓目标 curQty=0 → 落入 else 发出 `sell(code, px, 0)` 的 0 量卖单。
  价格方向却按 `diffQty` 决定（L617），出现「卖单用了买方价格、买单用了卖方价」的错位
- 复现步骤: 参见 BUG-R7-007 下修正后的空仓建仓、碎股及清仓定向测试；旧测试名和夹具已调整。
- 影响范围: 本地替身能观察到不符合目标方向的行为；真实柜台对零量或反向委托的处理尚未验证。
- 备注: 本条中的反向下单与零量下单是 BUG-R7-007 同一 `curQty` 覆盖/符号复用缺陷的表现，保留为补充证据，不单独作为编号计数。

## BUG-R7-019 Stock VWAP 将可用仓位和总仓位反向读取

- 状态: 本机合约对齐的替身测试复现；真实账户状态未验证
- 严重级别: Major（非 T+0 且总仓与可用仓不同时影响目标和下单判断）
- 模块/文件: `src/WtExeFact/WtStockVWapExeUnit.cpp:494-505`；接口 `src/Includes/ExecuteDefs.h:52-59`；参考 `src/WtCore/TraderAdapter.cpp:428-452`。
- 输入/前置条件: 执行上下文有总仓 1000、可用仓 200，非 T+0，目标清仓。`getPosition(code, true)` 和默认参数按接口为可用仓，`false` 为总仓。
- 期望行为: 按 false 获取总仓、true 获取可用仓，将不可卖的 800 保留在目标中，卖出量不能超过 200。
- 实际行为: 实现把默认 true 读取标为 `realPos`（总仓），把 false 读取标为 `vailyPos`（可用仓）；合约一致的替身下目标上调日志及卖单预期均未出现，本地 `insufficient_sellable_position_adjusts_target_up` FAIL。原测试的 fake 也把这两个参数反着实现，导致旧测试不能作为产品正确证据。
- 修复方向: 明确总仓与可用仓访问语义并交换参数，按同一时点一致的仓位计算目标和可卖量；再与 BUG-R7-007 的方向/分片修复一起覆盖可用仓小于总仓、空仓、T+0 和清仓。未进行真实柜台/持仓验证。

## BUG-R7-009

- 状态: 已撤回（两段式 `SSE.600000` 是已弃用简写，不是当前标准股票代码）
- 严重级别: Major
- 模块/文件: WtExeFact / WtStockVWapExeUnit.cpp:201、WtStockMinImpactExeUnit.cpp:122、WtStockVWapExeUnit.h:364、WtStockMinImpactExeUnit.h:328
- 相关测试: `r7_stock_vwap_unit_test.cpp DISABLED_two_segment_stock_code_out_of_range`、
  `r7_stock_minimpact_unit_test.cpp DISABLED_two_segment_stock_code_out_of_range`
- 输入/前置条件: 原用例传入已弃用简写 `SHSE.600000`（2 段）
- 期望行为: 按段数取值前先校验 `split(...).size() >= 3`，或直接取最后一段
- 实际行为: `std::stoi(StrUtil::split(stdCode, ".")[2])` 对 2 段代码是 vector 越界（未定义），
  段非数字时 `std::stoi` 抛 `std::invalid_argument` 且 init 不捕获
- 复现步骤: 启用任一 DISABLED 用例
- 影响范围: 当前源码与版本说明将股票标准代码规范为三段 `SSE.STK.600000`；原例不是当前有效输入，因此不能据此认定正常 init 不可用
- 备注: `updatelog.md:129` 明确记载股票简写不再支持，`CodeHelper.hpp:603-606` 也列出标准三段形式。 malformed third segment 的校验属于独立输入验证问题，不在本条范围。

## BUG-R7-010

- 状态: 已合并（迭代02：主编号 BUG-R17-002）
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtStockMinImpactExeUnit.cpp:100、155
- 相关测试: r7_stock_minimpact_unit_test.cpp `DISABLED_illegal_pricemode_reads_out_of_range_name_table`
- 输入/前置条件: `pricemode` 配 3（或 ≤ -2）
- 期望行为: 校验 `_price_mode ∈ [-1, 2]`，非法回退默认值并告警
- 实际行为: 日志里 `PriceModeNames[_price_mode + 1]` 直接做下标，3 → 下标 4 越界读（同一表达式其它 8 个占位符都会先求值，崩溃点在 init 内）
- 复现步骤: DISABLED 用例描述非法 `pricemode` 前置；本次未运行
- 影响范围: 一个数字写错就让执行器初始化阶段越界读
- 备注: `PriceModeNames` 长度为 4，非法值 3 对应下标 4；修复应在取日志名称前校验/映射模式。本次只静态确认越界路径，未验证进程崩溃形态。


- 迭代02复核: DUPLICATE。与 BUG-R17-002 同属对无效 pricemode 值执行 PriceModeNames[_price_mode+1] 而越界；本条只是在股票单元的同形实现，非法值合同仍待确认。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 已确认源码缺陷（越界日志索引静态确认；本次未运行）
## BUG-R7-011

- 状态: 已确认源码缺陷（配置缺段导致空指针解引用；本次未运行）
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtExecRunner.cpp:71-72
- 相关测试: r7_exec_runner_test.cpp `DISABLED_config_without_basefiles_section_derefs_null`
- 输入/前置条件: 合法的 JSON 配置内容（如 `{}`）但缺少 `basefiles` 段
- 期望行为: `cfgBF == NULL` 时给出明确错误并 `return false`
- 实际行为: `_config->get("basefiles")` 返回 NULL 后立刻 `cfgBF->get("session")` → 空指针解引用；
  对比同函数后面 L78-92 的 `cfgItem` 是判过空的，说明这里是漏写
- 复现步骤: DISABLED 用例描述缺少 `basefiles` 的配置前置；本次未运行
- 影响范围: 配置少写一段就让 exec 监控进程在 config 阶段崩溃（`exec_*` 入口之后第一步）
- 备注: 建议 `cfgBF==NULL` 时记录错误并返回 false；DISABLED 用例只是源码中的复现前置，本次未运行。

## BUG-R7-012

- 状态: 已确认源码缺陷（存在可达初始化回调路径；本次未运行）
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:170/388、WtTWapExeUnit.cpp:219/711、WtStockVWapExeUnit.cpp:289/613
- 相关测试: r7_vwap_unit_test.cpp `cancel_reports_decrement_cancel_cnt` 等价场景（本文件里 `cancel_reports_decrement_cancel_cnt` 在 r7_vwap_unit_test.cpp，断言止于 `cancelcnt -> 1`）
- 输入/前置条件: `on_channel_ready` 发现外部未完成单且本地监控为空，先提交方向撤单并增加 `_cancel_cnt`；在任何本地完整发单之前收到这些撤单回报，计数归零
- 期望行为: `_this_target` 应有确定初始值（0 或未定义即不重发）
- 实际行为: `_this_target` 构造函数未初始化；`on_channel_ready` 可以先把外部订单 id 放进 `_orders_mon`，并因 `_cancel_cnt!=0` 使 `do_calc` 早退，随后 `on_order` 在未写 `_this_target` 前读取它并计算重发量
- 复现步骤: 原启用测试未覆盖该启动顺序；本次只由调用控制流确认，未运行
- 影响范围: 启动时清理外部挂单的取消回报可能触发未定义的重发数量
- 备注: 构造函数初始化只能提供确定值，仍应在未形成有效 `_this_target` 时禁止重发；需新增具体启动顺序测试。

## BUG-R7-013

- 状态: 已确认源码缺陷（可选配置缺省时读取未初始化成员；本次未运行）
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:118-119（成员 `_min_open_lots` 构造函数未初始化）、WtTWapExeUnit.cpp:158-159、WtStockVWapExeUnit.cpp:188-189
- 相关测试: 本轮所有用例都显式配 `minopenlots` 以规避
- 输入/前置条件: 配置里没有 `minopenlots` 键
- 期望行为: 缺省值 1.0（或与 `_min_hands` 一致的品种最小量）
- 实际行为: 只有 `if (cfg->has("minopenlots"))` 才赋值，`_min_open_lots` 在构造函数初始化列表里缺席
  （WtVWapExeUnit.cpp:18-36、WtTWapExeUnit.cpp:~30-49 同样如此），后续 `max(_Vwap_vol, _min_open_lots)` 读取未初始化成员，行为未定义
- 复现步骤: 去掉 `minopenlots` 后运行发单用例可作为验证前置；本次未运行，具体数量表现不能由源码唯一确定
- 影响范围: 缺参配置下发单量不可复现；本轮无法为该分支写可证伪断言
- 备注: 现有用例均显式传入 `minopenlots`，没有覆盖省略键路径。应定义明确缺省值或在 init 统一赋值；本次未改生产代码。

## BUG-R7-014

- 状态: 待验证（撤单重发价差静态确认；模式契约与预期报价仍需业务确认，本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:401 与 463、WtTWapExeUnit.cpp:724 与 498、WtStockVWapExeUnit.cpp:626 与 705
- 相关测试: r7_vwap_unit_test.cpp `fire_at_once_price_mode_1_takes_ask_price`、
  r7_twap_unit_test.cpp `fire_at_once_mode_1_and_2_pricing`、
  r7_twap_unit_test.cpp `fire_at_once_unknown_price_mode_has_no_preclose_fallback`
- 输入/前置条件: 同一笔买入，分别经 do_calc 与 fire_at_once 出单
- 期望行为: 「最优价 / 对手价」在两条出单路径上含义一致；fire_at_once 也应有零价兜底
- 实际行为: VWAP 与 StockVWAP 的 `do_calc` 模式 1 均取己方最优价（买 bid、卖 ask），对应 `fire_at_once` 却取对手价（买 ask、卖 bid）；TWAP 的两条路径模式 1 均取己方最优价。TWAP `fire_at_once` 对非法模式留在零价后再加偏移，VWAP/StockVWAP 则把其它模式映射到对手价。路径差异是源码事实，但外部配置对模式及重发价的统一承诺未找到
- 复现步骤: 测试源码有不同路径的价格断言；本次未运行，且现有断言只证明当前实现输出，不单独证明哪种价格是产品期望
- 影响范围: 若 mode 1 契约为己方最优价，VWAP/StockVWAP 撤单重发报价偏离常规分片；非法模式是否接受及零价订单是否可能到柜台需进一步验证
- 备注: 先统一 `price_mode` 契约，再按同一 bid/ask 测试覆盖常规与撤单重发路径；不可把测试中的当前输出当成业务 oracle。

## BUG-R7-015

- 状态: 已确认源码缺陷（持有引用被非配对释放；本次未运行）
- 严重级别: Minor
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:445、WtTWapExeUnit.cpp:769、WtStockVWapExeUnit.cpp:670
- 相关测试: r7_vwap_unit_test.cpp / r7_twap_unit_test.cpp / r7_stock_vwap_unit_test.cpp 文件头的引用计数说明
- 输入/前置条件: do_calc 成功发单
- 期望行为: 与 fire_at_once（L452/L475/L686 先 `retain()` 再 `release()`）对称，do_calc 解引用 `_last_tick` 不需要额外释放
- 实际行为: do_calc 只做 `WTSTickData* curTick = _last_tick; ... curTick->release();`，没有配对的 retain
  → 每成功发一次单就把单元自己持有的那一份引用吃掉，引用计数归零时 `_last_tick` 仍指向该对象，
  下一次 on_tick/析构再 release 就是二次释放（对象回池后被别的 tick 复用，跨用例污染）
- 复现步骤: 用例侧通过「发过单的 tick 不再由用例 release」来避免踩坑（见三个文件头注释）
- 测试边界更新: `r7_twap.last_slice_showhand_buys_remaining_with_5_tick_premium` 已修正原先反复设置目标导致未走尾桶的夹具，并在保持同一目标、模拟部分成交/余单终结后运行 PASS。该测试使用基于当前引用数的临时交接以规避本缺陷；此 PASS 只覆盖尾桶数量和报价，不证明 tick 生命周期安全。修复本缺陷后应移除该规避并单独验证标准 retain/release。
- 影响范围: 按 `on_tick` 已 retain、替换/析构时再 release 的所有权模式，do_calc 额外 release 会使成员仍指向已少一份引用的对象；后续替换或析构可能重复释放。对象池后果未运行验证
- 备注: 建议删除非配对 release，或给临时借用增加独立 retain/release；测试文件为此调整 tick 释放方式，但本次未运行。

## BUG-R7-016

- 状态: 已确认源码缺陷（manager 返回借用指针；本次未运行）
- 严重级别: Major
- 模块/文件: WtExeFact / WtStockVWapExeUnit.h:373-388、WtStockMinImpactExeUnit.h:332-347（`get_minOrderQty`）
- 相关测试: r7_stock_vwap_unit_test.cpp `mkSComm` 的额外 retain 注释；`init_clamps_min_order_for_*`
- 输入/前置条件: 任何一次 `init`（两个股票单元都调用 `get_minOrderQty(stdCode)`）
- 期望行为: 只读取上下文提供的品种信息，不改变其引用计数；判空要在使用之前
- 实际行为: `ExecuteContext` 的 getter 向上游 raw pointer 透传；`WtBaseDataMgr::getCommodity` 从持有对象的 map 直接返回且不 retain。`init` 已对保存的 `_comm_info` retain，而 helper 再
  `strcmp(comm_info->getProduct(), cbondStr)`（**未判空就解引用**），末尾又
  `if (comm_info) comm_info->release();` —— 对借来的指针做一次多余 release，
  把 WtBaseDataMgr 里共享的品种对象引用计数打漏
- 复现步骤: 用例里 `create` 后补一次 `retain()` 才能安全收尾（已在 r7_stock_vwap / r7_stock_minimpact 落实）
- 影响范围: 品种信息可能被提前销毁；后续任何使用该代码的单元拿到悬空指针
- 备注: 判空必须先于 `getProduct()`；删除 helper 对借用指针的 `release()`。单元已有持有的 `_comm_info` 可复用。本次未改代码或运行验证。

## BUG-R7-017

- 迭代02第二轮前状态（历史）: - 状态: 待验证（公共回调会无条件重算；生产成功回报的可达性及 OMS 顺序未确认）
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: Major
- 模块/文件: WtExeFact / src/WtExeFact/WtStockMinImpactExeUnit.cpp:393-407
- 相关测试: r7_stock_minimpact_unit_test.cpp `successful_entrust_also_recalculates`
- 输入/前置条件: 收到 `bSuccess == true` 的委托回报
- 期望行为: 只有委托失败才需要重算（注释 L405 写的就是「委托不成功，重新处理」）
- 实际行为: `do_calc()` 在 `if (!bSuccess)` 块外，无论成功失败都会调用。测试 fake 在成功回报后保持 `undone==0`，因此可再次发单；生产环境中成功回报与 OMS 未完成量更新的先后顺序未由该单元测试覆盖
- 复现步骤: `r7_stkmin.manually_invoked_success_entrust_callback_recalculates` 本机单项 PASS，证明手动调用成功回报时 fake context 的订单数量增加；仓内 `TraderAdapter::onRspEntrust` 的失败分支会转发 `false`，尚未找到对应成功回报的生产转发路径。该测试不证明线上存在这个回调顺序。
- 影响范围: 若成功回报到达时 `getUndoneQty` 尚未反映已接收订单，存在重复发单窗口；该窗口及风险大小需用真实回调顺序验证
- 备注: 源码与注释不一致；修复倾向仅在失败时重算，或需提供为何成功也重算且有防重条件的契约。

## BUG-R7-018（环境/接缝阻塞，非产品缺陷）

- 状态: 已撤回（仅为覆盖限制，不是产品缺陷；集成测试边界另行保留）
- 严重级别: 不适用（覆盖边界）
- 模块/文件: WtExeFact / WtVWapExeUnit.cpp:259-275、WtTWapExeUnit.cpp:388-419、WtStockVWapExeUnit.cpp:421-443、WtStockMinImpactExeUnit.cpp:328-366；WtExecMon / WtExecRunner.cpp:61-178、220、342、354-369、401-404
- 相关测试: `r7_exec_runner_test.cpp` 已有拒绝分支和空 adapter `run()` 覆盖；本次未运行。超时集成路径未见注入时钟测试
- 输入/前置条件: 见「实际行为」
- 期望行为: 需要验证时间驱动的超时撤单集成逻辑；注入时钟或抽出纯函数可让该路径确定性可测
- 实际行为: `on_tick` 超时检查直接取 `TimeUtils::getLocalTimeNow()`，所以依赖挂钟的集成路径不适合用不等待时间的单元测试；`WtOrdMon::check_orders` 自身接受显式 `curTime`，仍可独立测试。对行数不可测上限的旧结论过强。`WtExecRunner::config()` 在 basefiles/data 为空对象、给受控的空 `bspolicy` 文件且不配置 adapter 路径时，可避开真实基础数据、存储 plugin 及 adapter 线程；`initDataMgr()` 还忽略 `_data_mgr.init()` 返回值。空 adapter `run()` 已有测试源码。真实 reader 插件和已装配 adapter 的集成仍需受控环境
- 复现步骤: 不适用；上述最小 config 可测性是静态路径推断，本次未运行
- 影响范围: 可确认存在挂钟耦合与真实 reader/plugin 集成依赖；约 59 行和 278 行的 blocked 估算没有证据，不应作为覆盖率上限
- 备注: 该条是测试设计/集成边界，不登记为产品 bug。后续可为超时判定加时钟 seam；最小 config 路径可用隔离临时文件做无 provider 冒烟。`WtSimpDataMgr::_cur_*` 仍由 BUG-N05-001 管理。
