# q01_wtcore3 交付清单（第五波 · WtCore Sel/Hft 策略上下文）

- 工作流/lane: `q01`，目标二进制 `CoverageTests_wtcore`（只编 `src/WtCore/*.cpp` + WTSTools/WTSUtils）
- 简报: `docs/迭代计划/迭代01/manifests/api-brief-L07.md`
- 目录: `src/TestUnits/coverage/q01_wtcore3/`（已在 CMake `wtcore` lane 登记 `dir:q01_wtcore3`，未改任何 CMakeLists）
- 日期: 2026-10-30

## 新增文件

| 文件 | 行数级 | TEST_F（启用/禁用） | 主题 |
| --- | --- | --- | --- |
| `q01_sel_env.h` | ~360（非 .cpp，不入 GLOB） | — | 共用替身 |
| `q01_sel_query_test.cpp` | ~180 | 8 / 0 | Sel 查询透传、参数存取、日志、空簿零值 |
| `q01_sel_savedata_test.cpp` | ~200 | 4 / 0 | Sel `save_data/load_data` 往返 + 手写完整夹具 + `init_outputs`/userdata |
| `q01_sel_callback_test.cpp` | ~210 | 7 / 0 | Sel `on_bar/on_tick/on_schedule/on_session_begin/end/stra_sub_ticks` |
| `q01_sel_position_test.cpp` | ~180 | 4 / 1 | Sel `stra_set_position` 前置校验 + `append_signal` + 崩溃类收口 |
| `q01_hft_ctx_test.cpp` | ~200 | 7 / 0 | Hft `HftStraBaseCtx`（裸引擎，不触 `_trader`） |
| 合计（5 .cpp） | ~970 | **30 启用 / 1 DISABLED** | |

替身（全部封在 `namespace q01sel`，规避与 p01/p11 全局同名 `FakeBaseDataMgr/FakeHotMgr` 的 ODR 冲突）：
`Q01TempEnv`（`WtHelper::setGenerateDir` 重定向到 `temp_directory_path()/q01sel_<tag>_<pid>/`，析构 `remove_all`）、
`BaseDataMgr`（`IBaseDataMgr` 替身）、`HotMgr`（`IHotMgr` 14 纯虚全实现，规则一律空）、
`SelEngine`（`WtSelEngine` 派生，仅把 protected 的 `_base_data_mgr/_hot_mgr` 接线，不 init/不 run/不起线程，`_data_mgr` 故意 NULL）、
`SelCtx`（`SelStraBaseCtx` 具体子类，只补纯虚 `on_bar_close`，并记录 `on_tick_updated/on_strategy_schedule`）、
`HftEngine/HftCtx`（Hft 侧裸引擎与 `get_inner_code` 的 `using` 白盒暴露）。
`HftStraBaseCtx` 自身已实现 `IHftStraCtx` 全部纯虚，直接实例化。

## 覆盖符号

### SelStraBaseCtx.cpp（930 未覆盖 → 主力）
- 生命周期/装配：`on_init`→`init_outputs`（:59-140 建 5 个 csv）、`load_data`（:229 文件缺失早退 + :251-377 fund/positions/details/signals 完整解析分支）、`load_userdata`（:198 缺失早退 + :210-227 读回）
- 落盘：`save_data`（:379-481 positions/details/fund/signals 四段 JSON）、`save_userdata`（:170-196 rj 落盘）+ **save↔load 往返**（信号恢复、仓位恢复）
- 回调：`on_bar`（:485 NULL 早退 + :490-499 `period+times` 拼接 + `_kline_tags` + `on_bar_close`）、`on_tick`（:555 `_price_map` 写入 + 信号分支（无交易时段不触发）+ `update_dyn_profit` + `on_tick_updated` 分发 + `_ud_modified` 落盘）、`on_schedule`（:588-634 `_is_in_schedule` 置位期 `stra_get_date/time` 命中调度值 + 未命中持仓 `autoexit` + `_emit_times`）、`on_session_begin`（:636 解冻 `frozen_date<uTDate`）、`on_session_end`（:682 写 positions/funds + `save_data`）
- 查询透传：`id`、`stra_get_tdate/date/time`（非调度 & 调度两态）、`stra_get_price`（本地表命中）、`stra_get_comminfo`（命中/NULL）、`stra_get_rawcode`（无规则返回空）、`stra_get_sessinfo`（命中/NULL）
- 空簿与夹具仓位查询族：`stra_get_position`（含 `bOnlyValid`/`userTag` 命中与未命中）、`stra_get_position_avgpx`、`stra_get_position_profit`、`stra_get_first_entertime`、`stra_get_last_entertime`、`stra_get_last_exittime`、`stra_get_last_enterprice`、`stra_get_last_entertag`、`stra_get_detail_entertime/cost/profit`（flag 0/1/-1/2/-2）
- 资金：`stra_get_fund_data`（flag 0/1/2/3/default；含从夹具 fund 段读入的 total_profit/dynprofit/fees）
- 下单前置校验：`stra_set_position`（:734 品种缺失拒绝 / `canShort()` 拒绝 / 目标==当前早退 / 正常 `append_signal`）、`append_signal`（:770 `_sig_map` 落地 + `log_signal` + `save_data`）
- 订阅/枚举：`stra_sub_ticks`（:978 `_tick_subs` 去重 + `WtEngine::sub_tick`，无规则走 else 分支安全）、`enum_position`（:659 仓位∪信号视图）
- 日志：`stra_log_info/debug/warn/error`（:1046-1064）
- 顺带真实链路（非本 lane 指标）：`WtEngine::get_commodity_info/get_contract_info/get_rawcode/get_session_info/sub_tick`

### HftStraBaseCtx.cpp（655 未覆盖 → 可确定子集）
- `id`（:55）、`name`、`setTrader`（:60）、`stra_get_date/time/secs`（透传 `WtHftEngine`）
- `on_init`→`init_outputs`（:65 `_data_agent=true` 建 4 csv / `=false` 早退）+ `load_userdata`
- `on_tick/on_order_queue/on_order_detail/on_transaction/on_bar`（:143-186 仅处理 `_ud_modified`→`save_userdata`，NULL 数据指针安全）
- `save_userdata`（:726 rj 落盘）+ `load_userdata`（:754 缺失早退 + 读回）
- `get_inner_code`（:202 未映射原样返回，经 `HftCtx::using` 白盒）
- `stra_save_user_data/stra_load_user_data`（:711-724）
- `stra_buy`（:211-259 非规则分支 `getContract==NULL`→`log_error` 返回空 `OrderIDs`，不 deref `_trader`）
- `stra_log_info/debug/warn/error`

## 跳过目标及原因

| 目标 | 原因 |
| --- | --- |
| `SelStraBaseCtx::do_set_position`（:786-932，最大单块） | 只能由 `on_tick`（信号 + 交易时段）驱动；末尾 `_engine->handle_pos_change`→`WtSelEngine`（:427-488）→`WtEngine::append_signal`→`get_cur_price`（:672）→裸引擎 `_data_mgr` 解引用段错误（同源 BUG-P01-002）。属崩溃类，整类收进唯一 `DISABLED_SelCrashClass_null_history_mgr`，启用零残留。需完整引擎（exec_mgr/port_fund/data_mgr）才能启用，与 p01 同因。 |
| `SelStraBaseCtx::stra_get_day_price/stra_get_bars/stra_get_ticks/stra_get_last_tick` | 分别解引用 `_data_mgr->grab_last_tick`（WtEngine:720/594/599）；`_data_mgr=NULL` 段错误，收进同一 DISABLED。 |
| `SelStraBaseCtx` 的 `isT1` 冻结拒绝分支（:755-765） | 需 `TM_LongT1` 品种 + 既有仓位 + `qty<冻结量` 的夹具组合，构造成本高、收益低，本 wave 未安排；其余三条 `stra_set_position` 拒绝分支已覆盖。 |
| `HftStraBaseCtx` 全部下单/查仓/撤单族（`stra_buy/sell/cancel/enter_*/exit_*` 成功路径、`stra_get_position/price/undone`、`on_trade/on_order/on_position/on_channel_*`） | 均 deref `_trader`（`TraderAdapter*`）；裸 `_trader=NULL` 段错误，且需 p11 那样真实柜台回报链驱动，超出本 lane（p11 已单独覆盖 TraderAdapter 侧）。仅测 `stra_buy` 未注册合约的 `log_error` 早退分支。 |
| 引擎/上下文的任何 `run()/start()`/线程/条件变量 | 一律不进入启用用例（约束 2）。 |
| `WtEngine/WtCtaEngine` 非线程分支 | 本 wave 预算（60 调用）内已把 Sel/Hft 两个最大空块做出可确定覆盖；引擎非线程分支留待下一 lane（n02/p02 已部分覆盖）。 |

## 待审核缺陷（bugs/q01_wtcore3.md）

- BUG-Q01-001（Major）：`SelStraBaseCtx::log_close`（:159-168）守卫 `_close_logs` 却写 `_trade_logs`，`closes.csv` 永不落数据。
- BUG-Q01-002（Minor）：`SelStraBaseCtx::load_data`（:268）对 `positions` 无 `HasMember` 守卫——**同源，引用已有编号 BUG-P01-004**。
- 引用：`_data_mgr=NULL` 崩溃与 do_set_position 段错误引用已有 BUG-P01-002 / BUG-P11-001 系。

## 门禁结果（逐文件，写完即跑）

仓库根执行：
```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/q01_wtcore3/<file>.cpp
```
5/5 个 `.cpp` **全部 0 error 0 warning**（末轮一次性 batch 复核通过）。`q01_sel_env.h` 作为 5 个 TU 公共头被完整检查（无独立编译单元）。
首轮暴露并已修：`namespace q01sel` 内漏 `USING_NS_WTP`（wtp 类型不可见）、`SelCtx` 不可拷贝（unique_ptr 成员 + 用户析构，`loadCtx` 改 `prep`+就地构造）、`get_inner_code` protected（`HftCtx::using` 暴露）。
未运行 cmake/ninja/测试二进制，未自行编译运行用例（运行与判定交主控）。

## 约束遵守说明

1. 崩溃/挂死类输入整类收进同一 `DISABLED_SelCrashClass_null_history_mgr`（Sel 6 个入口），启用用例零残留；Hft 侧 `_trader` 依赖分支根本不进入。
2. 无阻塞路径：`SelEngine/HftEngine` 只构造、不 init/不 run；`WtEngine::release()` 仅在析构 join 从未创建的 `_thrd_task`。
3. 白盒访问用替身/派生 `using`（`HftCtx::get_inner_code`、`SelCtx::on_bar_close`），未用 `#define private public`。
4. 显式 include：`boost/core/noncopyable.hpp`、`Includes/IBaseDataMgr.h`、`IHotMgr.h`、`WTSContractInfo.hpp`、`WTSSessionInfo.hpp`、`WTSDataDef.hpp`、`WTSTypes.h`、`IHftStraCtx.h`、`WTSTradeDef.hpp` 等（BUG-L00-004 预防）。
5. 所有断言的期望值均由 `SelStraBaseCtx.cpp`/`HftStraBaseCtx.cpp`/`WtEngine.cpp` 实现推导；未为变绿迁就返回值（BUG-Q01-001 明确按“应写 closes.csv”登记而非断言现状）。
6. 落盘仅在 `temp_directory_path()/q01sel_*_<pid>/`，`Q01TempEnv` 析构清理；`dist/` 全程只读；未断言墙钟/时区（引擎时间固定 `set_date_time(20250110,931,…)`）。
7. 只引用 `WtCore`/`Includes`/`Share`/`WTSUtils`，未引用 `WtUftCore/WtBtCore/WtDtCore` 同名类（BUG-L00-005）；替身加 namespace 规避跨 lane 全局符号 ODR。
8. 未修改任何 `src/**` 生产代码与 `CMakeLists.txt`。

## 剩余

无阻塞项。后续可选（下一 lane）：补 Sel `isT1` 冻结拒绝分支；Sel/Hft 完整引擎装配下的 `do_set_position` 实体链（需 exec_mgr + port_fund + data_mgr）。
