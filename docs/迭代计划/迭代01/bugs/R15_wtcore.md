# R15 WtCore 缺陷登记（lane R15，只登记不改产品代码）

范围：`HftStraBaseCtx.h` `HftStraContext.cpp` `SelStraBaseCtx.cpp` `WtExecuterFactory.cpp` `CtaStrategyMgr.cpp` `HftStrategyMgr.cpp` `ParserAdapter.cpp` `WtLocalExecuter.cpp`。
分级：P0 崩溃/进程终止 · P1 功能错误 · P2 健壮性/资源 · P3 一致性/可读性 · 需接缝（可达性阻塞，非缺陷但挡覆盖率）。
所有行号取自本 lane 实读的源码；未修改任何产品代码、未放宽任何断言。

---

## BUG-R15-001 【P0】HftStraBaseCtx::setUserTag 对 capacity==0 的 circular_buffer 执行 push_back ⇒ assert-abort / UB，订单 userTag 链条整体失效
- 位置：`src/WtCore/HftStraBaseCtx.h` L774（`boost::circular_buffer<OrderTag> _orders;` 默认构造，**全工程没有任何 `set_capacity` 调用**，构造/`on_init`/`init_outputs` 均不填容量）、L646-649（`setUserTag`：`_orders.push_back({ localid, usertag });`）。
- 触发条件：任何一次带标签的下单回报登记 —— `stra_buy/stra_sell`（`HftStraBaseCtx.cpp` L234-236、L255-257、L298-300、L318-320 的 `for (auto localid : ids) setUserTag(localid, userTag);`）。boost::circular_buffer 在 `empty() && capacity()==0` 时 push_back 走「满 ⇒ remove_one」分支，对未初始化的槽位执行 destroy/construct：debug 构建 `BOOST_CB_ASSERT` 失败即 abort，release 为未定义行为。
- 连带后果：即使 push 侥幸不崩，`getOrderTag`（L625-637）永远返回 `""`，`HftStraContext::on_trade/on_order/on_entrust` 转发给策略的 userTag 恒为空串 —— 策略侧「按标签跟踪订单」的功能事实上不可用。
- 测试侧处置：本 lane 的 `r15_hft_stra_context.cpp` 中两条依赖 push 的用例
  `R15CtxTest.DISABLED_order_tag_set_get_erase_end_to_end`、
  `R15CtxTest.DISABLED_order_tag_lookup_miss_returns_neighbor_tag`
  已整条 DISABLED（解开即命中本触发条件）；启用态只跑「空表查询/删除早退」的安全子集。
- 期望：构造或 `on_init` 里 `_orders.set_capacity(N)`（N≥并发挂单上限），或退回 `wt_hashmap<uint32_t, std::string>`。
- 状态: 待审核

## BUG-R15-002 【P1】getOrderTag/eraseOrderTag 的 lower_bound 结果不校验键相等 ⇒ 查询失配返回后继标签、删除失配抹掉后继
- 位置：`src/WtCore/HftStraBaseCtx.h` L625-637（`getOrderTag`：`lower_bound` 后只判 `it == _orders.end()`，没有 `it->_localid == localid`）、L656-668（`eraseOrderTag` 同形，失配时直接 `_orders.erase(it)`）。
- 触发条件：`_orders` 里有 (1,AAA)、(5,EEE) 时 `getOrderTag(3)` 返回 `"EEE"`（契约上应为 `""`）；`eraseOrderTag(2)` 会把 5 号订单的标签整条删掉（静默数据丢失）。注意本缺陷实际可观测的前提是 BUG-R15-001 先修复。
- 测试侧处置：`DISABLED_order_tag_lookup_miss_returns_neighbor_tag` 按实际算法写了期望值（`getOrderTag(3)=="EEE"`），把「应返 ""」的期望留在本登记里，未塞进任何启用用例。
- 期望：命中后校验 `it->_localid == localid`，失配返回 `""` / 不删除。
- 状态: 待审核

## BUG-R15-003 【P2】SelStraBaseCtx 的 load_data/load_userdata 对 rapidjson 字段零校验，缺字段/类型不符即 assert-失败或 UB
- 位置：`src/WtCore/SelStraBaseCtx.cpp`
  - `load_userdata` L198-227：L222-224 `m.name.GetString()`/`m.value.GetString()` 不判 `IsString()` —— ud 文件里任何一个数值/对象值都直接踩 rapidjson assert。
  - `load_data` L229-376：fund 段 L250-258 无条件 `jFund["total_profit"].GetDouble()`、`jFund["tdate"].GetUint()`；仓位段 L265-300 无条件 `pItem["code"].GetString()`、`["closeprofit"]`、`["lastentertime"].GetUint64()` 等 8 个字段；明细段 L305-334 无条件取 9 个字段（只有 `maxprice/minprice/opentdate` 有 HasMember 守卫，见 L313-324 —— 说明作者知道要防，但只防了三处）。
- 触发条件：手工编辑/旧版本格式的 `stradata/<name>.json`、`usrdata/ud_<name>.json`（缺任意一个「无守卫」字段），或磁盘半写状态。策略进程启动即崩。
- 测试侧处置：`r15_sel_persist.cpp` 所有手拼 JSON 一律带齐三段全字段（文件头注释写明原因）；解析失败/空文件的**合法**早退（L238-244）已有启用用例 `broken_json_and_empty_file_are_ignored`。缺字段形态会崩宿主，未构造用例。
- 期望：`HasMember + IsXxx` 双守卫后再取值，缺失记 error 并跳过该段。
- 状态: 待审核

## BUG-R15-004 【P2】WtLocalExecuter::writeLog 用 strcat 拼固定 2048 thread_local 缓冲，无长度上限（与 BUG-R10-010 同族的新位置）
- 位置：`src/WtCore/WtLocalExecuter.cpp` L400-409：`static thread_local char szBuf[2048] = {0}`；`fmtutil::format_to(szBuf, "[{}]", _name)` 之后 `strcat(szBuf, message)`。
- 触发条件：`message` 长度不受调用方约束（ExecuteUnit 撮合日志、错误回报文案），`[name] + message > 2047` 即越界写 thread_local 数组。对照：`WtArbiExecuter.cpp` L352-358（R10 已登记 BUG-R10-010）、本文件是同一家族的第三处；`WtDiffExecuter.cpp` L440-445 用单步 `format_to` 才是正确形态。
- 测试侧处置：启用用例只喂短消息（`r15_mgr_executers.cpp` 的 `local_executer_null_init_and_empty_channel_events`），越界形态不构造。
- 期望：`fmtutil::format_to(szBuf, "[{}] {}", _name, message)` 单步格式化并截断。
- 状态: 待审核

## BUG-R15-005 【P2】SelStraBaseCtx::on_tick 信号触发段解引用 get_session_info 的结果且无判空
- 位置：`src/WtCore/SelStraBaseCtx.cpp` L562-571：
  ```cpp
  WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);
  if (sInfo->isInTradingTime(_engine->get_raw_time(), true))
  ```
  而 `WtEngine::get_session_info`（`WtEngine.cpp` L104-112）在 commodity 的 session id 查不到时可以返回 NULL —— 同函数 L127-129（`WtEngine::append_signal` 链）也是同样的裸解引用形态。
- 触发条件：任何「先有仓/信号、后改基础数据表把时段删掉/打错」的运维形态；真实 `WtBaseDataMgr`（键不存在返回 NULL，不像本 lane 替身那样兜底取首个会话）下就是启动崩溃。
- 测试侧处置：本 lane 替身 `q01sel::BaseDataMgr::getSession` 对未知 sid 兜底返回首个会话，因此启用用例里 `sInfo` 恒非 NULL，掩盖不了也证明不了该分支；NULL 形态未构造用例（会崩宿主）。
- 期望：`if (sInfo == NULL) { log_error; return/跳过触发 }`。
- 状态: 待审核

## BUG-R15-006 【需接缝】三个 Ticker 的 run 循环 + WtHftEngine::run 落地段整体不可达（规则 2 禁线程/挂钟）
- 位置：
  - `src/WtCore/WtCtaTicker.cpp` L37/L122/L196/L202/L250/L271（`run/stop/cleanup/task_loop` 的 `TimeUtils::getDateTime`、`StdUniqueLock`、`sleep_for`）；
  - `src/WtCore/WtHftTicker.cpp` L47/L114/L177/L183/L228/L246（同形）；
  - `src/WtCore/WtSelTicker.cpp` L47/L117/L180/L186/L226（同形）；
  - `src/WtCore/WtHftEngine.cpp` L56/L61（release/init 转调 WtEngine，线程 + DLL 缝，同 R10「WtSelEngine run/init」登记口径）、L81-110（`run()` 里 marker.json 组包落地，前置依赖 `_tm_ticker->init(_data_mgr->reader(), ...)` —— reader 只能插件装载（BUG-R10-007）、`_adapter_mgr` 未初始化）。
- 现象：ticker 的 `on_tick/on_bar` 入队口虽可在主线程调用，但入队后没有任何公开 peek/drain 观测口（`_task_queue` 私有、无 count），断言不可证伪；循环体本身必须起线程。
- 估计留白行数：ticker 三文件本轮 UNCOVERED 列出的 17 行 + run 循环周边约 **55 行**；WtHftEngine run/init 约 **45 行**。合计 ~100 行。
- 建议接缝：ticker 增加 `size_t queued_tasks() const` / `bool pop_task_for_test(Task&)`；WtHftEngine 把「marker 落地」块抽成可独立调用的 `dump_running_marks()`。
- 状态: 待审核

---

## 复核前人与规则口径（规则：引用前先自验）
- R10 标注的六个「需要接缝」面（EventNotifier ~340、HftStraBaseCtx 报单链 ~260、WtDtMgr reader ~230、执行单元工厂 ~210、Sel run/init ~75、线程池 ~70）本 lane **一律未硬闯**；`WtExecuterFactory::loadFactories` 只打目录扫描/`free_library` 分支，单元主体仍被 BUG-R10-002 挡着。
- `CodeHelper` 合约键形：本轮全部用三段式分月合约（`SHFE.ag.2512`、`CFFEX.no.2512`），与 `CodeHelper.hpp:601-616` 规则一致。
- BUG-R15-001/002 的组合意味着：HFT 策略 API 里 `userTag` 全链路（下单登记 → 回报转发 → 平仓归属）**当前没有任何一条能安全跑通**，建议评审时按「功能面整体回炉」处理，而不是只修 push_back 一行。
