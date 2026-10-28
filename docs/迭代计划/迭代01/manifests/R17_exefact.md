# 用例清单 — R17_exefact（CoverageTests_exefact / dir:r7_exefact，前缀 r17_）

3 个用例文件、37 条 TEST（启用 30、DISABLED 7），全部通过真实构建口径门禁：

```
cd build-cov && ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_exefact.dir/r7_exefact/r7_vwap_unit_test.cpp.o | tail -1
# 把 -o X -c Y 换成 -fsyntax-only <用例>；3/3 PASS（含 -Werror、-std=gnu++17）
```

本轮范围 = 全量报表（reports/coverage-20261027T113454Z-full.txt）UNCOVERED 段 WtExeFact/WtExecMon 两目录：
`WtDiffMinImpactExeUnit.cpp 260 + .h 9`、`WtMinImpactExeUnit.cpp 41`、`WtSimpDataMgr.cpp 125(可达 ~35)`、
`WtStockVWapExeUnit.h 38`、`WtStockMinImpactExeUnit.h 25`、`WtOrdMon.h 11`、`WtExecPorter.cpp 25`、
`WtTWapExeUnit.h 8`、`WtVWapExeUnit.h 8`（合计 ~550 行）。行号均为本轮 `Read/cat -n` 实读。

## 1) src/TestUnits/coverage/r7_exefact/r17_diff_minimpact_unit_test.cpp（18 条：16 启用 + 2 DISABLED）
生产文件 `src/WtExeFact/WtDiffMinImpactExeUnit.cpp`（522 行，missed 260）与 `.h`（missed 9）

| 用例 | 生产行段 |
| --- | --- |
| names_and_position_guards | .cpp:62-65, 71-74, 490-499, 503, 518-522 |
| buy_by_lots_mode0_and_on_trade_updates_diff | .cpp:346-350, 331-341, 353-357, 370-381, 427-431, 469-473, 266-275 |
| sell_mode_minus1_and_close_cap | .cpp:385-389, 422-426, 474-478 |
| mode1_buy_single_offset_sell_double_offset | .cpp:432-436, 448-449（双重扣减 BUG-R17-003） |
| entrust_span_blocks_then_fires | .cpp:353-355 |
| stale_tick_gate_blocks_recalc | .cpp:360-368 |
| byrate_rate_floor_and_sell_side_qty | .cpp:371-377 |
| mode2_auto_price_and_cancel_times_premium | .cpp:392-418 + on_order 撤单段 136-145 |
| mode2_zero_prices_fall_back_to_preclose | .cpp:411-415（mp=0 走 else，见 BUG-R17-007 注） |
| upper_limit_buy_not_cancellable_by_expire_scan | .cpp:455-459, 472 + WtOrdMon.cpp:61-62 跳过分支 |
| channel_ready_cancels_unmanaged_and_blocks_calc | .cpp:157-177, 318-322 |
| channel_ready_drops_unconfirmed_local_orders | .cpp:178-189 + WtOrdMon.h:85-88（clear_orders 内联） |
| channel_ready_consistent_state_no_action | .cpp:190-197, 337-341, 287-291 |
| on_entrust_fail_erases_and_refires | .cpp:285-296 |
| calc_flag_blocks_reentrant_calc | .cpp:311-316 + .h:186-216（CalcFlag 三内联体重入路径） |
| cancelled_order_stays_monitored_and_recancels | .cpp:117-146, 241-252（BUG-R17-001 证据：zombieHits=2） |
| DISABLED_init_pricemode_3_reads_past_name_table | .cpp:103-104（BUG-R17-002） |
| DISABLED_null_comm_info_crashes_price_offset_branch | .cpp:448（扩展 BUG-R7-006 至本文件） |

## 2) src/TestUnits/coverage/r7_exefact/r17_minimpact_gap_unit_test.cpp（7 条：全启用）
生产文件 `src/WtExeFact/WtMinImpactExeUnit.cpp`（601 行，n05 后残余 missed 41）+ `.h` CalcFlag

| 用例 | 生产行段 |
| --- | --- |
| clear_branch_forced_sell_when_net_flat_long_exists | .cpp:397-412（getPosition(code,true,1) 分流才可达 L405-411）, 414-416, 544-548 |
| entrust_span_blocks_second_order | .cpp:393-395 |
| stale_tick_gate_blocks_recalc | .cpp:419-424 |
| open_lots_raised_to_min_open_lots | .cpp:455-459 |
| mode_minus1_bid_and_zero_price_fallback | .cpp:492-496, 512-516 |
| mode2_cancel_times_price_premium | .cpp:481-488 + 157-184, 278-289 |
| calc_flag_blocks_reentrant_calc | .cpp:343-345 + .h:190-220 |

## 3) src/TestUnits/coverage/r7_exefact/r17_simpdata_porter_unit_test.cpp（12 条：7 启用 + 5 DISABLED）
生产文件 `src/WtExecMon/WtSimpDataMgr.cpp`（233 行，missed 125，可达片 ~35）与 `WtExecPorter.cpp`（161 行，missed 25）

| 用例 | 生产行段 |
| --- | --- |
| simpdata_init_without_store_section_returns_false | WtSimpDataMgr.cpp:84-88, 44-46 |
| simpdata_store_module_load_failure_returns_false | 48-58（module 空默认分支 L49-50 刻意不触达，见「留白」） |
| simpdata_reader_null_paths_and_noop_callbacks | 90-93, 120-123, 125-128, 130-133, 164-167, 178-181, 187-190 + .h:155 |
| porter_get_version_build_string | WtExecPorter.cpp:102-118 |
| porter_write_log_both_raw_paths_without_logger_init | 127-139 |
| porter_init_and_config_tolerate_missing_files | 48-60, 68-75（顺带 WtExecRunner.cpp:40-50, 61-68，R7 已测的 runner 行不重复记账） |
| porter_set_and_commit_positions_before_config | 147-161（顺带 WtExecRunner.cpp:181-194, 407-416） |
| DISABLED_simpdata_init_null_cfg_derefs | WtSimpDataMgr.cpp:84-87（BUG-R17-005） |
| DISABLED_simpdata_push_quote_without_session_info | 143, 153-161（BUG-R17-004；_cur_* 部分引 BUG-N05-001） |
| DISABLED_simpdata_basedata_mgr_null_runner | 95-103（BUG-R17-006） |
| DISABLED_porter_run_exec_spawns_threads | WtExecPorter.cpp:81-85（BUG-R17-008，禁线程） |
| DISABLED_porter_release_exec_stops_uninited_logger | 91-95（BUG-R17-008；根因同 R7 DISABLED_release_stops_uninited_logger） |

## 与 R7/n05 的分工（不重复搭）
- `WtMinImpactExeUnit` 主发单链与 `on_channel_ready` 三分支已由 n05 三条用例覆盖，本文件只打残余分支；
  `WtOrdMon.h` 内联（missed 11）与 `WtStockVWapExeUnit.h`(38)/`WtStockMinImpactExeUnit.h`(25)/
  `WtTWapExeUnit.h`(8)/`WtVWapExeUnit.h`(8) 属报表基线（20261027T113454Z）早于 R7/n05 收口运行的既有面，
  启用用例重跑即回收，不再为其加写重复用例（get_minOrderQty 面 = R7 四条启用用例已断言）。
- FakeExecCtx 每文件自带一份于匿名命名空间；Diff/Gap 两个 fake 分别带 `onAfterBuy` 重入钩子
  与 `posType` 分流的 `getPosition`（前者触达 CalcFlag 占用分支，后者触达清仓兜底 L405-411）。
- 不读挂钟：超时扫描走 `_ctx->getCurTime()` 注入时间；不起线程、无 socket/shm；
  tick/comm/variant 引用计数一一对应（沿用 n05 纪律）。
- 分月合约一律三段式 `SHFE.rb.2405`（CodeHelper.hpp:601-616 形制）。

## 留白面（估计行数，均已在 bugs 登记触发条件）
- WtDiffMinImpactExeUnit.cpp：on_tick 集合竞价过滤 L224-229（需真实 WTSSessionInfo，~4 行）、
  清仓/涨跌停组合分支少量路径未穷举（~10 行）。
- WtSimpDataMgr.cpp：initStore 成功面 61-81、handle_push_quote 正常面 135-161、
  get_kline_slice 合成面 192-232、get_date/get_min_time/get_secs 105-118（依赖 BUG-R17-004 面，
  且 `_cur_*` 值不确定不能写可证伪断言，引 BUG-N05-001）——合计 ~90 行（BUG-R17-008）。
- WtExecPorter.cpp：run_exec/release_exec 11 行（BUG-R17-008）。
- WtExecRunner.h(3)/WtSimpDataMgr.h(3) 内联 get_real_time/get_trading_day：同上不可断言，未动。
