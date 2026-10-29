# 05_replay：第二轮逐条复核

本组复核 6 项。最终分类：**PRODUCT 3，TEST 2，NOT_BUG 1**。每项重新追了当前源代码、公开接口、正常调用顺序和测试前提；没有沿用第一轮的未决结论。

## 定向运行

主代理刷新 CoverageTests 目标后，本组在隔离目录各启动一个测试进程；每次使用独立 cwd、LLVM_PROFILE_FILE 和 GTest XML，超时上限 15 秒。没有构建，也没有运行 DISABLED 测试。

| 用例 | 结果 | 可执行文件 SHA-256 |
| --- | --- | --- |
| `P04ReplayerConfigTest.initWithoutBasefilesReportsFailure` | 1 test，1 failure，rc=1。子进程进入 `init` 后因 signal 11 退出；测试期望安全返回失败，因此该红测确认产品崩溃。 | `430676e26c035f16dc733f23f4e72196a86ed5d4373f3b86ebce7d554be370d4` |
| `P04ReplayerConfigTest.prepareAdvancesDateAndTradingDate` | 1/1 PASS，rc=0 | `430676e26c035f16dc733f23f4e72196a86ed5d4373f3b86ebce7d554be370d4` |
| `R43ReplayerInitBasefilesTest.badAdjFactorFileDoesNotBreakInit` | 1/1 PASS，rc=0；证明坏 adjfactor 的可选降级仍成立，不外推为所有 basefiles 都可忽略。 | `430676e26c035f16dc733f23f4e72196a86ed5d4373f3b86ebce7d554be370d4` |
| `R41EngineFixture.R41StandbySignalFiresOnNextTick` | 1/1 PASS，rc=0；只覆盖有效 session 控制路径。 | `779ecd25f633ec0b11f2cf44f0afc9f2c4edcf438b9851f8ac00d418940c30ce` |

原始 stdout、GTest XML、return code、准确测试数、命令、cwd、profile 路径和二进制 SHA 均保存在 `docs/迭代计划/迭代02/复核185/evidence/05_replay/run_01..04/`。R58-003 的缺失 session 反例、R58-004 的缺失 session 回调和 R80 的 DISABLED 用例本轮没有运行。

## 逐项结论

### BUG-R171-003 — NOT_BUG

`SelMocker::proc_tick` 有非零 `_desprice` 分支，但策略公开接口 `ISelStraCtx::stra_set_position(stdCode, qty, userTag)` 不接收价格。`SelMocker::append_signal` 在 private 区域；公开实现以及 `autoexit` 调用均未传价格，`_desprice` 保持默认 0。因此通过支持的策略 API 不可达目标价分支，不能认定公开目标价功能故障。若产品以后需要目标价信号，应另增有文档的公开 API 和端到端测试。

### BUG-R58-003 — PRODUCT

`HisDataReplayer::init` 对配置中的 session/commodity 文件调用 loader，却丢弃失败结果，最后仍 `return true`。`loadSessions` 和 `loadCommodities` 对文件缺失或解析失败会返回 false。该问题限定为显式配置的 session/commodity 依赖加载失败未传到初始化结果；不把坏 adjfactor 纳入必需文件，已运行的 adjfactor 对照 PASS。

建议在 `r43_replayer_init_basefiles_test.cpp` 新增完整配置、唯独 session 文件路径不存在的用例，期望 `init == false`；保留当前 adjfactor 对照。这个测试请求已发主代理，等待统一安排。

### BUG-R58-004 — PRODUCT（静态确认）

本轮补出了先前未确认的可达链：引擎从 `datas.json` 重载 positions 时不按当前基础数据筛选；`WtCtaEngine::on_schedule` 明确处理旧/过期持仓，并对非零旧仓位排入目标 0 standby signal。后续行情经 `WtCtaTicker::trigger_price` 调用 `WtCtaEngine::on_tick`。当前基础数据缺少该品种或有效 session 时，`get_session_info(code, true)` 可返回空，而 `WtEngine::on_tick` 对其直接调用 `isInTradingTime`。这是一条由正常持久化和调度路径形成的产品风险，不是只在测试中手工传空指针。

本轮仅运行有效 session 的 R41 standby 对照，未动态复现旧 portfolio 路径。已向主代理建议在 `r41_engine_base_test.cpp` 加“重载缺 session 的过期持仓、调度归零、同代码 tick 不崩溃”的用例，并保留有效 session 正向控制。该空 session 信号族与 `BUG-P01-001` 有交集，但这里额外确认了 `WtEngine::_sig_map` 和 portfolio 重载路径；主代理需跨组统一根因表述。

### BUG-R80-001 — TEST

`DISABLED_DailyTaskClosesCurrentDayBars` 将 `run_by_bars` 的当日收盘行为当成 daily-task 模式的隐含保证。`run_by_tasks` 对 15:00 任务先算出 14:59，随后在该前一分钟推进并发出任务回调；运行态 ticker 也在分钟闭合后调度，task API 分别传当前时间和 fireTime。session-end 另由实际 close minute 决定。当前断言不能证明产品违反公开调度约定。

建议保持该用例禁用，或改为分别验证回调时点和 session-end 后的日线推进。测试修改建议已发主代理；本轮没有运行 DISABLED 测试。

### BUG-P04-001 — PRODUCT（本轮 LOCAL_REPRO）

公开 `init` 收到缺少 `basefiles` 的配置时，在 `cfgBF->get("session")` 处解引用空指针。本轮执行 `initWithoutBasefilesReportsFailure`，子进程日志到达 `P04_INIT_BEGIN` 后以 SIGSEGV 退出，用例原本期待函数返回 false。结果是这条测试正确捕获产品崩溃，而非测试误报。有效配置的 prepare 对照 PASS。

修复点是 `HisDataReplayer::init` 读取任何 basefiles 成员前校验该配置节点；缺少节点时记错并返回 false。实现修复后重跑负例与有效 prepare 对照。

### BUG-P04-002 — TEST

`prepare()` 调用 listener 的 `handle_init()`，而 sink 用于接收回放事件。检查到的 `WtBtRunner` 和 `WtBtPorter` 正常装配链都在 `prepare()` 前注册 sink；现有成功 prepare 测试也遵循该顺序。候选的 no-sink 调用跳过了生命周期步骤，不能据此认定有效回放流程存在产品故障。若以后要求忘记注册 sink 时可恢复失败，可另做 API 防御改进，不应保留当前产品 bug 主张。

## 后续本地动作

- 主代理安排 R58-003 的缺失 session 测试，并在错误传播修复后验证失败返回与 adjfactor 可选对照。
- 主代理安排 R58-004 的过期 portfolio signal 测试，并在空 session 防护后验证不崩溃和有效 session 正向行为。
- P04-001 代码修复后重跑现有缺配置用例和有效 prepare 对照。
- R80 用例继续保持 DISABLED，直到测试 oracle 按 task fireTime 与 session-end 分开表达。

本报告和 triage 仅新增在本组授权目录；本组没有修改源码、测试、构建文件、旧报告或主总表。
