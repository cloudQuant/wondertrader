# 缺陷记录复核 — n05_exec_factories（WtExeFact / WtExecMon）

> 复核范围：对照当前实现、`IDataReaderSink`/数据管理接口与本 lane 测试源码。`DISABLED_` 用例未执行；本记录不把静态推断写成动态复现，也未运行 TSan。

## BUG-N05-001 — WtSimpDataMgr 时钟字段未初始化

- 状态: 静态问题确认；动态未验证
- 严重级别: Major（首次行情前读取时钟字段的影响仍需调用链证据）
- 模块/文件: `src/WtExecMon/WtSimpDataMgr.cpp` 构造函数；`src/WtExecMon/WtSimpDataMgr.h` 的 `_cur_*` 字段
- 接口依据: `IDataReaderSink` 将 `get_date/get_min_time/get_secs` 定义为当前时间读取接口，但没有规定首个 tick 前的默认值。
- 源码事实: 构造函数只初始化 `_reader/_runner/_bars_cache/_rt_tick_map`；`_cur_date/_cur_act_time/_cur_raw_time/_cur_min_time/_cur_secs/_cur_tdate` 未初始化。对应 getter 直接返回这些字段。
- 测试事实: `n05_riskmon_datamgr_test.cpp` 中的 `DISABLED_n05_simpdatamgr.uninitialized_push_quote_crashes_and_getters_read_uninit` 只读取 `get_date()`，且整个用例被禁用；没有读取其余 getter 的断言。故该测试不是运行证据，也没有覆盖所有字段。
- 输入/前置条件: 构造后、首个有效行情写入前读取上述 getter。
- 结论: 该路径读取未初始化标量，属于静态可确认的未定义/不确定值风险；尚未找到实际 reader 在此生命周期读取 getter 的调用证据，不能据此声称正常运行链路已复现。
- 修复方向: 为所有时钟字段提供一致的零值或显式“尚无行情”状态，并约定 getter 在首个行情前的语义；增加启用态边界测试。

## BUG-N05-002 — WtSimpDataMgr 缺少有效 session 时仍接受行情

- 状态: 已合并（迭代02：主编号 BUG-R17-004）
- 严重级别: Major（依赖缺失/错误 session 配置）
- 模块/文件: `src/WtExecMon/WtSimpDataMgr.cpp` `initStore`、`handle_push_quote`
- 输入/前置条件: 初始化时 `store.session` 未能由 runner 的基础数据管理器解析，或在未成功初始化的对象上直接调用 `handle_push_quote`。
- 源码事实: `initStore` 将 `_runner->get_session_info(..., false)` 直接赋给 `_s_info`，未检查返回值，仍返回 `true`；`WtExecRunner::get_session_info` 对缺失 session 可返回 `NULL`。后续 `handle_push_quote` 对非空 tick 直接调用 `_s_info->timeToMinutes/isLastOfSection/minuteToTime`。构造函数也未初始化 `_s_info`。
- 测试事实: 同一个禁用用例在未初始化对象上推 tick。此时 `_s_info` 是未初始化指针，不是已证实的空指针；对它解引用属于未定义行为，不能据此声称“稳定段错误”或确定性 NULL 崩溃。该用例未执行。
- 影响边界: 若初始化成功标志掩盖了无效 session，随后行情回调会走到无效 `_s_info`；启动早期竞态和生产可达性没有动态证据。
- 修复方向: 初始化阶段验证 session 并在失败时返回失败；对未完成初始化的行情入口 fail closed。若生命周期契约禁止提前推送，应在调用边界明确并验证该契约。


- 迭代02复核: DUPLICATE。与 BUG-R17-004 同为 session 信息缺失后行情处理解引用 _s_info；R17-004 已包含 runner 可返回 NULL、初始化返回成功和行情入口无判空的完整链路，本条不重复计数。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 静态问题确认；动态未验证
## BUG-N05-003 — WtOrdMon 对订单表的并发访问未统一加锁

- 状态: 静态锁审计确认；并发/TSan 未验证
- 严重级别: Major（仅当这些入口实际并发时构成数据竞争）
- 模块/文件: `src/WtExeFact/WtOrdMon.cpp` `check_orders/enumOrder`；`src/WtExeFact/WtOrdMon.h` `has_order/clear_orders`
- 源码事实: `push_order/erase_order/check_orders/enumOrder` 使用 `_mtx_ords`，但 `check_orders` 与 `enumOrder` 在取得锁前读取 `_orders.empty()`；`has_order` 对 `empty/find` 均不加锁，`clear_orders` 也直接清表。因而这些读写入口若并发，仍会访问同一 `unordered_map` 而形成数据竞争。执行单元中确有 `check_orders`、订单回调中的 `erase_order` 及 `has_order/clear_orders` 调用点；线程是否在运行时重叠，当前没有 TSan 或时序证据。
- 测试事实: `n05_ordmon_test.cpp` 只有单线程语义用例，没有并发压力或 TSan 结果。原记录中的“TSan 可复现”没有可核对命令/报告，已降为未验证。
- 修复方向: 对 `_orders` 的每次读写统一使用同一互斥锁；将空判断放入锁内，并审视回调在锁内执行的重入语义。另加并发测试后再报告 TSan 结论。
