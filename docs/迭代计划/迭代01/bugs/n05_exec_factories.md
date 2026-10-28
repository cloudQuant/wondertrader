# 缺陷记录 — n05_exec_factories（WtExeFact / WtExecMon / WtRiskMonFact）

## BUG-N05-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.h、WtSimpDataMgr.cpp
- 相关测试: src/TestUnits/coverage/n05_exec_factories/n05_riskmon_datamgr_test.cpp `DISABLED_n05_simpdatamgr.uninitialized_push_quote_crashes_and_getters_read_uninit`
- 输入/前置条件: 直接构造 `WtSimpDataMgr`（未调用 `initStore`/`init`），读取 `get_date()/get_min_time()/get_secs()/get_raw_time()/get_trading_day()`
- 期望行为: 构造函数应把 `_cur_date/_cur_min_time/_cur_secs/_cur_raw_time/_cur_act_time/_cur_tdate` 初始化为 0（或 getter 在未初始化时有定义值）
- 实际行为: 构造函数初始化列表只初始化了 `_reader/_runner/_bars_cache/_rt_tick_map`，其余 `_cur_*` 标量成员未初始化，读取值为不确定内存（ASan/MSan 下测试不可断言）
- 复现步骤: 见 DISABLED 用例第一段
- 影响范围: WtExecMon 实盘监控时间推进；未初始化读取属 UB
- 备注: 阻塞该类 getter 的可断言覆盖；建议修复方向为成员就地初始化 `= 0`（不实施）。

## BUG-N05-002

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: WtExecMon / src/WtExecMon/WtSimpDataMgr.cpp `WtSimpDataMgr::handle_push_quote`（L130-162）
- 相关测试: 同上 DISABLED 用例第二段
- 输入/前置条件: 未调用 `init`（`_s_info` 为空）即推送任意非空 tick
- 期望行为: 对 `_s_info` 判空并安全返回，或明确契约禁止 init 前推送
- 实际行为: 无条件解引用 `_s_info->timeToMinutes(...)` / `isLastOfSection(...)` / `minuteToTime(...)`，空指针段错误崩溃
- 复现步骤: 见 DISABLED 用例；同类输入（任何未 init 场景的 tick 推送）全部归入该同一 DISABLED 用例，不允许有启用态同类输入
- 影响范围: WtExecMon 行情回调线程可能在启动早期竞态中崩溃
- 备注: 崩溃类，测试保持 DISABLED；建议修复方向为入口判空（不实施）。

## BUG-N05-003

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtExeFact / src/WtExeFact/WtOrdMon.cpp `WtOrdMon::check_orders`（L57 附近）
- 相关测试: src/TestUnits/coverage/n05_exec_factories/n05_ordmon_test.cpp（现有单线程用例不受影响）
- 输入/前置条件: 线程 A 执行 `check_orders`，线程 B 同时 `erase_order`
- 期望行为: 空判断也应在锁内完成
- 实际行为: `if (_orders.empty()) return;` 在加锁前读取共享状态，存在数据竞争（TOCTOU，良性但违反自身锁纪律；`enumOrder` 同型）
- 复现步骤: TSan 并发压测可复现竞争报告
- 影响范围: 执行单元 on_tick 撤单扫描线程安全
- 备注: 不阻塞覆盖率；建议修复方向为把判空移入锁内（不实施）。
