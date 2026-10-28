# R2 用例清单 — WtBtCore（第七波）

- 派生时间：2026-10-27T11:34:54Z 之后，lane R2 收口
- 用例目录：`src/TestUnits/coverage/q02_btcore3/`（拆分表 `btcore` 行只逐个列 `dir:`，
  不含 `r2_btcore`，故按主控指引落进已入库的 `q02_btcore3/`，文件名前缀 `r2_`，**未改 CMakeLists**）
- 门禁口径：`build-cov && ninja -t commands
  TestUnits/coverage/CMakeFiles/CoverageTests_btcore.dir/q02_btcore3/q02_cta_replayer_integration_test.cpp.o | tail -1`
  取整行 flags，改 `-fsyntax-only <本文件>`。**3/3 文件通过**（0 error / 0 warning，含 -Werror）。

## 用例 → 生产文件:行段

### `r2_replayer_loader_test.cpp`（7 条启用）
| 用例 | 生产文件:行段 |
|---|---|
| finalBarsFromLoaderCachedOnce | HisDataReplayer.cpp L171-174（init 收 `_bt_loader`）、L2229-2241（get_kline_slice 的 extloader 优先分支）、L3589-3600、L3673-3712（cacheFinalBarsFromLoader 的 extloader 装载臂）、L593-634（locate_barindex）、L2132-2290 |
| autoTransDumpsDsbAndSecondReplayerHitsDsb | HisDataReplayer.cpp L3610-3633（目录+文件名拼装）、L3636-3672（dsb 命中臂，含 uncompress）、L3704-3731（isAutoTrans 转储 dsb） |
| loaderBackedDailyReplayEmitsBarsAndDumpsState | HisDataReplayer.cpp L677-741（run 主K线自动判定 + 分派）、L771-938（run_by_bars 日线全回放到收尾）、L518-556（dump_btstate 落 btenv.json）、L557-593（notify_state 无 notifier 早退）、L634-649 |
| ticksFromLoaderFeedTickSliceAndLastTick | HisDataReplayer.cpp L2828-2881（checkTicks 的 loader 臂 L2852）、L3443-3470（cacheRawTicksFromLoader）、L2470-2547（get_tick_slice 游标定位）、L2881-2915（get_last_tick）、L3470（csv 臂未走通的反证：`_tickRet=false` → NULL） |
| adjFactorsPreferLoaderThenFile | HisDataReplayer.cpp L302-335（loadStkAdjFactorsFromLoader + 回调内排序）、L297-301（init 尾部的「loader 优先、失败退文件」） |
| binModeWithLoaderCachesBarsViaRawInterface | HisDataReplayer.cpp L4484-4527（cacheRawBarsFromBin 前置拆解）、L4527-4570（extloader `loadRawHisBars` 臂）、L4570-4600（barsSections 汇总）、L2262-2270 |
| binModeWithoutLoaderOrDsbYieldsNoSlice | HisDataReplayer.cpp L4484-4526、L4536-4556（HisDtMgr 失败 warn + 空 buffer 返 false）、L2258-2278（get_kline_slice 外层 try/catch 兜异常） |

### `r2_replayer_public_api_test.cpp`（6 条启用）
| 用例 | 生产文件:行段 |
|---|---|
| feeTemplateAndCalcFeeMatrix | HisDataReplayer.cpp L2946-2983（loadFees 三分支：不存在/解析失败/正常）、L2983-3020（calc_fee：byvolume 三档 + 越界 default + 未登记合约 + 比例档）、HisDataReplayer.h L929-935（apply_fee_minimum） |
| level2DataRefusedInCsvMode | HisDataReplayer.cpp L3089-3167（sub_tick/sub_order_detail/sub_order_queue/sub_transaction，含空代码早退与复权后缀拆解）、L2702-2744 / L2744-2786 / L2786-2828（checkOrderDetails/Queues/Transactions 的 csv 拒绝臂）、L2547-2595 / L2595-2643 / L2643-2691（三个 slice 入口的第一层早退）、L2470-2472（tick 未启用直接 NULL） |
| stopBeforeRunQuitsReplayEarly | HisDataReplayer.cpp L634-649（stop：未运行 / 已终止重复调用 / 置位）、L771-812（run_by_bars 首回合 `_terminated` 退出，不产生 handle_replay_done） |
| clearCacheForcesReloadOnNextPrepare | HisDataReplayer.cpp L436-459（clear_cache 全表清空）、L459-518（reset 游标还原）、L649-677（prepare 二次进入）、L2132-2290（缓存清空后重新装载） |
| registeredTaskSwitchesToTaskReplayAndSchedules | HisDataReplayer.cpp L407-436（register_task 的 5 种周期 + TPT_None）、L677-741（`_task != NULL` 分派）、L940-1243（run_by_tasks 调度与收尾） |
| metadataAndTimelineQueries | HisDataReplayer.cpp L2915-2921（get_commodity_info）、L2921-2933（get_rawcode）、L2933-2946（get_session_info 两种入参）、L3021-3034（get_cur_price）、L3034-3089（get_day_price 无 tick 的 `_day_cache` 两臂）、HisDataReplayer.h L782-1021（set_time_range / enable_tick / register_sink / 时间轴 getter / is_tick_enabled） |

### `r2_mocker_tick_fill_test.cpp`（4 条启用）
| 用例 | 生产文件:行段 |
|---|---|
| marketSignalFilledByTickAndDumpsTrade | CtaMocker.cpp L898-916（handle_tick 公开入口）、L695-723（proc_tick 信号撮合臂）、L1586-1603（append_signal）、L1603-1650（do_set_position 开仓）、L973-1016（update_dyn_profit）、L1027-1054（on_tick_updated 无订阅早退）、L685-691（handle_replay_done 落盘）、L330-400（dump_outputs：trades/signals/positions/funds） |
| signalWithDespriceFillsAtTargetNotLastPrice | CtaMocker.cpp L703-715（`desprice != 0` 用目标价成交）、L1603-1640（do_set_position 传入价覆盖 `_price_map`）、L1792（stra_get_bars 主K线只登记一次） |
| exitLongSignalClosesPartOfPosition | CtaMocker.cpp L1355-1420（stra_exit_long → `append_signal(curQty-qty)`）、L1650-1770（do_set_position 减仓/平仓明细 + log_close）、L685-691 |
| selHftUftTickUpdatesPriceMapWithoutStrategy | SelMocker.cpp L186-230（handle_tick + `_price_map` 二段式）、L452-476（on_tick / on_tick_updated 的 `if(_strategy)` 守卫）、L275-305（handle_init / handle_bar_close / handle_session_begin / handle_session_end）；HftMocker.cpp L275-300（IDataSink 事件面）、L345-432（on_tick：`_price_map` + update_dyn_profit + procTask 空队列直接 return + `_match_this_tick=false` 臂）；UftMocker.cpp L189-215（事件面）、L228-289（on_tick 同结构） |

## 口径说明
- **启用 17 条，DISABLED 0 条**（本轮没有新增挂死/崩溃路径；`SelMocker::handle_replay_done`
  的已知整数除零沿用 q02 的整类 DISABLED，本文件未调用它，也未复活它）。
- 未新增 `#define private public`、未起线程、未 sleep。
- 与上一轮 lane 上报结论的**唯一分歧**：`WTSTickData` 其实有公开池工厂
  `WTSTickData::create(WTSTickStruct&)`（WTSDataDef.hpp L884），`handle_tick` 是 IDataSink
  公有重写，因此四个 Mocker 的 tick 撮合链**不需要** private 接缝即可进入 —— 本轮据此打通了
  CtaMocker 的成交/平仓主链。
