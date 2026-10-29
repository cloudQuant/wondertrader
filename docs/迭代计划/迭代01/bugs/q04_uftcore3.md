# q04_uftcore3 待审核缺陷（lane q04 / WtUftCore 交易回报与风控推进）

> 2026-10-29 复核记录。本次只编辑本文件；区分源码可确认事实、定向测试观察和未取得的动态证据。
> Q04 测试使用替身 API/基础数据及本地临时文件，不构成真实柜台、实盘或生产会话验证。
> 与 `docs/迭代计划/迭代01/bugs/n03_uftcore_index.md`（BUG-N03-001…008）、
> `docs/迭代计划/迭代01/bugs/p06_uftcore2.md`（BUG-P06-001…006）重复的根因在下文标明 canonical 条目，不另行计数。

## BUG-Q04-001

- 状态: 静态缺陷成立；2026-10-29 定向用例 FAIL
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:284-302`（`getOrders`）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_reply_machine_test.cpp` → `q04_trader_reply.onRspOrders_counts_stats_and_registers_alive_orders`
- 输入/前置条件: 通道登记了标准代码 `SHFE.ag2406` 的活单，调用 `getOrders("SHFE.ag2406")`
- 期望行为: 与同文件 `cancelAll`（`TraderAdapter.cpp:414` 用 `cInfo->getFullCode()` 比较）一致，按标准代码过滤出该合约的委托
- 实际行为: `getOrders` 直接比较 `ordInfo->getCode()` 与输入。当订单保存柜台裸代码 `ag2406`、调用方传标准代码 `SHFE.ag2406` 时二者不等，查不到该订单；空字符串走“返回全部”分支，传裸代码则可能匹配。不能据此概括为所有柜台订单都只接受空过滤。
- 复现步骤: 已将用例改为断言 `getOrders(kStdAg)` 返回该活单及 local ID；2026-10-29 重新构建后单跑 FAIL，实际 size 为 0。空代码查询仍保留对照。
- 影响范围: 对含柜台裸代码订单使用全限定 `stdCode` 查询的调用方会漏掉匹配订单；尚无证据证明所有策略、风控或清理路径都会使用该查询。
- 复核边界: `onRspOrders` 可通过基础数据查到合约用于统计，但没有把该 `cInfo` 写回 `WTSOrderInfo`；本定向测试以替身订单复核了裸代码/标准代码差异。结果是本机测试证据，不是柜台报文覆盖。
- 修复建议: 以合约完整代码过滤；优先使用订单已有合约信息，缺失时按裸代码与交易所从基础数据查询，并明确查不到合约时的处理。覆盖空过滤、裸代码、标准代码、无合约信息及不同交易所同裸代码。

## BUG-Q04-002

- 状态: 测试替身路径 FAIL；真实柜台返回语义待核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:311-343`（`doEntrust`）、`:994-1108`（四个下单入口）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp` → `q04_trader_entrust.insert_failure_returns_uint_max_without_counting_undone`
- 输入/前置条件: 测试替身 `orderInsert` 同步返回 `-1`，调用 `openLong("SHFE.ag2406", 5000, 2, 0)`；此处将负值解释为本笔未被接受。`ITraderApi` 基类默认返回 `-1`，但基类注释未统一规定各柜台插件返回值语义。
- 期望行为: 委托失败时回滚本笔 `updateUndone(+qty)`，未完成量回到下单前的值
- 实际行为: 四个入口都在 `doEntrust` 前增加未完成量；`doEntrust` 遇到负返回值只记日志并返回 `UINT_MAX`，不回滚。本替身不再发送后续错误回报，因此拒单后立即保留了该数量；若真实 API 随后回调，`onRspEntrust` 另有扣减路径，最终量及重复/并发回报行为还需按插件契约验证。失败单没有相应 `_orders` 条目。
- 复现步骤: 2026-10-29 重新构建后的用例单跑 FAIL：同步拒单后期望 0，实际为 2；随后成功 3 手时期望 3，实际为 5。
- 影响范围: 在同步返回负值且没有后续错误回报时，依赖 `getUndoneQty` 的调用方会读到本次虚增量；尚无真实插件或实盘证据证明此路径在各柜台上的表现。
- 修复建议: 明确 API 的同步拒绝契约后，仅对确定未被接受的委托回滚本笔预占量，或将计入时点移到接受之后；为同步返回与异步拒单建立同一单笔幂等状态，避免重复扣减。扩展验证四个入口、后续成功单、多笔在途单、异步/重复错误回报。

## BUG-Q04-003

- 状态: 已合并（主编号 BUG-N03-004；Q04 测试作为补充证据）
- 严重级别: 沿用 BUG-N03-004（Major）
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:1970-1984`（`getRiskParams`）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp` → `q04_trader_risk.product_cancel_limit_overrides_permissive_default`
- 输入/前置条件: `riskmon.policy` 里同时配了 `default` 与品种组 `ag`，检查代码 `SHFE.ag2406`
- 期望行为: 取品种 id 段 `ag2406` 的字母前缀 `ag`，即 `std::string(stdCode + idx + 1, eIdx - idx - 1)`
- 实际行为: 对 `SHFE.ag2406`，切片结果为 `ag24`，而非 `ag`；若有 `default` 会回落到默认参数，若没有 `default` 则落入 BUG-N03-004 所记的无默认组问题。
- 动态证据边界: 修正后的本地 Q04 测试先核对撤单计数已累计，期望限额 1 的 `ag` 组拒绝，实际返回 true，测试 FAIL。此 fixture 缺模块配置、init 按测试前提早退后保留了风控表，属于局部替身证据；`check*Limits` 在正常下单/撤单入口的调用目前均被注释，不能据此声称实盘风控已因此失效或必然触发交易所处罚。
- 归并建议: 产品根因、修复和严重级别归入 BUG-N03-004；修复产品键切片和缺省组处理后保留该 `ag` 组限额回归。

## BUG-Q04-004

- 状态: 源码问题成立；动态频率路径未验证且当前未接入正常下单/撤单流程
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:337-341`、`:391-392`、`:419-420`（被注释的时间戳写入）、`:1875-1903`、`:1937-1965`（只读的时间窗）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp`（两个 `check*` 用例只能命中总量支）
- 输入/前置条件: 要求频率限额生效，且调用下单/撤单路径后进入 `checkOrderLimits/checkCancelLimits`。
- 期望行为: 下单/撤单成功后把时间戳写入 `_order_time_cache` / `_cancel_time_cache`，`check*Limits` 的时间窗分支据此判定频率超限
- 实际行为: 写入时间戳的语句被注释，当前源码没有其他缓存写入点；`check*Limits` 只能读取缓存，频率窗口分支没有由本模块填充的数据。与此同时，`buy/sell/open*/close*/doCancel` 以及 `UftStraContext::stra_cancel_all` 中调用限额检查的语句也被注释。因此正常委托路径当前没有证据表明会执行这些频率限额。
- 动态证据边界: Q04 风控测试直接调用限额方法并覆盖总量路径；没有频率缓存写入入口，不能把这些用例描述成频率超限实测，也没有运行出频率型 FAIL。
- 影响范围: 静态确认频率窗口实现当前不可由适配器正常路径驱动；“生产频率风控失效”须先确认产品要求与该门控原本的启用状态。覆盖率不是产品影响。
- 修复建议: 若频率限额仍属产品契约，恢复并接通下单/撤单门控及时间戳写入，统一写入/查询 key（当前注释写入使用裸代码、检查使用标准代码），再用可控时钟验证阈值内/超阈值、窗口过期和缓存裁剪。只有产品明确取消该能力后，才考虑移除配置和死分支。

## BUG-Q04-005

- 状态: 已合并（主编号 BUG-N03-004 的缺省组子情形；当前仅静态确认）
- 严重级别: 沿用 BUG-N03-004（Major）
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:1978-1983`（`getRiskParams` 回落）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp` → `DISABLED_q04_trader_risk.no_default_group_and_null_statmap_class` 第 (3) 段
- 输入/前置条件: `riskmon.active=true` 但 policy 没有 `default`，或风控未激活/未配置后直接调用公开 `check*Limits`。初始化对缺少 `default` 只记告警，不拒绝装配；检查函数没有先按 `_risk_mon_enabled` 放行。
- 期望行为: 对活动策略缺少默认组时，在初始化阶段明确拒绝配置或定义确定的无匹配行为；风控未激活时，检查方法应确定性放行。并不预设 `getRiskParams` 必须返回 `NULL`。
- 实际行为: 未找到品种规则后仍查找 `default` 并直接取 `it->second`；map 为空或无默认组时对 `end()` 解引用，属于未定义行为。
- 动态证据边界: `DISABLED_q04_trader_risk.no_default_group_and_null_statmap_class` 第 (3) 段未运行；不能记录为已复现崩溃。正常订单/撤单集成门控也处于注释状态，此风险限于直接调用检查方法或未来重新接线的路径。
- 归并建议: 与 BUG-N03-004 同一函数、同一缺省查找根因，合并其前置条件和修复，不作为独立缺陷计数。该分支与 N03-002 仅属相似的 end 解引用模式，不是同一代码缺陷。

## BUG-Q04-006

- 状态: 已合并（主编号 BUG-N03-006；BUG-R204-001 为同源记录）
- 严重级别: 沿用 BUG-N03-006
- 模块/文件: `src/WtUftCore/TraderAdapter.h:192`（声明）与 `src/WtUftCore/TraderAdapter.cpp:1908-1917`（定义）
- 相关测试/已有证据: Q04 测试注释回避该调用；此处与 `docs/迭代计划/迭代01/bugs/n03_uftcore_index.md` BUG-N03-006、`docs/迭代计划/迭代01/bugs/R204.md` BUG-R204-001 为同一声明/定义问题。
- 输入/前置条件: 任何 `WtUftCore` 之外的 TU（含 gtest 用例）调用 `TraderAdapter::isTradeEnabled`
- 期望行为: 要么在头文件里就地给出定义，要么去掉声明上的 `inline`
- 实际行为: 公共头文件将成员声明为 `inline`，定义仅在 `TraderAdapter.cpp`；外部翻译单元没有可见函数体。R204 记录了对象文件缺少该外部符号的 `nm` 证据和 `-Wundefined-inline` 影响。
- 动态证据边界: 本条不新增一次 Q04 编译失败或运行结果；Q04 风控用例注释仅是测试回避证据。
- 影响范围: 外部 TU 直接 odr-use 该接口的编译/链接可用性；“永远进不了覆盖率”范围过宽，定义 TU 内部仍可达。
- 归并建议: 修复和计数归 BUG-N03-006（另有 BUG-R204-001 记录）；去掉头文件 `inline` 或把定义放回头文件。BUG-L00-004 是不同的头文件依赖问题，不作交叉归类。

## BUG-Q04-007

- 状态: 静态缺陷与本机定向测试 FAIL；映射指针失效尚无动态崩溃证据
- 严重级别: Major
- 模块/文件: `src/WtUftCore/UftStraContext.cpp:1128-1214`（`load_local_data` 的持仓块装载/交易日切换）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_uftctx_local_report_test.cpp` → `q04_uftctx_local.trading_day_switch_rebuilds_stale_blocks`
- 输入/前置条件: 先 `on_channel_ready(20240315)` 并推入 4 手成交（本地净持仓 4），再 `on_channel_ready(20240318)`
- 期望行为: 切换交易日时从持久化明细重建内存持仓，每笔明细只累计一次；重映射前后，内存明细引用都指向当前有效映射。
- 实际行为: 源码清理文件块并重载时没有重建/清零 `_positions`，同一持仓被再次累计。本机定向测试观察到 4 手变 8 手。另有静态生命周期风险：`PosInfo::_details` 保存映射内 `DetailStruct*`，而 `_pos_blk._file.reset(...)` 会销毁旧映射；重映射后旧指针是否仍有效取决于映射地址与后续访问，当前证据没有证明必然崩溃。
- 复现步骤: 测试已改为断言切日无新成交时仍为 4；2026-10-29 重建后单跑 FAIL，实际返回 8。
- 影响范围: 本机替身路径已证明交易日切换后本地净持仓重复累计；真实策略回调/柜台时序尚未验证。指针生命周期是需单独验证的风险，不能标为已复现随机崩溃。
- 修复建议: 在加载前按持久化明细重置或整体重建对应 `PosInfo`，同时更新/清空所有 `_details` 指针和 `_valid_idx`，再允许旧 mmap 释放。扩展验证重复切日、切日前后成交、持仓平仓及动态检查工具下的引用访问。

## BUG-Q04-008

- 状态: 本机隔离回归崩溃；空指针路径由源码支持
- 严重级别: Major（仅代码过滤且订单未携带合约信息时）
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:399-427`（`cancelAll`），对照 `:354-374`（`doCancel`）与 `:1271-1292`（`onRspOrders`）
- 相关测试: `DISABLED_q04_trader_entrust.cancel_all_with_lookup_only_contract_info` 单独覆盖可回查但订单未附合约的非空标准码过滤；`q04_trader_entrust.cancel_all_filters_alive_orders_by_full_code` 是订单附合约的对照。
- 输入/前置条件: 回报订单没有附带 `ContractInfo`，但 `_bd_mgr` 能按裸代码和交易所找到合约，因此订单仍被 `onRspOrders/onPushOrder` 接受并进入 `_orders`；随后以非空标准代码调用 `cancelAll(stdCode)`。
- 期望行为: `cancelAll` 像 `doCancel` 一样在合约信息为空时从基础数据回查；回查失败时跳过/报告该订单，不解引用空指针。
- 实际行为: 回报路径将查到的 `cInfo` 仅保存在局部变量，没有写回订单；`cancelAll` 的非空过滤分支直接对 `orderInfo->getContractInfo()` 解引用。相反，`cancelAll("")` 会因 `isAll || ...` 短路绕过该处，不能作为此故障复现。
- 动态证据边界: 原复合用例未注册合约且使用空过滤，不能证明此路径。现将其拆出：新测试使用已登记合约和适配器生成的有效 usertag，检查订单已进入订单表且无内嵌合约，再以 `SHFE.ag2406` 调用。重建后独立运行该禁用回归 exit 139，未取得崩溃栈；附带合约的正控 PASS。崩溃的具体指令位置仍以源码路径支持，不声称已由调试器定位。真实通道与回查失败路径未验证。
- 修复建议: 在 `cancelAll` 过滤中补基础数据回查并检查 NULL，或在回报接收时安全地绑定查得的合约；明确引用所有权。修复后启用该回归，另验证空串全撤、回查失败和 `onPushOrder` 路径。

## BUG-Q04-009

- 状态: 未初始化访问前提无效；`_orders` 析构清理为独立静态泄漏问题，待动态量测
- 严重级别: Minor
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:72-88`（构造/析构）
- 相关测试: 启用用例按 `initExt`、`run`、登录回报的顺序装配；`DISABLED_q04_trader_reply.uninitialized_and_null_deref_class` 第 (1) 段在未 `initExt` 时调用 `onLoginResult`，实际首先触发 `_trader_api == NULL`，不是 `_trading_day/_bd_mgr/_policy_mgr` 缺陷的有效独立复现。
- 输入/前置条件: 正常生命周期要求先由 `init` 或 `initExt` 装配 API/基础数据/策略，再 `run` 并接收登录回报。当前没有动态证据表明此合法顺序会读取未初始化的 `_trading_day/_bd_mgr/_policy_mgr`。
- 实际行为: 构造函数未初始化上述字段；但 `_bd_mgr/_policy_mgr` 由 `init/initExt` 设置，`_trading_day` 由登录成功回报设置，未装配直接调用回调属于无效前提，不能据此立案为已复现的运行时缺陷。
- 保留的独立问题: 若适配器处理过活单，`_orders` 由 `OrderMap::create()` 在堆上创建且 `add` 会 retain 订单；析构只释放 `_stat_map`，没有释放 `_orders`。`WTSMap::release()` 才会清空并释放订单引用。因此适配器对象析构时存在可静态确认的泄漏路径。`OrderMap::create()` 不是池分配；`TraderAdapter::_positions` 是 `PosItem` 值容器，不应要求 `release()`。这项尚无 LeakSanitizer/堆快照等动态量测。
- 修复建议: 先确认并文档化对象生命周期；初始化标量/指针以保持确定状态，并在析构中释放 `_orders`（及需拥有的对象）而非把 `release()` 当析构。按实际管理器销毁/重建流程补泄漏检测。该字段未初始化问题不重复引用 BUG-N03-005（`_stat_map` 未创建时被解引用）或 BUG-P06-001（`WtUftEngine` 未初始化成员），根因与对象不同。
