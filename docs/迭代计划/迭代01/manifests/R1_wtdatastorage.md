# R1 用例清单（wtdatastorage 加深波）— 20261027

Lane: R1。目录: `src/TestUnits/coverage/r1_storage/`（主目标 CoverageTests，GLOB_RECURSE 自动收）。
门禁: 全部 4 个 cpp 用 `ninja -t commands ...CoverageTests.dir/q03_storage3/q03_reader_file_test.cpp.o` 整行真实 flags 做 `-fsyntax-only`，4/4 通过。
启用用例 35 条，DISABLED 1 条（越界崩溃类）。缺陷记录: `docs/迭代计划/迭代01/bugs/R1_wtdatastorage.md`（5 条待审核 + 3 类接缝留白）。

## r1_datadef_slices_test.cpp → src/Includes/WTSDataDef.hpp
| 用例 | 生产位置（实读行段） |
|---|---|
| value_array_append_at_negative_index_and_resize | WTSDataDef.hpp L39-170（WTSValueArray create/at/translateIdx/set/resize） |
| value_array_max_min_with_abs_and_invalid_holes | L84-146（maxvalue/minvalue，isAbs 与 INVALID 跳空） |
| kline_slice_multi_block_at_walks_offsets | L204-317（create/appendBlock/get_block_*/at 跨块 + translateIdx L232-241） |
| kline_slice_max_min_price_and_empty_behavior | L333-371（maxprice/minprice）、L388-415（extractData 空守卫） |
| kline_slice_extract_data_all_field_types | L388-441（KFT_OPEN..KFT_TIME 分支） |
| kline_slice_extract_svolume_big_volume_clamped_branch | L421-427（KFT_SVOLUME 的 vol>INT_MAX 钳位两条分支） |
| kline_data_flags_period_and_range_bounds | L492-506（setClosed/setPeriod/setUnixTime）、L513-560（maxprice/minprice 越界 INVALID） |
| tick_slice_append_insert_and_index_walk | L1390-1487（WTSTickSlice create/appendBlock/insertBlock/at/get_block_size INVALID_UINT32 L1451-1457） |
| his_tick_data_append_tick_applies_adj_factor | L1302-1380（WTSHisTickData::appendTick 复权修正 L1370-1378） |
| orddtl_ordque_trans_slice_core_access | L1498-1648（WTSOrdDtlSlice/WTSOrdQueSlice/WTSTransSlice create/at） |

## r1_basedatamgr_test.cpp → src/WTSTools/WTSBaseDataMgr.cpp
| 用例 | 生产位置 |
|---|---|
| loaders_reject_missing_and_broken_files | L245-249, L330-334, L396-400, L573-577（StdFile::exists 分支）+ 各 load_from_file NULL 分支 |
| sessions_loaded_and_missing_sections_skipped | L243-298（loadSessions，含 auction/auctions L267-280、BADSEC continue L283）L177-185 getAllSessions |
| commodities_lookup_and_missing_session_branches | L74-89, L328-391（缺 session warn L355-359 / ghost 会话 L364-369）、L871-878 getSessionComms、L192-199 getSessionByCode |
| contract_lookup_by_code_fullcode_altcode_and_dates | L91-134 getContract 三分支、L479-483/534-545 altcode、L501-507 日期窗、L429-461 rules 自动品种、L463-467 未知 product 跳过 |
| code_list_sorted_by_fullcode_and_index_queries | L136-175（getContractSize/getContracts）、L153-159 getContractByIndex、L553-567（std::sort+setTotalIndex，即简报 L554） |
| holidays_and_trading_date_arithmetic | L201-219 isHoliday、L571-603 loadHolidays、L760-888（getTradingDate/getNextTDate/getPrevTDate/isTradingDate/setTradingDate） |
| boundary_time_offsets_and_weekend_shift | L605-671（未知 pid→0 L621-623、offset==0 L641-647、offset<0 L649-657、offset>0 L658-670、isSession L613-618） |
| calc_trading_date_branches | L673-758（未知 pid L692-694、offset>0 跨夜 L720-733、offset==0 周末 L750-754、isSession L684-689） |
| missing_commodity_pid_falls_back_to_empty_tpl | L880-888 getTplIDByPID 缺失品种 → ""（经 isHoliday 公开触发） |
| DISABLED_getTplIDByPID_single_segment_pid_is_oob | L882-883 `ay[1]` 越界（bug #3） |

## r1_rdm_hft_slice_test.cpp → src/WtDataStorage/WtRdmDtReader.cpp
| 用例 | 生产位置 |
|---|---|
| ordque_his_roundtrip_and_same_day_window | L611-782（readOrdQueSliceByRange his 分支：L705 路径、L711 read_file_content、L717-727 尺寸校验、L732 解压、L744-782 窗口索引） |
| ordque_cross_day_window_returns_full_prefix | L765-772（beginTDate!=endTDate 全前缀分支） |
| ordque_cache_hit_survives_file_removal | L701-703（_his_ordque_map 命中）+ clearCache 路径 |
| ordque_size_check_failures_log_error_and_recover_never | L712-717（头不足）、L721-727（_size 不符）两处 LL_ERROR |
| ordque_missing_file_returns_null_without_error_log | L706-707（StdFile::exists false → NULL） |
| ordque_today_window_without_rt_block_returns_null | L653-656（isToday → getRTOrdQueBlock NULL → return NULL） |
| orddtl_his_first_read_roundtrip | L786-955（his 分支 L870-955；只测首次读，避开 bug #2 的顺序缺陷） |
| trans_his_first_read_roundtrip | L960-1130（his 分支 L1045-1130） |
| trans_size_check_short_file_logs_error | L1057-1062（头不足 LL_ERROR） |

## r1_reader_bars_slice_test.cpp → src/WtDataStorage/WtDataReader.cpp
| 用例 | 生产位置 |
|---|---|
| day_his_raw_v2_tail_window_by_count | L1720-1917（readKlineSlice：非当日 else L1906-1912 尾窗）；cacheHisBarsFromFile L1650-1698（his/day/<exchg>/<code>.dsb + proc_block_data RAW_V2 读回 L44-88 版本矩阵在 fixture 层复用） |
| day_his_cmp_v2_and_old_struct_upgrade | L1428-1436（proc_block_data 升版：老结构体→新结构体）、L1668 read_file_content |
| day_size_check_failure_logs_error_and_returns_empty | L1431-1436/L1671-1676（尺寸校验 LL_ERROR + return false） |
| min5_his_roundtrip_uses_min5_dir | L1708-1712（pname min5）、L1654-1660（_his_dir 多目录命中） |
| min1_splices_his_tail_with_rt_block_of_today | L1800-1894（bHasToday：getRTKilneBlock + lower_bound 截断 + his/rt 双块 appendBlock）、L2131-2185（getRTKilneBlock subdir=min1 路径） |
| bars_cache_hit_survives_file_deletion | L1731-1743（_bars_cache 命中跳过 loader/文件） |
| missing_his_file_returns_empty_slice_not_null | L1906-1912（空 barsList 尾窗，关联 bug #5 的 &v[0]） |

## 本轮明确未覆盖（见 bug 文件「需要接缝」A/B/C）
- `WtDataWriter.cpp` 全文件：init 无条件起线程 + 其余入口全 private，禁宏禁线程下无可达公开入口 → 0 新增用例。
- HOT 主力连续 / 股票复权拼接分支（WtRdmDtReader L1162-1362 起、WtDataReader L1011-1445 段）：夹具 getContract 恒 NULL。
- getRT*Block 容量变化重映射分支（WtDataReader L1920-2130 部分）。
