# N04 WtBtCore 模拟撮合/数据管理缺陷记录（迭代 01）

本轮只读复核了生产代码、调用路径和相关测试前提；未修改生产代码或测试。候选状态按证据校正，重复项保留原编号并标明既有登记。

## BUG-N04-001 ExecMocker 未成功初始化仍进入回放回调

- 审核裁定：产品缺陷，存在生产调用路径；与 `bugs/R20_wtbtcore.md` 中 ExecMocker 未初始化空指针链重复登记。`BUG-R121-002` 是相邻但不同的情形：工厂 `createExeUnit()` 返回 NULL 后在 `init()` 内解引用。
- 代码锚点：`src/WtBtCore/ExecMocker.cpp:28-39,49-85,139-161`；`src/WtBtRunner/WtBtRunner.cpp:138-143`；`src/WtBtCore/HisDataReplayer.cpp:649-674`。
- 证据：构造函数未初始化 `_exec_unit`、`_volunit`、`_target`、`_sig_time`；`init()` 仅在 DLL 和 creator 成功后、且存在 `executer` 配置时给 `_exec_unit` 赋值。`WtBtRunner` 忽略 `init()` 返回值后仍注册 sink；`prepare()` 随后无条件调用 sink 的 `handle_init()`，该入口无条件调用 `_exec_unit->on_channel_ready()` 和 `set_position()`。因此模块装载失败或缺少 `executer` 配置可进入未初始化指针调用。具体行为属于未定义行为，不能只表述为确定的 NULL 解引用。
- 证据边界：禁用用例从直接调用 `handle_entrust()` 开始，未建立生产注册/回放顺序；该用例自身不能证明生产路径。`_period[0]` 在 C++11 及之后的 `std::string` 中，索引等于 `size()` 时返回终止空字符，不构成该条所称的 UB。`handle_order/trade` 的 `_last_tick` 裸用在常规 matcher 回调中通常发生于 `handle_tick()` 已缓存 tick 之后；直接手工调用回报回调的前提尚不能代表正常生产流程。
- 修复方向：构造时初始化所有基础成员；`init()` 要求且验证 executer/unit 非空，失败返回 false；`WtBtRunner` 在注册 sink、调用 `prepare()` 前检查初始化结果并停止回放。事件入口按允许的状态检查 unit/tick，避免部分初始化对象继续工作。

## BUG-N04-002 HisDataMgr 模块装载失败后仍解引用 reader

- 审核裁定：产品缺陷；与 `bugs/R20_wtbtcore.md` 的 HisDataMgr `init()` 失败臂空指针问题完全重复。
- 代码锚点：`src/WtBtCore/HisDataMgr.cpp:36-74`，尤其 `:46-71`；`src/WtBtCore/HisDataMgr.h:40`。
- 证据：构造函数将 `_reader` 初始化为 NULL。`load_library()` 失败时只记录日志；creator 符号缺失时也只记录日志；两个路径均落到 `:71 _reader->init(cfg, this)`。此配置错误场景会在初始化时触发空指针解引用，`init()` 不会返回预期的失败值。
- 无效前提：`init(nullptr)` 是空配置调用，当前接口没有声明支持空指针；不作为该产品缺陷的复现条件。现有禁用测试把默认模块、不存在模块、重复 init 和 `init(nullptr)` 串在一个用例里，不能逐项证明：默认路径依赖运行目录，且前一次成功留下的 `_reader` 会影响后续分支。
- 修复方向：模块装载、符号获取或 reader 创建失败时记录具体错误并立即 `return false`。`IBtDtReader::init` 当前返回 `void`，若要传播 reader 内部初始化失败，需先设计并评估接口变更，不能直接检查其返回值。测试应按每个失败路径使用独立 fixture/进程，另测 `load_raw_*` 在 reader 缺失时的既有安全早退。

## CtaMocker 空态接口族（不计入产品缺陷汇总）

- 审核裁定：撤销“空态调用族崩溃/挂死”的聚合结论；现有证据不足以登记为产品缺陷。唯一可确认的子点是非零持仓且 commodity 缺失时动态盈亏计算会裸解引用，已由 `BUG-P03-002` 提及；其运行期可达性仍待证明。
- 代码锚点：`src/WtBtCore/CtaMocker.cpp:415-431,695-720,973-1004,1297-1301,1504-1518,1520-1527,1603-1619,1885-1899`；`src/WtBtCore/HisDataReplayer.cpp:3021-3030,3034-3086`。
- 逐项核对：`init_cta_factory(nullptr)` 在 `:417-418` 返回 false；`stra_get_price/day_price` 检查 `_replayer`，回放器空缓存时返回 0；`stra_get_fund_data` 只读资金字段；`do_set_position` 在 commodity 缺失时早退；`get_cond_entrusts` 只返回/创建容器。`proc_tick()` 空信号、空持仓下不会进入持仓明细解引用。它们不支持原文所列的空态崩溃结论。
- 测试前提问题：`n04_cta_mocker_state_test.cpp:134-151` 将多类输入合并在同一禁用用例，首先执行 `handle_tick(kUnknown, nullptr, 0)`；`handle_tick` 需要有效 tick，此输入本身不满足事件接口前提，后续断言不可达。`init_cta_factory(nullptr)` 在该用例里也不会崩溃。该禁用测试尚待拆分和修正，不应把尚未到达的分支当成缺陷证据。
- 待验证子点：`update_dyn_profit()` 只有在 `_pos_map` 有非零持仓时才查询 commodity 并在 `:990` 解引用；SelMocker 同构。正常开仓路径要求 commodity 存在，当前未证明回放期间可经受支持接口删除/重载该 commodity。若有这种运行期变更契约，应单独立项并以该契约构造测试。
- 修复方向：不对被否定的接口族统一添加空态补丁。若确认支持持仓期间移除商品元数据，再为 Cta/Sel 动态盈亏路径增加一致的失效处理，并单独验证。
