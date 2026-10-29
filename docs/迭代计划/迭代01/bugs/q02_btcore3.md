# Q02 WtBtCore 第五波缺陷登记（q02_btcore3）

- Lane: q02_btcore3（迭代 01 · 第五波）
- 范围: `src/WtBtCore/CtaMocker.cpp/.h`、`src/WtBtCore/SelMocker.cpp`、`src/WtBtCore/HisDataReplayer.cpp`、四个 `init_*_factory`
- 说明: 本波未修改生产代码；本次审核只做源码与测试前提静态核查，未独立运行测试或 sanitizer。
  与既有编号同源的（BUG-P03-001/002、BUG-P04-007/010、BUG-P07-007、
  BUG-P08-001/002/006、BUG-L00-005）只引用不重复立案。各条结论与尚缺证据见「审核结论」。

---

## BUG-Q02-001 · CtaMocker 落地的 `tdate`/`settime` 取自未初始化成员

- 状态: 已确认源码缺陷（未做运行时/MSan 复现）
- 严重级别: 中（非确定性输出 + MSan/valgrind 报未初始化读）
- 位置: `src/WtBtCore/CtaMocker.cpp:146`（`jFund.AddMember("tdate", _cur_tdate, ...)`）、
  `src/WtBtCore/CtaMocker.cpp:202`（`jCond.AddMember("settime", _last_cond_min, ...)`）
  成员声明 `src/WtBtCore/CtaMocker.h:1037`、`src/WtBtCore/CtaMocker.h:1039`
- 证据/审核结论: 构造函数初始化列表（`CtaMocker.cpp:60-79`）不含 `_cur_tdate`、
  `_last_cond_min`，类内声明（`CtaMocker.h:1037-1039`）也没有默认成员初始化；
  `dump_stradata()` 在 `CtaMocker.cpp:146,202` 直接读取两者，且
  `handle_replay_done()` 在 `CtaMocker.cpp:677-681` 无条件调用 dump。
  因而在相应 session/计算回调赋值前结束回放会读取未初始化标量。`_cur_bartime`
  虽未在构造函数初始化，但本条 dump 路径没有读取它，不能作为本条证据。
- 影响边界: 源码已足以确认未初始化读；具体产物是否出现可观测差异未运行验证。
  `fund.tdate` 也未被 `load_incremental_data()` 读回，但当前读取回放交易日使用 replayer
  状态，未找到消费该 JSON 字段的路径；这不是独立的状态丢失证据，见 Q02-003。
- 修复方向: 给 `_cur_tdate`、`_last_cond_min` 合理的零值/显式有效状态；若字段只有在
  session/计算后才有意义，则 dump 时按有效状态省略或写明文档规定的哨兵值。用构造后立即
  `handle_replay_done()` 与已赋值场景分别断言 JSON，不要把随机数固定为期望。
- 测试处理: `q02_cta_dump_incremental_test.cpp` 对字段键/结构的断言不能证明标量已初始化；
  修复后应断言 0 或约定值，并覆盖没有 session/计算回调就结束的情况。不要在 Q02-003
  再把 `fund.tdate` 不读回计为一项状态丢失。

## BUG-Q02-002 · SelMocker::handle_replay_done 在零调度时整数除零

- 状态: 已合并（主编号 BUG-R43-001；零调度除零源码成立，未独立运行）
- 严重级别: 高（除零是 C++ 未定义行为；不保证不同架构/编译器的具体结果）
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
- 审核边界: 已由源码确认零调度会执行除零表达式，但本轮未独立运行该路径；不推断 x86/arm64
  的具体 trap/返回表现。测试注释中的架构说法不能作为证据。
- 修复方向: 采用与 CtaMocker 相同的 `_emit_times > 0` 分支，只在非零时计算平均耗时；
  增加不加载模块、不启 hook 的“未触发 schedule 后完成”独立回归用例。
- 测试处理: `q02_mocker_factory_cfg_test.cpp::DISABLED_RealModuleLoadAndSelDivZeroFamily`
  把此入口与真实模块/hook 分支合在一个禁用用例中，因此当前启用集没有此路径的回归证据。

## BUG-Q02-003 · CtaMocker dump/load 字段不对称；续跑影响待契约确认

- 状态: 字段差异已确认；继续状态是否属于缺陷待增量快照契约确认
- 严重级别: 中
- 位置: 写出 `src/WtBtCore/CtaMocker.cpp:104-121`（positions）、`143-146`（fund）、
  `163-165`（signals）；读回 `src/WtBtCore/CtaMocker.cpp:544-568`、`576-578`、`587-590`
- 证据/审核结论: `dump_stradata()` 写持仓总量及 details（`CtaMocker.cpp:97-136`），
  但没有写 `PosInfo::_frozen`（`CtaMocker.h:873`）；写信号时仅输出 usertag/volume/
  sigprice/gentime（`CtaMocker.cpp:151-170`），没有 `SigInfo::_desprice/_sigtype`
  （`CtaMocker.h:915-916`），`load_incremental_data()` 也只读前述四个信号字段
  （`CtaMocker.cpp:581-591`）。后两字段参与 `proc_tick()` 的成交价格及条件触发回调
  （`CtaMocker.cpp:695-714`），所以若 checkpoint 允许待处理信号跨段继续，恢复语义会改变。
  `_frozen` 用于 `valid()` 与可用持仓计算（`CtaMocker.h:896`、`CtaMocker.cpp:2024`），
  但 `on_session_begin()` 会清零（`CtaMocker.cpp:1215-1228`）；不能泛化为所有跨回测都丢失
  T+1 冻结量。`fund.tdate` 仅 dump 不 load，未找到其被恢复消费者读取的代码，故不据此认定缺陷。
- 测试边界: `strategyDumpRoundTripsIntoSecondMocker` 只检查 JSON 结构及可查询持仓，
  没有验证 pending signal 的指定价/类型跨段恢复，也没有覆盖同交易日冻结状态；当前测试不足以
  确立这些状态的产品续跑契约。
- 修复方向: 先明确 checkpoint 是“仅持仓/资金快照”还是可在同一交易日续跑的完整状态。
  若承诺 pending signal 继续生效，把 `_desprice/_sigtype` 加入带版本的 dump/load，并以
  恢复后首笔 tick 校验成交价及 callback；若承诺同日延续，再定义 `_frozen` 的恢复/重置时点并测试。
  若不承诺这些状态续跑，则在 checkpoint 边界清理或显式拒绝未完成状态，并文档化；避免静默退化。

## BUG-Q02-004 · load_incremental_data 对残缺 JSON 无防护

- 状态: 异常输入保护缺口已确认；文件输入契约与严重度待确认
- 严重级别: 中（畸形快照路径存在断言/越界写风险；本轮未复现）
- 位置: `src/WtBtCore/CtaMocker.cpp:533-616`
- 证据/审核结论: `d.ParseStream()` 后未检查 parse error（`CtaMocker.cpp:530-535`）；
  对 positions/fund/signals/conditions 的类型和必需子键多处直接 `operator[]/GetXxx`
  （`CtaMocker.cpp:536-616`），仅 `details` 顶层有存在性判断。
  `_opentag` 与条件单 `_usertag` 都是 32 字节数组（`CtaMocker.h:849`、`CtaMocker.h:57-68`），
  读 JSON 后以 `strcpy` 不限长复制（`CtaMocker.cpp:567,606`）。因此畸形/缺键/错误类型 JSON
  或超过缓冲区容量的 tag 可触发 RapidJSON 断言/越界写风险。没有运行时复现，且当前由程序
  自己写出的正常 dump 是预期格式；外部损坏/手工编辑 JSON 是否受支持仍需确认。
- 测试前提: `q02_cta_dump_incremental_test.cpp` 有完整 schema 的装载以及“无 details 键”分支，
  但不覆盖 parse error、必需子键缺失、错误类型或超长字符串；不能据此说已验证安全。
- 修复方向: 打开文件后先校验句柄、parse error、根对象和字段类型/必需键；采用明确的拒绝/
  跳过策略并记录路径与原因，避免对不完整快照静默部分恢复。对固定数组字段检查容量或使用
  有界复制；单独补齐 malformed/truncated/oversized fixture 的回归覆盖。

## BUG-Q02-005 · get_kline_slice 的 "f"（半小时线）被按小时线重采样

- 状态: 已合并（BUG-P07-004）
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
- 审核裁定: 与 `p07_btcore_replayer2.md` BUG-P07-004 是同一代码行、同一根因，保留本处交叉引用，
  不作为第二个产品缺陷计数。源码注释、两个 period 分支赋值相同的 HOUR sentinel，以及
  `onMinuteEnd` 中 HALF sentinel 对应 `"f"` 的分支共同支持该方向；未独立运行半小时线测试，
  不声称已观察实际 bars/callback。
- 修复方向: 统一在 BUG-P07-004 处理 `"f"` 到 HALF sentinel 的映射，并用分钟底座断言分组边界
  与回调周期串；本条不另行维护重复测试或修复项。

## BUG-Q02-006 · load_incremental_data 重复加载会追加已有状态（幂等契约待确认）

- 迭代02第二轮前状态（历史）: - 状态: 重复装载的追加行为已确认；幂等性是否为缺陷待契约确认
- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 低
- 位置: `src/WtBtCore/CtaMocker.cpp:461-524`
- 证据/审核结论: 五个 CSV 分支逐行追加到对应 stringstream（`CtaMocker.cpp:461-524`）；
  JSON 持仓总量字段是赋值，但 details 向 vector 追加（`CtaMocker.cpp:541-569`），条件单也
  `push_back`（`CtaMocker.cpp:594-614`）；信号 map 则按 key 覆写字段，不能笼统说所有 map 都累积。
  因而同一实例重复载入同一快照会重复历史日志、明细和条件单，代码行为有据；“幂等”是额外
  API 契约，当前文档/测试未证明调用方可重复装载。重复一次是线性追加，不保证“指数膨胀”。
- 测试前提: 当前测试每个实例只装载一次；没有重复调用或对同名输出目录再次读回的证据。
- 修复方向: 若产品契约要求重试安全，按规范化快照路径/内容标识记录已载入源，或在载入前清理
  可重建状态并以事务方式替换；若调用只允许一次，则显式拒绝第二次或文档化 one-shot 约束。
  增加同一实例双次装载用例后再裁定是否立为产品缺陷。
