# L06 WtBtCore 缺陷记录（迭代 01）

状态一律「待审核」；按 `bugs/README.md` 模板，测试与修复提交分离，未修改任何生产代码。

## BUG-L06-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/MatchEngine.cpp `update_lob` L242-246
- 相关测试: l06_match_engine_test.cpp `updateLobHandlesEmptyBookWithoutCrash`（仅回归路径，无法直接断言私有成员）
- 输入/前置条件: 任意含买/卖档位的 tick
- 期望行为: 买档价格对应的挂单量应取 `curTick->bidqty(i)`
- 实际行为: 买档 px 写入的是 `curTick->askqty(i)`（多空量混用），LOB 买侧深度全部被卖量污染
- 复现步骤: 构造 bid1=(10.0,100)/ask1=(10.1,200) 的 tick 调 `handle_tick`，调试可见 `_items[bid_px]=200`
- 影响范围: 排队量估计、被动单成交时点（间接影响回测撮合精度）
- 备注: 建议修复方向：`volume = curTick->bidqty(i);`，不实施。

## BUG-L06-002

- 状态: 待审核
- 严重级别: Blocker（涨跌停单边盘口下崩溃）
- 模块/文件: src/WtBtCore/MatchEngine.cpp `update_lob` L251-260
- 相关测试: 无法在公开 API 上安全复现（需私有成员构造），测试中仅覆盖安全路径
- 输入/前置条件: 单边盘口 tick（如涨停封板：bid1 有量、ask1=0）。此时 `ask_px=0`，`_items` 仅含 bid 价一档
- 期望行为: `lower_bound` 后移得到 `end()` 时应跳过擦除
- 实际行为: `sit = lower_bound(bid_px)` 命中唯一档后 `sit++` 变成 `end()`，随后 `sit->first <= eit->first` 无条件解引用 `end()` 迭代器 → UB
- 复现步骤: `handle_tick` 喂入 bid1=(10.0,1000)、ask 全 0、price=10.0 的 tick（update_lob 的擦除块即触发）
- 影响范围: 涨跌停/单边行情回测进程崩溃（撮合主链路）
- 备注: 不阻塞覆盖率目标（本 lane 测试用例刻意避开该输入）；建议对 `sit == _items.end()` 判空。

## BUG-L06-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/MatchEngine.cpp `cancel(const char*, bool, double, FuncCancelCallback)` L364-392
- 相关测试: l06_match_engine_test.cpp `cancelBySideCollectsActiveOrdersOnly`
- 输入/前置条件: 多合约、多笔同向活跃订单，qty>0 的按量撤单
- 期望行为: 仅撤销 stdCode 指定合约、指定方向、总量≈qty 的订单
- 实际行为: ① stdCode 形参被完全忽略（`/*stdCode*/`），撤销所有合约同方向订单；② `double left = qty;` 在循环体内每轮重置，`left -= ordInfo._left` 的累减跨轮失效，按量撤单只可能撤一轮就 `break` 或全撤，数量语义错误
- 复现步骤: 见测试注释；对 A/B 两合约各挂买单后 `cancel("A", true, 0, cb)` 会连 B 一起撤
- 影响范围: 撤单回报数量与实盘/回测持仓对账不一致
- 备注: 建议修复方向：把 left 提到循环外 + 按 `_code` 过滤，不实施。

## BUG-L06-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: src/WtBtCore/MatchEngine.cpp `match_orders` L141-143、L152、L199-201、L209
- 相关测试: 被动/主动撮合用例（`positiveBuyMatchesAtAskPrice` 等）依赖现行为写断言
- 输入/前置条件: ① 主动单对手档量为 0 但 tick 总量>0；② 部分成交剩余量为浮点小数
- 期望行为: 对手量为 0 时不应成交；剩余量判定应使用 epsilon 比较
- 实际行为: `if (decimal::eq(qty, 0.0)) qty = 1;` 凭空成交 1 手；随后 `if (ordInfo._left == 0)` 用 double 与 0 精确比较，`_left -= qty` 的累计误差可能使订单永不删除（幽灵挂单）
- 复现步骤: askqty(0)=0、tick.volume>0 时对主动买单可复现凭空成交 1 手
- 影响范围: 成交量约束与涨跌停/盘口枯竭场景的回测正确性
- 备注: 修复建议区分 qtyTick 并改用 `decimal::eq(_left, 0.0)`，不实施。

## BUG-L06-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtBtCore/MatchEngine.cpp `fire_orders` L51-64
- 相关测试: 单合约用例掩盖了该问题（断言均按同码编写）
- 输入/前置条件: 合约 A 存在 state==0 的新订单，此时收到合约 B 的 tick
- 期望行为: 仅激活 `_code == stdCode` 的订单，且回报使用订单自身代码
- 实际行为: 遍历全部订单并用入参 `stdCode` 作为回报代码，A 的委托回报挂在 B 的代码下发出
- 复现步骤: buy(A) 后 handle_tick(B, tickB)
- 影响范围: 多合约回测的委托回报代码错误
- 备注: 不阻塞覆盖率。

## BUG-L06-006

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtBtCore/EventNotifier.cpp `init` L70-73
- 相关测试: l06_wthelper_event_test.cpp `inactiveConfigSkipsModuleLoading`
- 输入/前置条件: `init(NULL)`
- 期望行为: 与 MatchEngine::init 一致，cfg 为空直接 return false
- 实际行为: 无判空，`cfg->getBoolean("active")` 空指针解引用崩溃
- 复现步骤: `EventNotifier n; n.init(nullptr);`（测试中以注释标记，未执行）
- 影响范围: 配置缺失时进程启动即崩
- 备注: 建议入口加 `if (cfg == NULL) return false;`，不实施。

## BUG-L06-007

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: src/WtBtCore/HisDataReplayer.cpp `calc_fee` L3007
- 相关测试: l06_replayer_runtime_test.cpp `calcFeeForUnconfiguredCode`（仅覆盖安全路径）
- 输入/前置条件: fees 模板中存在某 stdPID，但基础数据（comms）中没有对应品种
- 期望行为: commInfo 缺失时返回 0 或告警
- 实际行为: `commInfo->getVolScale()` 未判空直接解引用，NULL 时崩溃；另外按额计费且 `price/qty` 任一为 0 时 `rate=0` 走 minfee 分支被正确跳过，但负费率 `(int32_t)(ret*100+0.5)` 对负数向零截断，与正数四舍五入不对称
- 复现步骤: 构造仅含未知品种键的 fees.json 并 calc_fee
- 影响范围: 费率表与基础数据不一致时回测启动崩溃
- 备注: 不实施修复，等待评审。
