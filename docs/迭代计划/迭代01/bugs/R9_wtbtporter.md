# R9 缺陷清单（迭代01 第七波 · WtBtPorter）

- 范围: 本 lane 仅记录，未修改任何生产代码；`src/**`（除 `src/TestUnits/coverage/q06_btporter/r9_*.cpp`）一字未动。
- 关联用例: `src/TestUnits/coverage/q06_btporter/r9_exp_mocker_bridge_test.cpp`、`src/TestUnits/coverage/q06_btporter/r9_mocker_wired_capi_test.cpp`
- 关联既有记录: `_hft_mocker`/`_exec_mocker` 不在 `WtBtRunner` 构造初始化列表（q06 波次已在 `q06_btporter_capi_test.cpp:201-208` 以「缺陷 Q06-004」登记），本文件只做交叉引用（BUG-R9-006），不重复主张新发现。

## BUG-R9-001 config_backtest 的幂等守卫永不生效，重复 config 会反复 new mocker 并泄漏 _cfg
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtBtPorter/WtBtPorter.cpp:174-185`（对照 `:155-165` 的 init_backtest）
- 现象: `config_backtest` 开头写 `static bool inited = false; if (inited) return;`，但函数体（L181-185）只调用 `getRunner().config(...)`，**没有任何一处把 `inited` 置 true**；同文件 `init_backtest` 的 L164 就有 `inited = true;`，可见是漏写。L172 的注释「该函数只会执行一次，重复调用会被忽略」与实现相反。
- 影响范围: 语言绑定（Python/Go/Rust porter）里重复 `config_backtest` 不报错也不生效一次以上——每次都会重新 `load_from_file` 并整段重跑 `WtBtRunner::config`（`WtBtRunner.cpp:449-507`）：L476/483/491/503 各自 `new Exp*Mocker` 直接覆盖旧槽位而不 `delete`（对比 `initCtaMocker` L269-273 是有 delete 的），旧 mocker 连同其 `_pos_map`/`_userdata` 泄漏；`_replayer.register_sink`（`HisDataReplayer.h:804-807`）被反复改指；`_cfg`（L457）也从不 release。
- 复现: `r9_exp_mocker_bridge_test.cpp::ConfigBacktestMissingFileLeavesMockersNull` 连续两次 `config_backtest(不存在的路径)`，钉住「两次都走进 runner::config 的 `Loading config failed` 早退、mocker 槽位保持为空」。成功路径不可用（要真配置+存储），故 L465-506 的泄漏分支只能静态举证，未写启用用例。

## BUG-R9-002 WtBtRunner::_inited 全文件只读不写，config 的「已初始化」早退是死代码
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtBtPorter/WtBtRunner.cpp:85`（唯一写入=构造初值 false）、`:451`（唯一读取）
- 现象: `grep -n "_inited" WtBtRunner.cpp` 只有这两行命中——初始化列表把 `_inited` 置 false，`config()` 判它，但没有任何路径（`init()`/`run()`/`release()`）把它置 true。于是 `WTSLogger::error("WtBtEngine has already been inited")`（L453）与 L454 的 return 永不执行。
- 影响范围: 同一实例上 `config()` 可无限重入（与 BUG-R9-001 叠加），且 L451-455 这 5 行在覆盖率里永远是 0，属于不可达分支而非未测分支。
- 复现: 未写用例（不可达分支不可证伪）。建议删除该守卫，或在 `init()`（L437-447）末尾置 `_inited = true`。

## BUG-R9-003 ExpHftMocker 的周期重命名与 CTA 侧口径相反，且把非 'd' 单位一律报成分钟
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtBtPorter/ExpHftMocker.cpp:19-27`（对照 `src/WtBtCore/CtaMocker.cpp:950-951`、`src/WtBtPorter/ExpCtaMocker.cpp:133-139`）
- 现象: 包装层只在 `period[0]=='d'` 时拼 `"{}{}"`（`"d1"`+3 → `"d13"`），否则**丢弃入参 period**、固定生成 `"m{times}"`。于是 `"w1"`/`"s1"`/`"h1"`（周线/秒线/小时线）都会被报成 `"m2"` 这类分钟串；而 `CtaMocker::on_bar` 对同样的 `(period, times)` 恒用 `"{}{}"`（`"m1"`+5 → `"m15"`），`ExpCtaMocker::on_bar_close` 又原样透传 `"m5"` 不改写——同一根 K 线在三个出口给外部回调的 period 串互不相同，策略侧用 period 字符串做订阅键/缓存键时跨引擎必然错位。`"d13"` 也不是任何合法周期标识。
- 影响范围: HFT 外部回调（`ctx_on_bar`，`WtBtRunner.cpp:315-325`）的 period 语义失真；行情重放出的日线倍数（如 d1×3=3 日线）被写成 `"d13"`，策略里 `strcmp(period,"d1")` 之类的判断全部失效。
- 复现: `r9_exp_mocker_bridge_test.cpp::HftBarPeriodRenamedByTimes` 逐一断言三组实测换算：`("m1",5)→"m5"`、`("d1",3)→"d13"`、`("w1",2)→"m2"`（期望值取自 L21/L23 两条 `fmtutil::format<64>` 的实参拼接）；`CtaBarClosePassesPeriodUnchanged` 断言 CTA 侧 `("m5")→"m5"` 原样透传，形成同一口径的对照证据。

## BUG-R9-004 ExpCtaMocker/ExpSelMocker::on_bar_close 不校验 newBar，NULL 被原样外发给策略回调
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtBtPorter/ExpCtaMocker.cpp:133-139`、`src/WtBtPorter/ExpSelMocker.cpp:122-127`（对照 `src/WtBtPorter/ExpHftMocker.cpp:16-17`）
- 现象: 同层的 `ExpHftMocker::on_bar` 第一句就是 `if (newBar == NULL) return;`，而 CTA/SEL 两个 `on_bar_close` 没有任何判空，直接把 `newBar` 交给 `CtaMocker::on_bar_close`（`CtaMocker.cpp:1021-1025`，`_strategy` 为空时静默）再交给 `getRunner().ctx_on_bar(...)`（L138/L126）→ 外部 `FuncStraBarCallback` 收到 `bar == NULL`，策略里一句 `bar->close` 就是空指针解引用；`period` 也是同样的裸 `const char*`（L20 直接取 `period[0]`，传空串即越界读）。
- 影响范围: 任何以 NULL bar 表示「bar 关闭但无实体」的上游（回放器异常分支/未来接入的 feeder）都会把崩溃推给策略侧；三个包装层守卫口径不一致，无法用统一约定推理。
- 复现: `r9_exp_mocker_bridge_test.cpp::CtaBarClosePassesPeriodUnchanged` 先喂一根 `close=15.25` 的正常 bar（断言 1 次回调 + 字段透传），再喂 `NULL` 并断言回调仍被调用一次且探针记录 `last_bar_null==true`——把「NULL 被外发」写成可证伪断言而不是放宽期望；`HftBarNullGuardSkipsDispatch` 用同一输入在 HFT 侧断言回调次数为 0，构成不对称的对照。

## BUG-R9-005 ExpHftMocker 微观结构三个出口既不过订阅表也不调父类，与 on_tick_updated 前置相反
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtBtPorter/ExpHftMocker.cpp:86-99`（对照同文件 `:76-84`）
- 现象: `on_ordque_updated`/`on_orddtl_updated`/`on_trans_updated` 各自一行 `getRunner().hft_on_xxx(_context_id, stdCode, ptr)`，既不查 `_tick_subs`（同文件 on_tick_updated L78-80 必须先命中订阅表才外发），也不调用 `HftMocker` 的同名基类方法（基类里维护订阅与缓存的动作被整体跳过）。结果是：策略没订阅任何合约也会持续收到逐笔/委托队列/成交明细回调，且 mocker 自身状态与实际外发给策略的数据可能不同步。
- 影响范围: 高频回测的外部回调量与实盘（WtPorter 侧）语义不一致，订阅过滤在三类微观数据上失效；同时 ptr 为空时也无守卫（`hft_on_order_queue` 直接 `&newOrdQue->getOrdQueStruct()`，`WtBtRunner.cpp:395-411`）。
- 复现: `r9_mocker_wired_capi_test.cpp` 不适用；实际证据是 `r9_exp_mocker_bridge_test.cpp::HftMicroStructIgnoresSubscription` —— 只向订阅表插入 `SSE.600000`，却用 `SHFE.rb2410` 调用三个出口，断言三者仍各回调 1 次且 `last_code=="SHFE.rb2410"`（期望来源：L88/L93/L98 无任何前置判断）。

## BUG-R9-006 _hft_mocker/_exec_mocker 未进构造初始化列表（交叉引用 q06 的 Q06-004）
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtBtPorter/WtBtRunner.cpp:82-83`（只列 `_cta_mocker`/`_sel_mocker`）、成员声明 `src/WtBtPorter/WtBtRunner.h:635-638`
- 现象: `_hft_mocker`、`_exec_mocker` 未在初始化列表里，全靠静态存储零初始化才恰好为 NULL；`WtBtPorter.cpp:1470-1524` 等处的 `mocker == NULL` 判断在指针未真正置零的构造路径下会变成野指针解引用。
- 影响范围: 同 Q06-004（q06 波次已在 `q06_btporter_capi_test.cpp:201-208` 登记并把前提写成显式断言）。本轮新增证据是「接线后」的对照：`init_hft_mocker` 一旦调用，`hft_mocker()` 才返回可用实例，说明 L1470 的 NULL 分支此前完全依赖零初始化。
- 复现: `r9_exp_mocker_bridge_test.cpp::MockersAreNullAndAsyncFalseBeforeInit`（三个 getter 均为 nullptr）与 `r9_mocker_wired_capi_test.cpp::InitHftMockerWiresSlotAndIsRepeatable`（置位后非空且 id 递增）各钉一侧，本条不重复主张为新发现。

## BUG-R9-007 _running 只在异步 lambda 内置位，同步回测无法被 stop() 中断
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtBtPorter/WtBtRunner.cpp:509-548`（run）、`:550-583`（stop）、`:511`、`:552`
- 现象: `run()` 只在 `bAsync` 分支的子线程 lambda 里执行 `_running = true`（L531），同步分支（L524-527）直接 `_replayer.run(bNeedDump)` 阻塞到底、`_running` 始终为 false。于是另一个线程调用 `stop()` 时 L552 的 `if (!_running)` 命中，只 join 一个不存在的 `_worker` 就 return，**不会走到 L562 的 `_replayer.stop()`**——同步回测没有任何中断手段。异步侧还有反向竞态：`run(bAsync=true)` 返回时 lambda 可能尚未执行 L531，此时 `stop()` 同样看到 `_running==false`，但 `_worker` 已存在 → 走 L554-558 直接 `join()`，即「停止」退化成等整轮回放跑完。
- 影响范围: `stop_backtest()`（`WtBtPorter.cpp:223-226`）在两种模式下都不满足「中断当前回测」的文档语义；L562-578 的通知/收尾分支（含 `_cta_mocker->step_calc()`、`_hft_mocker->step_tick()`）在 `_running` 恒 false 时永不可达。
- 复现: 本轮**未写启用用例**——必须先让 `_replayer.prepare()/run()` 真跑（读存储、回放整段区间，同步会阻塞、异步要起线程并 join），违反本 lane「不起线程 / 不跑回测」硬约束。已用 `r9_exp_mocker_bridge_test.cpp::DISABLED_RunConfigSuccessPathsNeedStorageAndThreads` 与 `r9_mocker_wired_capi_test.cpp::DISABLED_OrderAndSlicePathsNeedLoadedStorage` 占位；触发条件：调用 `getRunner().run(false, false)`（同步阻塞至区间结束）或 `run(false, true)`+`stop()`（起线程并 join）。建议把 `_running = true` 提到 `run()` 主体（lambda 之前），并给 `_replayer.run()` 增加可注入的步进接缝。
