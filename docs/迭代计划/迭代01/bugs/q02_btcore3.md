# Q02 WtBtCore 第五波缺陷登记（q02_btcore3）

- Lane: q02_btcore3（迭代 01 · 第五波）
- 范围: `src/WtBtCore/CtaMocker.cpp/.h`、`src/WtBtCore/SelMocker.cpp`、`src/WtBtCore/HisDataReplayer.cpp`、四个 `init_*_factory`
- 说明: 本波未修改任何生产代码；以下条目均由「实现/注释/约定」推导得出，
  对应可复现代码位置见每条的「证据」。与既有编号同源的（BUG-P03-001/002、BUG-P04-007/010、
  BUG-P07-007、BUG-P08-001/002/006、BUG-L00-005）只引用不重复立案。

---

## BUG-Q02-001 · CtaMocker 落地的 `tdate`/`settime` 取自未初始化成员

- 状态: 待审核
- 严重级别: 中（非确定性输出 + MSan/valgrind 报未初始化读）
- 位置: `src/WtBtCore/CtaMocker.cpp:146`（`jFund.AddMember("tdate", _cur_tdate, ...)`）、
  `src/WtBtCore/CtaMocker.cpp:202`（`jCond.AddMember("settime", _last_cond_min, ...)`）
  成员声明 `src/WtBtCore/CtaMocker.h:1037`、`src/WtBtCore/CtaMocker.h:1039`
- 证据: `CtaMocker::CtaMocker` 的初始化列表（`CtaMocker.cpp:60-79`）不含 `_cur_tdate`、
  `_cur_bartime`、`_last_cond_min`，构造函数体（L79-81）只赋 `_context_id`。
  因此在 `on_session_begin` 之前调用 `dump_stradata()`（经 `handle_replay_done`）
  会把栈上未初始化的值写进策略 dump JSON。
- 期望: 三者应在构造时初始化为 0，或 `dump_stradata` 不写出未定义的字段。
- 影响: 增量回测的 `<name>.json` 内容不可复现；同一输入两次回测产物 diff 非空。
- 测试处理: `q02_cta_dump_incremental_test.cpp` 只断言 `conditions.settime`/`fund.tdate`
  的**键存在**，不断言其值（避免把随机数固化成期望值）。
- 修复后需同步收紧: `strategyDumpRoundTripsIntoSecondMocker` 可加 `tdate` 值断言。

## BUG-Q02-002 · SelMocker::handle_replay_done 在零调度时整数除零

- 状态: 待审核
- 严重级别: 高（x86 上 SIGFPE 直接终止回测进程；arm64 靠 UDIV 不陷入侥幸返回 0）
- 位置: `src/WtBtCore/SelMocker.cpp:305-307`
- 证据:
  ```
  "Strategy has been scheduled for {} times,...,average of {} microsecs",
  _emit_times, _total_calc_time, _total_calc_time / _emit_times);
  ```
  `_emit_times` 构造为 0（`src/WtBtCore/SelMocker.cpp:43`），唯一自增点是
  `on_schedule`（`SelMocker.cpp:507`）。当回放没有产生任何调度（例如选股模式下
  底座无该周期数据、或 `prepare` 后立即 `stop`），`handle_replay_done` 必然除零。
  同一函数在 `CtaMocker.cpp:665` 有 `if(_emit_times > 0)` 保护，SelMocker 缺失该保护。
- 期望: 与 CtaMocker 一致的零值分支。
- 测试处理: 该入口（也是 SelMocker 唯一能触达 private `dump_outputs/dump_stradata` 的公开路径）
  整体收进 `q02_mocker_factory_cfg_test.cpp` 的 `DISABLED_RealModuleLoadAndSelDivZeroFamily`，
  启用集合零残留。**待裁定**：若主控认为 arm64 可接受，可在修复前把 SelMocker 的
  `dump_stradata`/`dump_outputs`（SelMocker.cpp:59-213，约 150 行）纳入启用用例。

## BUG-Q02-003 · CtaMocker 的 dump/load 字段不对称，增量回测丢失三类状态

- 状态: 待审核
- 严重级别: 中
- 位置: 写出 `src/WtBtCore/CtaMocker.cpp:104-121`（positions）、`143-146`（fund）、
  `163-165`（signals）；读回 `src/WtBtCore/CtaMocker.cpp:544-568`、`576-578`、`587-590`
- 证据: 以下成员既不在 `dump_stradata` 写出、也不在 `load_incremental_data` 读回：
  - `PosInfo._frozen`（`CtaMocker.h:873`）——T+1 冻结量丢失；
  - `SigInfo._desprice`（`CtaMocker.h:915`）——限价/止损目标价丢失，
    而 `proc_tick`（`CtaMocker.cpp:705-708`）正是用它决定成交价，恢复后信号会退化成市价成交；
  - `SigInfo._sigtype`（`CtaMocker.h:916`）——`proc_tick`（`CtaMocker.cpp:712`）
    依赖 `== 2` 才回调 `on_condition_triggered`，恢复后条件单触发通知丢失；
  - `fund.tdate` 写出不读回（见 BUG-Q02-001）。
  另外 `dump_stradata` 写 `lastentertime/lastexittime`，但 `load_incremental_data`
  读回后没有任何接口能校验（`stra_get_last_entertime` 走的是 `_last_entertime`，口径一致，仅提示）。
- 期望: dump 与 load 字段集合一一对应，或在文档里明确声明「增量回测不恢复限价/冻结」。
- 测试处理: `incrementalLoadRestoresPositionQueries` 与
  `strategyDumpRoundTripsIntoSecondMocker` 只断言可恢复字段；
  `_desprice`/`_sigtype`/`_frozen` 的丢失不做「迁就当前返回值」的断言。

## BUG-Q02-004 · load_incremental_data 对残缺 JSON 无防护

- 状态: 待审核
- 严重级别: 中（外部产物驱动的控制流，越界/断言失败风险）
- 位置: `src/WtBtCore/CtaMocker.cpp:533-616`
- 证据:
  - `d.ParseStream(...)` 后没有 `d.HasParseError()` 判定；
  - `positionEntry["code"]`/`["volume"]`/`["lastentertime"]` 等直接下标访问，
    仅 `details` 有 `HasMember` 保护（L550）；
  - `d["fund"]["total_profit"]` 只在 `d.HasMember("fund")` 一层保护内，
    子成员缺失时 rapidjson 的 `operator[]` 断言/未定义行为；
  - `strcpy(curPosDetail._opentag, ...GetString())`（L567）与
    `strcpy(condEntrust._usertag, ...)`（L606）未做长度校验。
  rapidjson 在 `RAPIDJSON_ASSERT` 生效时 abort；被关掉时是越界读。
- 期望: 解析失败或必需成员缺失时按「跳过该项」处理，与 `dump_stradata` 的宽容写出对称。
- 测试处理: 本波只喂**字段完整**的报文；非法/残缺报文整类不在启用用例构造。

## BUG-Q02-005 · get_kline_slice 的 "f"（半小时线）被按小时线重采样

- 状态: 待审核
- 严重级别: 中（功能失效，非崩溃）
- 位置: `src/WtBtCore/HisDataReplayer.cpp:2196-2201`
- 证据:
  ```
  else if (strcmp(period, "f") == 0)    //半小时线
  {
      kp = KP_Minute5;
      baseTimes = 5;
      realTimes = PERIOD_TIMES_HOUR;     // 与上面 "h"(小时线) 分支完全相同
  }
  ```
  紧邻的 "h" 分支（L2190-2194）赋的是同一个 `PERIOD_TIMES_HOUR`。
  注释声明 "f" 是半小时线，则倍数应为小时线的一半；
  当前实现下 `get_kline_slice(code,"f",…)` 与 `get_kline_slice(code,"h",…)`
  产出完全相同的 bar，半小时线无法使用。
- 期望: "f" 用半小时倍数（`PERIOD_TIMES_HOUR/2` 或独立的 `PERIOD_TIMES_HALFHOUR`）。
- 测试处理: 该分支需要可只读的分钟底座 + `WTSDataFactory::extractKlineData` 的
  分组口径断言，本波未纳入启用用例（p07 已覆盖 "m"+15 的合并分支），
  留给「待裁定」——若主控确认这是有意为之（f 表示 1 小时），请回复并关闭本条。

## BUG-Q02-006 · load_incremental_data 非幂等

- 状态: 待审核
- 严重级别: 低
- 位置: `src/WtBtCore/CtaMocker.cpp:461-524`
- 证据: 五个 csv 分支都是 `_xxx_logs << str << "\n"` 的纯追加，
  且 `PosInfo`/`SigInfo`/`CondEntrust` 用 `_pos_map[code]`/`_condtions[code].push_back()`
  累积。同一实例调用两次 `load_incremental_data("A")` 会得到
  双份成交/平仓/资金流水和双份条件单，`dump_outputs` 落地后再次增量装载会指数膨胀。
- 期望: 同一 base 重复装载应幂等（先 clear 或按 base 名去重）。
- 测试处理: `q02_cta_dump_incremental_test.cpp` 每个用例都只装载一次，
  不对重复装载行为写断言（既不固化正确行为，也不迁就错误行为）。
