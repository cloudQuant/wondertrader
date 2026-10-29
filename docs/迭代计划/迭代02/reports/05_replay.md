# 05_replay 输入审计

## 范围与方法

逐条复核本组 37 条候选：对照原 bugs 记录，检查当前实现、公开接口/配置约定以及对应测试的输入前提、启用状态和实际断言。审计为静态阅读；本轮未运行测试、构建、ASan 或网络检查，因此所有 JSON 条目均标记 STATIC，没有把旧记录中的 FAIL/CRASH 当作本轮复现。

逐条的代码/接口与测试行号、分类依据、修复和验收建议见 [triage/05_replay.json](../triage/05_replay.json)。本报告的分类是进入迭代的审计裁定；UNDECIDED 必须先补契约或有效输入证据，不能当作已确认产品缺陷。

## 分类数量

| 分类 | 数量 | 含义 |
|---|---:|---|
| PRODUCT | 24 | 当前代码与有效输入/公开契约之间存在可静态确认的问题；运行时影响仍需按验收项补证。 |
| TEST | 1 | 原测试复现前提不成立；应修 fixture 或 oracle，不据此登记产品复现。 |
| DUPLICATE | 2 | 并入已存在 canonical 项，不新增修复任务。 |
| UNDECIDED | 6 | 缺少输入有效性、配置 schema、生命周期或日期语义等契约证据。 |
| NOT_BUG | 4 | 原问题主张不成立或没有展示公开行为错误。 |
| **合计** | **37** | |

## 逐项裁定

### PRODUCT（24）

| ID | 静态裁定 |
|---|---|
| BUG-R103-001 | TraderMocker::handle_read 收到零字节后早退，不再挂起 UDP receive。现有空包后账户查询不是 tick 接收断言；应追加合法 tick 并验证仍可接收。 |
| BUG-R103-002 | get_last_tick 首次定位落在末项之后时可访问 _items[size]。现有关联分钟 slice 测试不请求 tick，需直接测空/首前/命中/尾后边界。 |
| BUG-R190-001 | FuncDeleteExeFact typedef 与插件导出删除函数的指针形参不一致，函数类型/ABI 静态不匹配成立。R111 的 signal 11 没有栈帧，不能作为崩溃归因证据。 |
| BUG-R215-004 | RT bar 追加前先把 count 截到缓存数量，导致符合条件的实时 bar 被截掉。旧 count=10 主断言被禁用；应恢复独立稳定 fixture。 |
| R2_wtbtcore#2 | getAdjFactors 用共享 char[20] 承载无界格式化键，有长度越界及并发写竞态风险；现有测试只用短键。 |
| BUG-R43-001 | _emit_times==0 时无条件整数除法存在风险，且 replay completion 会调用该处理器。旧用例禁用，本轮没有观察到 SIGFPE；需用无调度回放安全完成验收。 |
| BUG-R43-002 | 仅 bid 的盘口会使 ask 迭代器为 end() 后仍被解引用。旧测试禁用且无断言；买量字段错误需另用可观察盘口状态验收。 |
| BUG-R70-001 | RT 首项之前查询时，queue/detail/trans 的边界回退均可能在 begin 上自减并构造非法范围。现有用例禁用且只有 queue、无结果断言。 |
| BUG-R91-001 | detail/queue 缺数据时错误写入 transaction cache，导致同日后续 transaction 查询被缓存命中短路。缺单 fixture 测试禁用；应独立验证 trans 存在、order 缺失。 |
| BUG-R91-002 | bin K 线失败留下空列表，自动主周期选择不排除空列表，可能遮蔽有效日线并进入空主 bars 路径。现测试未覆盖自动 run 选主周期。 |
| BUG-R91-003 | detail/queue/transaction 三种 slice 在末项之后或间隙时存在越界区间风险，公开 etime 右界为 inclusive。当前测试只检查回调次数，需读取真实返回记录。 |
| BUG-P04-004 | header-only CSV 产生空 bars 后仍读取首尾下标；需在首尾访问前处理空序列。 |
| BUG-P04-006 | 有序查询区间完全早于首 bar 时可能在 begin() 上自减；当前反序 begin/end 用例没有覆盖该前提。 |
| BUG-P04-009 | 缺失主 CSV 时先登记 _main_key，失败返回后 run() 仍进入主 bars 路径。现有测试用 isMain=false，没覆盖崩溃风险。 |
| BUG-P04-010 | 起始日在首个日 bar 收盘前时游标定位跳过首根 bar；当前测试 oracle 已修正为全部 bars，但未在本轮运行。 |
| BUG-P04-011 | 公开头文件声明 14 位时间和 HHMMSS，实现按 12 位时间解析并返回 HHMM；需 API owner 明确兼容策略并统一代码与文档。 |
| BUG-P07-001 | one-based HFT 游标在 cursor>=count 时把最后一项当成耗尽；tick 尾项测试已有正确预期，Level2 尾项仍应独立覆盖。 |
| BUG-P07-002 | 日内 gap 跳过条件在游标有效时不会推进，午间休市 tick 可能被作为下一事件返回；现有测试仅有 session 内 tick。 |
| BUG-P07-003 | 月频公开 day 为 1..31，代码却拿 YYMMDD 整值与 day 比较；月频逻辑需按日分量和明确的短月/假日规则实现。 |
| BUG-P07-004 | "f" slice 映射到 HOUR，而周期分组只为 HALF 处理 "f"；与 Q02-005 同源，R171 记录是关联项。 |
| BUG-P07-005 | 回调期间 cursor 更新与 get_last_tick 的 cursor-1 定位不一致，get_day_price 复用该 getter，可能返回非当前回调 tick；测试应逐回调比字段。 |
| BUG-P07-008 | 年度任务仅比较 MMDD，跨年时 12 月 31 已大于 01 月 01；遇新年假日可跳过目标触发日。 |
| BUG-P07-006 | tick 回放只按日期停止，逐事件循环未检查精确 _end_time；旧测试的 end 晚于所有 fixture tick，未覆盖截止越界。 |
| BUG-P07-007 | 订阅但无 transaction 数据时空列表有 count=0、无效 cursor，播放循环可能访问 _items[cursor-1]。旧禁用测试只订 tick 且没有交易数据，hang 主张不成立；本项产品风险是空交易列表访问。 |

### TEST（1）

| ID | 裁定与测试修正 |
|---|---|
| BUG-R58-002 | 旧 rc=139 的 fixture 根路径少一级，session 数据未装载，不能证明有效输入触发空 session；当前测试已加载有效 session。保留有效会话控制；如需测缺 session，使用符合配置结构的负向 fixture 并验证 init 的明确拒绝策略。源码空指针仍是未决风险，不等于旧测试复现成立。 |

### DUPLICATE（2）

| ID | canonical | 裁定 |
|---|---|---|
| BUG-R215-005 | BUG-R91-003 | 同一组 detail/queue/transaction slice 的 lower_bound 末端边界和 inclusive etime 处理；不新增实现任务，把缺少的 detail/trans 记录断言补入 canonical 回归。 |
| BUG-P04-008 | BUG-P04-010 | 起始首日定位设置 cursor=eIdx+1 的同一规则和边界；单 bar 可变成越界，多 bar 会跳过首 bar。并入 P04-010 的运行路径测试。 |

### UNDECIDED（6）

| ID | 尚缺证据/契约 |
|---|---|
| BUG-R171-003 | 私有目标价参数无公开非零入口；需 API owner 确认目标价信号是否为公开支持行为。 |
| BUG-R58-003 | init 忽略 loader 失败可由源码确认，但尚无每类 base file 的必需/可选清单；需配置 schema 和降级契约。 |
| BUG-R58-004 | WtEngine::on_tick 待触发 signal 路径可空指针解引用；需证明公开接受的 instrument 可合法缺 session，并确认 lifecycle 策略。 |
| BUG-R80-001 | daily task 与逐 bar 路径 endTDate 口径不同；需明确 task 回调日期是 T-1 还是当前日视图。 |
| BUG-P04-001 | 缺少 basefiles 段会触发空指针风险；需确认该配置段是否必需，之后才能确定拒绝还是默认初始化。 |
| BUG-P04-002 | prepare() 依赖 listener，但 API 未明确 register_sink 前置条件；旧测试调用顺序未满足该条件，需先定 lifecycle 契约。 |

### NOT_BUG（4）

| ID | 裁定 |
|---|---|
| BUG-R171-004 | _ticks 未写入不证明 getter 返回错误；getter 退回 replayer 结果。只有具备性能契约后，才有独立缓存 benchmark 需求。 |
| R2_wtbtcore#4 | 第二次 proc_tick 位于 tick-updated 回调后，供回调中新建 signal 使用；第一次已消费的 signal 不因此重复成交。 |
| BUG-P04-003 | 零 end time 会解析为当前日，日循环有日期边界；旧“无界挂死”测试禁用且无 timeout，结论不成立。零值语义仍可另行文档化。 |
| BUG-P04-007 | 默认 session TRADING 存在于当前样例 sessions.json；未知自定义 session 是另一类配置校验问题。 |

## 待修正的测试主张

- src/TestUnits/coverage/p07_btcore_replayer2/p07_task_schedule_test.cpp:191-193 周频 day 合法范围应为 **0..6**（接口见 src/WtBtCore/HisDataReplayer.h:355-363,810-820），测试应改断言 day >= 0 && day <= 6。定向 filter：P07TaskScheduleTest.weeklyTaskFiresOnlyOnTargetWeekday。本轮未运行。
- BUG-R58-002 的旧 fixture 应修正 session 数据根目录；当前重建测试位于 src/TestUnits/coverage/n04_btcore_mockers/r43_cta_mocker_hook_test.cpp:76-96,110-135，使用有效会话 SSE.STK.000001。若覆盖 missing-session，需另加符合配置 schema 的无效输入并先定接口预期。
- BUG-P04-006 旧测试反转 begin/end，应新增 ordered before-first 区间；BUG-R91-002 需覆盖自动 run 选主 bars；BUG-P07-006 需将 end 放在同日 fixture 中间；BUG-P07-007 必须订阅 transaction 并构造空 transaction 列表。这些修正只作为建议，本组没有改动测试文件。

## 未验证项

没有执行任何测试/构建/ASan，因此所有产品项都只是静态证据；产品影响、具体崩溃/越界行为和新验收断言仍待主代理统一运行。测试误报裁定仅针对候选所给的旧复现主张，不能反向证明相关实现没有其他缺陷。所有输出保持在本组允许新增的 triage JSON 与本报告中。
