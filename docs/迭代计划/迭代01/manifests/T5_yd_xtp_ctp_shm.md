# T5 lane 交付清单（收发族第六波：TraderYD / TraderXTP / ParserCTP / ParserShm）

- 用例目录：`src/TestUnits/coverage/t_TraderYD/`、`t_TraderXTP/`、`t_ParserCTP/`、`t_ParserShm/`
- 门禁（唯一合法口径，全部 `-Werror`，`-fsyntax-only`）：

```
for m in TraderYD TraderXTP ParserCTP ParserShm; do
  for f in src/TestUnits/coverage/t_$m/t5_*.cpp; do
    sh docs/迭代计划/迭代01/tools/gate_flags/t_$m.sh "$f"; done; done
```

实测（2026-10-31）：**6 个文件全部 rc=0**。

| 文件 | TEST 总数 | DISABLED_ | 启用 |
|---|---|---|---|
| `t_TraderYD/t5_yd_api_test.cpp` | 18 | 2 | 16 |
| `t_TraderYD/t5_yd_map_test.cpp` | 8 | 0 | 8 |
| `t_TraderXTP/t5_xtp_api_test.cpp` | 21 | 3 | 18 |
| `t_TraderXTP/t5_xtp_map_test.cpp` | 8 | 0 | 8 |
| `t_ParserCTP/t5_ctp_parser_test.cpp` | 15 | 1 | 14 |
| `t_ParserShm/t5_shm_parser_test.cpp` | 15 | 3 | 12 |
| 合计 | 85 | 9 | **76** |

`gate_placeholder_test.cpp` 四个目录各一份，未删、未加用例。
两个 `t5_*_map_test.cpp` 用的是「跨 TU 声明生产 `constexpr inline`/`inline` 自由函数」手法
（nm 实测符号为 `T`：`__Z17wrapDirectionTypeii`、`__Z11IsErrorInfoP16XTPRspInfoStruct` 等），
**如果主控不接受，删掉这两个文件即可**，其余 4 个文件不依赖其中任何符号。

---

## 1. t_TraderYD/t5_yd_api_test.cpp → `src/TraderYD/TraderYD.cpp`

夹具 `SetUp()` = `init(cfg)` + `registerSpi(&sink)`（规避 T5-01 未初始化成员），
`becomeCaughtUp()` = 公开回调 `notifyCaughtUp()`，把状态推到 `WS_ALLREADY` 且 `m_bCatchup=true`。

| 用例 | 生产文件:行段 | 覆盖的分支 |
|---|---|---|
| `init_accepts_config_and_falls_back_to_default_module` | `TraderYD.cpp:640-659`（`init` 全体：`646-648` 默认模块名回落、`654-655` load_library/get_symbol 失败→NULL）+ `740-754`（`login` 判 NULL）+ `772-780`（`logout` 判 NULL）+ `1114-1117`（`isConnected`） | init 正/负配置、未 connected 拒绝 |
| `all_business_calls_rejected_without_api` | `782-787`（`orderInsert` 守卫）、`839-842`（`orderAction` 守卫）、`868-873`/`885-890`/`915-920`/`940-945`（四个 `query*` 守卫） | 未登录/未就绪的 6 条拒绝返回值 |
| `make_entrust_id_validates_buffer_and_increments_ref` | `721-729`（NULL/len==0 两条否决 + `format_to "{}#{:010d}"`） | 格式化零填充宽度、自增 |
| `register_spi_then_ready_event` | `731-738`（`registerSpi`+`getBaseDataMgr`）、`247-251`（`notifyReadyForLogin`→`WTE_Connect`）、`33-43`（`write_log` 的 sink==NULL 早退） | 注册/事件外发 |
| `login_failure_callback_reports_and_keeps_disconnected` | `258-261`+`294-301`（`notifyLogin` 失败分支：`WS_LOGINFAILED`、`getErrorMsg`、`onLoginResult(false,...)`） | 登录失败状态机 |
| `DISABLED_login_success_branch_needs_real_api` | `260-293`（成功分支 `267` 解引用 `m_pUserAPI->getTradingDay()`） | 崩溃面，整条留白 |
| `failed_order_and_failed_cancel_ignored_before_catchup` | `304-307`、`327-330`（`if (!m_bCatchup) return;`） | 追数据前的吞回报 |
| `caught_up_publishes_login_success_only_once` | `610-624`（`m_bApiInited` 幂等守卫 + `onLoginResult(true,"",m_lDate)`） | 就绪状态机 |
| `failed_cancel_after_catchup_maps_error_code_to_wts_error` | `327-343`（1028 忽略分支 / 否则 `makeError`+`onTraderError`）、`1053-1057`（`makeError`）、`122-206`（`getErrorMsg`） | 撤单废单错误码映射 |
| `failed_order_after_catchup_converts_input_order_into_entrust` | `304-325`（两条分支都进：`makeEntrust` 命中 → `onRspEntrust`；NULL → `onTraderError`）、`1026-1051`（`makeEntrust` 全量字段）、`1095-1099`（`generateEntrustID`）、`90-105`/`55-58`/`60-71`（映射） | 厂商 `YDInputOrder`→`WTSEntrust` |
| `order_notification_maps_yd_order_into_wts_order_info` | `426-429`+`519-523`（转换+主推）、`965-1024`（`makeOrderInfo` 全体：`977-983` 方向/开平/价格类型/订单标志/量拆、`993` 状态、`1003-1004` 委托号与系统号、`1008-1012` userTag 回落） | 厂商 `YDOrder`→`WTSOrderInfo` |
| `order_zero_price_maps_any_price_and_rejected_sets_error_flag` | `980`（`price==0`→`WPT_ANYPRICE`）、`993-1001`（`YD_OS_Rejected`→`WOS_Canceled` + `setError`）、`107-120` | 拒单/全价单 |
| `order_of_unregistered_instrument_produces_no_push` | `967-971`（`getContract` NULL → return NULL） | 合约表未命中丢弃 + 换算入参可观测 |
| `trade_notification_maps_amount_with_volume_scale` | `527-530`+`605-607`、`1059-1093`（`makeTradeRecord`：`1070-1072` 量价成交号、`1080-1086` 方向/开平/引用单/成交类型、`1087-1088` 金额=`volScale*Volume*Price`、`1090` oidCache.get） | 厂商 `YDTrade`→`WTSTradeInfo` |
| `account_notification_guarded_by_empty_fund_array` | `626-633`（`m_ayFunds` 空 / `accInfo==NULL` 两条守卫） | 资金回调否决分支 |
| `ready_state_without_api_still_rejects_insert_and_bad_entrustid` | `784-787`（api 判空优先于状态）、`839-846`（状态已过 → `extractEntrustID` 失败→-1）、`1101-1112`（两个 `#` → false） | 撤单参数校验路径 |
| `DISABLED_order_action_with_valid_entrustid_dereferences_null_api` | `848`（`m_pUserAPI->getInstrumentByID`） | 崩溃面留白 |
| `yd_plugin_factory.create_and_delete_roundtrip_null_safe` | `208-224`（`createTrader`/`deleteTrader`，含 `NULL` 入参分支） | 工厂入口 |

**未进入**：`connect()` `681-711`、`doLogin()` `756-770`、`notifyFinishInit` `346-423`、
`notifyOrder/notifyTrade` 的本地缓存更新链 `432-516`/`536-603`（原因与判空缺失见 bugs T5-11/T5-15 与「未进入的面」）。

## 2. t_TraderYD/t5_yd_map_test.cpp → `src/TraderYD/TraderYD.cpp`（自由映射函数直测）

| 用例 | 行段 |
|---|---|
| `position_direction_only_long_is_long` | `45-48` |
| `vendor_direction_and_offset_fold_into_wts_direction` | `50-53`（4 组合 + 平今） |
| `wts_direction_and_offset_fold_back_into_vendor_direction` | `55-58` |
| `direction_roundtrip_is_stable_for_the_four_combinations` | `50-58` 往返闭合（并证明平今/平昨不闭合） |
| `offset_type_two_way_tables` | `73-88`、`90-105`（含 default 强平行） |
| `price_type_with_order_flag_precedence` | `60-71`（FAK/FOK 优先于价格类型） |
| `order_state_table_and_default_is_submitting` | `107-120`（6 个 case + default） |
| `error_msg_known_codes_and_unknown_fallback` | `122-206`（约 70 条 case 的 switch：正常码、1026/1028 交易所码、未知码回落 `"Unknown Error"`） |

## 3. t_TraderXTP/t5_xtp_api_test.cpp → `src/TraderXTP/TraderXTP.cpp`

夹具 `SetUp()` = `init(cfg)`（含 `flowdir`）+ 登记 SSE 现货(`CC_Stock`)与 SSE 期货(`CC_Future`)两张合约 + `registerSpi`。

| 用例 | 生产文件:行段 | 覆盖的分支 |
|---|---|---|
| `init_then_all_business_calls_rejected_without_api` | `582-621`（`init` 读 9 个参数、`604-606` creator=NULL）、`819-831`/`833-840`/`842-847`/`879-884`、`712-715`（`isConnected`） | init 正配置 + 5 条拒绝 |
| `init_defaults_flowdir_and_release_is_repeatable` | `599-602`（`_flowdir.empty()` 回落 `"XTPTDFlow"` + `standardisePath`）、`623-640`（`release` 三个判空）、`707-710`（`disconnect`→`release`） | init 负配置、可重复释放 |
| `make_entrust_id_format_and_reject` | `734-751`（NULL/len==0 否决、`741-743` 三段式 `user#day#ref`、try/catch 外壳） | 委托号格式与自增 |
| `on_error_routes_vendor_message_into_trader_log` | `357-361`（`_sink && error_info` 双守卫 + error_msg 落日志） | 厂商错误回调 |
| `on_disconnected_publishes_close_event_with_reason` | `345-348`（`WTE_Close` + reason）；`350-354` 的 `post` 只入队（不跑 io_context，不起线程） | 断线事件 |
| `cancel_error_guarded_by_error_id` | `400-409`（`IsErrorInfo` 真/假两条 + `WTSError(WEC_ORDERCANCEL)`） | 撤单失败 |
| `order_event_without_error_converts_stock_order` | `375-385`（无错分支→`makeOrderInfo`→`onPushOrder`→`release`）、`230-295`（`makeOrderInfo` 全体：`233-237` market→SSE/SZSE、`248` 价格类型、`251-258` 现货/期权分支、`260-268` 量拆与日期拆分、`270-277` 状态与 orderID、`279-283` eid 未命中→userTag 回落 entrustID） | `XTPOrderInfo/XTPQueryOrderRsp`→`WTSOrderInfo` |
| `order_event_szse_unregistered_and_rejected_mapping` | `238-240`（未登记→NULL）、`270-272`（`>= REJECTED`→`WOEF_Normal`）、`140-158`（default 状态） | 交易所归属换算 + 拒单标记 |
| `order_event_non_stock_uses_wrap_tables` | `253-258`（非现货方向走 wrap 表、非期权 offset 由 side 决定） | `XTP_SIDE_*`→WTS 方向 |
| `trade_event_converts_report_into_trade_info` | `388-398` + `297-343`（`makeTradeInfo`：`300-305` market 换算、`312-321` 量价成交号与时间拆分、`323-330` 方向/开平、`332-340` 引用单/成交额/userTag） | `XTPTradeReport`→`WTSTradeInfo` |
| `trade_event_of_unregistered_contract_dropped` | `305-307` | 合约未命中丢弃 |
| `query_order_accumulates_then_publishes_on_last` | `411-439`（`!IsErrorInfo && order_info` 累积、`is_last` 外发 + `clear`、`order_info==NULL` 时把 `_orders`(NULL) 直接透传） | 分页聚合与空指针透传现状 |
| `query_trade_skips_row_when_error_info_present` | `441-469`（`error_id!=0` 行被跳过 → 数组保持 NULL；非 last 不外发） | 查询回报错误过滤 |
| `query_position_maps_direction_and_availability` | `471-542`（`484-489` market/代码、`494-501` key 与新建持仓项、`502-514` 方向/昨今仓/保证金/成本/可用拆分、`518-541` last 聚合外发 + `release`） | `XTPQueryStkPositionRsp`→`WTSPositionItem` |
| `query_asset_maps_into_account_info` | `544-577`（`552` 三重条件、`554-568` 12 个字段映射、`570-575` 数组外发与释放） | `XTPQueryAssetRsp`→`WTSAccountInfo` |
| `query_asset_negative_deposit_withdraw_currently_lost` | `563-566`（`else if (<0) setWithdraw(0)`） | **现状断言**，锁定 bugs T5-05 |
| `order_event_with_error_converts_entrust_and_error` | `363-374` + `190-228`（`makeEntrust`：现货方向恒定、价格类型、offset、`221-225` eid 缓存） | 下单被拒→`onRspEntrust` |
| `DISABLED_connect_calls_null_creator_and_writes_flow_dirs` | `688-686`/`651-686`（`reconnect` 的 `fs::create_directories` + `m_funcCreator` 空调用 + 起 2s 重连线程） | 崩溃/线程面留白 |
| `DISABLED_query_apis_dereference_null_api_without_guard` | `901-951` | 见 bugs T5-02 |
| `DISABLED_order_event_error_branch_releases_null_entrust` | `365-373` | 见 bugs T5-03 |
| `xtp_plugin_factory.create_and_delete_roundtrip_null_safe` | `37-53` | 工厂入口 |

## 4. t_TraderXTP/t5_xtp_map_test.cpp → `src/TraderXTP/TraderXTP.cpp`

| 用例 | 行段 |
|---|---|
| `is_error_info_requires_nonzero_id_and_accepts_null` | `58-64`（NULL / id==0 / id!=0 三分支） |
| `position_direction_only_long_is_long`（`position_direction_short_only`） | `66-74` |
| `wts_side_to_vendor_side_uses_offset` | `76-88` |
| `vendor_side_to_wts_direction_uses_position_effect` | `90-102`（含默认 `pe=0` 的 `INIT` 分支） |
| `offset_type_two_way_and_closeyesterday_alias` | `104-116`、`118-130`（并证明 `124` 是不可达死分支，bugs T5-07） |
| `price_type_limit_versus_everything_else` | `132-138` |
| `order_state_table_and_default` | `140-158`（6 个 case + default） |
| `make_ref_id_is_seeded_and_monotonic` | `160-166`（时间播种 `%100==0` + `fetch_add` 连续） |

## 5. t_ParserCTP/t5_ctp_parser_test.cpp → `src/ParserCTP/ParserCTP.cpp`

夹具：`registerSpi(&sink)`（**不调 init**，见 bugs T5-11），登记 `SHFE.ag2512`(volScale=5) 与 `CZCE.SF509`(volScale=20)。

| 用例 | 生产文件:行段 | 覆盖的分支 |
|---|---|---|
| `lifecycle_without_init_keeps_api_null_and_returns_success` | `84-96`（构造/析构）、`148-156`（`connect` 判空仍 return true）、`158-168`（`disconnect` 判空）、`143-146`（`release`）、`448-451`（`isConnected`） | 未 init 的生命周期全链 |
| `register_spi_pulls_base_data_manager` | `453-459`（`registerSpi` + `getBaseDataMgr`）+ 换 sink 后旧 sink 不再收事件 | Spi 替换 |
| `front_connected_publishes_event_and_skips_login_request` | `175-184`（日志 + `WPE_Connect` + `ReqUserLogin`）、`368-373`（api 为空 early-return） | 前置连接事件 |
| `rsp_user_login_ignored_until_last_packet` | `186-189`（`bIsLast && !IsErrorRspInfo` 短路） | 非末包不动作 |
| `DISABLED_rsp_user_login_last_packet_dereferences_null_api` | `190`（`m_pUserAPI->GetTradingDay()`） | 见 bugs T5-09 |
| `rsp_error_publishes_nothing_at_all` | `170-173` + `426-429`（`IsErrorRspInfo` 恒 false） | 空实现的错误回调（bugs T5-08） |
| `subscribe_before_login_only_stores_filter` | `431-442`（`m_uTradingDate==0` → 只缓存）、`444-446`（`unsubscribe` 空实现） | 未登录订阅分支 |
| `heartbeat_and_logout_callbacks` | `362-366`（心跳日志）、`208-214`（`WPE_Logout`）、`350-360`（`OnRspSubMarketData` 两分支皆空）、`225-228`（`OnRspUnSubMarketData` 空） | 心跳/登出/订阅回执 |
| `ParserCTPNoSink.depth_dropped_without_base_data_mgr` | `232-235`（`m_pBaseDataMgr == NULL` 守卫） | 未注册 spi |
| `depth_of_unregistered_instrument_dropped` | `237-239`（`getContract` NULL → return） | 合约表未命中 |
| `depth_maps_core_fields_into_tick_struct` | `278-314`（`280-281` create+setContractInfo、`284` exchg 取品种表、`286-287` action_date/time、`289-294` 价/量/交易日、`295-305` 结算价与成交额非 CZCE 透传、`307-314` 持仓/涨跌停/昨收昨结）+ `344-345`（`handleQuote(tick,1)`）+ `60-77`（`strToTime("09:30:15")*1000+450`） | **CTP 深度行情→WTSTickData 主字段映射** |
| `depth_maps_five_level_quotes` | `316-342`（五档 ask/bid 价与量共 20 行展开） | 盘口映射 |
| `depth_sanitize_dbl_max_and_flt_max_sentinels` | `79-82`（`checkValid`：`DBL_MAX`/`FLT_MAX`→0）+ `289-292`/`309-313`/`317-328` 各字段的哨兵落值 | 无效值清洗 |
| `depth_czce_turnover_scaled_by_volume_scale` | `297-300`（`strcmp(exchg,"CZCE")==0` → `Turnover*volScale`） | 郑商所专属分支 |
| `ctp_plugin_factory.create_and_delete_roundtrip_null_safe` | `40-56` | 工厂入口 |

**未进入**：`init()` `98-141`（真实 `CreateFtdcMdApi` + cwd 建 flow 目录，macOS 分支还 `#ifdef` 掉了 dll 装载路径）、
`connect()`→`Init()`（起 IO 线程）、`disconnect()`→`Release()`、`DoSubscribeMD()` `389-424`（要 `m_uTradingDate!=0` 才进得来，
而交易日只能由会崩的 `OnRspUserLogin` 设置）、夜盘时间纠正块 `254-275`（依赖进程真实时钟，环境相关）。

## 6. t_ParserShm/t5_shm_parser_test.cpp → `src/ParserShm/ParserShm.cpp` + `ParserShm.h`

夹具 `SetUp()` = `registerSpi(&sink)`（`getGlobalSize()=8`，合约 `totalIndex=3`）——
**不建真实共享内存段、不起线程**（按 lane 约定，参考提交 `c3525f22`）。

| 用例 | 生产文件:行段 | 覆盖的分支 |
|---|---|---|
| `init_accepts_full_and_partial_config` | `76-83`（`path`/`gpsize` 默认 1000/`checkspan`/`cpu` 读取，恒 return true） | init 正/负配置 |
| `connection_state_and_disconnect_idempotent` | `271-276`（`disconnect` 置 `_stopped`）、`278-281`（`isConnected` 即 `_queue!=NULL`）、`85-90`（`release`：`_thrd_parser` 为空则不 join） | 连接状态判定 |
| `register_spi_warns_only_on_replacement` | `315-329`（`bReplaced` 两分支、`getBaseDataMgr`、`_subbed.resize(getGlobalSize())`） | 监听者替换 + 位图定尺寸 |
| `subscribe_splits_std_code_into_raw_code_and_exchange` | `284-298`（`split('.')` → `getContract(ay[1], ay[0])`、未登记 `continue`） | **合约代码双向换算/交易所归属** |
| `subscribe_uses_second_segment_only` | `292-293`（多段码只取第 2 段的现状语义） | 同上（可证伪） |
| `unsubscribe_splits_the_same_way` | `301-313`（同一套拆分 + `continue` 分支） | 退订换算 |
| `subscribe_empty_set_touches_nothing` | `284-298`、`301-313`（空集合循环体不执行） | 空入参 |
| `data_item_default_ctor_zeroes_whole_record` | `ParserShm.h:31-43`（`_DataItem` + `memset(this,0,sizeof)`） | 头部/条目清零语义 |
| `union_members_alias_at_same_address` | `ParserShm.h:34-40`（四成员 union 同址） | 线格式别名 |
| `queue_header_fields_and_item_offset` | `ParserShm.h:45-59`（`_capacity=N`、`_readable=UINT64_MAX`、`_writable`、`_pid` 构造初值 + `pack(8)` 下 items 落在偏移 32 + 取模落位在数组内） | **地址/尺寸计算** |
| `template_queue_honours_custom_capacity` | `ParserShm.h:45-55`（模板参数 N=16 时 capacity 与环形回绕下标） | 尺寸可配 |
| `DISABLED_subscribe_without_register_spi_uses_uninit_bd_mgr` | `63-69` + `293` | 见 bugs T5-12 |
| `DISABLED_subscribe_with_zero_global_size_writes_out_of_range` | `286-297` + `327` | 见 bugs T5-14（含可复现的 `ZeroSizeShmSink`） |
| `DISABLED_receive_path_needs_a_real_mapped_file` | `92-269`（整段接收状态机） | **留给主控裁定** |
| `shm_plugin_factory.create_and_delete_roundtrip_null_safe` | `43-59` | 工厂入口 |

---

## 环境事实（供门禁/主控核对）

- `getBinDir()` 下没有 `libyd*/libxtp*` dylib → 两个 Trader 的 `m_funcCreator` 恒 NULL → `m_pUserAPI/_api` 恒 NULL。
  这是本轮所有「拒绝分支可确定性覆盖、发送分支必崩」判断的根因（已用 `nm` 复核 `TraderYD.cpp.o`/`TraderXTP.cpp.o` 符号）。
- 未调用 `connect()`（YD 起 worker 线程、XTP 调 NULL creator、CTP 起 IO 线程、SHM 起解析线程），未 `sleep`，未连真柜台，
  未建 `shm_open`/`boost::interprocess` 段；`ParserShm` 的队列对象是普通 `new` 堆内存（约 6.8MB 一次分配，用后即 `delete`）。
- `WtKVCache::get` 在未 init 时安全（靠 `_indice` 未命中返回 `""`），所以所有 eid/oid 缓存未命中路径都能干净覆盖；
  `put` 不安全（bugs T5-04），故本轮不触发任何 put 路径。
- 用例里出现的目录写入：`TraderXTP::init` 只算路径不建目录（建目录在 `reconnect`，已被 DISABLED_），
  `ParserCTP::init` 会建 flow 目录（整条未调用）→ 本轮用例不在仓库里落任何文件。
