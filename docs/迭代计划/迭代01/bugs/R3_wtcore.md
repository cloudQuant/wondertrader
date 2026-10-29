# R3 缺陷登记（WtCore 第三波）

lane: R3 · 2026-10-27 · 全部条目 `状态: 待审核`，未改任何生产代码。
证据用例都在 `src/TestUnits/coverage/q01_wtcore3/`（已过真实构建口径 `-fsyntax-only` 门禁，运行由主控统一做）。
本次复核另尝试旧 `build-cov` 二进制的单条频率用例，进程退出码 138，在断言执行前退出；没有完整因果栈，不能归因给该测试或生产代码，也不作为 R3-002 的证据。R3-002 的契约测试结果见对应条目。

## BUG-R3-001 风控频率限流的键空间不一致：缓存按交易所原始代码写、按标准代码查
- 状态: 本地预检与提交顺序回归 FAIL；真实柜台未验证
- 严重级别: Major
- 位置: `src/WtCore/TraderAdapter.cpp` L517（`_order_time_cache[entrust->getCode()]`，而 L498 已把 code 改写成 `cInfo->getAltCode()`）、L1286（`_cancel_time_cache[ordInfo->getCode()]`）↔ L609-661 / L544-596（`checkOrderLimits` / `checkCancelLimits` 的入参）、L1259（`doCancel` 传的是 `stdCode`）
- 现象与证据边界: 源码确认 `doEntrust` 将合约码转成 `getAltCode()` 后写缓存，频率检查则由上层以标准代码调用；`cancel(localid)` 也在标准代码检查后按订单原始代码写缓存。新 `order_freq_window_precheck_uses_standard_code_after_raw_submission` 按预检→提交顺序运行：先让标准码与 altCode 对齐，观察到 4 次后预检被拒；再用标准码与原始码不同的合约提交相同次数，下一次标准码预检仍允许，API 调用数增加。本地假 API 定向 FAIL，独立于阈值算术的具体放行次数；撤单键路径及真实柜台仍需独立验证。
- 影响范围: HFT 与执行器等先按标准代码预检、再调用交易适配器的路径可能因键不一致而绕过窗口限制。范围不包括 `cancel(stdCode, ...)` 的缓存写入：该重载当前未追加撤单时间戳，属于另一处独立待核项。
- 与既有登记的关系: 与 lane p11 的 `P11-003` 是同一写入/查询键路径，属于重复登记；P11 复核已纠正“不可达”的旧判断。R3-001 不应再被描述为对该条的独立复现或另计一项。
- 定向测试建议: 保留现有对齐码与异名码对照，按控制组实测拒绝点比较相同次数后的行为，使 R3-002 的阈值修复不破坏本测试；另加按 localid 撤单的同类键一致性回归。
- 修复建议（待评审，勿在本波实施）: 在 `doEntrust`/`doCancel` 用与上层 checker 一致的标准代码入缓存，或统一所有写入/查询的规范化函数；避免简单双键查询导致重复计数。

## BUG-R3-002 频率窗口阈值算术与配置名不符（`cnt >= boundary` 配 `times > boundary`，且多减 1）
- 状态: 本机契约测试 FAIL；公开最大次数契约不满足
- 严重级别: Minor
- 位置: `src/WtCore/TraderAdapter.cpp` L636-643（下单）与 L571-578（撤单）
- 现象与证据边界: `TraderAdapter.h:138-143` 将 boundary 定义为时间段内的最大下单/撤单次数；实现使用 `times = cnt - sIdx - 1` 再判断 `times > boundary`。以 boundary=2 且查询键与缓存键相同为例，已有 2 或 3 条窗口内记录时仍放行下一次预检，已有 4 条时才拒绝下一次预检，即可能先允许 4 次再拒绝第 5 次，与公开注释的最大 2 次不符。主线程现已用 fake contract 的标准码键和“预检查→提交”顺序分别运行 `R3TraderTest.order_freq_window_denies_submission_after_boundary`、`R3TraderTest.cancel_freq_window_denies_cancel_after_boundary`；两例均 rc=1，boundary=2 时前两次操作后第三次预检实际返回 true，未满足测试要求的拒绝。这是当前本机契约测试 FAIL，直接隔离了 R3-001 的 raw/std 键不一致；它证明 checker 与公开最大次数契约不一致，但不等于联机柜台/生产运行证据。旧 `order_freq_window_blocks_only_for_the_raw_code_key` 的“第 4 笔”叙述仍不作为本结论依据。
- 影响: 与交易所报单频次罚则（例如 1 秒 N 次）对照时容易低估 1 次；同一函数里 `>=` 与 `>` 混用也没有注释说明。
- 修复建议（待评审）: 以头文件公开的“最大次数”语义统一计数边界；不要靠改配置名保留与契约冲突的行为。保留上述两条定向契约测试作为回归：同一标准码键，boundary=2，前两次允许并提交，第三次预检拒绝；分别断言订单/撤单被拒绝且实际动作未发生。此测试不应依赖 R3-001 的别名键行为。

## BUG-R3-003 频率缓存过期裁剪缺少可控时间测试路径
- 状态: 复核为测试可达性观察；未确认产品缺陷
- 严重级别: 不适用（当前“长跑无限增长”结论不成立）
- 位置: `src/WtCore/TraderAdapter.cpp` L588-591、L653-656（`cache.erase(cache.begin(), tit)`）
- 复核: 这是缺少可控时钟时的单元测试可达性问题，不是源码证明的无限增长缺陷。裁剪发生在有新缓存事件并且窗口已向前移动时；长期持续事件会触发清理，停流时缓存最多保留最近一次仍在窗口内的记录，不会仅因运行时间持续增长。现有注释描述的是需裁剪旧前缀，不等于生产分支不可达。若需要稳定覆盖 elapsed-time 边界，可引入时钟 seam 或在测试中用可控缓存状态；本轮不建议仅为测试接缝修改生产接口。

## BUG-R3-004 `stra_exit_long` 与 `stra_exit_short` 的可平量口径不对称
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 不适用（条件性防御建议）
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L1238-1243（多头平仓：读 `bOnlyValid=true` 的可平持仓，收盘那根 bar 例外）↔ L1295-1297（空头平仓：`stra_get_position(stdCode)` 全量，不看冻结量）
- 复核: `WTSContractInfo::canShort()` 仅在 `TM_Both` 为真，`isT1()` 仅在 `TM_LongT1` 为真；这两个独占枚举模式不会在同一合约同时成立。T+1 冻结用例证明冻结可发生，但没有证明可做空合约能同时进入该冻结状态。源码注释也把“空头冻结量为 0”列为持仓路径不变量。因此当前证据不支持把空头平仓口径不对称认定为实际错误。若配置迁移或恢复路径允许违反该不变量，应先补出可达状态，再决定统一可平量或增加断言；现有条件单测试不能单独证明缺陷。


- 迭代02复核: NOT_BUG。平多读取可平量、平空读取净空头量的实现表面不对称；但当前公开交易模式实现中 canShort 仅对 TM_Both 返回 true，isT1 仅对 TM_LongT1 返回 true，提出的“可做空且有 T+1 冻结量”前提不可由同一 TradingMode 构造。没有证明其他冻结来源会进入空头平仓口径。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 当前可由公开枚举不变量解释，未证明可达缺陷
## BUG-R3-005 `do_set_position` 反手时不清零 `_dynprofit`（全平才清）
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 不适用（待定向测试）
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L1477-1479（`if (decimal::eq(pInfo._volume, 0)) pInfo._dynprofit = 0;`）与 L1507（按剩余量等比缩放）、L1535-1563（反手新建明细）
- 复核: 对持仓量与明细一致的正常状态，反手会先关闭旧明细；每条全平旧明细按剩余量缩放 `_dynprofit` 至零，随后才建立反向明细。现有 `set_position_through_zero_reverses_side_with_single_detail` 初始 `_dynprofit` 为零且不检查利润字段，既不能证明旧值残留，也不能证明清零正确。当前应归为未证实，而不是已确认缺陷。定向用例应先通过正常盈亏更新建立非零 `_dynprofit`，在不喂下一 tick 的情况下执行穿零反手，然后立即检查 position profit/持久化字段与新明细一致；只有该断言失败才修复重算逻辑。


- 迭代02复核: NOT_BUG。反手先按明细逐笔平仓，动态盈亏按剩余明细比例缩放；当一侧明细全部平完时分子剩余量为零，_dynprofit 已归零后才建立反向明细。现有反手测试只覆盖零动态盈亏，但源码控制流不支持“旧浮盈直接留给新仓”的因果主张。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 证据不足；原缺陷的因果判断未获源码控制流支持
## BUG-R3-006 `on_tick` 里 session 信息无判空（崩溃族，同 p01 已登记的 DISABLED 条目）
- 状态: 已合并（主记录 BUG-P01-001；同一 on_tick session 空指针路径）
- 严重级别: 待评估（不据此标记 Blocker）
- 位置: `src/WtCore/CtaStraBaseCtx.cpp` L804-805（`WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true); if (sInfo->isInTradingTime(...))`）
- 触发条件与证据边界: `_sig_map` 有待处理信号且 `get_session_info(stdCode, true)` 返回 NULL 时，源码随后直接调用 `sInfo->isInTradingTime`，构成条件性空指针风险。当前没有本条运行结果；p01 的 `DISABLED_P01CtaCrashClass.unchecked_null_pointers_segfault` 是泛化的相同崩溃类登记，不能作为此调用点的动态证明，也不必重复登记相同测试。修复方向是在解引用前判空并给出明确失败/保留信号策略，再用缺 session 的定向用例验证。
- 相邻支路复核: 已配置 session 但交易时段列表为空时，时间转换返回 invalid 值，信号检查可能不通过并保留待处理信号。现有持仓测试添加交易时段只是为了走到持仓变更路径，不证明空时段配置属于错误或信号“永远”无法处理。应先确认空表是否为受支持配置；若受支持且信号长期堆积，再补有日志/恢复策略的测试，当前不作为独立已确认缺陷。

## BUG-R3-007 `WtCtaEngine::handle_pos_change` 在引擎未 init 时也会落盘组合层数据
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）
- 严重级别: 不适用（条件性生命周期防御）
- 位置: `src/WtCore/WtCtaEngine.cpp` L429-488（尤其 L452 建仓 + L477-478 `append_signal`/`save_datas`）
- 复核: `r3_cta_position_persist.cpp` 通过 fake engine 直接调用 context/tick 路径，绕过 `WtCtaEngine::on_tick` 对 `_ready` 的正常门禁；这些用例只能证明被测 helper 在构造状态下会提交，不能证明引擎未初始化时生产回调会调用它。源码显示正常 tick 分发在 ready 前被拦截，`save_datas` 对空 `_port_fund` 另有判空。当前没有未就绪生产调用的证据，应归类为测试构造与产品生命周期不同；若公开支持 pre-ready 的直接 `handle_pos_change`，再以完整状态文件契约补测试并考虑 guard。


- 迭代02复核: TEST。已知测试通过上下文/tick helper 驱动持仓，不是在未就绪的 WtCtaEngine 回调链中运行；生产 on_tick 仅在 _ready 为 true 时分发到上下文，_ready 在 session begin 后才置 true。handle_pos_change 本身无 ready guard，但没有证据表明正常生命周期在 ready 前调用；原测试现象不足以支撑产品缺陷。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 测试调用顺序绕过正常就绪门禁；未证明产品调用路径可达
## BUG-R3-008 按规则限量后未完成量仍计入原始请求

- 状态: 产品缺陷（本机测试复现）
- 严重级别: Major
- 位置: `src/WtCore/TraderAdapter.cpp:664-715`，`buy()` 在按 action rule 限量和拆单前以请求量调用 `updateUndone`；`sell()` 有同形路径。
- 输入/前置条件: 申请买入 5 手，规则只允许 3 手，单笔上限 2 手；本地 fake API 成功接收两笔，数量为 2 和 1。
- 期望行为: 未完成量与实际提交的未完成订单数量一致，为 3 手。
- 实际行为: `R3TraderTest.buy_splits_by_rule_cap_and_single_order_max_volume` 重建后单例 FAIL，订单按 2+1 提交，但 `getUndoneQty` 返回 5。此为本机替身路径，真实柜台回报与异步拒单未验证。
- 修复方向: 以实际接受提交的订单数量累计未完成量，或对被规则截去和发送失败部分正确回滚；覆盖买卖、限额为零、拆单部分失败及后续回报并发顺序，避免重复回滚。

## BUG-R3-009 标准化合约代码查询不到已登记订单

- 状态: 产品缺陷（本机测试复现）
- 严重级别: Major
- 位置: `src/WtCore/TraderAdapter.cpp:467-484`；`TraderAdapter.h` 将 `getOrders` 参数定义为标准化合约代码。
- 输入/前置条件: 订单表已有 `ag2512` 活单，按标准码 `SHFE.ag.2512` 查询。
- 期望行为: 返回该活单及其 local ID。
- 实际行为: `R3TraderTest.position_and_order_tables_read_seeded_state` 返回空集合；用原始代码查询返回 1 条，确认订单确已入表。实现直接比较订单原始码与标准码。与 WtUftCore 的 BUG-Q04-001 同形，但属于不同实现。
- 修复方向: 按统一标准码或交易所加原始码比较，并覆盖同名跨交易所合约、空查询和非活跃订单。

## BUG-R3-010 空合约代码全撤分支在判空前解析字符串

- 状态: 产品缺陷（本机测试复现，公开空码契约仍需明确）
- 严重级别: Major
- 位置: `src/WtCore/TraderAdapter.cpp:1293-1300`；调用 `CodeHelper::extractStdCode` 在计算 `isAll = strlen(stdCode) == 0` 之前。
- 输入/前置条件: 存在买向活单，调用 `cancel("", true)`；实现内部有空代码代表全部合约的显式分支，但头文件未单独描述此参数用法。
- 期望行为: 若保留该空代码全撤能力，应正常返回对应订单 ID；否则应明确拒绝并安全返回，而不是崩溃。
- 实际行为: `R3TraderTest.cancel_empty_code_cancels_all_buy_orders_without_crashing` 用子进程期待正常退出，实际 signal 10/11，父测试 FAIL；去掉空代码调用后包装器测试 PASS。空字符串解析的底层健壮性风险也见 BUG-L01-013，但此处为实际调用顺序及全撤分支的独立证据。
- 修复方向: 先识别空代码并走全部合约分支，仅对非空值执行标准码解析；另对非法标准码安全拒绝。覆盖空码全撤、合法单合约与无订单路径。

## 本轮被接缝/平台挡住、需要主控给接口或进排除清单的面
（详见 `manifests/R3_wtcore.md` 的同名章节，这里只列条目）
- `CtaStraBaseCtx.cpp` L1577-1675 历史切片族：需 `WtEngine::_data_mgr`（`WtDtMgr`），只在 `WtEngine::init()` 里赋值，而 `init` 会拉起线程与共享内存 → 本波禁止。
- `WtEngine.cpp` L285-361 / L1086-1130 / L1143-1186：`init` 装配 + `task_loop`/`_pool` 线程 + `init_riskmon` 的 `DLLHelper` 插件加载（BUG-L00-002：hidden SHARED 无 C++ 链接缝）。
- `WtCtaEngine.cpp` L61-114（`run` 起 `WtCtaRtTicker`）、L116-235（`init`/session 广播）、L490-560（`on_tick` 订阅分发，需 `_ctx_map`+`_exec_mgr` 真装配）。
- `TraderAdapter.cpp` L99-208 `init` 的厂商柜台装配（CTP/XTP/Femas 真实 dll）→ 覆盖排除清单候选。
- `HftStraBaseCtx.cpp` 全量 987 行（missed 356）：**本波预算内未开工**，队列/快速通道分支需先接 `q01_hft_ctx_test.cpp` 的替身；已排下一波第一位。
