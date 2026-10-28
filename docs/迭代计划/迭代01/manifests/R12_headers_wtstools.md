# R12 用例清单（用例 → 生产文件:行段）

lane: R12　用例目录: `src/TestUnits/coverage/r12_headers/`（由主目标 `CoverageTests` 的 `GLOB_RECURSE` 自动收编，未改 `src/TestUnits/coverage/CMakeLists.txt`）
门禁口径（真实构建 flags，8/8 文件 exit 0，含 `-Werror`）：

```
cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests.dir/l03_includes/l03_session_info_test.cpp.o | tail -1
# 去掉 -o/-c/-MD/-MT/-MF 后追加 -fsyntax-only <case.cpp>
```

统计：8 个 TU，97 个 TEST 块 = **91 条启用 + 6 条 DISABLED**。
行号均取自本轮实际 Read/Grep 到的生产源码。

## 1. r12_data_def_test.cpp（20 启用 + 1 DISABLED）→ `src/Includes/WTSDataDef.hpp`、`src/Includes/WTSStruct.h`

| 用例 | 生产行段 |
| --- | --- |
| r12_value_array.size_empty_append_and_at | WTSDataDef.hpp:49-74（create/size/empty/at 越界语义） |
| r12_value_array.negative_index_translates_from_tail | WTSDataDef.hpp:66-84（at + translateIdx） |
| r12_value_array.maxvalue_minvalue_range_semantics | WTSDataDef.hpp:90-150（maxvalue/minvalue，INVALID_DOUBLE 跳过分支） |
| r12_value_array.set_resize_and_operator_index | WTSDataDef.hpp:155-196（append/set/resize/operator[]/getDataRef） |
| r12_kline_slice.create_at_and_block_accessors | WTSDataDef.hpp:215-317（ctor/translateIdx/create/get_block_*/at 两个重载） |
| r12_kline_slice.append_block_and_empty_slice | WTSDataDef.hpp:236-278（空切片 create + appendBlock 拒绝分支） |
| r12_kline_slice.maxprice_minprice_and_setcode | WTSDataDef.hpp:326-375（maxprice/minprice/size/code/setCode） |
| r12_kline_slice.extract_data_per_field_type | WTSDataDef.hpp:384-434（KFT_OPEN..KFT_TIME 八个 switch 分支） |
| r12_kline_slice.extract_from_empty_slice_returns_null | WTSDataDef.hpp:384-393（`_count==0 → NULL`） |
| r12_kline_data.property_accessors_and_sized_create | WTSDataDef.hpp:458-506、563-570（ctor 默认值/create/setClosed/setPeriod/setUnixTime/size/IsEmpty/setCode） |
| r12_kline_data.field_by_index_accessors | WTSDataDef.hpp:576-738（open/high/low/close/volume/openinterest/additional/bidprice/askprice/money/date/time 越界回落） |
| r12_kline_data.append_bar_overwrites_same_time_and_extacts | WTSDataDef.hpp:746-811、830-848（extractData 全量语义 + at/appendBar 同时刻覆盖） |
| r12_kline_data.maxprice_minprice_and_release_keeps_slice_refs | WTSDataDef.hpp:514-558、802、817-825（getDataRef 直写 + maxprice/minprice 越界 + release） |
| r12_tick_data.create_from_struct_and_field_accessors | WTSDataDef.hpp:870-960、1012-1021（create(WTSTickStruct&) + 21 个字段读取 + limit flag） |
| r12_tick_data.create_by_code_setcode_and_limit_flag | WTSDataDef.hpp:870-878、900-910、1014-1021（setCode/isNewHigh/isNewLow/getContractInfo） |
| r12_tick_ref.wraps_external_struct_without_copy | WTSDataDef.hpp:1050-1120、1171-1175（引用语义 + valid/operator bool） |
| r12_tick_slice.block_append_insert_and_index | WTSDataDef.hpp:1390-1487（create/appendBlock/insertBlock/get_block_*/at 跨块与越界） |
| DISABLED_r12_tick_slice.count_uninitialized_when_created_without_ticks | WTSDataDef.hpp:1390-1424（`_count` 未初始化 → BUG-R12-06） |
| r12_detail_slices.orddtl_trans_slice_create_and_at | WTSDataDef.hpp:1498-1543、1602-1648（两个切片的 create 拒绝分支/负索引） |
| r12_pool_data_objects.orddtl_ordque_trans_code_and_contract | WTSDataDef.hpp:1189-1300（三个池对象的 create/setCode/getXxxStruct/日期时间） |
| r12_struct_compat.old_bar_and_tick_assignment | WTSStruct.h:26-46、94-146、148-230（WTSBarStruct::operator=(WTSBarStructOld&) 与 tick 同构，版本兼容面） |

## 2. r12_session_calc_test.cpp（13 启用 + 0 DISABLED）→ `src/Includes/WTSSessionInfo.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_session.create_id_and_offset_accessor | WTSSessionInfo.hpp:57-110、315（create/id/name/getOffsetMins/setOffsetMins/getSectionCount） |
| r12_session.offset_time_zero_point_alignment | WTSSessionInfo.hpp:507-527（0 点左/右对齐三条分支） |
| r12_session.original_time_is_inverse_of_offset | WTSSessionInfo.hpp:529-542（offset==0 直通 + 反算回绕） |
| r12_session.get_offset_date_three_branches | WTSSessionInfo.hpp:117-135（>=1440 次日 / <0 次日回退 / 当日） |
| r12_session.trading_sections_are_stored_with_raw_and_offset | WTSSessionInfo.hpp:24-40、78-106、332-361、454-467（TradingSection 双值、开收盘、0 点收盘修正） |
| r12_session.time_to_minutes_multi_section | WTSSessionInfo.hpp:144-189（三段累计、竞价归 0、收盘后 INVALID） |
| r12_session.time_to_minutes_auto_adjust_and_empty | WTSSessionInfo.hpp:144-147、174-186（autoAdjust 命中“小于下边界”分支 + 空时段） |
| r12_session.minute_to_time_round_trip_both_directions | WTSSessionInfo.hpp:191-243（bHeadFirst 两分支 + 段边界 + 超时回落 getCloseTime） |
| r12_session.night_session_minute_mapping_with_offset | WTSSessionInfo.hpp:78-96、144-243、387-405（offset=-1260 夜盘轴与 originalTime 反算） |
| r12_session.seconds_mapping | WTSSessionInfo.hpp:245-330（timeToSeconds/secondsToTime 命中、竞价 0 秒、越界 INVALID） |
| r12_session.trading_duration_and_seclist | WTSSessionInfo.hpp:363-434（getTradingSeconds/getTradingMins/getSecMinList 缓存与空表 1440 分支） |
| r12_session.in_trading_time_and_section_boundary_flags | WTSSessionInfo.hpp:442-481（isInTradingTime 严格模式、isLast/isFirst） |
| r12_session.auction_multi_section_and_update | WTSSessionInfo.hpp:83-99、340-346、483-498（setAuctionTime 两条分支、addAuctionTime、0-0 跳过、空竞价 -1） |

## 3. r12_collection_test.cpp（13 启用 + 2 DISABLED）→ `src/Includes/WTSCollection.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_collection.array_append_at_grab_refcount | WTSCollection.hpp:49-146（create/size/empty/at/grab/operator[]/append 两种 retain 模式/idxOf/clear） |
| r12_collection.array_idxof_null_and_missing | WTSCollection.hpp:92-99、137-146（idxOf 的 -1 → INVALID_UINT32） |
| r12_collection.array_set_replaces_and_releases_old | WTSCollection.hpp:168-180（set 的 swap + 旧值 release + 越界早退） |
| r12_collection.array_append_array_steals_without_retain | WTSCollection.hpp:184-195、109-116（整串搬移 + 模板 at<T>） |
| r12_collection.array_sort_and_iterator | WTSCollection.hpp:44、242-297（sort 回调/begin/end 两套迭代器） |
| r12_collection.array_release_twice_is_noop | WTSCollection.hpp:218-238（release 的 m_uRefs 归零路径） |
| r12_collection.map_add_get_grab_remove | WTSCollection.hpp:330-430、533-540（add 覆盖旧值/get/grab/[]/remove/last/size） |
| r12_collection.map_find_bounds_and_erase_iterator | WTSCollection.hpp:433-531（find/lower_bound/upper_bound/erase 三种重载） |
| r12_collection.hashmap_string_key_add_get_remove | WTSCollection.hpp:614-727（create/add/get/grab/remove/find/begin/end/clear） |
| r12_collection.queue_push_front_back_and_refcount | WTSCollection.hpp:761-806（push/front/back 的 retain 与空队列早退） |
| r12_collection.queue_swap_and_clear_release_each_once | WTSCollection.hpp:828-856（clear 计数与 swap 语义） |
| DISABLED_r12_collection.queue_pop_does_not_release_element | WTSCollection.hpp:767-770（pop 不 release → BUG-R12-04） |
| DISABLED_r12_collection.queue_clear_crashes_on_null_slot | WTSCollection.hpp:772-777、828-836（NULL 槽 → BUG-R12-02） |
| r12_collection.stack_lifo_top_pop_clear | WTSCollection.hpp:879-921（push/top/pop 判空与 release/clear） |
| r12_collection.stack_release_destructor_clears_elements | WTSCollection.hpp:924-935（release + ~WTSStack 的 clear） |

## 4. r12_variant_more_test.cpp（8 启用 + 1 DISABLED）→ `src/Includes/WTSVariant.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_variant_object.append_and_read_each_scalar_type | WTSVariant.hpp:64-135、165-290、292-360（八种 append 重载 + 九个 as*/get* 换算；create(double) 的 %.10f 定标在 :96-118） |
| r12_variant_object.missing_keys_return_defaults | WTSVariant.hpp:153-163、292-400（has 的跨类型 false + 七个默认值回落 + get(std::string)） |
| r12_variant_object.overwrite_key_releases_old_value | WTSVariant.hpp:400-433、519-533（同键覆盖旧值 release、NULL item 拒绝） |
| r12_variant_object.member_names_and_size_of_children | WTSVariant.hpp:690-720（size 的数组/对象/标量三分支 + memberNames） |
| r12_variant_array.append_all_types_and_index_read | WTSVariant.hpp:145-152、388-398、551-660（数组八种 append + get(idx) 越界） |
| r12_variant_array.append_wrong_container_returns_false | WTSVariant.hpp:400-433、485-545、551-660（容器类型不符的早退分支全覆盖） |
| r12_variant_refcount.retain_release_pairing_on_children | WTSVariant.hpp:724-726、695-712（release 递归释放 _map；引用计数与 map add 的 bAutoRetain） |
| r12_variant_conversions.cross_type_numeric_conversion | WTSVariant.hpp:165-290（字符串→整数/浮点、对象类型不匹配全部回落 0/""） |
| DISABLED_r12_variant_refcount.as_boolean_on_container_is_type_confused | WTSVariant.hpp:282-290 + 712-722（union 误读 → BUG-R12-03） |

## 5. r12_codehelper_test.cpp（12 启用 + 1 DISABLED）→ `src/Share/CodeHelper.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_codehelper.extract_std_monthly_code_three_segment | CodeHelper.hpp:601-616、620-655（三段式分月合约 + stdCommID 拼接 :78-85） |
| r12_codehelper.extract_std_code_stock_and_exright | CodeHelper.hpp:592-660（SSE.STK.600000 / `-` / `+` 复权位 :629-633 / 两段式 :612-619） |
| r12_codehelper.extract_std_code_hot_and_2nd_suffix | CodeHelper.hpp:640-655（ruleTag 为空时原样落 code，hotMgr==NULL 分支） |
| r12_codehelper.extract_std_chn_futopt_per_exchange_branches | CodeHelper.hpp:547-590（CFFEX/SHFE/INE/CZCE 四类 code 与 product 组装） |
| r12_codehelper.is_std_chn_futopt_state_machine | CodeHelper.hpp:136-232（状态机 12 态与六个失败出口） |
| r12_codehelper.is_monthly_code_reverse_state_machine | CodeHelper.hpp:374-441（反向状态机：数字尾 3-5 位 + 字母前置） |
| r12_codehelper.index_code_month_and_raw_comm_id | CodeHelper.hpp:272-282、527-545（首个数字位置；A..z 前缀截断） |
| r12_codehelper.raw_month_code_to_std_code | CodeHelper.hpp:286-341（isComm 分支 + 3 位月份按首位补 1/2） |
| r12_codehelper.raw_flat_code_to_std_code_branches | CodeHelper.hpp:342-372（code==pid 与 plen==0 两个短路分支） |
| r12_codehelper.std_code_to_hot_and_2nd_wrappers | CodeHelper.hpp:482-513（尾部 NUL 使 c_str 比较成为唯一可靠口径 → BUG-R12-12） |
| r12_codehelper.std_code_to_std_comm_id_and_opt_raw | CodeHelper.hpp:247-270、443-480、515-526（三段/两段品种 ID；期权码双向互转的 CFFEX/DCE/其他三分支） |
| r12_codehelper.code_info_helpers_clear_and_stdcommid | CodeHelper.hpp:66-99（CodeInfo 的 clear/hasRule/isExright 系列与 stdCommID 空段拼接） |
| DISABLED_r12_codehelper.dotless_input_runs_off_the_end_of_the_string | CodeHelper.hpp:104-133（无符号反向扫描越界读 → BUG-R12-01） |

## 6. r12_charconv_safe_test.cpp（8 启用 + 1 DISABLED）→ `src/Share/charconv.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_charconv_utf8tochar.ascii_passthrough_and_empty_inputs | charconv.hpp:55-96、141-160（空指针/空串/纯 ASCII 直通 + operator const char*/c_str） |
| r12_charconv_utf8tochar.unconvertible_input_yields_empty_buffer | charconv.hpp:96-133（iconv 分支：不写出字节时缓冲保持 memset 全零） |
| r12_charconv_chartoutf8.ascii_passthrough_and_empty_inputs | charconv.hpp:240-270、315-330（ChartoUTF8 同构直通分支） |
| r12_charconv_chartoutf8.unconvertible_input_yields_empty_buffer | charconv.hpp:270-313（iconv 失败路径） |
| DISABLED_r12_charconv_utf8tochar.chinese_conversion_is_platform_dependent | charconv.hpp:118-133（依赖 gb2312 别名，期望值不可唯一确定 → BUG-R12-10） |
| r12_charconv_encoding_helper.prenum_counts_leading_ones | charconv.hpp:601-617（0..8 全部首码点档位） |
| r12_charconv_encoding_helper.isutf8_valid_sequences | charconv.hpp:635-670（1/2/3/4 字节合法序列与混排、len==0） |
| r12_charconv_encoding_helper.isutf8_rejects_broken_sequences | charconv.hpp:635-670（续字节缺失/错误、孤立 0x80、0xFE 首字节、截断序列） |
| r12_charconv_url.encode_decode_round_trip_on_ascii_subset | charconv.hpp:388-457、469-532（URLEncode 的 %20 与 URLDecode 的 `+`/免编码字符/断链 `%zz`） |

## 7. r12_trade_def_test.cpp（9 启用）→ `src/Includes/WTSTradeDef.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r12_trade_error.code_and_message | WTSTradeDef.hpp:23-45（WTSError::create/getMessage/getErrorCode，池对象 release） |
| r12_trade_entrust.create_defaults_and_field_writers | WTSTradeDef.hpp:110-172（委托默认值、setCode/setExchange 带长度、四个类型枚举、ID/UserTag 双重载） |
| r12_trade_entrust.credit_business_and_net_direction | WTSTradeDef.hpp:140-176（isCreditBusiness 三值判定 + setNetDirection/isNet/isNetBuy + CreditBalance） |
| r12_trade_entrust.extras_string_and_contract_binding | WTSTradeDef.hpp:107-135、183-189（无参 create、setextras、Contract/Extras 空值） |
| r12_trade_action.cancel_action_fields | WTSTradeDef.hpp:223-280（createCancelAction 与 create 两路 + EntrustID/OrderID/UserTag 双向） |
| r12_trade_order.create_from_entrust_copies_fields | WTSTradeDef.hpp:339-382、402-447（委托派生 11 个字段与 m_dVolLeft 初值；空委托全零对象） |
| r12_trade_info.fields_and_net_direction | WTSTradeDef.hpp:523-560、543-560（成交字段、金额、日期时间、净额方向） |
| r12_trade_position.derived_totals_and_frozen | WTSTradeDef.hpp:652-700（getTotalPosition/getAvailPosition/getFrozenPosition/getFrozenNewPos/getFrozenPrePos 派生式） |
| r12_trade_account.money_fields | WTSTradeDef.hpp:747-786（币种与六项资金字段） |

## 8. r12_base_datamgr_test.cpp（8 启用）→ `src/WTSTools/WTSBaseDataMgr.cpp`（查询面；加载面由 lane R1 覆盖，不重复）

| 用例 | 生产行段 |
| --- | --- |
| r12_base_datamgr.commodity_lookup_by_stdpid_and_two_args | WTSBaseDataMgr.cpp:74-90（两个 getCommodity 重载与缺失键；品种模板见 WTSContractInfo.hpp:52） |
| r12_base_datamgr.contract_lookup_and_attributes | WTSBaseDataMgr.cpp:91-135（getContract 的日期窗口与 opendate/expiredate，字段见 WTSStruct 侧 WTSContractInfo.hpp:230-231） |
| r12_base_datamgr.contract_lists_size_and_index | WTSBaseDataMgr.cpp:136-176、getGlobalSize 见 WTSBaseDataMgr.h:46（getContractSize/getContracts/getContractByIndex） |
| r12_base_datamgr.session_lookup_by_id_and_code | WTSBaseDataMgr.cpp:177-200（getAllSessions/getSession/getSessionByCode）+ :871-878（getSessionComms） |
| r12_base_datamgr.holiday_and_trading_date_window | WTSBaseDataMgr.cpp:201-242（isHoliday 三分支）+ :845-855（isTradingDate） |
| r12_base_datamgr.trading_date_cursor_set_next_prev | WTSBaseDataMgr.cpp:760-844、856-870（getTradingDate/getNextTDate/getPrevTDate/setTradingDate） |
| r12_base_datamgr.calc_trading_date_day_and_night | WTSBaseDataMgr.cpp:673-759（日盘当日 / 夜盘次日 / 未知 pid 原样返回） |
| r12_base_datamgr.boundary_time_start_and_end | WTSBaseDataMgr.cpp:605-672（未知 pid→0、isStart 两分支、偏移>0 的夜盘分支） |

## 与既有 lane 的边界

- `l02_wtstools_mq/l02_variant_collection_test.cpp`、`l03_includes/l03_session_info_test.cpp`、`l03_contract_info_test.cpp`、`r1_storage/r1_basedatamgr_test.cpp` 已占用的分支本轮只做**不同入参、不同断言**的补集（例如 SessionInfo 走夜盘 offset=-1260 与竞价多段更新；BDM 只走查询面）。
- 未启用 `isGBK` 相关任何输入（主控第 3 条），`0xD6 0xF7` / `0xD6 0x20` 系列未进入本轮用例。
