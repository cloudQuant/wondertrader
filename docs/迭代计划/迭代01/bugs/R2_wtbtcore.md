# R2 缺陷与接缝台账 — WtBtCore（第七波）

只记录，不改产品代码。每条 `- 状态: 待审核`。

## A. 疑似缺陷

- 标题: checkOrderDetails 无数据时把兜底空表写进 `_trans_cache`（写错缓存表）
  - 状态: 待审核
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L2702-2744（`checkOrderDetails` 的 `if (!hasData)` 块，约 L2728-2738）
  - 现象: 兜底代码写的是 `auto& dataList = _trans_cache[stdCode];`，而本函数管理的是 `_orddtl_cache`；
    同一模式的 `checkOrderQueues`（L2744-2786）、`checkTransactions`（L2786-2828）里也各有一份同形代码，
    需要一并核对是复制粘贴还是有意为之。
  - 影响: 委托明细取不到数据时，`_orddtl_cache` 里没有「当日无数据」的空标记，下一次 `get_order_detail_slice`
    （L2547）会对 `_orddtl_cache[stdCode]` 新建默认项并读到 `_cursor` 未初始化的语义；同时 `_trans_cache`
    被无关合约的空条目污染，逐笔成交的日期判断（`dataList._date != uDate`）会被反复重置。
  - 复现: `r2_replayer_public_api_test.cpp#level2DataRefusedInCsvMode` 已把三个 slice 入口的返回固化为 NULL；
    bin 模式下（需 dsb 逐笔文件）才会走到写错表的那一段。
  - 分级: S3（数据口径错误，当前仅被 csv 早退挡住）

- 标题: getAdjFactors 使用 `static char key[20]` 拼缓存键
  - 状态: 待审核
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L4192-4200
  - 现象: `static char key[20]` + `fmtutil::format_to(key, "{}.{}.{}", exchg, pid, code)`。
    `exchg(≤10)+pid(≤20)+code(≤32)` 的组合远大于 20 字节；且函数返回值是对该静态表的引用，
    多线程/多次调用互相覆盖（`cacheIntegratedFutBarsFromBin`、`cacheAdjustedStkBarsFromBin` 都调它）。
  - 影响: 长代码（如 `SSE.STK.600000` 形态的三段码）会被截断 → 复权因子命中错键；潜在缓冲区溢出。
  - 分级: S3（复权链路；本轮公开可达但未做该用例，见 C 节）

- 标题: cacheAdjustedStkBarsFromBin 的 lower_bound 区间右端少算一根
  - 状态: 待审核
  - 位置: `src/WtBtCore/HisDataReplayer.cpp` L4360-4420（复权因子倒序调整循环，约 L4400-4412）
  - 现象: `std::lower_bound(firstBar, firstBar + lastIdx - 1, bar, ...)` 把区间最后一根排除在搜索范围外，
    随后无条件 `if (pBar->date < bar.date)` 解引用返回的迭代器。`lastIdx == 1` 时搜索范围为空区间，
    `pBar == firstBar`，读到的是范围外语义的那根K线。
  - 影响: 复权窗口边界可能漏调最后一根，或在极短序列上把不该调整的那根带进循环（`while (pBar && curIdx < lastIdx)`
    的 `lastIdx` 又取自 `endBar - firstBar`）。
  - 分级: S3

- 标题: CtaMocker::handle_tick 对同一 tick 可能两次触发 proc_tick
  - 状态: 待审核
  - 位置: `src/WtBtCore/CtaMocker.cpp` L898-916（`proc_tick` 调用于 L~921，`if(pxType != 3)` 再调用于 L~948）
  - 现象: 第一次 `proc_tick` 已把 `_sig_map` 里的信号 erase 并成交；第二次只在「有价格条件单」时再撮合。
    对逐 tick 回放（`pxType==0` 且 tick 非模拟）而言，两次调用之间 `_price_map` 已被更新，
    `left_px/right_px` 边界因此退化为同一个价，条件单的「左右边界」语义失效。
  - 影响: 条件单成交价与真实盘口触发价存在系统性偏差（源码注释也承认这段是重复复制）。
  - 分级: S4（语义风险，需业务裁定）

- 标题: append_signal 的信号时间戳秒位恒为 0
  - 状态: 待审核
  - 位置: `src/WtBtCore/CtaMocker.cpp` L1586-1603（`_gentime = date*1e9 + get_raw_time()*100 + ...`）
  - 现象: `_replayer->get_raw_time()` 返回的是 HHMM（分钟精度）而非 HHMMSS，
    与 `handle_tick` 里 tick 自身的 `action_time` 无关联，信号时间戳只能到分钟。
    同一段口径与 BUG-P04-010 的时间对齐说明同源。
  - 影响: signals.csv 的时序在同分钟内不可区分；跨 tick 的信号排序退化为插入顺序。
  - 分级: S4

- 标题: SelMocker::handle_replay_done 整数除零（沿用，不复活）
  - 状态: 待审核
  - 位置: `src/WtBtCore/SelMocker.cpp`（`handle_replay_done` 内的计数除法）
  - 现象: q02 已整类 `DISABLED_`，本轮 `r2_mocker_tick_fill_test.cpp#selHftUftTickUpdatesPriceMapWithoutStrategy`
    只调 `sel.handle_init / handle_session_begin / handle_tick / handle_bar_close / handle_session_end`，
    **显式不调用** `sel.handle_replay_done`。
  - 分级: S2（崩溃级，已归档，等主控裁定修复口径）

## B. 真正确诊「被接缝挡住」的面（本轮公开接口进不去）

说明：上一轮把整族 `bin/cache*FromBin` 都记成「private 且需要 `#define private public`」。
本轮实测**部分推翻**该结论：`init(cfg, notifier, IBtDataLoader*)`（HisDataReplayer.h L748）
是公开接缝，`get_kline_slice / checkTicks / get_last_tick` 会优先调用 `_bt_loader`，
所以 `cacheFinalBarsFromLoader`（L3589）、`cacheRawTicksFromLoader`（L3443）、
`cacheRawBarsFromBin` 的 loader 臂（L4527）都已在本轮从公开接口打通（见清单 A 节）。

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

## C. 公开可达但本轮因预算未做（不是接缝阻挡，下一轮可直接接）

- `cacheAdjustedStkBarsFromBin` L4236-4484 + `getAdjFactors` L4192-4236：约 **290 行**。
  可达路径本轮已验证：`mode="bin"` + `IBtDataLoader`（`loadFinalHisBars` 返回 false 才会退到
  `cacheRawBarsFromBin`）+ 复权后缀合约码 + `loadAdjFactors` 给因子；
  按 L4380-4420 的算法，qfq（`_exright==1`）且因子 `{19900101:1.0, 20200106:2.0}` 时
  期望收盘价应为 `{5.25, 5.75, 12.5, 13.5, 14.5}`（原始 `{10.5,11.5,12.5,13.5,14.5}`）。
  本轮未落笔原因：需要再确认 `SUFFIX_QFQ` 字符与 `CodeHelper::extractStdCode` 对三段码 + 后缀的拆解。
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

## D. 本轮实测的正面结论（可写回上一轮的「障碍清单」）

- `WTSTickData::create(WTSTickStruct&)`（`src/Includes/WTSDataDef.hpp` L884）是 public 池工厂，
  与 `release()` 同池，**可以**安全构造 tick；p08 反对的是伪造对象跨池 release，不适用于工厂产物。
- 四个 Mocker 的 `handle_tick` 均在 `public:` 段（IDataSink 重写），`_strategy == NULL` 时
  `on_tick_updated`/`on_tick`/`procTask` 一律守卫早退，`Hft/Uft::procTask()` 在 `_tasks` 为空时
  直接 `return`（不 lock、不 wait），因此**不起线程也不会挂死**。
- `mode="bin"` 且 `_his_dt_mgr` 指向空目录时，`load_raw_bars` 只 warn 返回 false，
  不抛异常、不 abort（`r2_replayer_loader_test.cpp#binModeWithoutLoaderOrDsbYieldsNoSlice` 固化）。
