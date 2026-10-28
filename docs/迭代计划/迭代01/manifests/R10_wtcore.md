# R10_wtcore 用例清单（lane R10，WtCore 第六波收尾）

构建归属：`CoverageTests_wtcore`（目录 `src/TestUnits/coverage/q01_wtcore3/`，前缀 `r10_`，未新建目录、未改 CMakeLists）。
门禁：4 个文件全部用真实编译行（`ninja -t commands ...CoverageTests_wtcore.dir/q01_wtcore3/r3_cta_position_persist.cpp.o`，把 `-o/-c` 换成 `-fsyntax-only`）跑通，`-Werror` 下 exit 0。

| 文件 | 用例数 | 启用 | DISABLED | 门禁 |
|---|---|---|---|---|
| `r10_wtcore_dtmgr.cpp` | 11 | 11 | 0 | exit 0 |
| `r10_wtcore_executers.cpp` | 19 | 19 | 0 | exit 0 |
| `r10_wtcore_hft_evt.cpp` | 13 | 13 | 0 | exit 0 |
| `r10_wtcore_selengine.cpp` | 13 | 12 | 1 | exit 0 |
| 合计 | 56 | 55 | 1 | 4/4 |

替身只引用 WtCore 一族：`q01_wtcore3/q01_sel_env.h`（BaseDataMgr/HotMgr/SelCtx/Q01TempEnv）；
`p11_trader_fixture.h` 本轮**没有**接入（`TraderAdapter::buy` 的回报链路需要连带读 TraderAdapter 内部，预算不足，见「需要接缝」）。

## 用例 → 生产文件:行段（行号取自本 lane 实读的源码）

### src/WtCore/WtDtMgr.cpp（369 missed / 383）
| 用例 | 行段 | 说明 |
|---|---|---|
| fresh_manager_has_no_reader_and_queries_return_null | L27-35, L98(头), L242-245, L277-280, L378-381, L386-389, L394-397, L421-424 | 构造 + 5 个切片查询的 `_reader==NULL` 早退 |
| init_without_store_sets_engine_and_returns_false | L50-53, L89-102, L119-142 | init 先置 `_engine` 再 initStore(NULL) 早退 ⇒ 5 个委托 getter 可达 |
| init_forwards_loader_registered_before_init | L78(头), L89-101, L129-132 | `regsiter_loader`/`loader()`/`reader()` 读写 |
| adjusting_factor_and_flag_fall_back_without_reader | L255-261, L263-275 | 无 reader 的因子 1.0 / 标志 0 + static 记忆化二次命中 |
| handle_push_quote_null_tick_is_ignored | L219-225 | newTick==NULL 早退 |
| handle_push_quote_caches_and_grab_retains | L224-240, L242-253 | 建缓存 / 命中 retain / 未命中 |
| destructor_releases_rt_tick_cache | L38-48 | `_rt_tick_map` 非空的 release 分支 |
| on_bar_without_subscription_and_without_cache_returns_early | L104-107, L149-181 | 未订阅 ⇒ 不入 `_bar_notifies`；`_bars_cache==NULL` 早退 |
| on_bar_other_periods_also_return_early | L155-169 | switch 的 m/5m/d 三条 |
| reader_log_passes_every_level | L144-147 | 四个级别转发 |
| kline_slice_null_reader_for_every_period | L277-280, L421-424 | times!=1 也在 reader 判空前早退 |

未覆盖（需要接缝，见 BUG-R10-007）：L55-87（插件装载 WtDataStorage）、L109-117（`_bar_notifies` 派发）、L183-217（on_bar 重采样）、L229-239（`_ticks_adjusted` 支路）、L287-375（后复权 tick 缓存 + lower_bound 切片）、L426-542（get_kline_slice 重采样缓存）。合计约 **230 行**。

### src/WtCore/WtArbiExecuter.cpp（331 missed / 513）
| 用例 | 行段 |
|---|---|
| arbi_init_null_and_null_trader_guards | L33-42, L74-77, L217-237, L247-281, L292-345 |
| arbi_stub_backed_context_queries | L219-223, L233-236, L366-391（`setStub` 注入 IExecuterStub） |
| arbi_getunit_creates_nothing_and_callbacks_early_return | L163-202, L448-450 型早退, L548-552, L578-582, L615-619, L648-652, L679-683 |
| arbi_set_position_group_math_emits_nothing_without_unit | L74-154（groups 解析 L127-148）, L404-441（组合换算）, L444-450 |
| arbi_strict_sync_scans_channel_holds | L83-84, L98-101, L481-525, L821-831 |
| arbi_channel_events_over_empty_unit_map | L707-729, L738-759, L779-801 |
| arbi_autoclear_matrix_not_monthly_code | L821-835 |
| arbi_autoclear_matrix_not_prev_hot | L837-844 |
| arbi_autoclear_matrix_exclude_list | L114-118, L848-858 |
| arbi_autoclear_matrix_include_list_miss | L119-124, L862-869 |
| arbi_autoclear_matrix_will_clear_but_no_unit | L119-124, L872-888 |
| arbi_write_log_and_set_trader_null | L60-66, L352-358 |

未覆盖：单元创建成功之后的 `set_position/on_tick/on_trade/on_order/on_entrust/on_position_changed/clear_all_position` 主体（L452-510、L555-567、L601-602、L622-633、L655-666、L686-698、L711-728、L742-758、L783-800、L874-888 的 unit 分支），约 **90 行**；`_pool` 调度分支（poolsize>0 ⇒ boost 线程池起线程）约 **40 行**，规则 2 禁止。见 BUG-R10-002。

### src/WtCore/WtDiffExecuter.cpp（290 missed / 507）
| 用例 | 行段 |
|---|---|
| diff_init_null_and_guards_without_trader_or_data | L41-50, L84-88, L304-337, L348-368, L379-432 |
| diff_stub_queries_and_data_forwarding | L306-324, L334-341, L440-445, L454-481, L897-900 |
| diff_load_data_then_set_position_roundtrip | L113-172（targets/diffs 解析 + 非法合约 continue）, L597-616, L641, L180-232（save_data 回写） |
| diff_set_position_on_missing_file_creates_empty_json | L119-122, L554-562, L641, L185-231 |
| diff_load_data_broken_json_and_empty_file_are_ignored | L126-133, L554-556, L641 |
| diff_channel_ready_recovers_loaded_diffs | L773-817（含 `_diff_pos` 恢复循环）, L824-845, L862-884 |
| diff_callbacks_without_unit_early_return | L501-505, L650-654, L680-684, L721-725, L748-752 |

未覆盖：`_target_pos/_diff_pos` 真正变更后的撮合主体（L508-545、L564-595、L617-639、L657-668、L690-708、L727-738、L754-765、L782-813、L833-843、L871-881），约 **120 行**（价量约束/部分成交/超时撤单都在 ExecuteUnit 里 + BUG-R10-002 的工厂缝）；`_pool` 分支约 **30 行**。

### src/WtCore/HftStraBaseCtx.cpp（476 missed / 659）
| 用例 | 行段 |
|---|---|
| on_trade_opens_position_then_avgpx_is_volume_weighted | L536-553, L785-807, L830-903（反手块 L884-902）, L611-629, L795-796 |
| on_trade_close_is_fifo_and_leaves_the_last_detail | L831-882（FIFO 平仓 + 等比缩放 L861 + count 清理 L876-881） |
| update_dyn_profit_uses_bid_price_and_vol_scale | L906-939, L601-609, L611-620 |
| volume_zero_position_resets_dynprofit | L912-915, L838-839 |
| slippage_shifts_fill_price | L811-814, L834-835 |
| report_callbacks_flush_userdata | L538-542, L555-566, L569-594, L720-753 |
| session_end_writes_fund_line_when_agent_on | L946-967, L65-135（新建表头 + 同名二次 on_init 的 seek_to_end 分支） |
| close_and_trade_logs_stay_empty_without_trades | L969-987（`_trade_logs/_close_logs` 判空守卫）, L202-209 |
| price_query_falls_back_to_engine | L687-694, L416-424 |

未覆盖：需要 `_trader` 的报单/撤单/队列链路（L188-200、L211-415 的 `_trader->buy/sell` 与 `checkOrderLimits` 分支、L631-685 的 `getPosition/getUndoneQty`、L941-944 之外的 `emit_event` 系），约 **260 行**；`_data_agent=true` 时 `on_trade` 的 `_sig_logs` 写盘块（L544-548，因 `stra_get_position` 无判空而崩溃，见 BUG-R10-003）约 5 行；`do_set_position` 的 `_trader` 相关分支与 `_fund_info` 结算。

### src/WtCore/WtSelEngine.cpp（298 missed / 337）
| 用例 | 行段 |
|---|---|
| not_ready_until_session_begin | L45-57（析构亦触发）, L59-83 |
| comm_sess_and_real_time_from_engine_members | L490-509 |
| add_context_registers_and_duplicate_is_rejected | L390-416, L418-425 |
| on_bar_without_subscription_only_logs | L85-90, L102 |
| on_bar_dispatches_only_to_subscribed_context | L91-100（含 `_ctx_map.find==end` 分支） |
| on_tick_pushes_data_and_dispatches_raw_code | L105-114, L141-144, L148-163 |
| on_tick_adjusting_suffixes_for_qfq_and_hfq | L164-213（QFQ '-' / HFQ '+' 两条 + newTick 释放） |
| on_tick_ignored_before_ready_flag | L141（_ready false 整块跳过） |
| minute_end_skips_tasks_by_time_and_holiday | L224-241 |
| minute_end_holiday_task_is_skipped | L243-256（含 isHoliday continue + while 顺延） |
| minute_end_period_branches_that_do_not_fire | L260-326（TPT_None/Minute/Monthly/Weekly/Yearly 的 bIgnore 判定） |
| handle_pos_change_creates_pos_and_signal | L427-488（过滤器放行、`_pos_map` 建条目、append_signal、`_exec_mgr` 空路由） |

未覆盖：L328-335 任务派发（**std::terminate**，见 BUG-R10-004）、L339-380 `run()`（`_cfg`/`_data_mgr->reader()`/WtSelRtTicker/`_adapter_mgr` 四重依赖）、L382-388 `init()`（转调 WtEngine::init，线程 + DLL 缝）。合计约 **75 行**。

### src/WtCore/EventNotifier.cpp（400 missed / 405）
| 用例 | 行段 |
|---|---|
| init_inactive_returns_false | L37-43, L58-61 |
| init_without_mq_module_returns_false | L63-76 |
| every_notify_returns_before_enqueue | L46-56（析构）, L114-117, L143-146, L169-172, L197-200, L214-217, L307-310, L338-341, L369-372 |
| null_trade_and_order_infos_are_guarded | L199, L216 |

未覆盖：L81-111（`_creator/_remover/_publisher/_register` 赋值 + worker 线程）、L118-141、L147-167、L173-195、L201-212、L218-230、L232-265（tradeToJson）、L267-305（orderToJson）、L311-336、L342-367、L373-400。合计约 **340 行**，全部被 `_mq_sid==0` 挡在入队之前；即使 `_mq_sid!=0`，`_asyncio` 也是私有成员、只能靠 worker 线程 `run_one()` 排空 ⇒ 双重接缝（BUG-R10-006；引用计数泄漏本身另记 BUG-R10-005）。

## 「需要接缝」汇总（估计行数）
| 面 | 估计行 | 缺的接缝 |
|---|---|---|
| EventNotifier 事件序列化 + 分发 | ~340 | `_mq_sid`/`_asyncio` 私有；无 poll/drain 接口 |
| HftStraBaseCtx 报单/撤单/队列/回报 | ~260 | `_trader` 只能挂真 TraderAdapter（需柜台语义）；无 ITraderSink 抽象（BUG-R10-003 的判空不一致同源） |
| WtDtMgr 重采样与后复权缓存 | ~230 | `_reader` 只能由 WtDataStorage 插件装载，无 setter |
| WtArbi/WtDiff 执行单元主体 | ~210（120+90） | `WtExecuterFactory::_factories` 只能扫目录加载 dylib，无进程内注册口 |
| WtSelEngine run/init/任务派发 | ~75 | WtEngine::init 线程 + DLL；任务派发是崩溃类 |
| 线程池分支（poolsize>0） | ~70 | boost::threadpool 起线程，规则 2 禁止 |
| 时钟相关（TimeUtils::getLocalTimeNow） | ~0（本 6 文件内未构成独立支路） | 无注入接缝，R3 已登记 |
