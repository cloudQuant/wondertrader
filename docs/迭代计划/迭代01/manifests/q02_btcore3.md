# q02_btcore3 交付清单（迭代 01 · 第五波）

- Lane: q02_btcore3，靶子 `src/WtBtCore/`（第一方覆盖率 22.05%、WtBtCore 6152 行未覆盖）
- 产出目录: `src/TestUnits/coverage/q02_btcore3/`
- 专用目标: `CoverageTests_btcore`（只编 `src/WtBtCore/*.cpp`）。**未引用** `WtCore`/`WtUftCore`/`WtDtCore`
  的任何类（规避 37 个同名 `wtp::` 类的惰性抽取择一，BUG-L00-005），只用 `Includes/`、`Share/`
  与 `WtBtCore` 自己的 `IBtDataLoader`/`IDataSink`/`ICtaStraCtx`/`ISelStraCtx` 接口
- 未改任何 `CMakeLists.txt`；生产代码全程只读
- 语法门禁: **4/4 文件通过**，0 error 0 warning（Apple clang / macOS arm64），命令见文末
- 用例总数: **23 条**（19 启用 + 4 `DISABLED_`）
- 落盘: 仅 `std::filesystem::temp_directory_path()/q02_cta_*`、`q02_ctaint_*`、`q02_fac_*`、`q02_ctapm_*`
  （目录名带 pid，SetUp/TearDown 双向 `remove_all`）；仓库 `dist/` **只读复用**
  `dist/common/{sessions.json,stk_comms.json,holidays.json}`，未写入
- Bug 记录: `docs/迭代计划/迭代01/bugs/q02_btcore3.md`，**6 条**（BUG-Q02-001…006），状态均「待审核」，未修源码

## 新增文件与覆盖符号

### 1. `q02_cta_dump_incremental_test.cpp`（约 350 行，7 启用 + 1 DISABLED）
`CtaMocker.cpp` 的「结果落地 + 增量回测装载」整块（p03/p04/p07/p08 全部 0 覆盖）：
- `dump_stradata`(L88-L226)：持仓/明细/资金/信号/条件单四段 JSON；`_persist_data` 写盘守卫（L208-L225）
- `dump_chartdata`(L228-L339)：`_chart_code` 空时回落 `_main_code/_main_period`、
  `btchart.json` + `indice.csv` + `marks.csv` 三产物与空日志表头（L329-L337）
- `dump_outputs`(L341-L395)：`trades/closes/funds/signals/positions` 五份 csv 表头 + 拼接内容
- `load_incremental_data`(L454-L622)：五个 csv 的 `fs::exists` 真/假分支 + json 的
  positions/details/fund/signals/conditions 五段恢复 + 无 `details` 键分支 + 目录缺失的 warn 分支
- `on_session_end`(L1264-L1295) 的 `_pos_logs`/`_fund_logs` fmt 精确行、`volume==0` 的 continue
- 公开查询矩阵：`stra_get_position`(L1999)/avgpx(L2044)/profit(L2064)/first_entertime(L1937)/
  detail_entertime(L2074)/detail_cost(L2093)、`enum_position`(L1241) 的信号覆盖持仓合并
- `_strategy==NULL` 判空链：`handle_init/handle_session_begin/handle_session_end/handle_replay_done`
  （`on_bactest_end` 在 `ICtaStraCtx.h:113` 是空默认实现，不依赖策略指针）
- 关键手法：**不使用 `#define private public`**。private 的 `dump_*` 通过 public 的
  `handle_replay_done`(L661-L693) 驱动；`load_incremental_data` 本身就是 public(L248)
- `DISABLED_NullReplayerDerefAndBlockingHookFamily`：`_replayer==NULL` 时
  `stra_enter_long/short/stra_set_position/stra_get_price/stra_get_bars/stra_get_ticks/`
  `stra_get_last_tick/stra_sub_ticks` 的空指针解引用 + `install_hook/enable_hook(true)/step_calc` 阻塞

### 2. `q02_cta_replayer_integration_test.cpp`（约 330 行，4 启用 + 1 DISABLED）
`CtaMocker` 挂在**真实 `HisDataReplayer`**（`<base>/csv/SSE.STK.000001_d.csv` 三个交易日）上的集成面：
- `queriesDelegateToReplayer`：`stra_get_comminfo`(L598)/`stra_get_rawcode`(L645)/
  `stra_get_date/time/tdate`(L494-L506) 与 replayer 的口径一致性、`stra_get_bars(...isMain=true)`
  经 `get_kline_slice`(L2132) 登记主K线、`stra_get_last_tick` 无 tick 缓存返回 NULL、
  `stra_save_user_data/stra_load_user_data`(L678/L686) 含默认值、四个 `stra_log_*`(L652-L670)
- `marketSignalQueuesAndDumpsWithoutFill`：`stra_enter_long` 无价单 → `append_signal`(L1590) →
  `_sig_map`；`stra_get_position` 的「有信号未成交时回落取信号量」语义(L1999-L2012)、
  `stra_enter_short` 覆盖同一合约信号、落地后 `signals` 段可被 `load_incremental_data` 读回
- `limitPriceRegistersConditionOnly`：`stra_enter_long` 带 `limitprice`/`stopprice` 的
  条件单登记分支(L1323-L1347，`WCF_NEWPRICE` + `WCT_SmallerOrEqual`/`WCT_LargerOrEqual` +
  `COND_ACTION_OL`)、不动持仓、dump 的 `conditions.items` 带回 target/qty/action，
  再由第二个实例 `load_incremental_data` 恢复条件单（`_condtions`）
- `unknownCodeAndFlatQueriesAreSafe`：未知合约的 `stra_enter_long/short/stra_set_position` 早退
  (`commInfo==NULL`, L1307-L1312)、`handle_section_end`(L651-L659 清 `_price_map`)、
  `handle_session_begin`(L1215-L1239 释放 `_frozen`)
- `DISABLED_RealTickAndBlockingHookFamily`：`handle_tick/on_tick/on_tick_updated` 需要非空
  `WTSTickData*`（p08 已论证无安全构造入口，且 `release()` 有错池风险）；
  `proc_tick`(L695) 是 **private**（`CtaMocker.h:233` 位于 private 段），故成交回写不能在本 lane
  直连驱动；`onlyValid` 依赖 `SSE.STK` 是否被判 T+1（`stk_comms.json` 未显式配 `t1`）→ 待裁定

### 3. `q02_mocker_factory_cfg_test.cpp`（约 230 行，5 启用 + 1 DISABLED）
四个 Mocker 的策略工厂入口（p08 明确留白「正路径未覆盖」）：
- `CtaMocker::init_cta_factory`(L415-L452)、`SelMocker::init_sel_factory`(L234-L270)、
  `HftMocker::init_hft_factory`(L215-L250，含 L222-L226 三个撮合参数解析 + `log_info`)、
  `UftMocker::init_uft_factory`(L130-L165，含 L137-L141)
- 分支：`cfg==NULL` 早退、`module` 不存在、`module` 空串、cfg 完全无 key（默认值路径）、
  `DLLHelper::load_library` 返回 NULL 的早退（`createStrategy`/`free_library` 不可达）
- `repeatedFailedLoadLeavesNoHalfInitFactory`：同一 cfg 三轮装载全失败后，
  四个 Mocker 的析构（`CtaMocker.cpp:429`、`SelMocker.cpp:248`、`HftMocker.cpp:235`、
  `UftMocker.cpp:150` 的 `free_library` 分支）不解除引用半初始化的 `_factory._fact`
- `DISABLED_RealModuleLoadAndSelDivZeroFamily`：真实策略 so 装载（需外部产物 + BUG-L00-005）、
  **`SelMocker::handle_replay_done`(L305-L307) 的 `_total_calc_time / _emit_times` 整数除零**
  （`_emit_times` 构造为 0，`SelMocker.cpp:43`；CtaMocker 同位置有判空 `CtaMocker.cpp:665`）、
  `install_hook/enable_hook(true)` 的条件变量

### 4. `q02_cta_position_query_matrix_test.cpp`（约 300 行，5 启用，0 DISABLED）
`load_incremental_data` 恢复出**带明细的持仓**后，把 `stra_get_position` 的 userTag 过滤循环
（`CtaMocker.cpp:2017-L2043`，p03 从未走到）与配套明细查询吃干净：
- 两合约四条明细（多 60@10.5 + 多 40@11.0；空 -30@5.5 + 多 20@5.2）的
  `stra_get_position(code, onlyValid, userTag)` 逐 tag 精确量、不匹配 tag 返回 0、空 tag 取全量
- `stra_get_detail_entertime/cost` 成对命中、`stra_get_first/last_entertime`、`stra_get_last_exittime`
- `stra_get_position_avgpx` 的加权口径（含**空头净持仓下的有符号均价** 6.1 = -61 / -10）
- `onlyValid` 与全量一致（`_frozen` 不被持久化，BUG-Q02-003 的可观测面）
- `positionOverridesSignalWhenBothExist`：`stra_enter_long` 在已有净持仓时按 `curQty+qty`
  追加目标量（L1317-L1321，100+500=600），`enum_position` 里 `_sig_map` 后写覆盖 `_pos_map`
- `loadedStateSettlesIntoCsv`：装载态直接结算 -> `funds.csv` 行
  `7.00,2.50,8.00,1.50`（dynbalance = 总盈 + 浮盈 − 费用）、`positions.csv` 两行、
  `<name>.json` 回写 `shortA/-30/total_fees 1.5`

## 有意未覆盖 / 留给下一波（不虚构断言）

- `HisDataReplayer.cpp` 的 2672 行缺口**本波没有正面切入**，原因是可驱动面已被 p04/p07 收走：
  - `cacheRawBarsFromBin/cacheAdjustedStkBarsFromBin/cacheIntegratedFutBarsFromBin/`
    `cacheRawTicksFromBin/cacheRawOrd*FromBin`：`loadStkAdjFactorsFromFile`(L335)、
    `getAdjFactors`、这些 `cache*FromBin` 在 `HisDataReplayer.h` 全部是 **private**
    （public/private 分界见 L374/L378/L739/L1041），本波禁用 `#define private public`，
    没有可绕过它们的公开入口（`init` 的 `mode=="bin"` 分支需要 `HisDataMgr` 动态库 + 真实 wtd 目录）
  - `cacheRawTicksFromCSV`(L3470-L3588) 的 csv 分支源码 L3509 已无条件 `return false`，
    L3511-L3584 是注释掉的死代码（BUG-P07-007 已登记，不重复）
  - level2 装配：`checkOrderDetails/Queues/Transactions`(L2702/L2744/L2786) 在 csv 模式直接
    `return false`(L2721/L2763/L2805)，`IBtDataLoader` 也没有 level2 接口 → 只有 bin 能喂
- `get_kline_slice` 的 `period=="h"/"f"` 重采样分支（L2190-L2203）需要
  `WTSDataFactory::extractKlineData` 的 SD0930 分组口径才能写确定值断言，
  本波预算内未做；顺带发现 "f"(半小时线) 的 `realTimes` 被赋成和 "h" 一样的
  `PERIOD_TIMES_HOUR` → **BUG-Q02-005（待裁定）**
- `SelMocker.cpp:59-L213`（`dump_stradata`/`dump_outputs`，约 150 行）：唯一公开入口
  `handle_replay_done` 有整数除零 UB（BUG-Q02-002，x86 SIGFPE），按硬约束① 整类收进 DISABLED_。
  **请主控裁定**：若确认只跑 arm64（UDIV 不陷入），可以放行 `q02_mocker_factory_cfg_test.cpp`
  里的 `DISABLED_RealModuleLoadAndSelDivZeroFamily` 第一句，那 150 行就能吃到
- `CtaMocker.cpp` 的 `on_bar/on_schedule/on_calculate/step_calc/install_hook`(L1054-L1220)
  与 `MatchEngine`/`ExecMocker`/`EventNotifier`/`HisDataMgr`/`WtHelper`：l06/n04/p03 领域，未触碰
- 阻塞路径：`run()`、`step_calc`、`install_hook`、`enable_hook(true)`、条件变量、线程、join、socket
  **零条启用用例进入**（`handle_replay_done` 的阻塞分支由 `_has_hook=false` 天然跳过）

## 验证命令（逐文件已执行，4/4 rc=0，0 error 0 warning）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/q02_btcore3/<file>.cpp
```

## 运行/评审注意（交主控裁决）

- 链接需求与 l06/p04/p07/p08 一致：`WtBtCore` + `WTSTools`（`WTSBaseDataMgr`/`WTSHotMgr`）+ `WTSUtils`。
  `makeLocalOrderID` 由库内 `HftMocker.cpp` 提供，测试未重复定义。
- 未使用 `EventNotifier`/`HisDataMgr`/`MatchEngine` 实例，不触发 MQ、动态库、线程。
- `q02_cta_replayer_integration_test.cpp` 与 `q02_cta_position_query_matrix_test.cpp`
  依赖 `dist/common/{sessions.json,stk_comms.json,holidays.json}` 可只读访问
  （`stk_comms.json` 的 `SSE.STK`/`SZSE.STK` 指向 `SD0930`，`pricetick=0.01`，`volscale=1`，
  **未配 `t1`** —— 这是本波不断言 `onlyValid` 冻结口径的根因）。
- `fees` 配了空串 → `calc_fee` 未登记品种返回 0，因此资金落地行完全可推导；
  若主控把默认费用模板注入底座，`q02_cta_replayer_integration_test.marketSignalQueuesAndDumpsWithoutFill`
  与 `q02_cta_position_query_matrix_test.loadedStateSettlesIntoCsv` 的 `funds.csv` 精确串需要重算。
- 文件 1 的 `strategyDumpRoundTripsIntoSecondMocker` 做了「dump → 二次装载 → 查询口径一致」闭环，
  源码若按 BUG-Q02-003 补上 `_desprice/_sigtype/_frozen` 的持久化，该用例仍应通过（只加了字段）。
- 断言不含随机数、不读墙钟/时区；一条用例一个进程；用例间零共享状态（每例自建 temp 目录）。
