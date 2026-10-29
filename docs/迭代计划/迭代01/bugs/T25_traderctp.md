# T25 TraderCTP 缺陷复核（2026-10-29）

初次复核时使用 `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_t_TraderCTP` 对旧版本 7 条启用用例逐条运行全部 PASS；该结果不代表后续修改过的断言。后续已定向复测 T25-002、004、006、008、010、011，具体见各条。未修改生产代码；`DISABLED_` 崩溃/挂死用例仍未执行，旧 rc 记录未重新取证。

## BUG-T25-001 — 未初始化成员（与 T2 条目重复，需合并）

- 状态: 已合并
- 复核状态：`已合并（主编号 T2-CTP-01、T2-CTP-02）`。
- 证据：`TraderCTP.cpp:203-217` 未初始化 `m_frontID`、`m_sessionID`、`m_orderRef`、`m_lDate`、`m_sink`、`m_bdMgr` 等字段；其中 `m_uLastQryTime` **已初始化为 0**，不应再列为漏初始化。`makeEntrustID` 读取会话字段（`:381-391`）；`registerSpi` 写 sink 和基础数据管理器（`:401-406`）；`init` 设置 creator（`:254-273`）。标准适配器在 `connect()` 前登记 SPI（`TraderAdapter.cpp:412-415`），所以未初始化指针的危险路径依赖非标准调用顺序/初始化失败。
- 结论与边界：会话字段部分已由 T2-CTP-01 登记；`m_sink`/creator 的失败初始化路径与 T2-CTP-02 重叠。没有本轮动态证据支持单独 Major 项，建议合并去重。建议构造时初始化全部裸指针和标量，并让 `connect()` 对 creator、返回 API 和 sink 失败进行受控处理。
- 测试：相关 `DISABLED_plugin_created_trader_has_indeterminate_sink` 本轮未运行；未声称复现。

## BUG-T25-002 — 畸形 entrustID 未解析仍发单

- 复核状态：`确认的输入校验缺陷；仅在带 usertag 且 ID 格式错误时触发`。
- 证据：`TraderCTP.cpp:492-500` 忽略 `extractEntrustID()` 的布尔返回值；解析失败时 `fid/sid/orderref` 未赋值，仍把未初始化 `orderref` 格式化进请求并写缓存。解析器在 `:1373-1394` 对缺少 `#` 明确返回 false；`orderAction` 有检查返回值的对照实现。
- 动态证据：旧测试将下发畸形 ID 当成通过；现改为 `order_insert_rejects_malformed_entrust_id_when_usertag_set`，要求失败返回且不发请求/不写缓存。修正后本地 stub 定向 FAIL：实际返回 0、调用 API 1 次并写入缓存。测试没有断言未初始化 `OrderRef` 的具体内容；有效 ID 的映射对照 PASS。
- 修复方向：解析失败时在构造请求和写缓存前返回错误；增加有效三段 ID 对照测试。严重性建议 Major；不把任意上游畸形 ID 单独表述成无条件 Blocker。

## BUG-T25-003 — 回调空指针保护缺失

- 复核状态：`条件风险；厂商回调载荷和 SPI 解绑时序待验证`。
- 证据：`OnRspError`（`TraderCTP.cpp:1159-1162`）无条件解引用 `m_sink`、`pRspInfo`；`OnRspQrySettlementInfo` 在已有结算内容且末帧时（`:1088-1096`）无条件读取 `pSettlementInfo->TradingDay` 并调用 `m_sink`。本地 CTP 头文件只给回调签名，没有说明这两个参数在末帧/解绑后是否可空。
- 测试边界：现存 `DISABLED_rsp_error_with_null_sink_crashes` 和 `DISABLED_settlement_info_last_row_without_payload_crashes` 为人工注入空指针的测试，不代表本轮运行或证明柜台会按该形状回调。`OnRspError` 在 `registerSpi(NULL)` 后的 NULL sink 路径尤其依赖 unregister 后是否仍有在途回调。
- 修复方向：回调边界判空；结算信息在收到有效行时保存交易日，末帧使用已保存值。先从 CTP SDK/柜台确认空末帧与解绑后的回调语义，再分别测试空查询、有效行后空末帧及 SPI 解绑时序。

## BUG-T25-004 — authenticate 同步失败被吞

- 复核状态：`确认的失败处理缺口`。
- 证据：`authenticate()`（`TraderCTP.cpp:1476-1488`）丢弃 `ReqAuthenticate()` 的同步返回值并恒返回 0；调用方 `login` 已将状态置为 `WS_LOGINING` 后调用它（`:426-427`）。当 API 返回非 0 且不产生回调时，没有日志、错误结果或状态回退。`OnRspAuthenticate` 仅处理实际异步回报（`:725-738`）。
- 动态证据：旧测试将吞码视为通过；现以 `authenticate_sync_failure_returns_error_without_waiting_for_login` 要求非零返回且不继续等待，定向 FAIL：stub 返回 7 后实际 login 返回 0、状态仍为 `WS_LOGINING`。这不证明真实柜台的具体错误码分布。
- 修复方向：检查同步返回码，记录发送失败并将状态置为失败/通知登录失败；测试断言非零返回可见、零返回仍等待异步认证结果。

## BUG-T25-010 — 登录请求同步发送失败后状态仍等待回调

- 状态: 本地 stub 定向复现；真实柜台行为未验证
- 严重级别: Major（同步拒绝后若无异步回报，登录状态可能停留等待）
- 位置: `src/TraderCTP/TraderCTP.cpp:432-447,725-729`。认证请求成功受理后，认证回报触发 `doLogin()`；`ReqUserLogin` 非零时只写日志并仍返回 0，没有回退登录状态。
- 相关测试: `T25TraderCTPTest.do_login_send_failure_is_logged_without_waiting_for_response`。先让认证请求返回 0，再将本地 stub 的登录发送返回设置为 5，模拟认证成功回报；定向 FAIL，状态仍为 `WS_LOGINING`。请求次数及错误日志断言未失败。
- 修复方向: 区分认证及登录请求的同步发送失败，为当前登录代次设置失败或可重试状态，并向上游提供失败通知；不依赖未成功发送请求的异步回报。明确回调与重试的去重方式。

## BUG-T25-011 — 委托和撤单同步发送失败仍报告成功

- 状态: 本地 stub 定向复现；真实柜台行为未验证
- 严重级别: Major（同步拒绝可能被上层当作已发送）
- 位置: `src/TraderCTP/TraderCTP.cpp:543-549,580-586`。`ReqOrderInsert` / `ReqOrderAction` 返回非零时只记录日志，方法仍返回 0；`TraderAdapter` 按负值判同步失败。
- 相关测试: `T25TraderCTPTest.order_insert_and_action_send_failures_return_error_and_are_logged` 使用有效 entrust ID 和本地 stub 返回 9；两次方法实际均返回 0，正确的负值断言 FAIL，调用和日志对照未失败。
- 修复方向: 将发送失败映射为上层可识别的负值并保留可诊断信息；协调预占、缓存及异步拒单回调，防止重复回滚或把未发送请求当成在途委托。

## BUG-T25-005 — 查询无终帧时 disconnect 可能永久等待

- 复核状态：`条件性生命周期缺陷；厂商无响应场景未在本轮动态复现`。
- 证据：worker 在 `TraderCTP.cpp:337-363` 将 `m_bInQuery` 设为 true 后，依赖查询回调的 `bIsLast` 清零；`OnFrontDisconnected` 只改 wrapper 状态（`:713-718`），不清查询状态。`disconnect()` 把 `release()` 排队后 `join()`（`:368-379`）；若 outstanding 查询既无末帧也无错误回调，队列里的 release 无法运行，join 没有超时。worker 每 1ms 睡眠轮询，不能描述为持续占满一个 CPU 核。`m_bInQuery` 在 worker 和回调线程间共享且不是原子量，另有数据竞争风险。
- 测试边界：`DISABLED_disconnect_hangs_when_no_response_clears_in_query` 用 stub 发出请求但不回调，能够检查该假设下的阻塞；本轮未运行。CTP 断线时是否保证补齐查询末帧/错误回调，尚无 SDK 契约证据。
- 修复方向：为查询状态提供互斥或原子同步和有限超时；断线/请求发送失败时按 request id 清理 outstanding 状态；disconnect 采用可中断等待并确保 API release 与 worker 收尾有序。新增有界测试，不以真实柜台无响应作为唯一覆盖方式。

## BUG-T25-006 — 未支持时间条件留下未初始化订单标志，现有测试 oracle 不成立

- 复核状态：`存在未初始化输出；GTD 等回报是否属于产品支持输入待确认；测试 oracle 已修正并 PASS`。
- 证据：`makeOrderInfo` 和 `makeEntrust`（`TraderCTP.cpp:1205-1215, 1281-1291`）只映射 GFD/IOC，没有给其他 `TimeCondition` 设值。`WTSEntrustStruct` 构造函数将 `m_orderFlag` 初始化为 `WOF_NOR`（`WTSTradeDef.hpp:84-93`），而 `_WTSOrderStruct` 构造函数没有初始化它（`:286-328`）。`ObjectPool::construct()` 执行 `new(mem) T()`（`ObjectPool.hpp:53-60`）；由于这里调用用户定义的默认构造函数，不能据此断言未初始化成员会被整块清零。
- 测试边界：现用例 `unsupported_time_condition_keeps_other_order_mappings`（测试 `:1735-1788`）不再读取 `_WTSOrderStruct::m_orderFlag`，只断言回调明确映射的价格类型、开平和方向；本地定向运行 PASS。GTD 是人工构造回报，仓库没有证据说明正常适配器会发送/收到该条件，因此 PASS 不证明该字段输出契约。
- 修复方向：先明确未知条件的输出契约；若回退为 `WOF_NOR`，初始化 `_WTSOrderStruct::m_orderFlag` 或在转换分支显式赋值，并把测试改为断言 `WOF_NOR`。若 GTD 不属于产品输入契约，删除“固定输出 0”的测试并改测支持的 GFD/IOC 映射；不要断言未初始化内存值。
- 后续测试处理（2026-10-29）：已移除 GTD 回报中对未初始化字段的读取与固定为 0 的断言，保留确定的方向、开平和价格类型映射；该用例及支持类型对照 PASS。未确定 GTD 的产品默认值契约。

## BUG-T25-007 — cache init 失败后仍允许写入

- 复核状态：`确认的失败路径缺陷；禁用崩溃用例本轮未重跑`。
- 证据：`OnRspUserLogin` 调用两个 `WtKVCache::init()` 却未处理返回值（`TraderCTP.cpp:758-781`）。映射失败时 `WtKVCache::init` 将 `_file` 清空并返回 false，而 `_block` 没有可用映射（`WtKVCache.hpp:135-142`）；之后 `put()` 在插入新键时直接读 `_cache._block->_size`（`:211-230`）。带 usertag 的 `orderInsert()` 会走 `put()`（`TraderCTP.cpp:498-500`）。
- 动态证据：`login_warns_and_continues_when_cache_dir_is_not_writable` 本轮 `PASS`，只证明登录路径继续运行且发出告警。`DISABLED_order_insert_after_failed_cache_init_crashes` 本轮未执行；旧记录中的 SIGSEGV 未重新验证。
- 修复方向：登录阶段处理 cache init 失败（阻止进入可交易状态或明确降级为不缓存），并在 `WtKVCache::put()` 对未初始化 block 安全失败；测试覆盖失败初始化后的带/不带 usertag 下单。

## BUG-T25-008 — 断线后迟到确认回调可将状态恢复为就绪

- 复核状态：`状态机缺陷候选；已修正的人工迟到回调测试 FAIL，真实回调时序待验证`。
- 证据：`OnFrontDisconnected` 设 `WS_NOTLOGIN`（`TraderCTP.cpp:713-718`）；`OnRspQrySettlementInfoConfirm` 不检查先前状态，只要非错误且确认日期满足条件就设 `WS_ALLREADY` 并报告登录成功（`:806-827`）。
- 测试边界：旧测试在断线后人工注入确认查询回报，却断言 `WS_ALLREADY`。现已改为断言仍未登录、未连接且成功登录计数不增加；重建后单例 FAIL，实际状态复活且成功计数增加。此为人工乱序回调复现，不证明真实 CTP 一定产生该时序。
- 修复方向：检查认证/登录/查询状态，或给连接和查询分配代次并丢弃旧代次回报；与 CTP teardown 回调约定一起验证。

## BUG-T25-009 — 结算查询/确认发送失败后状态没有恢复

- 复核状态：`确认错误处理缺口；永久等待是条件结论；查询发送失败子项与 T25-005 重叠`。
- 证据：`confirm()` 对同步发送失败会记日志并返回 -1（`TraderCTP.cpp:1450-1473`），但 `OnRspQrySettlementInfoConfirm` 的两个调用点丢弃返回值（`:833, 839`），状态保留 `WS_CONFIRM_QRYED`。`queryConfirm()` 的 worker lambda 对请求返回码只记日志（`:1422-1447`）；worker 在调用 lambda 前已经置 `m_bInQuery=true`，若同步失败且之后无回调，阻塞机制与 T25-005 相同。
- 动态证据：旧 `confirm_send_failure_logs_error_and_state_stays_confirm_qryed` 将停滞状态当作预期，且人工补入成功回调；现改为 `DISABLED_confirm_send_failure_outcome_pending_contract`，删除该现状式断言和伪造回调，待明确重试/失败通知合同。`query_confirm_send_failure_is_logged_inside_worker_item` 是独立 worker stub 观察；不能由这些测试证明真实系统会或不会继续回报。
- 去重与修复方向：T25-009 保留“确认发送返回码被调用方吞掉、登录结果不终结”问题；查询发送失败导致 worker 卡住的部分并入 T25-005。确认发送失败应回滚/重试或明确报告登录失败，不能只依赖假设中的后续成功回调。

## BUG-T25-010 — disconnect 对查询队列无锁 push

- 状态: 已合并
- 复核状态：`已合并（主编号 T2-CTP-03）`。
- 证据：`disconnect()` 的 `m_queQuery.push()`（`TraderCTP.cpp:368-372`）无锁；worker 的 `empty/front` 读取也无锁（`:339-355`），其他查询入队点使用 `m_mtxQuery`。与 T2-CTP-03 已记录的 CTP 队列线程安全/生命周期问题重叠；竞态本轮未动态复现，`disconnect_without_worker_only_queues_release` 不覆盖并发读写。
- 修复方向：队列所有读写统一受同一锁保护，或用条件变量队列；不要只给 push 加锁却保留 worker 的锁外 `empty/front`。

## 汇总

| 处置 | 编号 | 结论 |
| --- | --- | --- |
| 有条件的源码缺陷 | T25-002、004、005、007、009 | 失败输入/无响应/初始化失败前提见各条；T25-005/009 的无响应后果尚未以本轮动态测试复现 |
| 条件风险或输入契约待证 | T25-003、006、008 | 需确认 CTP 回调空载荷、产品支持的时间条件以及断线后回调顺序 |
| 合并/去重 | T25-001→T2-CTP-01/02；T25-010→T2-CTP-03；T25-009 查询失败分支→T25-005 | 避免同一成员初始化/队列问题重复计数 |
| 已修正测试的定向结果 | T25-006、T25-008 | T25-006 映射用例 PASS；T25-008 按断线后不得复活的契约断言 FAIL。迟到回调由测试人工注入，未证明真实 CTP 会产生该时序 |
