# R11 缺陷登记 — WtUftCore（第七波）

- lane: R11，范围 = `src/WtUftCore`（TraderAdapter/UftStraContext/EventNotifier/ShareManager/ParserAdapter/WtUftTicker/WtUftEngine/UftStrategyMgr）
- 只登记，不改 `src/**`。分级：S=崩溃/UB，A=静默错误结果，B=死代码/接缝缺失，C=轻微。
- 前几波已登记、本轮**不重复开单**的事实：`WtUftEngine.cpp:209-213` `return NULL;` 之后约 30 行不可达（已登记，本轮直接绕开）；`UftStraContext.cpp:941` 的连带后果同源。

---

## BUG-R11-001 — ShareManager 真实读写面缺共享内存接缝，150 行无法在禁 shm 条件下进入

- 文件: `src/WtUftCore/ShareManager.cpp`
- 现象: 除 `initialize()`/`self()` 外的全部 20 个读写入口（`set_value`×6、`get_value`×6、`allocate_value`×6、`init_domain`、`commit_param_watcher`、`start_watching`）都以 `_inited` 为闸门；`_inited` 只能在 `initialize()` 成功 `dlopen` WtShareHelper 后置真（L40-42），一旦置真，后续调用就会打 `_init_master` / `_set_double` / `_allocate_string` 等真实函数指针，等价于创建真实共享内存段。
- 影响行数（估计）: **约 150 行**（L40-73 装载与取符号、L83-121 监控线程体、L132-141 init_domain 主体、L156-157、L176-266、L285-380、L401-506 的非 guard 行）。本轮只拿到约 40 行 guard。
- 需要的接缝: 把 22 个 `func_*` 指针做成可注入的接口（或在 `ShareManager.h` 暴露一个 `set_helper_hooks()` 测试注入口），让单测能挂一个纯内存替身。
- 状态: 已撤回（测试接缝需求不是产品缺陷）
- 复核：`_inited` 为假时各共享操作按实现返回 false/default/null；成功加载 helper 后调真实函数指针是该功能的正常实现路径。当前单测缺少可注入 helper 的接缝，只能说明覆盖受限，不能据此登记 150 行产品错误。若要扩大覆盖，可另立测试设计任务，不计产品缺陷。

## BUG-R11-002 — UftStraContext::watch_param 的 5 个数值重载解引用 nullptr

- 文件: `src/WtUftCore/UftStraContext.cpp` L700-727
- 现象: 形如 `return *ShareManager::self().allocate_value(_name.c_str(), name, val, false, true);`。共享模块缺失时 `allocate_value` 返回 `nullptr`（ShareManager.cpp L396-506 的 guard），这里直接 `*` 解引用 → 段错误。字符串重载（L696-699）没有解引用，所以是安全的——同一族接口行为不一致。
- 复现触发条件: ShareManager 因共享模块缺失未初始化时，通过 UftStraContext 调用数值 `watch_param`；不能将这一条件泛化为所有离线环境。
- 处置: `R11UftCtxParam.numericWatchParamOfflineFallbackCandidate` 在独立子进程分别调用五个重载，要求正常退出及返回候选初始值。每种类型分别报告失败，不把崩溃作为成功。
- 分级: S（崩溃）
- 状态: 本机五种数值重载分别复现 signal 11；真实共享模块及线上环境未验证
- 复核：`ShareManager::allocate_value` 在未成功初始化时返回 nullptr，五个数值重载直接解引用；缺失模块场景下五个子进程分别在完成上下文构造、打印对应类型的调用前标记后因 signal 11 未能正常退出，离线 read/sync/string 三项对照通过。没有调试器栈，具体指令位置依据源码而非崩溃栈判断。基类默认实现返回初始值只是一个可考虑的回退方案，并不足以单独证明派生类对失败情况的公开合同。修复应对分配失败先判空，明确失败通知和回退语义，避免静默声称监听成功；在合同确定后用这五种类型及共享模块正常初始化路径验证。当前测试中的返回初始值断言是候选合同，并非已确认要求。

## BUG-R11-003 — TraderAdapter::getRiskParams 在缺少 "default" 组时解引用 end 迭代器

- 文件: `src/WtUftCore/TraderAdapter.cpp` L1970-1988
- 现象: 末尾 `it = _risk_params_map.find("default"); return &it->second;` 未判 `it != end()`。`_risk_params_map` 只在 `init()`（L103 起，解析 `riskmon` 配置节）里填充；走 `initExt()` 的通道（所有单测路径，以及任何未配 riskmon 的实盘通道）表是空的 → `&it->second` 是 UB。
- 附带: 中间的 key 切片 `std::string(stdCode + idx + 1, eIdx - idx + 1)` 长度比实际品种号多算 1 个字符（`eIdx - idx` 才对），所以品种级风控参数**永远查不到**，只能落到 default —— 即使 default 存在，按品种配置也失效。
- 分级: S（UB）+ A（静默择一）
- 状态: 已合并（主编号 BUG-N03-004；同根因见 BUG-Q04-003/005）
- 复核：源码确有 `ag2406` 被切成 `ag24` 的长度错误，以及未命中时直接取 `end()` 成员的未定义行为。但 `checkOrderLimits/checkCancelLimits` 在下单/撤单入口的调用目前均为注释，不能据此宣称现行实盘风控已生效后又失效；缺 default 的崩溃用例为 DISABLED，本次未运行。保留产品修复和验收归属到 BUG-N03-004，不重复计数。

## BUG-R11-004 — TraderAdapter::getOrders(stdCode) 按标准全码查询恒返回空集合

- 文件: `src/WtUftCore/TraderAdapter.cpp` L284-302
- 现象: 过滤条件是 `strcmp(ordInfo->getCode(), stdCode) == 0`，其中 `ordInfo->getCode()` 是柜台原始代码（如 `ag2406`），而调用方（含 `UftStraContext`、上层风控）传的是标准全码（如 `SHFE.ag2406`）。两者永不相等，只有 `stdCode` 为空串（`isAll`）时才有结果。
- 复现: `r11_trader_reply_deep_test.cpp::orderResponseFiltersBadEntries` —— 同一张 `_orders` 里有一笔 `SHFE.ag2406` 的活单，`getOrders("")` 得到 size=1，`getOrders("SHFE.ag2406")` 得到 size=0。
- 分级: A（接口静默返回错误结果，调用方无从察觉）
- 状态: 已合并（主记录 BUG-Q04-001）
- 复核：函数按 `stdCode` 参数名接受标准码，却将其与订单对象原始 `getCode()` 直接比较。`orderResponseFiltersBadEntries` 已将现状零结果改为期望同一活单；定向运行 FAIL：全量查询找到该订单，但标准码查询返回空。此为本地替身证据，未验证真实柜台对象字段或所有调用方影响；合并到 Q04-001 统一处理。

## BUG-R11-005 — WtUftRtTicker 的定时槽/交易日推进与线程强耦合，缺"显式时间入参"接缝

- 文件: `src/WtUftCore/WtUftTicker.cpp`
- 现象: `_thrd` 只能由 `run()`（L170-240）赋值，而 `run()` 里 `new StdThread` 并在循环中 `sleep_for(10ms)`/`sleep_for(10s)`（L198、L246）。`on_tick()` 的 `_thrd == NULL` 早退（L67-75）之后的全部分钟槽推进逻辑（L77-165：`timeToMinutes`/`minuteToTime`/`isLastOfSection`、`_cur_pos`/`_last_emit_pos` 比较、`on_minute_end` 触发、`_next_check_time` 计算）在无线程条件下**不可达**，因为 `_thrd` 是私有成员且无注入口。另外 `init()`（L55-62）无条件解引用 `_engine`，且用 `TimeUtils::getDateTime` 读挂钟。
- 影响行数（估计）: **约 120 行**（on_tick L77-165 主体 ≈ 89 行 + run() 线程体 L176-240 ≈ 45 行，含线程内锁 L125/L205）。本轮只拿到 ctor/dtor/init/stop/早退 ≈ 20 行。
- 需要的接缝: 把 `on_tick` 的时间推进段抽成一个接受 `(date, time)` 显式入参的私有/受保护函数（或在头里加 `#ifdef WT_UNITTEST` 的 `tick_for_test`），并允许在不 `run()` 的情况下把 `_thrd` 标记为"已初始化"。
- 状态: 已撤回（线程测试接缝缺失不构成产品缺陷）
- 复核：正常 `WtUftEngine::run()` 会初始化并启动 ticker，再由行情入口调用 `on_tick`；`_thrd == NULL` 的直接转发分支服务于未启动状态，不能用未启动实例无法推进分钟槽来证明产品路径缺陷。缺显式时间/线程替身是测试设计限制。交易时段为空指针属于另一项（BUG-P06-006）；停机等待长休眠也已由 BUG-R85-002 记录，不在本项重复。

## BUG-R11-006 — EventNotifier 的 JSON 组装与广播只能经 `_mq_sid != 0` 的 asio 任务进入，缺同步排空接缝

- 文件: `src/WtUftCore/EventNotifier.cpp`
- 现象: 5 个 `notify*` 入口第一件事都是 `if (... || _mq_sid == 0) return;`（L110-113、L138-141、L165-168、L192-196、L211-215），而 `_mq_sid` 只有 `init()` 成功 `dlopen` WtMsgQue 并调用 `_creator(_url)` 后才非 0（L88-92）——那一步会真的起一个 MQ server（UDP socket）并 `new StdThread` + `sleep_for(2ms)` + `_asyncio.run_one()`（L96-105）。`tradeToJson`(L230-271) / `orderToJson`(L274-303) 还是 **private**，测试侧完全没有入口。任务体里 rapidjson 的组装代码（L121-135、L149-162、L176-190、L205-209、L222-226）只有 `_asyncio` 被驱动才执行，而 `_asyncio` 也是 private。
- 影响行数（估计）: **约 255 行**（本文件 267 行未覆盖中，除 ctor/dtor/5 条 guard/init 失败分支约 12 行外全部）。
- 需要的接缝: (1) 把 `tradeToJson`/`orderToJson` 提为 public 或 static 可测函数；(2) 给 `_publisher` 一个注入点（函数指针可替换）并允许"同步排空"模式（`_asyncio.poll()` 由测试驱动，不起 worker 线程）。
- 状态: 已撤回（异步测试接缝需求不是产品缺陷）
- 复核：`_mq_sid == 0` 时静默返回是未挂载消息队列的保护路径；成功初始化后由生产 worker 驱动 asio 任务。后续 R143 用 `inproc://` 与生产 worker 覆盖 JSON/发布路径，说明不必新增同步排空接口才能验证。private 发布函数/指针不便直接注入属于测试便利性，不是产品行为缺陷。

## BUG-R11-007 — ParserAdapter 的 filter 分支在 initExt 路径上是死代码

- 文件: `src/WtUftCore/ParserAdapter.cpp` L111-127（init 内）与 L181-197（initExt 内）
- 现象: `_filters` 只在 `init()` 解析 `cfg->getString("filter")` 时填充（L106-110）；`initExt()` 完全不接受 cfg，因此 `_filters` 恒空。于是 `initExt` 里的 `if (!_filters.empty()) { ...按 exchg/code/fullCode/fullPid/altCode/fullAltCode 六路匹配... }` 整块（L181-197，约 16 行）在 initExt 下永不进入；而 `init()` 那一块（L111-127）又必须先成功 `dlopen` 一个真实 parser 动态库才可达，离线单测同样进不去。两处逻辑还是逐字复制的。
- 影响行数（估计）: 约 32 行（两块各 16 行）
- 分级: B（死分支 + 复制粘贴）
- 状态: 已撤回（正常 `initExt` 没有 filter 配置，不代表过滤功能缺陷）
- 复核：`_filters` 仅由配置式 `init()` 装载；`initExt()` 的接口只接收 API，没有 filter 入参，fresh adapter 上自然走全量订阅分支。过滤分支只有混用两种初始化方式时才可能命中，而该组合没有有效调用契约或仓库生产调用点。该分支是冗余/未用路径，不能据此声称 `initExt` 丢失了本应配置的过滤规则。

## BUG-R11-008 — TraderAdapter::onLogout 是空实现，登出不通知任何 sink

- 文件: `src/WtUftCore/TraderAdapter.cpp` L1149-1152
- 现象: 函数体全空。柜台登出后 `_state` 不回退、`_sinks` 收不到 `on_channel_lost`，上层策略会以为通道仍然可用（对比 `onTraderError` L1841-1845，它连 `on_channel_lost` 都不发，只写日志）。
- 证据: `r11_trader_reply_deep_test.cpp::logErrorAndLogoutAreNarrow` 断言 `onLogout()` 后 `_sink._lostCnt == 0` 且 `_readyCnt == 0`。
- 分级: C（行为缺失，非崩溃；需产品确认是否有意为之）
- 状态: 待验证（回调语义与影响未证）
- 复核：`onLogout()` 当前确为空实现，测试只证明现状不会通知 sink。`ITraderSpi` 仅将它描述为“注销回报”；仓库另有 `handleEvent(WTE_Close)` 明确通知 `on_channel_lost`，尚未证明 `onLogout` 等价于意外断线，还是一次正常主动注销回报。因此 `_state` 是否应变化、是否应通知 sink 需先按接口/插件契约确认；不能把测试对现状的断言当成期望行为。确认注销即不可用后，再补状态迁移和对应 sink 通知，并覆盖与 WTE_Close 的去重。

## BUG-R11-009 — WtUftTicker::on_tick 的 `else if (_cur_pos < minutes)` 分支在首笔行情下会漏触发闭合

- 文件: `src/WtUftCore/WtUftTicker.cpp` L112-148
- 现象: `_cur_pos == 0` 时只做 `_cur_pos = minutes`（L110），不更新 `_last_emit_pos`；下一根进入 `_cur_pos < minutes` 分支后，`_last_emit_pos(0) < _cur_pos` 成立才会补发一次 `on_minute_end`，但补发用的是 `_cur_pos`（旧位置）而非跨越的中间分钟，跨多分钟时中间的分钟线闭合被合并成一次。`WtUftEngine.cpp:209-213` 的不可达 return 使这条链在实盘里也无法由引擎兜底。
- 分级: A（静默少发事件，行情断续时可见）
- 状态: 待验证（跨分钟缺口的产品语义未定义）
- 复核：首个行情仅设 `_cur_pos` 是预期初始化；`run()` 启动的定时线程会在 `_next_check_time` 到达且 `_last_emit_pos < _cur_pos` 时闭合该位置，所以“首笔行情未推进 `_last_emit_pos` 即漏掉首次闭合”不成立。数据跳过多个分钟时，tick 分支一次最多闭合旧位置；是否必须为没有行情的中间分钟合成结束事件，当前接口/产品契约未说明，且 `WtUftEngine::on_minute_end()` 目前为空实现。本次未运行线程测试，不据此计为确认缺陷；若未来实现分钟级消费逻辑，应先定义缺口语义再验收。
