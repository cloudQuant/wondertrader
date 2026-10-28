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
- 状态: 待审核（阻塞项，需要接缝决策，非本轮可解）

## BUG-R11-002 — UftStraContext::watch_param 的 5 个数值重载解引用 nullptr，必崩

- 文件: `src/WtUftCore/UftStraContext.cpp` L700-727
- 现象: 形如 `return *ShareManager::self().allocate_value(_name.c_str(), name, val, false, true);`。共享模块缺失时 `allocate_value` 返回 `nullptr`（ShareManager.cpp L396-506 的 guard），这里直接 `*` 解引用 → 段错误。字符串重载（L696-699）没有解引用，所以是安全的——同一族接口行为不一致。
- 复现触发条件: `UftStraContext ctx(&engine, "name");` 后调用 `ctx.watch_param("k", 1.0)`（或 uint32/int32/uint64/int64 任一数值重载），在 ShareManager 未初始化（=所有离线单测环境）下立即 SIGSEGV。
- 处置: 用例 `DISABLED_R11UftCtxParam_numericWatchParamDereferencesNull`（`src/TestUnits/coverage/q04_uftcore3/r11_uftctx_param_log_test.cpp`）整条禁用。
- 分级: S（崩溃）
- 状态: 待审核（本文件唯一崩溃条目；同族的 `read_param`/`sync_param` 实测安全，q04 曾把它们误判为"需要 shm"而 DISABLED，本轮已由启用用例推翻）

## BUG-R11-003 — TraderAdapter::getRiskParams 在缺少 "default" 组时解引用 end 迭代器

- 文件: `src/WtUftCore/TraderAdapter.cpp` L1970-1988
- 现象: 末尾 `it = _risk_params_map.find("default"); return &it->second;` 未判 `it != end()`。`_risk_params_map` 只在 `init()`（L103 起，解析 `riskmon` 配置节）里填充；走 `initExt()` 的通道（所有单测路径，以及任何未配 riskmon 的实盘通道）表是空的 → `&it->second` 是 UB。
- 附带: 中间的 key 切片 `std::string(stdCode + idx + 1, eIdx - idx + 1)` 长度比实际品种号多算 1 个字符（`eIdx - idx` 才对），所以品种级风控参数**永远查不到**，只能落到 default —— 即使 default 存在，按品种配置也失效。
- 分级: S（UB）+ A（静默择一）
- 状态: 待审核（本轮 R11 用例主动绕开，未写断言，避免不可复现的崩溃）

## BUG-R11-004 — TraderAdapter::getOrders(stdCode) 按标准全码查询恒返回空集合

- 文件: `src/WtUftCore/TraderAdapter.cpp` L284-302
- 现象: 过滤条件是 `strcmp(ordInfo->getCode(), stdCode) == 0`，其中 `ordInfo->getCode()` 是柜台原始代码（如 `ag2406`），而调用方（含 `UftStraContext`、上层风控）传的是标准全码（如 `SHFE.ag2406`）。两者永不相等，只有 `stdCode` 为空串（`isAll`）时才有结果。
- 复现: `r11_trader_reply_deep_test.cpp::orderResponseFiltersBadEntries` —— 同一张 `_orders` 里有一笔 `SHFE.ag2406` 的活单，`getOrders("")` 得到 size=1，`getOrders("SHFE.ag2406")` 得到 size=0。
- 分级: A（接口静默返回错误结果，调用方无从察觉）
- 状态: 待审核

## BUG-R11-005 — WtUftRtTicker 的定时槽/交易日推进与线程强耦合，缺"显式时间入参"接缝

- 文件: `src/WtUftCore/WtUftTicker.cpp`
- 现象: `_thrd` 只能由 `run()`（L170-240）赋值，而 `run()` 里 `new StdThread` 并在循环中 `sleep_for(10ms)`/`sleep_for(10s)`（L198、L246）。`on_tick()` 的 `_thrd == NULL` 早退（L67-75）之后的全部分钟槽推进逻辑（L77-165：`timeToMinutes`/`minuteToTime`/`isLastOfSection`、`_cur_pos`/`_last_emit_pos` 比较、`on_minute_end` 触发、`_next_check_time` 计算）在无线程条件下**不可达**，因为 `_thrd` 是私有成员且无注入口。另外 `init()`（L55-62）无条件解引用 `_engine`，且用 `TimeUtils::getDateTime` 读挂钟。
- 影响行数（估计）: **约 120 行**（on_tick L77-165 主体 ≈ 89 行 + run() 线程体 L176-240 ≈ 45 行，含线程内锁 L125/L205）。本轮只拿到 ctor/dtor/init/stop/早退 ≈ 20 行。
- 需要的接缝: 把 `on_tick` 的时间推进段抽成一个接受 `(date, time)` 显式入参的私有/受保护函数（或在头里加 `#ifdef WT_UNITTEST` 的 `tick_for_test`），并允许在不 `run()` 的情况下把 `_thrd` 标记为"已初始化"。
- 状态: 待审核（需要接缝，本轮按硬规则留白，不硬闯）

## BUG-R11-006 — EventNotifier 的 JSON 组装与广播只能经 `_mq_sid != 0` 的 asio 任务进入，缺同步排空接缝

- 文件: `src/WtUftCore/EventNotifier.cpp`
- 现象: 5 个 `notify*` 入口第一件事都是 `if (... || _mq_sid == 0) return;`（L110-113、L138-141、L165-168、L192-196、L211-215），而 `_mq_sid` 只有 `init()` 成功 `dlopen` WtMsgQue 并调用 `_creator(_url)` 后才非 0（L88-92）——那一步会真的起一个 MQ server（UDP socket）并 `new StdThread` + `sleep_for(2ms)` + `_asyncio.run_one()`（L96-105）。`tradeToJson`(L230-271) / `orderToJson`(L274-303) 还是 **private**，测试侧完全没有入口。任务体里 rapidjson 的组装代码（L121-135、L149-162、L176-190、L205-209、L222-226）只有 `_asyncio` 被驱动才执行，而 `_asyncio` 也是 private。
- 影响行数（估计）: **约 255 行**（本文件 267 行未覆盖中，除 ctor/dtor/5 条 guard/init 失败分支约 12 行外全部）。
- 需要的接缝: (1) 把 `tradeToJson`/`orderToJson` 提为 public 或 static 可测函数；(2) 给 `_publisher` 一个注入点（函数指针可替换）并允许"同步排空"模式（`_asyncio.poll()` 由测试驱动，不起 worker 线程）。
- 状态: 待审核（需要接缝；本轮仅收 12 行 guard）

## BUG-R11-007 — ParserAdapter 的 filter 分支在 initExt 路径上是死代码

- 文件: `src/WtUftCore/ParserAdapter.cpp` L111-127（init 内）与 L181-197（initExt 内）
- 现象: `_filters` 只在 `init()` 解析 `cfg->getString("filter")` 时填充（L106-110）；`initExt()` 完全不接受 cfg，因此 `_filters` 恒空。于是 `initExt` 里的 `if (!_filters.empty()) { ...按 exchg/code/fullCode/fullPid/altCode/fullAltCode 六路匹配... }` 整块（L181-197，约 16 行）在 initExt 下永不进入；而 `init()` 那一块（L111-127）又必须先成功 `dlopen` 一个真实 parser 动态库才可达，离线单测同样进不去。两处逻辑还是逐字复制的。
- 影响行数（估计）: 约 32 行（两块各 16 行）
- 分级: B（死分支 + 复制粘贴）
- 状态: 待审核（本轮覆盖到的是 `_filters` 为空的 else 全量分支）

## BUG-R11-008 — TraderAdapter::onLogout 是空实现，登出不通知任何 sink

- 文件: `src/WtUftCore/TraderAdapter.cpp` L1149-1152
- 现象: 函数体全空。柜台登出后 `_state` 不回退、`_sinks` 收不到 `on_channel_lost`，上层策略会以为通道仍然可用（对比 `onTraderError` L1841-1845，它连 `on_channel_lost` 都不发，只写日志）。
- 证据: `r11_trader_reply_deep_test.cpp::logErrorAndLogoutAreNarrow` 断言 `onLogout()` 后 `_sink._lostCnt == 0` 且 `_readyCnt == 0`。
- 分级: C（行为缺失，非崩溃；需产品确认是否有意为之）
- 状态: 待审核

## BUG-R11-009 — WtUftTicker::on_tick 的 `else if (_cur_pos < minutes)` 分支在首笔行情下会漏触发闭合

- 文件: `src/WtUftCore/WtUftTicker.cpp` L112-148
- 现象: `_cur_pos == 0` 时只做 `_cur_pos = minutes`（L110），不更新 `_last_emit_pos`；下一根进入 `_cur_pos < minutes` 分支后，`_last_emit_pos(0) < _cur_pos` 成立才会补发一次 `on_minute_end`，但补发用的是 `_cur_pos`（旧位置）而非跨越的中间分钟，跨多分钟时中间的分钟线闭合被合并成一次。`WtUftEngine.cpp:209-213` 的不可达 return 使这条链在实盘里也无法由引擎兜底。
- 分级: A（静默少发事件，行情断续时可见）
- 状态: 待审核（本轮无启用用例覆盖该分支——受 BUG-R11-005 的线程耦合阻塞，登记为待验证）
