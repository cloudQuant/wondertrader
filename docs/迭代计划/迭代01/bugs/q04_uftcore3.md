# q04_uftcore3 待审核缺陷（lane q04 / WtUftCore 交易回报与风控推进）

> 本文件只记录，不修改 `src/**`。状态一律「待审核」。
> 与 `docs/迭代计划/迭代01/bugs/n03_uftcore_index.md`（BUG-N03-001…008）、
> `docs/迭代计划/迭代01/bugs/p06_uftcore2.md`（BUG-P06-001…006）同一缺陷类的条目只做引用，不重复立案。

## BUG-Q04-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:284-302`（`getOrders`）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_reply_machine_test.cpp` → `q04_trader_reply.onRspOrders_counts_stats_and_registers_alive_orders`
- 输入/前置条件: 通道登记了标准代码 `SHFE.ag2406` 的活单，调用 `getOrders("SHFE.ag2406")`
- 期望行为: 与同文件 `cancelAll`（`TraderAdapter.cpp:414` 用 `cInfo->getFullCode()` 比较）一致，按标准代码过滤出该合约的委托
- 实际行为: `strcmp(ordInfo->getCode(), stdCode)` 里 `WTSOrderInfo::getCode()` 是柜台裸代码（`ag2406`），与带市场前缀的标准代码恒不相等，传任何标准代码都返回空表；只有 `getOrders("")` 能取到数据
- 复现步骤: 该用例里 `getOrders(kStdAg)` 断言 `size()==0`，`getOrders("")` 断言 `size()==1`
- 影响范围: 所有按合约查委托的调用方（策略/风控/清理逻辑），表现为「查不到自己的挂单」
- 备注: 期望值取自实现（非约定），若主控裁定应按标准代码匹配，则该断言需随修复翻转；本条状态待裁定

## BUG-Q04-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:311-343`（`doEntrust`）、`:994-1108`（四个下单入口）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_entrust_paths_test.cpp` → `q04_trader_entrust.insert_failure_returns_uint_max_but_keeps_undone`
- 输入/前置条件: 替身 `ITraderApi::orderInsert` 返回负值（柜台拒收），调用 `openLong("SHFE.ag2406", 5000, 2, 0)`
- 期望行为: 委托失败时回滚本笔 `updateUndone(+qty)`，未完成量回到下单前的值
- 实际行为: 四个入口都在 `doEntrust` 之前 `updateUndone(stdCode, qty)`，`doEntrust` 在 `ret<0` 分支只打日志并返回 `UINT_MAX`，不回滚；未完成量长期虚高，且 `_orders` 里没有可撤的对应单据
- 复现步骤: 用例断言 `getUndoneQty()==2.0`（实现值）
- 影响范围: 实盘拒单/断线后的未完成量统计，进而影响依赖 `getUndoneQty` 的策略判断
- 备注: 本条按「实现如此、语义待裁定」记录，未为了让断言变绿而调整期望

## BUG-Q04-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:1970-1984`（`getRiskParams`）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp` → `q04_trader_risk.product_level_rule_is_looked_up_with_a_wrong_key`
- 输入/前置条件: `riskmon.policy` 里同时配了 `default` 与品种组 `ag`，检查代码 `SHFE.ag2406`
- 期望行为: 取品种 id 段 `ag2406` 的字母前缀 `ag`，即 `std::string(stdCode + idx + 1, eIdx - idx - 1)`
- 实际行为: 长度写成 `eIdx - idx + 1`，多带 2 个字符 → 键为 `ag24`，永远命中不到品种组，静默回落到 `default`；品种级流量限额（撤单/下单总量与频率）全部失效
- 复现步骤: 用例里 `ag` 组 `cancel_total_limits=1`、`default` 放宽到 1000000，推 1 张撤单后 `checkCancelLimits` 仍返回 true
- 影响范围: 所有按品种配置的通道风控规则；交易所限流场景下会被罚单或熔断
- 备注: 修复方向是改正切片长度并补 `idx == npos` 的校验（该越界类已在 BUG-N03 立案）

## BUG-Q04-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:337-341`、`:391-392`、`:419-420`、`:1576-1578`（被注释的写入）、`:1875-1903`、`:1937-1965`（只读的时间窗）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp`（两个 `check*` 用例只能命中总量支）
- 输入/前置条件: 任意连续下单/撤单
- 期望行为: 下单/撤单成功后把时间戳写入 `_order_time_cache` / `_cancel_time_cache`，`check*Limits` 的时间窗分支据此判定频率超限
- 实际行为: 所有 `emplace_back(now)` 写入语句都被注释掉，两个缓存恒为空 → `checkOrderLimits/checkCancelLimits` 里各约 20 行时间窗逻辑（含 `lower_bound`、`erase` 清理）是不可达死代码，频率类风控整体失效
- 复现步骤: 反复调用 `checkCancelLimits` 并推大量撤单，函数仍只在总量超限时才返回 false
- 影响范围: 高频/瞬时流量场景（这正是该缓存注释里声明的设计目的）；同时无谓地压低了模块覆盖率
- 备注: 建议主控决定「恢复写入」或「删除死分支」，二者都能消除不可测代码

## BUG-Q04-005

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:1978-1983`（`getRiskParams` 回落）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp` → `DISABLED_q04_trader_risk.no_default_group_and_null_statmap_class` 第 (3) 段
- 输入/前置条件: `riskmon.active=false`（或 `policy` 里没有任何组）时调用 `checkCancelLimits` / `checkOrderLimits`
- 期望行为: 查不到组时返回 `NULL`，由调用方按「不限额」处理（调用方已写好 `if (riskPara == NULL) return true;`）
- 实际行为: `it = _risk_params_map.find("default"); return &it->second;` 未判 `it == end()`，对 `end()` 取 `second` 地址是未定义行为
- 复现步骤: 取消 `DISABLED_` 前缀运行第 (3) 段
- 影响范围: 未配置风控或风控未激活的通道；与 BUG-N03-002（`ActionPolicyMgr::getActionRules` 同样对 `end()` 解引用）同一缺陷类，不重复立案只作引用
- 备注: 启用用例已改为始终在 `policy` 里保留 `default` 组，不阻塞覆盖率目标

## BUG-Q04-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.h:192`（声明）与 `src/WtUftCore/TraderAdapter.cpp:1908-1917`（定义）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_risk_limits_test.cpp` 文件头注释（本波因此未能对 `isTradeEnabled` 下断言）
- 输入/前置条件: 任何 `WtUftCore` 之外的 TU（含 gtest 用例）调用 `TraderAdapter::isTradeEnabled`
- 期望行为: 要么在头文件里就地给出定义，要么去掉声明上的 `inline`
- 实际行为: 声明为 `inline` 却在 `.cpp` 里定义，外部 TU 一调用即报 `-Wundefined-inline`，在 `-Werror` 下直接编译失败；接口对外事实不可用，该 6 行也永远进不了覆盖率统计
- 复现步骤: 在任一 q04 用例里写 `adapter.isTradeEnabled("SHFE.ag2406")` 并按简报门禁命令编译
- 影响范围: 同类问题还潜伏在 `printPosition/getContract/updateUndone`（头里 `inline`、`.cpp` 里定义），它们目前是私有成员才没被触发
- 备注: 与 BUG-L00-004（生产头不自带 boost 依赖）同属头文件自洽性缺陷家族

## BUG-Q04-007

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/UftStraContext.cpp:1128-1214`（`load_local_data` 的持仓块装载/交易日切换）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_uftctx_local_report_test.cpp` → `q04_uftctx_local.trading_day_switch_rebuilds_stale_blocks`
- 输入/前置条件: 先 `on_channel_ready(20240315)` 并推入 4 手成交（本地净持仓 4），再 `on_channel_ready(20240318)`
- 期望行为: 交易日切换时先清空 `_positions`（含 `_volume/_opencost/_details/_valid_idx`）再从明细块重载，净持仓仍是 4
- 实际行为: 只清块内数据（`memset` + `_size` 回收），`_positions` 里同一条目的 `_volume` 被二次累加成 8；且重载前 `_pos_blk._file.reset(new BoostMappingFile)` 已经 unmap 旧块，`PosInfo._details` 里残留的旧指针变成悬垂指针，后续平仓遍历会读到已释放映射
- 复现步骤: 用例断言切换后 `stra_get_local_position("SHFE.ag2406") == 8.0`（实现值，非约定值）
- 影响范围: 隔夜跨交易日的策略；持仓翻倍会让风控/策略判断失真，悬垂指针是随机崩溃源
- 备注: 状态待裁定（若裁定应为 4.0，则该断言需随修复翻转）

## BUG-Q04-008

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:399-427`（`cancelAll`），对照 `:354-374`（`doCancel`）与 `:1271-1292`（`onRspOrders`）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_uftctx_local_report_test.cpp` → `q04_uftctx_local.order_and_cancel_forward_through_the_channel`（必须显式 `setContractInfo` 才能跑通）；`DISABLED_q04_trader_entrust.null_contract_and_bare_code_class` 第 (2) 段
- 输入/前置条件: 通过 `onRspOrders`/`onPushOrder` 登记委托（这两条路径只把 `cInfo` 用作局部变量，从不回写 `orderInfo->setContractInfo`），随后调用 `cancelAll(stdCode)`
- 期望行为: 和 `doCancel` 一样，`getContractInfo()` 为空时回落 `_bd_mgr->getContract(code, exchg)`
- 实际行为: `cancelAll` 直接 `cInfo = orderInfo->getContractInfo(); ... strcmp(stdCode, cInfo->getFullCode())`，`cInfo` 为 NULL 即解引用崩溃
- 复现步骤: 不给回报单设置 ContractInfo，直接 `cancelAll("")`
- 影响范围: 实盘一键撤单（`stra_cancel_all`）在绝大多数回报形态下必崩；`bForceClose` 依赖 `cancelAll` 的平仓链路同样受影响

## BUG-Q04-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:72-88`（构造/析构）
- 相关测试: `src/TestUnits/coverage/q04_uftcore3/q04_trader_reply_machine_test.cpp`（启用用例全部先 `initExt` + `onLoginResult` 再读交易日）；`DISABLED_q04_trader_reply.uninitialized_and_null_deref_class` 第 (1) 段
- 输入/前置条件: `TraderAdapter a; a.onLoginResult(true, "ok", 20240315);` 之后调用 `enumPosition` / `onRspPosition`
- 期望行为: 构造函数补齐 `_trading_day(0)`、`_bd_mgr(NULL)`、`_policy_mgr(NULL)`；析构释放 `_orders`（`OrderMap::create()` 的池对象）与 `_positions` 持有的引用
- 实际行为: 初始化列表缺 `_trading_day/_bd_mgr/_policy_mgr`，未 init 即读是野值/野指针；析构只 `release` 了 `_stat_map`，`_orders` 与其中登记的委托对象在通道重建（`release()` 不 delete 适配器）时持续泄漏
- 复现步骤: 取消 `DISABLED_` 前缀运行第 (1) 段（`onLoginResult` 里 `_trader_api->queryPositions()` 解引用 NULL）
- 影响范围: 与 BUG-N03-005 / BUG-P06-001 同一缺陷类（构造未初始化成员），本条只补记 `_orders` 未释放与泄漏面
