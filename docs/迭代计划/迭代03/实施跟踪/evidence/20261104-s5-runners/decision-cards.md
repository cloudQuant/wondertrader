# s5-runners 决策卡草稿（待用户裁定；不改变任何条目状态）

## 草稿 R13-A：SEL 调度时间/周期/日期编码（BUG-R13-004）

**要决定什么**：`create_sel_context` / 配置 `strategies.sel` 的 `time`、`period`、`date` 合同。

**现状证据（静态）**：
- 执行侧一律按 HHMM：`WtSelEngine::on_minute_end` 以 `tInfo->_time != nextTime` 比较（`nextTime=TimeUtils::getNextMinute(curTime)`，HHMM，`src/WtCore/WtSelEngine.cpp:253`）；回测 `HisDataReplayer` 以 `getNextMinute(_task->_time,-1)` 推算（HHMM）。
- 文档写 HHMMSS 与 `d1/w1/m1`：`src/WtPorter/WtPorter.h`（本轮已改为说明实际执行口径、标注未定）、`src/WtBtPorter/WtBtPorter.h:177-179`（不属本包，未改）。
- 解析侧（WtRunner、WtRtRunner、HisDataReplayer）只认 `d/w/m/y/min`。
- 月任务 `date`：回测按日（`_cur_date % 100 == _day`，1..31）；实盘按 `_cur_date % 1000000 == _day`（YYMMDD，与 1..31 永不相等，`WtSelEngine.cpp:297`），两侧不一致。
- `min` 周期：实盘先要求 `_time == nextTime`（HHMM）再按 `minutes % _time` 判断，二者语义冲突（`WtSelEngine.cpp:253/288`）；回测 `min` 走独立分支，`_time` 为间隔分钟数。
- q06_porter 现有大量用例以 HHMMSS（93000/153000/145500）创建上下文，仅为取得句柄；HHMMSS 值在实盘永不触发。

**选项**：
- A（以执行语义为准）：`time`=HHMM（`min` 周期为 1..1440 的间隔分钟），入口拒绝不可能命中的值（>2359 或分钟>59）；两份 porter 文档改为 HHMM；`date`：w=0..6（`getWeekDay` 口径）、m=1..31（实盘改为 `%100` 与回测一致）、y=MMDD。
- B（以文档为准）：入口接受 HHMMSS，秒为 0 时 /100 转 HHMM，否则拒绝；旧 HHMM 调用方（WtRunner 配置、回测）需迁移。
- C（兼容猜测）：>2359 视为 HHMMSS，≤2359 视为 HHMM；00:00:00–00:23:59 的 HHMMSS 与 HHMM 有歧义，不推荐。
- 别名：a 不支持 d1/w1/m1（入口返回 0）；b 支持 d1/w1/y1，m1 因“月/分钟”歧义拒绝；c 支持 m1=月。

**建议**：A + 别名 a。理由：所有执行路径都按 HHMM，当前任何 >2359 的值都不会触发，拒绝它只把“静默无效句柄”变为显式失败，不破坏可工作的调度。

**影响条目**：BUG-R13-004；需 engine-lifecycle 修 `WtSelEngine`（见 README“需他包改动”）；WtBtPorter 文档；q06 现有以 HHMMSS 取句柄的用例需随裁定改为 HHMM。

## 草稿 E/NEW-123：initExt 外部 parser 的停止通知与 release 回调期跨线程语义

**要决定什么**：(1) runner 释放时是否、何时向 `create_ext_parser` 建立的外部（Python）parser 发停止通知（`EVENT_PARSER_RELEASE`）；(2) 该通知能否作为回调排空证明；(3) teardown 正在执行外部回调时，另一线程调用 `release_porter()` 应等待还是立即返回（NEW-123 等待环）。

**现状（静态+z15 运行）**：卡9（9a=A+C）后 `ParserAdapter::requestStopLifecycle` 对无 V2 lifecycle 的 initExt API 返回 false（`src/WtCore/ParserAdapter.cpp:507`），`release()` 不再调用 `api->release()`，`ExpParser::release` 不执行，外部 parser 收不到 `EVENT_PARSER_RELEASE`；`ExecUnitStatusApiV2Fixture.ParserReleaseCallbackCannotReenterRunnerTeardown`、`...CanJoinChildReleaseWithoutDeadlock` 前提失效（z15 `runs/new123_*`）。若恢复通知，后一例的回调会 join 一个调用 `release_porter()` 的子线程，而该子线程在 teardown 进行中按 `ConcurrentReleaseCallsShareOneTeardown` 的合同等待 RELEASED，形成等待环（NEW-123 原始现象）。

**选项**：
- 通知：A 只通知、不作排空证明——teardown 在控制线程、不持锁时对每个 initExt parser 调一次 `api->release()`（产生 `EVENT_PARSER_RELEASE`），之后该 adapter 仍按卡9 隔离到进程退出，迟到推送由准入拒绝；B 版本化确认——新增外部 parser 生命周期导出（如 `parser_release_ack(id)`），外部确认排空后才视为可释放；C 维持现状，文档要求调用方在 `release_porter()` 前自行停止外部 parser，两例旧测试改写为反映“无通知”的合同。
- 跨线程：a 当 teardown 正在执行外部回调时，并发 `release()` 立即返回（记录“关闭进行中”），其余阶段保持等待；b 始终等待，文档禁止回调中 join 调用 release 的线程，旧测试改写为反例。

**建议**：A（+B 作为后续可证明排空的升级路径）与 a。理由：外部 parser 需要停止信号才会退出其推送线程；不把通知当作排空证明，与卡9 9a 一致。

**影响条目**：BUG-I02-NEW-123、z15 发现 E、`ExecUnitStatusApiV2Fixture.ParserRelease*` 两例；实现涉及 `WtCore/ParserAdapter`（不属本包）与 `WtRtRunner::completeRelease`（本包）。
