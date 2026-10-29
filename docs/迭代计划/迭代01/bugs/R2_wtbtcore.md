# R2 缺陷与接缝台账 — WtBtCore（第七波）

本文件记录 R2 lane 的候选项。本次复核只读源码、公开接口和现有测试前提；没有修改生产代码/测试，也没有运行测试或 sanitizer。以下状态按当前证据更新。

## A. 逐项复核

- 标题: checkOrderDetails 无数据时把兜底空表写进 `_trans_cache`（写错缓存表）
  - 状态: 已合并（与 `bugs/R91.md` BUG-R91-001 同根，R91 保留主记录）
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L2702-2744（`checkOrderDetails` 的 `if (!hasData)` 块，约 L2728-2738）
  - 证据/影响: L2731 将无逐笔委托明细时的空列表写入 `_trans_cache[stdCode]`；`checkOrderQueues` L2773 有相同错误；
    `checkTransactions` L2815 写 `_trans_cache` 则是正确目标。缺失委托明细/队列后，其目标 cache 没记录该日期，重复 slice 请求会重复装载；
    更严重的是错误写入会清空同合约已有的 transaction 列表并写入当前日期，后续 `checkTransactions` 看到日期匹配会跳过真实加载。
    后续 transaction slice/replay 因而可能丢成交明细或进入空列表状态；空列表后的具体运行时表现未独立复现。
  - 测试/重复边界: `r2_replayer_public_api_test.cpp#level2DataRefusedInCsvMode` 在 CSV 拒绝分支 L2721/L2763/L2805 提前返回，
    不执行写错 cache 的 bin 无数据分支，不能作为复现。R91-001 有专门的 DISABLED 场景设计，但本轮没有运行；不把 disabled 用例视为动态证据。
    R2 与 R91-001 是同代码、同根因，R2 不单独计数。R91 测试原将 `DISABLED_` 写在名称中间，GoogleTest 不会识别；现已改为名称开头，污染路径未运行。新增的正常成交读取对照 PASS，不证明缺明细后的污染已修复。
  - 修复方向: 分别将缺数据状态写入 `_orddtl_cache`、`_ordque_cache`；不要触碰 `_trans_cache`。负缓存再次命中时也必须继续作为“无数据”返回，不能仅因日期相同就让空列表进入 slice；
    当前 `checkTransactions` 的零条数日期命中也有相同的空列表风险，细节并入 R91-001。回归场景：先装入同合约 transaction，再查询缺失 order-detail/queue，随后确认 transaction slice/replay 仍含原成交；重复查询空流仍安全返回 NULL。
  - 分级: S3（同代码/日期的有效 transaction cache 可被清空；空列表下游细节未动态验证）

- 标题: getAdjFactors 使用 `static char key[20]` 拼缓存键
  - 状态: 固定缓冲区缺陷已确认；溢出/并发现场未独立运行
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L4192-4200
  - 证据/影响: L4194-4195 将 `exchg.pid.code` 无界写入 20 字节静态数组；`fmtutil::format_to`（`src/Share/fmtlib.h:15-20`）调用不带容量的
    `fmt::format_to`，并在返回位置写 NUL。`CodeHelper::CodeInfo` 的 exchange/product/code 字段均可容纳超过 19 字节的组合键（`src/Share/CodeHelper.hpp:44-48`；
    `MAX_INSTRUMENT_LENGTH=32`），因此受输入长度限制不足时可越界写；静态共享缓冲区并发调用也会相互覆盖。当前 R91 复权 fixture 使用短键，未覆盖这些边界。
  - 修复方向: 用每次调用独立的 `std::string`/`fmt::format` 构造键，不复用静态数组；若保留固定缓冲区，必须有可证明的组合长度上限并使用有界格式化。
    增加超长合法字段及并发不同键查询回归；本轮未运行这些验证。
  - 分级: S3（源码可达的复权因子路径；具体外部标识长度契约待核实）

- 标题: cacheAdjustedStkBarsFromBin 的 lower_bound 区间右端少算一根
  - 状态: 已撤回（原缺陷论据不成立；当前不计产品缺陷，边界用例仍可补充）
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L4360-4420（复权因子倒序调整循环，约 L4400-4412）
  - 证据/审核结论: 对有效非空的 `tempAy` 且 `lastIdx >= 1`，搜索终点 `firstBar + lastIdx - 1` 指向最后一根有效 bar，
    并非 vector 的末后迭代器；当 `lower_bound` 返回该端点时，L4412 比较该有效末 bar 的日期，目标日期不晚于它时 L4419 起的循环仍会处理末 bar，
    目标晚于它时则跳过。`lastIdx == 1` 的空搜索区间同样返回指向唯一有效 bar 的起点，随后比较决定是否调整。因此“排除末根导致漏调”不能由该区间写法推出。
    R91 现有前/后复权用例覆盖普通分段，但本轮未运行；`lastIdx==0` 或非空但不足一个 `WTSBarStruct` 的损坏 buffer 不属于本条已证明前提。
  - 修复/验证方向: 不按原论据改成 `lastIdx` 末后边界，以免改变现有哨兵算法；如需增加防回归，覆盖单根、因子日期等于末根/晚于末根、逐步缩短 `lastIdx`，
    并另行明确空/截断 bin buffer 的输入契约。

- 标题: CtaMocker::handle_tick 对同一 tick 可能两次触发 proc_tick
  - 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
  - 位置: `src/WtBtCore/CtaMocker.cpp` L898-916（`proc_tick` 调用于 L~921，`if(pxType != 3)` 再调用于 L~948）
  - 证据/审核结论: L919 先更新 `_price_map`，L926 与 L939 调用 `proc_tick` 时传入的 `last_px/cur_px` 完全相同；两次调用之
    间没有再次改写该价格边界。L714 会删除已处理信号，故源码未证明同一旧信号被重复成交。第二次调用位于 `on_tick_updated` 之后，注释 L930-936
    明确目的是撮合回调中新生成的信号；现有测试未验证“策略在 tick 回调内发单是否应当同 tick 成交”这一时序契约。
  - 测试/边界: `r2_mocker_tick_fill_test.cpp` 直接调用 `handle_tick` 并在 tick 之间发信号，能验证信号成交，不会证明/否定 callback 内发单的同 tick 语义。
    目前没有证据支持“价格边界因两次调用而退化”或系统性错价，原缺陷影响论据撤回。
  - 修复/验证方向: 不因本候选删除第二次调用；若契约要求回调内新信号延迟到下一 tick，增加一个 tick 回调内发信号的集成断言，再据结果调整事件顺序。


- 迭代02复核: NOT_BUG。一次 tick 的现有 signal 在第一次 proc_tick 后已被消费；第二次调用位于 on_tick_updated 之后，用于处理该回调中新建的 signal，代码注释说明了这个时序用途。现有测试覆盖两 tick 间下单，不证明同一订单重复成交。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）:   - 状态: 原重复撮合论据不成立；同 tick 新信号是否成交需按策略时序契约确认
- 标题: append_signal 的信号时间戳秒位恒为 0
  - 状态: 已撤回（原候选为误报；无已证产品缺陷）
  - 位置: `src/WtBtCore/CtaMocker.cpp:1586-1598`；时间字段接口 `src/WtBtCore/HisDataReplayer.h:901-908`
  - 证据/审核结论: 实际表达式为 `date*1e9 + get_raw_time()*100000 + get_secs()`，并非原记录的 `*100 + ...`。
    `get_raw_time()` 返回 HHMM，`get_secs()` 返回 SSmmm 余数（`HisDataReplayer.cpp:1791-1792,1921-1922` 在逐笔回放中拆分）；
    因而两者合成 HHMMSSmmm。`append_signal` 在正常 sink 回调中读取共享 replayer 时钟，与当前 callback 时间对齐。bar 模拟的秒/毫秒为零是该回放精度，不证明所有信号秒位恒零。
  - 测试边界: R2 的直接 mock 调用自行构造 tick，但不推进 replayer 时钟；不能用它校验 signal timestamp 与传入 tick 的 action_time 一致性。BUG-P04-010 是日线起始游标问题，
    与本项无共同根因；删除原重复引用。
  - 修复/验证方向: 当前证据不支持改生产表达式。若需验证毫秒精度，应通过 replayer 驱动非整秒 tick，并在回调内生成信号后检查 `signals.csv` 的时间字段。

- 标题: SelMocker::handle_replay_done 整数除零（沿用，不复活）
  - 状态: 已合并（主记录 BUG-Q02-002；重复引用，不在 R2 独立计数）
  - 位置: `src/WtBtCore/SelMocker.cpp:303-307`
  - 证据/审核结论: 零 schedule 时整数除法的源码问题已在 `bugs/q02_btcore3.md` BUG-Q02-002 记录；R2 的
    `selHftUftTickUpdatesPriceMapWithoutStrategy` 不调用 `handle_replay_done`，不构成该问题的复现或反证。
  - 修复方向: 保留 Q02-002 为主记录，按其零调度防护与独立完成路径回归处理；R2 不重复计数。

## B. 真正确诊「被接缝挡住」的面（本轮公开接口进不去）

说明：上一轮把整族 `bin/cache*FromBin` 都记成「private 且需要 `#define private public`」。
本轮接口源码复核**部分推翻**该结论：`init(cfg, notifier, IBtDataLoader*)`（HisDataReplayer.h L748）
是公开接缝，`get_kline_slice / checkTicks / get_last_tick` 会优先调用 `_bt_loader`，
所以 `cacheFinalBarsFromLoader`（L3589）、`cacheRawTicksFromLoader`（L3443）、
`cacheRawBarsFromBin` 的 loader 臂（L4527）均存在公开接口可达路径（见清单 A 节）；本轮没有执行这些路径的测试。

仍然进不去的，是「`IBtDataLoader` 接口本身没有对应方法」这一段硬缺口：

| 被挡函数 | 行段 | 预计可回收行数 | 阻塞原因 |
|---|---|---|---|
| `cacheRawOrdDtlFromBin` | L3356-3385 | ~30 | `IBtDataLoader` 无 `loadRawHisOrdDtl`；只能喂真实 level2 dsb |
| `cacheRawOrdQueFromBin` | L3385-3414 | ~30 | 同上，无 loader 接口 |
| `cacheRawTransFromBin` | L3414-3443 | ~30 | 同上，无 loader 接口 |
| `getNextOrdDtlTime` / `getNextOrdQueTime` / `getNextTransTime` | L1626-1769 | ~145 | 需上面三张表有数据 |
| `replayHftDatasByDay` / `replayHftDatas` | L1769-2013 | ~245 | 需 tick/level2 dsb 或 csv tick（`cacheRawTicksFromCSV` L3470-3589 需要 wtpy `store_ticks` 产物） |
| `cacheIntegratedFutBarsFromBin` | L3900-4192 | ~290 | 需 `_hot_mgr` 主力连续规则 + 逐合约 raw bars；`ruleTag` 非空即整条转入此函数，csv/stk 配置进不去 |
| `run_by_ticks` 主体 | L741-771 | ~30 | 同上（tick 数据依赖） |
| Cta/Hft/Uft/Sel 的策略回调链 | CtaMocker L530-690、L1054-1200；Hft L140-275；Uft L60-190；Sel L260-450 | 合计 ~500+ | `_strategy` 由 `DLLHelper::load_library` 产出；本轮不加载动态库、不起线程（`step_calc`/`install_hook`/`postTask` 走 `_cond_calc.wait`，属时序挂死风险） |

**需要主控裁定的接缝（共 3 类，预计总可回收 ~770 行，全部在 HisDataReplayer.cpp）**：
1. 给 `IBtDataLoader` 增加 level2（ordDtl/ordQue/trans）三个虚方法或提供受保护的测试注入点
   —— 可回收上表 L3356-3443 与 L1626-1769，约 235 行。
2. 允许 fixture 生成 tick/level2 dsb（走 `WtDataStorage` 的 `Section` 写接口，p10 已有写侧用例）
   —— 可回收 L1769-2013 与 `run_by_ticks`，约 275 行。
3. 允许主力连续（`_hot_mgr.loadHots` + 逐合约 raw bars）样例数据
   —— 可回收 L3900-4192，约 290 行。
（Mocker 的 500+ 行策略回调链不属于「接缝」问题，属于「动态库 + 线程」问题，建议另立 lane。）

## C. 公开可达但本轮未执行动态验证（不是接缝阻挡，后续可直接接）

- `cacheAdjustedStkBarsFromBin` L4236-4484 + `getAdjFactors` L4192-4236：约 **290 行**。
  当前仓库已有 `src/TestUnits/coverage/q02_btcore3/r91_replayer_bin_bars_test.cpp` 的前/后复权用例，使用
  `SSE.STK.600000-`/`SSE.STK.600001+` 并观察 loader 因子后的切片/回放值；因此后缀拆解和普通复权路径不再属于待接缝。
  该测试源覆盖并不证明 `getAdjFactors` 的 >19 字节键、并发调用或本条所列每个 lower_bound 边界正确；本轮未运行 R91。
  后续若继续补覆盖，应优先覆盖长键及单根/末因子日期边界，不再以“尚未确认后缀拆解”为阻塞理由。
- `loadStkAdjFactorsFromFile` L335-407：约 **70 行**。格式已读通
  （`{ "SSE": { "STK.600000": [ {"date":..,"factor":..} ] } }`，cfg 键 `adjfactor`），
  但 `_adj_factors` 无公开读口，要等 C 节第 1 条的复权用例一起做才有可证伪断言；
  本轮只固化了「loader 优先、失败退文件」的两条早退臂。
- `cacheRawBarsFromCSV` L3737-3900 的 `align_by_section=true` 分节对齐臂：约 **60 行**，
  公开 cfg 即可打开（p07 用的是 false）。
- `check_cache_days` L4607-4635：约 **28 行**，`run_by_bars` 内部按 `cache_clear_days` 触发，
  把 `cache_clear_days` 配成 0~1 即可从公开 `run()` 进入。
- `CtaMocker` 价格条件单撮合 L723-898（约 **175 行**）：`handle_tick` 公开可驱动，
  本轮未做是因为还需读 `add_price_condition`/`check_price_conditions` 的登记入口
  （`stra_enter_long` 的 `limitprice` 是否落进 `_condtions` 未确证）。

## D. 公开接口与现有测试源码的正面核对结果（可写回上一轮的「障碍清单」）

- `WTSTickData::create(WTSTickStruct&)`（`src/Includes/WTSDataDef.hpp` L884）是 public 池工厂，
  与 `release()` 同池；从对象所有权代码看，通过该工厂构造后可调用同池 release。p08 反对的是伪造对象跨池 release，不适用于工厂产物；本轮未运行专门的 create/release 测试。
- 四个 Mocker 的 `handle_tick` 均在 `public:` 段（IDataSink 重写），`_strategy == NULL` 时
  `on_tick_updated`/`on_tick`/`procTask` 一律守卫早退，`Hft/Uft::procTask()` 在 `_tasks` 为空时
  直接 `return`（不 lock、不 wait）。这是源码控制流判断，不代表本轮执行过线程/挂死验证。
- `mode="bin"` 且 `_his_dt_mgr` 指向空目录时，`load_raw_bars` 只 warn 返回 false，
  现有用例 `r2_replayer_loader_test.cpp#binModeWithoutLoaderOrDsbYieldsNoSlice` 有对应返回/空切片断言；本轮未运行该用例，故不把它记为本轮动态结果。
