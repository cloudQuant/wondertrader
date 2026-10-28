# 用例清单 — R7_exefact（CoverageTests_exefact / dir:r7_exefact）

5 个用例文件、112 条 TEST（启用 101、DISABLED 11），全部通过真实构建口径门禁：

```
cd build-cov && ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_exefact.dir/n05_exec_factories/n05_exe_fact_test.cpp.o | tail -1
# 把 -o X -c Y 换成 -fsyntax-only <用例>；5/5 PASS（-Werror、-std=gnu++17）
```

行号全部是本轮实际读到的（`cat -n` 输出），格式 `用例文件 :: 用例名 → 生产文件:行段`。

## 1) src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp（28 条：26 启用 + 2 DISABLED）
生产文件 `src/WtExeFact/WtVWapExeUnit.cpp`（513 行，missed 372）与 `.h`

| 用例 | 生产行段 |
| --- | --- |
| names_and_foreign_code_target_ignored | WtVWapExeUnit.cpp:89-97, 496-499 |
| init_missing_distribution_file_returns_early | 110-132（缺文件早退 128-131） |
| init_loads_distribution_and_first_bucket_buys | 134-146, 299-312, 341-345, 364-397, 434-446 |
| bucket_zero_falls_back_to_min_open_lots | 371-388（max 取 minopenlots 385） |
| calc_price_mode_1_buy_uses_bid_price | 395-402 |
| calc_price_mode_2_sell_uses_bid_and_sign_trick | 385, 392, 403-406, 434-440 |
| buy_price_capped_to_upper_limit | 410-413, 420-426 |
| sell_price_floored_to_lower_limit | 427-432 |
| last_bucket_uses_showhand_price | 374-383, 407-409 |
| stale_tick_gate_blocks_second_order | 286-297, 364-370 |
| on_entrust_ignores_unknown_order | 286-292 |
| clear_target_dbl_max_exits_without_order | 46-55, 319-320, 348-362(不可达, BUG-R7-004) |
| channel_ready_clears_unconfirmed_local_orders | 206-216 |
| channel_ready_cancels_unmanaged_and_blocks_calc | 185-205, 314-318 |
| cancel_reports_decrement_cancel_cnt | 148-160 |
| cancel_report_refires_at_once_with_cancel_times_offset | 162-177, 448-451, 456-471, 485-493 |
| fire_at_once_price_mode_1_takes_ask_price | 459-468（与 401 口径冲突, BUG-R7-014） |
| fire_at_once_upper_limit_cap | 472-476 |
| on_order_unknown_id_and_filled_report | 150-151, 162-165 |
| on_tick_rejects_null_and_foreign_code | 226-229, 511-513 |
| first_tick_skips_calc_when_target_already_covered | 231-256 |
| same_direction_live_order_blocks_this_round | 336-340 |
| opposite_live_order_is_canceled_before_firing | 321-335 |
| calc_without_tick_exits | 311-312, 341-345 |
| timestamp_after_close_clamps_to_last_bucket | 56-88（calTmStamp 上界 79-81） |
| init_logs_total_seconds_from_begin_end_window | 56-60, 122-123 |
| DISABLED_init_zero_total_times_divide_by_zero | 120（BUG-R7-002） |
| DISABLED_short_distribution_file_out_of_range_read | 372（BUG-R7-003） |

## 2) src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp（27 条：25 + 2）
生产文件 `src/WtExeFact/WtTWapExeUnit.cpp`（799 行，missed 325）

| 用例 | 生产行段 |
| --- | --- |
| names_and_init_derived_span | 108-121, 130-169（calTmSecs 87-92） |
| splits_remaining_qty_evenly_across_left_times | 544-553, 675-711 |
| rounds_half_away_from_zero_when_remainder_not_even | 702-706 |
| tiny_diffqty_floors_to_min_open_lots | 702-706（max 取 minopenlots） |
| sell_side_keeps_sign_and_splits_evenly | 675-706（符号保持 705）, 715, 726-729, 759-763 |
| last_slice_showhand_buys_all_with_5_tick_premium | 675, 694-701, 732-735 |
| fire_span_blocks_second_tick_recalc | 385-420（间隔判定 415） |
| price_mode_1_buy_uses_bid_and_mode_2_uses_ask | 718-729 |
| price_offset_shifts_buy_up_and_sell_down | 736-739 |
| zero_last_price_falls_back_to_preclose | 740-742 |
| limit_prices_cap_the_order_price | 744-757 |
| calc_before_channel_ready_does_nothing | 551-553, 300-309 |
| calc_exits_without_tick_then_blocks_on_live_order | 616-621, 626-632 |
| pending_cancel_blocks_calc | 262-283, 577-583 |
| opposite_live_order_canceled_first_without_new_order | 589-614 |
| channel_ready_drops_unconfirmed_local_orders | 284-298 |
| stale_tick_gate_and_entrust_reject | 445-459, 655-665 |
| on_order_drains_cancel_cnt | 183-201 |
| cancel_report_refires_with_cancel_times_price_step | 212-233, 468-472, 483-509, 527-536 |
| fire_at_once_mode_1_and_2_pricing | 488-509 |
| fire_at_once_unknown_price_mode_has_no_preclose_fallback | 490-509（缺 740-742 的兜底, BUG-R7-014） |
| unknown_price_mode_falls_into_counter_price | 726-729 |
| tick_guards_and_noop_callbacks | 316-320, 329-333, 432-435, 779-788 |
| repeated_target_is_ignored | 785-798, 675-706 |
| clear_target_with_flat_position_does_nothing | 74-79, 585-587, 633-653(不可达, BUG-R7-004) |
| DISABLED_init_zero_total_times_divide_by_zero | 166（BUG-R7-002） |
| DISABLED_null_commodity_info_crashes_price_offset_branch | 136-139 vs 734/738（BUG-R7-006） |

## 3) src/TestUnits/coverage/r7_exefact/r7_stock_vwap_unit_test.cpp（20 条：18 + 2）
生产文件 `src/WtExeFact/WtStockVWapExeUnit.cpp`（836 行，missed 444）+ `.h`

| 用例 | 生产行段 |
| --- | --- |
| names_and_fact_binding | .cpp:141-158 |
| normal_stock_clamps_min_open_lots_down_to_100 | .cpp:201-218, .h:361-392（get_minOrderQty）, .cpp:595-613, 658-666 |
| vwap_bucket_distribution_does_not_affect_fired_qty | .cpp:577-613（分桶值被 L609 覆盖, BUG-R7-007） |
| kcb_stock_clamps_min_open_lots_up_to_200 | .cpp:202-213, .h:368-382 |
| convertible_bond_min_hands_is_10 | .h:375-378, .cpp:216 |
| flat_position_buy_target_emits_zero_qty_sell | .cpp:601-609, 658-665（BUG-R7-008） |
| odd_lot_uses_available_position_as_qty | .cpp:521-526, 601-603, 822-836 |
| rounding_to_whole_lots_marks_finish_and_stops | .cpp:520-526, .h:348-351 |
| insufficient_sellable_position_adjusts_target_up | .cpp:501-508 |
| price_mode_and_offset_key | .cpp:617-638（mode1 减仓取卖一 626；价格偏移键是 "offset" 见 184） |
| buy_price_capped_to_upper_limit | .cpp:635-651 |
| sell_price_floored_to_lower_limit | .cpp:652-657 |
| zero_last_price_falls_back_to_preclose | .cpp:640-642 |
| channel_ready_branches | .cpp:313-370（三分支 322-346 / 348-359 / 361-366） |
| on_order_reports_and_refire | .cpp:258-303, 680-753 |
| on_trade_logs_and_on_entrust_recalculates | .cpp:455-459, 463-475, 569-575 |
| set_position_guards | .cpp:763-804（代码过滤 766、判等 770、负目标 774-778）、497-498（通道未就绪）、811-815 |
| clear_all_position_filters_code | .cpp:822-836 |
| DISABLED_channel_ready_before_first_tick_crashes | .cpp:517-519 早于 547-551（BUG-R7-005） |
| DISABLED_two_segment_stock_code_out_of_range | .cpp:201, .h:364（BUG-R7-009） |

## 4) src/TestUnits/coverage/r7_exefact/r7_stock_minimpact_unit_test.cpp（27 条：25 + 2）
生产文件 `src/WtExeFact/WtStockMinImpactExeUnit.cpp`（720 行，missed 409）+ `.h`

| 用例 | 生产行段 |
| --- | --- |
| names_and_fact_binding | .cpp:61-80 |
| init_clamps_min_order_for_normal_stock | .cpp:98-155（钳位 136-147）, .h:326-347 |
| init_clamps_min_order_for_kcb_and_cbond | .cpp:122-141, .h:334-343 |
| calc_waits_for_channel_ready | .cpp:500-511 |
| buy_by_lots_rounds_to_min_order_multiple | .cpp:593-610, 701-708 |
| non_cny_account_update_is_ignored | .cpp:244-252, 611 |
| cny_balance_limits_position_size | .cpp:244-251, 611-617 |
| sell_path_uses_available_and_rounds | .cpp:620-634, 710-718 |
| odd_lot_sell_liquidates_remainder | .cpp:546, 624-626 |
| rounding_match_marks_finish | .cpp:546-551, .h:314-317 |
| buy_by_rate_uses_counter_side_book | .cpp:594-601 |
| tiny_rate_is_raised_back_to_min_order | .cpp:599-600 |
| auto_price_mode_picks_side_by_book_imbalance | .cpp:641-655 |
| best_and_market_price_modes | .cpp:656-673 |
| zero_prices_fall_back_to_preclose | .cpp:675-680 |
| limit_prices_clamp_both_sides | .cpp:685-699 |
| entrust_span_blocks_second_calc | .cpp:586-591, 720 |
| stale_tick_is_filtered_in_on_tick | .cpp:311-319, 372-383, 225-227 |
| cancel_report_bumps_price_by_cancel_times | .cpp:184-204, 682-683 |
| full_fill_report_resets_cancel_times | .cpp:184-195 |
| successful_entrust_also_recalculates | .cpp:393-407（L406 在 if 之外, BUG-R7-017） |
| opposite_and_same_direction_live_orders | .cpp:553-578 |
| unmanaged_orders_canceled_only_when_configured | .cpp:259-280, 114-115 |
| target_guards | .cpp:415-440, 472-476 |
| clearing_state_rejects_zero_target | .cpp:421-427, 472-484, 490-493 |
| DISABLED_illegal_pricemode_reads_out_of_range_name_table | .cpp:155（BUG-R7-010） |
| DISABLED_two_segment_stock_code_out_of_range | .cpp:122, .h:328（BUG-R7-009） |

## 5) src/TestUnits/coverage/r7_exefact/r7_exec_runner_test.cpp（10 条：7 + 3）
生产文件 `src/WtExecMon/WtExecRunner.cpp`（452 行，missed 323，基线 0.00%）

| 用例 | 生产行段 |
| --- | --- |
| queries_return_null_before_base_data_loaded | 36-38, 371-382, 434-448 |
| init_with_missing_logcfg_file_returns_true | 40-50, 57-58 |
| init_with_invalid_inline_logcfg_returns_true | 51-54, 57-58 |
| config_missing_file_returns_false | 61-68 |
| config_empty_content_returns_false | 63-68 |
| handle_push_quote_ignores_null | 384-387 |
| position_cache_and_empty_run | 181-194, 407-416 |
| DISABLED_config_without_basefiles_section_derefs_null | 71-72（BUG-R7-011） |
| DISABLED_release_stops_uninited_logger | 401-404 |
| DISABLED_add_exe_factories_dlopens_plugins | 366-369 |

## 复用与约束（不要重复搭）
- 宿主替身沿用 `n05_exec_factories/n05_minimpact_calc_test.cpp` 的 FakeExecCtx 形态
  （buy/sell/cancel/writeLog 记录 + 可控 `pos/undone/nextId/cancelBydirRetCnt`），
  每个 .cpp 各留一份在匿名命名空间里，避免与 n05 的符号相互污染。
- 只引用 WtExeFact + WtExecMon 两族头文件（`Includes/*.hpp` + `WtExeFact/*.h` + `WtExecMon/WtExecRunner.h`），
  没有牵进 WtCtaStraFact/WtRiskMonFact 等兄弟插件的 TU（BUG-L00-003）。
- 不读挂钟、不起线程、不碰 socket/共享内存；`Vwap_*.txt` 分布文件写在进程 CWD 且文件名带 pid，用例结束时 `std::remove` 清理。
- 尚未入库的 `r7_exefact` 对象借同目标的 n05 编译行做 `-fsyntax-only` 门禁（主控下次构建前无 .o）。
