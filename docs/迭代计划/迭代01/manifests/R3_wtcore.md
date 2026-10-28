# R3 用例清单（WtCore 第三波）

lane: R3 · 生成时间: 2026-10-27
用例目录: `src/TestUnits/coverage/q01_wtcore3/`（拆分表 `dir:q01_wtcore3` → 目标 `CoverageTests_wtcore`）
门禁: `build-cov` + `ninja -t commands ...CoverageTests_wtcore.dir/q01_wtcore3/q01_hft_ctx_test.cpp.o | tail -1`，
取整行去掉 `-MD/-MT/-MF/-o/-c` 后加 `-fsyntax-only`（未自造 flags），逐文件 `0 errors`。

## 文件

| 文件 | 启用用例 | DISABLED | 门禁 |
|---|---|---|---|
| `r3_cta_position_persist.cpp` | 19 | 0 | 0 error |
| `r3_trader_risklimit.cpp` | 7 | 0 | 0 error |

（磁盘口径：`grep -c '^TEST_F'` 19 + 7 = 26 条启用，0 条 `DISABLED_`；本轮没有需要整条收进 DISABLED 的崩溃族，
`CtaStraBaseCtx::on_tick` 的空 session 崩溃与 p01 已登记的 `DISABLED_P01CtaCrashClass` 同族，只写进 bugs 不重复开。）

复用替身：`p01_wtcore_stractx/p01_cta_env.h`（WtCtaEngine/IBaseDataMgr/IHotMgr/WTSTickData 装配）、
`p11_wtcore_traderadapter/p11_trader_fixture.h`（FakeTraderApi/RecordingSink/FakeBaseDataMgr/P11TraderTest）。
本轮只补派生访问器（protected 簿记只读窥视），未新增任何跨 lane 的头文件。

## 用例 → 生产文件:行段

### r3_cta_position_persist.cpp → `src/WtCore/CtaStraBaseCtx.cpp`

| 用例 | 行段 |
|---|---|
| `init_outputs_writes_seven_headers_then_appends_on_reopen` | L77-188（7 个 csv 的新建/`seek_to_end` 两条支路）、L283-291 |
| `flat_signal_on_tick_opens_detail_and_pushes_portfolio_delta` | L795-818、L1411-1433、L1470-1479、L1534-1575（反手建仓 + `handle_pos_change`）、L190-207 |
| `same_direction_signal_appends_second_detail_and_keeps_avg_price` | L1434-1469（同向加仓 + T1 判定 + 手续费 + `log_trade`）、L1867-1885 |
| `partial_close_splits_detail_and_books_closeprofit` | L1470-1524（`min(dInfo._volume,left)` 拆分、盈亏与手续费按明细结算）、L209-218 |
| `set_position_through_zero_reverses_side_with_single_detail` | L1470-1524 + L1535-1563（剩余量反手）、L1811-1822 |
| `t1_entry_freezes_position_until_next_session_begin` | L1438-1445、L1539-1545（`isT1()` 冻结）、L1018-1038（释放 + 不释放两条）、L1840-1851 |
| `enum_position_for_execute_marks_signal_triggered_and_skips_portfolio` | L1041-1071（`bForExecute` 置 `_triggered`）、L807-815（`bFireAtOnce=false` 支路） |
| `condition_ol_and_cl_fire_on_matching_tick` | L825-876（COND_ACTION_OL）、L877-887（COND_ACTION_CL）、L1146-1168、L1253-1277 |
| `condition_os_cs_and_sp_fire_on_matching_tick` | L888-915（OS/CS/SP 三支）、L1202-1227、L1305-1329、L1360-1392 |
| `on_schedule_after_main_bar_close_clears_conditions_and_stamps_settime` | L937-1016（清条件单、`on_calculate`、`_last_cond_min`、`_last_barno++`）、L1972-1989 |
| `save_then_load_roundtrip_restores_positions_signals_conditions_userdata` | L495-643（save_data 五段）、L277-494（load_data 五段回读）、L1073-1109 |
| `broken_and_empty_data_files_are_ignored_without_state_change` | L253-261、L263-267（坏/空 json 早退）、L283-297 |
| `dyn_profit_tracks_best_and_worst_price_of_each_detail` | L752-793、L1935-1964（flag 0/1/-1/2/-2 五个取值） |
| `chart_apis_reject_outside_schedule_and_dump_chart_info_writes_json` | L1966-2063（4 个拒绝分支）、L679-750（rtchart.json） |
| `modified_userdata_is_flushed_on_tick_and_session_begin` | L219-244、L246-275、L930-934、L1034-1038、L1746-1759 |
| `on_bar_marks_tag_closed_and_only_notifies_subscribed_keys` | L647-666（含 `newBar==NULL` 早退与 `key==_main_key` 支路） |
| `full_close_drops_all_details_and_zeroes_dynprofit` | L1477-1532（`count` 逐条 erase）、L1490-1492 |
| `slippage_shifts_entry_up_and_exit_down` | L1447-1450、L1474-1475（`_slippage` 双向偏移） |
| `close_consumes_details_fifo_and_eroses_finished_ones` | L1481-1532（FIFO 扣减 + 清理） |

顺带覆盖（经由 `do_set_position` 的 `bFireAtOnce`）：
`src/WtCore/WtCtaEngine.cpp` L429-488（`handle_pos_change`：过滤、换月代码、`_pos_mtx` 建仓、风险缩放、
`append_signal` + `save_datas`）、`src/WtCore/WtEngine.cpp` L367-471（`save_datas` 的 positions/riskmon 段）。

### r3_trader_risklimit.cpp → `src/WtCore/TraderAdapter.cpp`

| 用例 | 行段 |
|---|---|
| `order_freq_window_blocks_only_for_the_raw_code_key` | L609-661（`_order_total_limits` 未越界 + L631-658 频率窗口、`times = cnt-sIdx-1` 的边界算术） |
| `cancel_freq_window_is_keyed_by_order_raw_code` | L544-596（L566-593 撤单频率窗口）、L1244-1291（`doCancel` + L1286 缓存写入、L1278 `SpinLock`） |
| `save_data_writes_long_short_positions_and_fund_block` | L303-390（持仓 long/short 四段 + funds 段，含 `ayFunds==NULL/size()==0` 支路）、L1514-1520（`onRspAccount` 带动）、L237-279（`initSaveData` 建目录） |
| `four_wrappers_stamp_direction_offset_price_and_flag` | L487-520（usertag 组装 + `orderInsert<0` 返回 UINT_MAX）、L1331-1404（四个包装的方向/开平/价格类型/flag）、L1293-1329（`cancel(stdCode,isBuy,qty)` 按方向筛选、空代码全撤） |
| `buy_splits_by_rule_cap_and_single_order_max_volume` | L663-762（qty==0 早退、非交易时段拒绝、`_limit_l`/`_limit` 截断、`unitQty` 拆单循环）、L1049-1093（`sell` 的空头开仓镜像） |
| `buy_skips_open_rule_when_daily_cap_already_consumed` | L718-724（`statItem.l_openvol >= _limit_l` → `continue`）、L1780（成交回报累加） |
| `position_and_order_tables_read_seeded_state` | L428-466（`getPosition` 全量/可用两口径）、L454-465（`enumPosition`）、L467-485（`getOrders` 空表/按代码） |

## 本轮没打到的面（接缝 / 平台限制，供覆盖排除清单评审）

| 生产位置 | 为什么本轮打不到 |
|---|---|
| `CtaStraBaseCtx.cpp` L1577-1675（`stra_get_bars` / `stra_get_ticks` / `stra_get_last_tick` / `get_kline_slice`） | 全部经 `WtCtaEngine::_data_mgr`（`WtDtMgr*`）读历史；`_data_mgr` 只有 `WtEngine::init()` 才赋值，而 `init()` 会拉起 `WtDtServo`/行情线程与真实共享内存段 → 禁止起线程的用例无法安全调用（沿用 p01 的 `_data_mgr=NULL` 约定） |
| `CtaStraBaseCtx.cpp` L1344-1351（`stra_get_day_price`）→ `WtEngine::get_day_price` L709-750 | 依赖 `_tick_sub_map`/`_price_map` 之外的日线簿记，p02 已覆盖空簿返回 0；带 tick 的分支需要 `WtDtMgr` |
| `CtaStraBaseCtx.cpp` L1709-1725（`stra_get_fund_data`） | 已由 p01 覆盖空簿口径，此处不再重复 |
| `TraderAdapter.cpp` L99-208（`init` 的 module/柜台Dll 装配） | 走 `DLLHelper::create_library` 真实厂商 dll（CTP/XTP/Femas）；p11 已打「配置解析 + 无 module 返回 false」支路，剩余部分属排除清单候选 |
| `TraderAdapter.cpp` L588-591 / L653-656（频率窗口里过期时间戳的 `cache.erase`） | 缓存时间戳由 `TimeUtils::getLocalTimeNow()` 在 `doEntrust/doCancel` 内部写入，公开接口无法注入历史时间点；需要主控给 `TimeUtils` 做可注入时钟（或把 `TimeCacheList` 提到测试可见 seam） |
| `TraderAdapter.cpp` L1426-1513（`onLoginResult`/`onRspEntrust` 的 `err != NULL` 分支 L1450-1454） | `WTSError::create` 属 `Includes/WTSError.hpp`，可编；但 `onRspEntrust` 的失败支路要求 `_stat_map` 就绪 + 已存在的 entrust 记录，属 p11 已登记的崩溃族，未重复触发 |
| `WtEngine.cpp` L285-361（`init`/`init_outputs`/`on_session_end`）、L1086-1130（`push_task`/`task_loop`）、L1143-1186（`init_riskmon`） | `init` 需要 `WtDtMgr` + `EventNotifier` + 真实 `_adapter_mgr`；`task_loop`/`_pool` 是线程；`init_riskmon` 走 `DLLHelper` 加载 `WtRiskMonFact` 插件（hidden SHARED，无 C++ 链接缝，见 BUG-L00-002） |
| `WtCtaEngine.cpp` L61-114（`run`）/L116-235（`init`/`on_session_*`）/L490-560（`on_tick` 分发） | `run()` 起 `WtCtaRtTicker` 线程并读 `_data_mgr->reader()`；`on_tick` 分发需要 `_ctx_map`/`_exec_mgr` 真实装配。留给主控的执行器插件链接缝（`CoverageTests_exeplugins`） |
| `HftStraBaseCtx.cpp` 全量（987 行，missed 356） | 本轮预算内未开工：hft 的队列/快速通道分支要先读 `q01_hft_ctx_test.cpp` 的替身 + 重写 `on_tick` 簿记，成本高于本波剩余配额；已排在下一波第一位 |
| `WtEngine.cpp` L1160-1180（`free_library`） | 厂商 dll 释放路径，属排除清单候选 |
