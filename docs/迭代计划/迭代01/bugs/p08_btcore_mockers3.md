# P08 WtBtCore 模拟交易第三轮疑似缺陷（迭代 01 第四波）

本轮只读复核生产代码、接口契约及既有测试前提；未修改生产代码或测试。状态按当前证据更新，重复登记保留原编号并交叉引用。

## BUG-P08-001 UftMocker 会话回调无条件解引用 `_strategy`

- 状态: 已合并（BUG-R121-001）
- 审核裁定：产品缺陷；与 `bugs/R121.md` 的 BUG-R121-001 完全重复，不需另开一条根因记录。
- 位置：`src/WtBtCore/UftMocker.cpp:343-366`；公开入口 `:205-213`。
- 证据：`on_session_begin()` 在冻结结转后无条件调用 `_strategy->on_session_begin()`；`on_session_end()` 首句无条件调用 `_strategy->on_session_end()`。同类事件回调多数先判 `_strategy`。`init_uft_factory()` 可成功加载工厂但不提供 `strategy` 节点，构造时 `_strategy` 为空；直接通过公开入口可触发空指针虚调用。
- 测试边界：p08 的禁用家族先调用会话入口，确会先触发此问题，因此它不能同时证明后面的商品空指针子项。
- 修复方向：统一守卫策略回调；没有策略时仍按接口语义完成持仓结转和会话日志。

## BUG-P08-002 on_session_begin 只结转多头持仓

- 状态: 已撤回（迭代02：TEST，原产品主张不成立）

- 审核裁定：条件性产品问题；与 `bugs/R201.md` 的 BUG-R201-001 同一实现路径。原报告“该强转必然 UB”的说法未证明，直接注入不合法空头成交的测试也不能单独证明支持空头 T+1 的业务契约。
- 位置：`src/WtBtCore/UftMocker.cpp:343-359`；`PosInfo` 成员布局见 `src/WtBtCore/UftMocker.h:334-341`。
- 证据：`it.second` 是 `PosInfo`，代码将其按 `PosItem&` 使用，随后仅对首个 `_long` 成员执行 frozen 检查和 `_newvol/_newavail` 结转，未触碰 `_short`。`update_position()` 对多空两侧使用同一 `commInfo->isT1()` 冻结规则（`:1155-1190`）；`stra_enter_short()` 没有 canShort 拒绝分支（`:638-676`）。不过 `isT1()` 仅对应不允许开空的 `TM_LongT1`；通过真实开仓和成交路径产生该状态尚未动态验证。若该无效空头入口被接受并成交，后续短仓会保持冻结。
- 证据边界：是否要将 `PosInfo*` 到首成员 `PosItem*` 的别名称为 UB 取决于类型的标准布局保证；目前没有 trait 或编译器证据证明这一点，不将其作为缺陷论据。逻辑漏结转本身不依赖该 UB 说法。
- 修复方向：首先对开空入口遵守 `canShort()` 约束建立回归，拒绝不支持开空的商品；明确历史非法持仓的恢复策略，再决定是否对两腿统一结转。测试覆盖合法多头 T+1 跨日及不允许开空的拒单行为。


- 迭代02复核: TEST。复现把品种设为 TM_LongT1（canShort=false）后直接 on_trade 注入 short，绕过交易模式/下单约束；不能证明有合法 short T+1 结转缺陷。TM_Both short 是否有 frozen 跨日语义也未证明。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
## BUG-P08-003 平仓明细清理区间漏掉最后一个零量明细

- 审核裁定：产品缺陷；原测试曾将缺陷行为写成期望，现已纠正。
- 位置：`src/WtBtCore/UftMocker.cpp:1211-1252`（offset 1）；`:1259-1310`（offset 2）。
- 证据：offset 1 将最后一个被平至零的 iterator 存入 `eit`，随后 `erase(begin, eit)` 排除 `eit`；offset 2 同样将最后处理位置存入 `eit`，随后 `erase(sit, eit)` 不包含它。单条明细全平后都留下 `_volume == 0` 的明细；多次开平会累积容器条目并增加后续遍历工作量。
- 测试证据：`CloseTodayBranchScalesDynProfit` 已改为断言全平后明细为空；重建后单独运行 FAIL，实际仍保留零量明细。
- 修复方向：平仓推进后清理所有零量明细（C++17 可用 erase-remove），保留部分平仓后仍非零的明细；覆盖 offset 1/2、多明细、全平与部分平仓。

## BUG-P08-004 UftMocker::stra_cancel_all 返回空容器

- 审核裁定：产品缺陷；测试断言“返回空”错误。
- 位置：`src/WtBtCore/UftMocker.cpp:456-470`；接口 `src/Includes/IUftStraCtx.h:93-103`；调用方 `src/WtUftStraFact/WtUftStraDemo.cpp:392-400`。
- 证据：方法遍历匹配合约的活单并调用 `stra_cancel()`，但从未向 `ret` 加入订单 ID。`WtUftStraDemo` 使用返回的 `OrderIDs` 更新自己的受管订单集合和撤单计数；恒空会导致订单已被撤、策略仍无法识别/跟踪这些取消单。
- 测试证据：`CancelAllScansMatchingCodeOnly` 已改为断言只返回目标合约的订单 ID，重建后单独运行 FAIL。`R62UftMockerGap.TickProcessesOrdersAndDrainsCancelTasks` 也已改正相同错误预期，独立运行期望 1 个 ID、实际返回 0 个，FAIL。
- 修复方向：遍历并命中合约时先将 localid 加入返回列表，再排入取消任务；保留当前异步任务执行语义。测试应断言只返回目标合约匹配的 ID。

## BUG-P08-005 平仓方向被当成持仓方向用于净挂单与撮合

- 审核裁定：产品缺陷；附带测试对 `stra_get_undone()` 的期望错误。
- 位置：`src/WtBtCore/UftMocker.cpp:392-405,679-798,889-960`；接口契约 `src/Includes/IUftStraCtx.h:243-247`。
- 证据：`OrderInfo::_isLong` 在 `on_trade()`/`update_position()` 中表示要更新哪一侧持仓（`:807-813,1155-1158`）。开多是买，开空是卖；平多是卖，平空是买。当前 `stra_get_undone()` 只按 `_isLong` 决定符号，因此平多加正、平空加负，违背接口明确的“买入为正、卖出为负”。当前 `procOrder()` 也仅按 `_isLong` 选 ask/bid 盘口（`:918,924`），导致平多取 ask、平空取 bid，方向相反。
- 测试前提：`p08_uft_dispatch_t1_test.cpp:224-225` 将平多 20 手断言成 undone `+20`，与接口注释相反。独立运行该测试时，在该断言之前还出现 `frozen() == -100`（测试行 212）：fixture 直接调用 `on_trade()` 平仓，绕过 `stra_exit_long()` 下单时对 `_preavail` 的预占，因而这项冻结断言不是完整生产下单/成交路径的证据；GoogleTest 的非致命 `EXPECT` 仍会继续执行后续 undone 断言。
- 修复方向：保留 `_isLong` 作为持仓腿标记；按 offset 与持仓腿推导交易方向。开仓时买入方向等于 `_isLong`，平仓时取反；`undone` 及 `procOrder()` 的盘口、限价判断统一使用该交易方向。补平多/平空对称用例，并从正常下单路径驱动。
- 可操作测试设计（只读审查建议，尚未实施）：将 T+1 隔夜平仓用例改为只预置昨仓和一条昨仓明细（例如 `_prevol = _preavail = 100`、明细开仓日早于回放交易日），不要先调用 `on_trade()`。调用 `stra_exit_long(code, 0.0, 20, false, 0)` 后先断言订单受理、`_preavail == 80`、总量仍为 100、`frozen() == 20`，并在订单仍挂着时断言 `stra_get_undone(code) == -20`。要继续验成交，应采用 `r121_uft_mocker_test.cpp:155-197` 的回放夹具模式：给 `HisDataReplayer` 配置同一交易日的 `r111::TickLoader`、`init/register_sink/prepare`，再喂 tick。原因是 `UftMocker::on_tick()` 最终的 `procOrder()` 在 `UftMocker.cpp:913` 通过 `stra_get_last_tick()` 从 replayer 取行情（`:1008-1010`），单独构造 tick 传给 `handle_tick()` 不能建立这个前提。平多成交 tick 应设置 `bidQty >= 20`、`askQty == 0` 且 bid/ask 价格不同，调用 `handle_tick()` 后断言 20 股成交、昨仓/可用量均为 80、`frozen() == 0`、昨仓平仓费及盈亏按明细计算、订单移除且 undone 回到 0；当前以 `_isLong` 直接读 ask 深度的实现会无法成交，该结果正好区分撮合方向修复。正确符号边界应在未成交订单仍存在时分别验证开多 `+Q`、开空 `-Q`、平多 `-Q`、平空 `+Q`；平空符号用允许双向交易的期货品种和已预置空头昨仓覆盖，不要拿 `TM_LongT1` 股票作为该边界。填满后订单已移除，undone 应为 0，不能在成交后用它判断方向。

## BUG-P08-006 平仓入口和撤单任务对 commodity 缺少空值保护

- 审核裁定：`stra_exit_long/stra_exit_short` 的缺 guard 是产品缺陷；`stra_cancel` 子项仅在订单已存在后 commodity 被移除时触发，移除场景待验证。
- 位置：平仓 `src/WtBtCore/UftMocker.cpp:679-746`；撤单任务 `:407-441`。对照开仓入口 `:472-485,534-547,597-610,638-650` 和 `update_position():1170-1172`。
- 证据：`stra_exit_long/short()` 直接对 `get_commodity_info()` 的结果调用 `getCoverMode()`，而同文件开仓入口对 NULL 早退。公开平仓入口接收未知标准合约时即可抵达该解引用；在查品种前还会通过 `_pos_map[stdCode]` 新建状态项。撤单 lambda 中也有相同裸用，但其成立要求已登记订单对应的商品元数据在撤单前消失，目前没有证明该变更可经支持接口发生。
- 测试前提：`DISABLED_NullStrategyAndNullCommDerefFamily` 首先调用 `on_session_begin()`，会因 BUG-P08-001 先失败，无法证明后续 exit/cancel 子项。
- 修复方向：平仓入口先解析并校验 commodity，再读取或修改仓位 map；NULL 时记录错误并返回 0。仅在确认元数据可运行期失效后，再为撤单任务设计稳定处理（NULL guard 或下单时保存 cover mode）。

## BUG-P08-007 HftMocker qty=0 按合约和方向撤单只处理第一笔

- 审核裁定：产品缺陷；保留此条。接口契约由 `src/Includes/IHftStraCtx.h:186-194` 明确规定 `qty == 0` 表示撤销全部，返回被撤订单 ID。
- 位置：`src/WtBtCore/HftMocker.cpp:574-595`；`src/WtBtCore/HftMocker.h:317`。
- 证据：实现找到第一笔匹配订单后，因 `left < qty` 对 `qty == 0` 为假而进入 `else break`；因此零数量请求只返回第一笔订单 ID。当前 `p08_hft_order_cancel_test.cpp` 已将正数量部分撤单保留为 `CancelByCodeAndDirection`，并新增 `CancelByCodeAndDirectionZeroCancelsAllMatching` 验证零数量全撤。主控最新运行报告：正数量用例通过，新增的零数量用例失败。
- 证据边界：本轮使用的预编译二进制对应测试源码修改前版本，其中旧 `CancelByCodeAndDirection` 用例通过；该结果不能代表当前新增 qty=0 回归测试通过。
- 修复方向：将 `qty == 0` 明确解释为不设数量上限并继续扫描全部匹配订单；非零 qty 才按累计剩余量停止。测试设置至少两笔同合约、同方向订单并断言全部 ID 返回。

## 独立复核记录

使用现有 `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_btcore`，并将 `LLVM_PROFILE_FILE` 指向 `/tmp/wtbtcore-audit-p08-%p.profraw`，过滤运行了 `CloseTodayBranchScalesDynProfit`、`CancelAllScansMatchingCodeOnly`、`T1OvernightPositionCloseable`、`CancelByCodeAndDirection`。在该旧预编译二进制中，清理、撤单返回和旧 Hft 测试通过；T+1 用例在 `p08_uft_dispatch_t1_test.cpp:212` 报失败，源码原因是直接 `on_trade()` 跳过平仓订单对可用量的预占。旧 Hft 测试结果不代表当前新增 qty=0 回归测试；当前源码的零数量测试结果以上述主控最新运行记录为准。没有运行包含故意空指针和阻塞分支的禁用家族。
