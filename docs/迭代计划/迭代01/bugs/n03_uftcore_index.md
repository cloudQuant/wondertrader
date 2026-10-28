# N03 — WtUftCore / WtIndexFactory / WtLatency* 待审核缺陷

> 工作流: lane N03（`src/WtUftCore`、`src/WtIndexFactory`、`src/WtLatencyHFT`、`src/WtLatencyUFT`）
> 本轮只记录，不修改任何生产代码。状态一律「待审核」。

## BUG-N03-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtIndexFactory/WtBlkIdxWorker.cpp:178`（`WtBlkIdxWorker::init`）
- 相关测试: `src/TestUnits/coverage/n03_uftcore_index/n03_idxfactory_worker_test.cpp` → `TEST(DISABLED_n03_blkidx, weight_alg_out_of_range_class)`
- 输入/前置条件: 板块指数配置里 `weight_alg` 取值 ≥ 3（合法值只有 0/1/2，见同文件 L12-17 的 `WEIGHT_ALGS[]`，只有 3 个元素）。代码直接 `WEIGHT_ALGS[_weight_alg]`，`_weight_alg` 来自 `config->getUInt32("weight_alg")`，无任何区间校验。
- 期望行为: 未识别的权重算法应回退为默认值或让 `init` 返回 false 并给出错误日志。
- 实际行为: 越界读取 `const char*` 数组，得到悬垂指针后再交给 fmt 打印，属未定义行为（实测大概率段错误）。
- 复现步骤: 用 `{exchg:SSE, code:BLK001, trigger:time, timeout:0, weight_alg:3, codes:[...]}` 调用 `WtBlkIdxWorker::init`。
- 影响范围: 所有板块指数（blk idx）配置的启动路径；配置写错一个数字即可让 index factory 所在进程崩溃。整类输入（3/4/10/99/0xFFFFFFFF）已收进同一个 DISABLED_ 用例。
- 备注: 建议修复方向（不实施）——`_weight_alg = std::min(_weight_alg, 2u)` 或用 `weight_alg < ARRAY_SIZE(WEIGHT_ALGS)` 做校验后再取名字。不阻塞其余覆盖率目标。

## BUG-N03-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtIndexFactory/WtBlkIdxWorker.cpp:300`（`WtBlkIdxWorker::generate_tick`）
- 相关测试: `n03_idxfactory_worker_test.cpp`（`fixed_weight_index_and_high_low`、`dynamic_interest_and_dynamic_volume_algs` 间接验证分母来源）
- 输入/前置条件: 权重算法 1（动态总持）或 2（动态成交量）时 `total_base` 从 0 累加；若所有成分的 `open_interest`/`total_volume` 均为 0，或 `_weight_scales` 为空（`total_weight==0`），则 `total_value / total_base / total_weight` 是 0/0。
- 期望行为: 数据不足时不产出指数（前面的 `action_date==0` 只校验了“有没有快照”，没校验分母）。
- 实际行为: 得到 `nan` 并被写入 `_cache.price/open/high/low`，随后广播给下游，高低价比较 `std::max/std::min(nan)` 结果不确定，脏值会一路传到行情链路。
- 复现步骤: 订阅两个成分合约，快照 `action_date` 非 0 但 `open_interest`/`total_volume` 全为 0，`weight_alg=1`，推一笔行情。
- 影响范围: 指数数值的正确性（资金/信号相关）。
- 备注: 建议修复方向——对 `total_base`、`total_weight` 做 `decimal::eq(x, 0.0)` 判空后直接 return。

## BUG-N03-003

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtUftCore/ActionPolicyMgr.cpp:152-162`（`ActionPolicyMgr::getActionRules`）
- 相关测试: `src/TestUnits/coverage/n03_uftcore_index/n03_action_policy_test.cpp` → `TEST(DISABLED_n03_action_policy, no_default_group_deref_end_class)`
- 输入/前置条件: 动作策略配置中不存在名为 `default` 的规则组（只有自定义组），随后用任意 pid 查询；三条路径都命中：已映射的组名被删、未映射品种回退、空串 pid。
- 期望行为: 找不到 `default` 组时应返回空规则组并报错，而不是继续解引用。
- 实际行为: `it = _rules.find("default")` 失败后仍是 `_rules.end()`，紧接 `assert(it != _rules.end()); return it->second;`。Release 构建（NDEBUG）里 assert 被裁掉，直接对 `end()` 迭代器解引用 → 野指针/崩溃。
- 复现步骤: 配置 `{"onlygroup":{"order":[{"action":"open","limit":1}],"filters":["SHFE.rb"]}}`，`init()` 成功后 `getActionRules("SSE.600000")`。
- 影响范围: UFT/HFT 交易通道启动即可能崩溃；所有下单动作限额判断。
- 备注: 建议修复方向——静态空组兜底（`static const ActionRuleGroup s_empty;`）或强制要求 `default` 组存在并在 `init` 里校验。

## BUG-N03-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:1970-1984`（`TraderAdapter::getRiskParams`）
- 相关测试: `n03_uftcore_helper_adapter_test.cpp` → `TEST(DISABLED_n03_trader_adapter, uninitialized_deref_class)`（经 `checkOrderLimits`/`checkCancelLimits` 到达）
- 输入/前置条件: 两处问题。
  1. 产品码切片长度写成 `std::string(stdCode + idx + 1, eIdx - idx + 1)`，比 `[idx+1, eIdx)` 多取 1 个字符：`"CFFEX.IF2202"` 取到 `"IF2"` 而不是 `"IF"`，`"SSE.600000"` 取到 `"60"` 而不是 `"6"`。
  2. `_risk_params_map` 中没有对应产品且没有 `"default"` 时，`return &it->second;` 对 `end()` 迭代器取地址。
- 期望行为: 取到准确的产品码；查不到策略时应返回 NULL（两个调用方都会走 `riskPara == NULL` 的“不限流”分支）。
- 实际行为: 键永远对不上配置（策略配置形同不生效），最坏情况解引用 `end()`。
- 复现步骤: 未 `init()`（`_risk_params_map` 为空）时调用 `checkOrderLimits("SSE.600000")`。
- 影响范围: 交易流量风控（限流/排除队列）整体失效，属于合规风险点。
- 备注: 建议修复方向——长度改为 `eIdx - idx - 1`（即 `eIdx - (idx+1)`），并在 map 无 `default` 时 `return nullptr`。

## BUG-N03-005

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:429-436`（`getInfos`）、`1857`（`checkCancelLimits`）、`1919`（`checkOrderLimits`）
- 相关测试: 同上 DISABLED_ 用例
- 输入/前置条件: 只调用 `TraderAdapter::initExt()`（或完全未初始化）而未走 `run()` 时，`_stat_map` 只在 `run()` 里创建（`TraderAdapter.cpp:208-221`），这三个公开接口都无条件 `_stat_map->get(...)`。
- 期望行为: `_stat_map == NULL` 时返回 0 / 放行。
- 实际行为: 空指针解引用。
- 复现步骤: `TraderAdapter adapter; adapter.getInfos("SSE.600000");`
- 影响范围: 单测/回放/任何在未连接状态下查询统计信息的调用；也使这三条分支在无真实柜台时不可达，压低可达覆盖率。
- 备注: 建议修复方向——把 `_stat_map` 的创建移到构造函数或加判空。

## BUG-N03-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtUftCore/TraderAdapter.h:192` 声明 `inline bool isTradeEnabled(...) const;`，定义在 `src/WtUftCore/TraderAdapter.cpp:1908`
- 相关测试: `n03_uftcore_helper_adapter_test.cpp` 内注释处（本轮为过门禁已改为不调用）
- 输入/前置条件: 任何其它翻译单元（含 gtest 用例）调用 `TraderAdapter::isTradeEnabled`。
- 期望行为: `inline` 声明必须在所有使用它的 TU 中可见定义。
- 实际行为: clang 报 `-Wundefined-inline`，在 `-Wall -Wextra -Werror` 下是硬错误；生产构建里也依赖链接器跨 TU 内联，语义上属于 ODR 风险。
- 复现步骤: 编译任意包含 `adapter.isTradeEnabled("x")` 的测试文件。
- 影响范围: 本函数的可测试性（当前无法覆盖）；同样的“头文件 inline 声明 + cpp 定义”写法在 `TraderAdapter.h:108/121/123/125` 也存在（其中 `id()`、`getUndoneQty`、`updateUndone` 在头里有定义，`printPosition/getContract` 无 inline 问题）。
- 备注: 建议修复方向——去掉 `inline` 或把定义搬回头文件。

## BUG-N03-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtUftCore/TraderAdapter.cpp:345-352`（`getContract`）、`311-319`（`doEntrust`）
- 相关测试: `n03_uftcore_helper_adapter_test.cpp` → `TEST(DISABLED_n03_trader_adapter, uninitialized_deref_class)` 内的 `openLong("600000", ...)` / `openShort(...)`
- 输入/前置条件: 传入不含 `.` 的合约代码（例如直接给裸代码 `"600000"`）。`StrUtil::findFirst`（`src/Share/StrUtil.hpp:64-78`）在找不到字符时返回 `std::string::npos`。
- 期望行为: 代码里没有分隔符时应直接判定为非法委托并返回 0/空。
- 实际行为: `getContract` 里 `buf[idx] = '\0'` 变成 `buf[npos]`，即 `*(buf - 1)`，向栈上缓冲区下方写 1 字节（静默栈破坏），随后 `getContract(整串, "")` 返回 NULL，调用方（`buy`/`sell`）不判空直接 `cInfo->getCommInfo()` → 段错误。`doEntrust` 的 `entrust->setExchange(stdCode, pos)` 同样以 npos 作为长度。
- 复现步骤: `TraderAdapter adapter; adapter.openLong("600000", 10.0, 1, 0);`
- 影响范围: 任何上层把裸代码（不带交易所前缀）传进 UFT 下单接口的路径；越界写属于可利用的内存破坏。
- 备注: 建议修复方向——`if(pos == std::string::npos) return NULL;`。

## BUG-N03-008

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtUftCore/ParserAdapter.cpp:39-58`（`init`）、`228-240`（`release`）
- 相关测试: `n03_uftcore_helper_adapter_test.cpp`（`init_ext_subscribes_and_pushes_quote` 里为绕开该问题把计数改成全局变量）
- 输入/前置条件: ①`init()` 在 `cfg->getString("module")` 为空时 `return false`，但此前已经 `_cfg->retain()`；②`release()` 在 `_remover == NULL` 时执行 `delete _parser_api`。
- 期望行为: ①失败路径要么不持有引用，要么释放掉；②由 `initExt()` 注入的 api 属外部所有权，适配器不应删除它。
- 实际行为: ①失败即泄漏一次引用计数（配置对象长期驻留）；②`initExt` 注入的 api 被适配器 `delete`，若调用方也持有并释放即为 double free，测试里表现为 use-after-free。
- 复现步骤: `ParserAdapter a; a.initExt("x", new SomeApi, &stub, &bdMgr); a.release();` 然后访问该 api 指针。
- 影响范围: 解析器适配器的生命周期管理；影响后续用例的可写性。
- 备注: 建议修复方向——`initExt` 与 `init` 用不同的所有权标志；失败路径补 `cfg->release()`。
