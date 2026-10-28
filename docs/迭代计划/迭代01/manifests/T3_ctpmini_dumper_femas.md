# T3 manifest — 收发族第六波：TraderCTPMini / TraderDumper / ParserFemas

- 波次: 迭代01 第六波（收发族）lane **T3**
- 目标二进制: `CoverageTests_t_TraderCTPMini`、`CoverageTests_t_TraderDumper`、`CoverageTests_t_ParserFemas`
  （每目录一个目标：三套用例各自 `extern "C"` 定义/引用 createTrader/createParser 等同名入口，不得同处一个二进制）
- 用例目录:
  - `src/TestUnits/coverage/t_TraderCTPMini/ctpmini_trader_callbacks_test.cpp`
  - `src/TestUnits/coverage/t_TraderDumper/dumper_adapter_test.cpp`
  - `src/TestUnits/coverage/t_ParserFemas/femas_parser_callbacks_test.cpp`
- 各目录的 `gate_placeholder_test.cpp` 保留未动；未改任何 `CMakeLists.txt` 或 `src/**` 生产代码。
- 门禁（唯一合法口径，全部 exit 0，含 `-Werror`）:
  `sh docs/迭代计划/迭代01/tools/gate_flags/t_TraderCTPMini.sh <cpp>` → 0
  `sh docs/迭代计划/迭代01/tools/gate_flags/t_TraderDumper.sh <cpp>` → 0
  `sh docs/迭代计划/迭代01/tools/gate_flags/t_ParserFemas.sh <cpp>` → 0
- 用例规模: TraderCTPMini **23 启用 + 5 DISABLED**、TraderDumper **15 启用 + 4 DISABLED**、
  ParserFemas **14 启用 + 2 DISABLED**。
  披露：TraderCTPMini 超出「每模块 8~15 条启用」上限，全部是一次过门禁且各自有可证伪断言的既有分支，
  删减只会丢覆盖点，保留并请主控裁量。
- 手法说明（无 `#define private public`）: `TraderCTPMini` 的转换/构造函数与状态字段是 **protected**
  （`TraderCTPMini.h:125-158`），用测试派生类 `Probe` + `using` 声明暴露；`ParserFemas`/`TraderDumper`
  的私有字段只能走公开入口与回调注入。
- 落盘纪律: TraderDumper 用例统一 chdir 到 `mkdtemp("/tmp/wt_dumper_XXXXXX")`，配置文件/目录只落在临时目录。

## 一、ParserFemas → `src/ParserFemas/ParserFemas.cpp`（462 行）

| 用例（`femas_parser.*`） | 生产位置 | 覆盖的分支 |
| --- | --- | --- |
| `lifecycle_without_api_reports_not_connected` | L83-94 构造、L136-139 `release`、L141-149 `connect`、L151-161 `disconnect`、L451-454 `isConnected` | `if(m_pUserAPI)` 假分支（未 init 时全部走空守卫）、`connect` 无条件 `return true` |
| `register_spi_pulls_base_data_mgr_from_sink` | L456-462 `registerSpi`、L163-166 `OnRspError`、L341-345 `OnHeartBeatWarning` | listener==NULL 时不取 bdMgr；无 sink 时日志/事件守卫 |
| `subscribe_before_login_only_caches_codes` | L410-445 `subscribe`、L447-449 `unsubscribe` | `m_uTradingDate==0` 只缓存 `m_filterSubs`；空集合 |
| `front_connected_emits_connect_event_then_skips_login` | L28-38 `write_log`、L168-177 `OnFrontConnected`、L347-366 `ReqUserLogin` | `handleParserLog`+`handleEvent(WPE_Connect)`；`ReqUserLogin` 的 `m_pUserAPI==NULL` 早退 |
| `front_disconnect_reports_close_event_and_reason` | L203-210 `OnFrontDisconnected` | `WPE_Close` + `LL_ERROR` 日志里带原因码 |
| `logout_rsp_emits_logout_event` | L195-201 `OnRspUserLogout` | `WPE_Logout`；无 sink 分支 |
| `heartbeat_and_sub_rsp_are_sink_optional_noops` | L330-339 `OnRspSubMarketData`、L212-215 `OnRspUnSubMarketData`、L163-166、L405-408 `IsErrorRspInfo` | 恒 false 的错误判定 → 「无错误」分支；两个回报回调的空实现 |
| `login_rsp_skipped_when_not_last` | L179-193 `OnRspUserLogin` | `bIsLast && !IsErrorRspInfo` 短路前半 |
| `depth_market_data_dropped_without_data_mgr` | L217-222 | `m_pBaseDataMgr==NULL` 直接丢包 |
| `depth_market_data_dropped_when_contract_unknown` | L255-257 | `getContract()==NULL` 丢包 |
| `depth_market_data_converts_prices_and_times` | L59-73 `strToTime`、L75-81 `checkValid`、L224-328 | 字段换算：日期/时间 `10:15:30`+毫秒、价/量、五档买卖、DBL_MAX/FLT_MAX 归 0、`SettlementPrice!=DBL_MAX`、非 CZCE 成交额、`procFlag=1`、`trading_date=m_uTradingDate` |
| `czce_turnover_multiplied_by_vol_scale` | L277-285 | CZCE 分支 `Turnover*getVolScale()`；无效结算价不覆盖 |
| `strtotime_strips_colons` | L59-73 | 冒号剔除 + `convert::to_uint32`，含空串 |
| `extern_c_factory_round_trip` | L40-56 `createParser`/`deleteParser` | 工厂 + 幂等二次删除 |
| `DISABLED_init_needs_vendor_dylib_and_null_creator_crashes` | L96-134 | **未覆盖**：flowdir/module 默认值、`create_directories`、`m_funcCreator` 调用（空指针崩溃，T3-FEMAS-001） |
| `DISABLED_login_rsp_last_true_calls_null_api` | L179-193、L368-403 | **未覆盖**：`GetTradingDay`、`SubscribeMarketData` 的 `.` 分割与下发 |

## 二、TraderCTPMini → `src/TraderCTPMini/TraderCTPMini.cpp`（1377 行）

| 用例（`ctpmini.*`） | 生产位置 | 覆盖的分支 |
| --- | --- | --- |
| `not_ready_channel_rejects_every_request` | L270-285 `login`、L304-322 `logout`、L324-329 `orderInsert`、L421-424 `orderAction`、L460-551 四个 `query*`、L1328-1331 `isConnected` | 未登录/无 api 的全部 `-1` 拒绝分支 |
| `ready_state_still_rejects_without_api_pointer` | L421-458 `orderAction`、L1276-1298 `extractEntrustID` | 已就绪但 eid 畸形 → `-1`；`query*` 的 api 空指针分支 |
| `make_entrust_id_rejects_bad_buffer` | L236-239 | `buffer==NULL || length==0` |
| `make_entrust_id_formats_front_session_ref_and_increments` | L241-254 | `{:06d}#{:010d}#{:06d}` 精确串 + `m_orderRef.fetch_add(1)+1` 递增 |
| `generate_and_extract_entrust_id_roundtrip` | L1271-1274、L1276-1298 | 正反换算；缺 `#` 的两次 `return false` |
| `request_id_is_monotonic` | L265-268 `genRequestID` | 原子自增 |
| `wrap_direction_and_offset_tables` | L961-1021 | 方向×开平 4 组、开平 5 组、CTP→WTS 反向 8 组、持仓方向 |
| `wrap_price_time_action_and_order_state` | L1023-1081 | 市价单 CFFEX 五档换算、价格类型双向、时间条件、撤单标志、`Unknown→WOS_Submitting` |
| `is_error_rsp_info_and_make_error` | L1300-1306、L1209-1213 | NULL/0/非 0 三态；错误码→`WTSError` 映射 |
| `front_events_move_state_and_notify_sink` | L553-570、L648-653、L930-933 | `WTE_Connect`/`WTE_Close`(带原因码)/`WTE_Logout`；心跳与 RspError 空实现 |
| `register_spi_caches_bd_mgr_pointer` | L256-263 | 注册/反注册、`listener->getBaseDataMgr()` |
| `authenticate_failure_marks_login_failed_and_reports` | L572-587 | 认证失败 → `WS_LOGINFAILED` + `onLoginResult(false,msg,0)` + 错误日志 |
| `order_action_success_branch_pushes_trader_error` | L669-681 | 语义反转分支（无错误才上抛 `onTraderError`），见 T3-MINI-002 |
| `instrument_status_push_is_forwarded` | L1322-1326 | 交易所/合约/状态三元透传 + 无 sink 守卫 |
| `make_order_info_maps_fields_and_night_order_date` | L1084-1167 | 夜盘下单日期倒推（周一→-3 天）、`makeTime`、方向/开平/价格类型/FOK/FAK/NOR 三组合、`volLeft`、`OrderSubmitStatus>=InsertRejected`、eid 生成、userTag 回落 |
| `make_order_info_and_trade_return_null_for_unknown_contract` | L1086-1088、L1217-1219、L1171-1173、L935-959 | 三个构造函数查不到合约 → NULL；两个推送回调的空回报 |
| `make_trade_record_computes_amount_and_night_trade_date` | L1215-1269 | `volscale*vol*price` 成交额、非夜盘/夜盘（周二→-1 天）、方向与开平反向换算 |
| `rtn_trade_and_order_reach_the_sink` | L935-959、L1109-1110 | `onPushTrade`/`onPushOrder` 实参校验（成交额、日期、剩余量、状态字符映射） |
| `rsp_qry_trading_account_builds_balance_and_clears` | L683-720 | 非末笔累积、末笔 `onRspAccount`、`balance` 公式入参逐项、错误笔不入表 |
| `rsp_qry_position_cover_one_computes_available` | L722-868 | `CM_OpenCover` 分支（new/pre/availNew/availPre/avgPrice/margin 累计）、`CM_CoverToday` 空头分支（用 LongFrozen 扣减）、未知合约不入表、末笔释放 map |
| `rsp_qry_orders_and_trades_forward_counts` | L900-928、L870-898 | 两笔累积后末笔回调、错误回报不追加 |
| `release_is_safe_before_any_api_was_created` | L139-162 | `m_pUserAPI`/四个容器全 NULL 的守卫链 |
| `DISABLED_init_and_connect_need_vendor_dylib` | L100-137、L164-221 | **未覆盖**：配置读取、flowdir 默认、`create_directories`、`m_funcCreator` 调用、`SubscribePublicTopic`、查询线程 |
| `DISABLED_disconnect_joins_worker_thread` | L223-234 | **未覆盖**：`m_queQuery` lambda 体（L467-551 的四个 Req 查询请求）与 `join()` |
| `DISABLED_order_insert_positive_path_needs_api` | L324-419 | **未覆盖**：下单请求组装、`m_eidCache.put`、WOF_FAK/FOK 下发 |
| `DISABLED_login_and_rsp_user_login_needs_api` | L281-302、L589-646、L1333-1345 | **未覆盖**：`authenticate`/`doLogin`/登录成功回报（`GetTradingDay`、eid/oid 缓存 init 落盘） |
| `DISABLED_qry_account_last_without_record_crashes` | L713-719 | **未覆盖**：首笔即错误回报且 `bIsLast` → `m_ayFunds` 空指针 `clear()`（T3-MINI-001） |

## 三、TraderDumper → `src/TraderDumper/`（Dumper.cpp / TraderAdapter.cpp / TraderDumper.cpp / WtHelper.cpp）

| 用例 | 生产位置 | 覆盖的分支 |
| --- | --- | --- |
| `dumper.forwards_account_record_to_registered_callback` | `Dumper.cpp:133-142` `on_account`、`Dumper.h:28-31` `register_callbacks` | 14 个参数逐项透传 |
| `dumper.forwards_order_trade_and_position_records` | `Dumper.cpp:144-166` | `on_order`/`on_trade`/`on_position` 全参数透传（含 `isLast`） |
| `dumper.records_are_dropped_when_no_callback_registered` | `Dumper.cpp:135/146/154/162` | `_cb_* == nullptr` 守卫 |
| `TempCwd.dumper_init_with_missing_log_profile_is_a_noop` | `Dumper.cpp:17-20` `init` → `WTSLogger.cpp:204-214` | propFile 不存在 → 早退；`release()`（`Dumper.cpp:127-131`，`_worker` 为空） |
| `TempCwd.dumper_config_registers_adapters_even_when_module_load_failed` | `Dumper.cpp:22-101`（`load_from_file`、refresh_span、basefiles 三个 `if`、traders 循环、`root->release()`）、`TraderAdapter.cpp:52-114` 的 dlopen 失败分支、`TraderAdapter.cpp:116-125` `run` | active/空 channelid/inactive 三条目；dlopen 失败仍登记（T3-DUMP-002） |
| `TempCwd.dumper_config_from_content_skips_inactive_and_empty_module` | `Dumper.cpp:26-29` `load_from_content`、`TraderAdapter.cpp:67-68` module 空 | `isFile=false` + 缺 `config` 节的 `if(cfg)` 假分支 |
| `TempCwd.adapter_init_rejects_null_params_and_missing_module` | `TraderAdapter.cpp:52-114`（L54 params NULL、L60 `_cfg!=NULL`、L67 module 空、L74-79 工作目录/模块目录探测、L82-86 加载失败、L88-100） | 三类负路径 + `_id/_bd_mgr` 已写入可继续驱动 |
| `TempCwd.adapter_close_event_only_logs_and_connect_failure_marks_done` | `TraderAdapter.cpp:137-156` `handleEvent`、`175-178` `onLogout`、`329-337` `getBaseDataMgr/handleTraderLog`、`196-213` `onRspAccount`、`296-321` `onRspPosition` | `WTE_Close`/`WTE_Connect(ec!=0)`/其他事件；`_done` 真值绕开空 api |
| `TempCwd.adapter_error_report_and_null_guard` | `TraderAdapter.cpp:323-327` `onTraderError` | `if(err)` 两侧 |
| `TempCwd.adapter_forwards_orders_trades_and_positions_to_dumper` | `TraderAdapter.cpp:252-294`（`onRspOrders`/`onPushOrder`）、`215-226` `onPushTrade`、`311-313` 持仓透传参数表 | 记录落盘格式化：`idx==size-1` 的 isLast、合约回查、未知合约 return、`isAlive()` 真分支 |
| `TempCwd.adapter_position_list_without_contract_is_skipped` | `TraderAdapter.cpp:296-315` | `cInfo==NULL → continue` |
| `TempCwd.extern_c_entries_route_to_the_singleton_dumper` | `TraderDumper.cpp:6-35` 全部 5 个导出入口（`init/register_callbacks/config/release` + `getDumper`） | C 层 → 单例路由；`config` 用 `getBinDir()` 作 modDir |
| `adapter_mgr.add_get_and_duplicate_rules` | `TraderAdapter.cpp:344-423`（`addAdapter` 三种拒绝、`getAdapter` 命中/未命中、`run`/`release`/`decAlive`/`refresh` 空表） | 查重、空名、空指针、`isAnyAlive` |
| `wt_helper.cwd_and_module_dir_are_round_trippable` | `WtHelper.cpp:20-36` `get_cwd`、`WtHelper.h:14-20` | standardisePath 末尾 `/`、set/get_module_dir |
| `dumper.DISABLED_run_once_loops_forever_and_run_threaded_spawns_worker` | `Dumper.cpp:103-125` | **未覆盖**：`g_adapterMgr.run()`、`for(;;)` 轮询、worker 线程 |
| `trader_adapter.DISABLED_on_rsp_trades_always_derefs_null_api` | `TraderAdapter.cpp:228-250` | **未覆盖**：成交回报转发 + 无条件 `queryOrders()` |
| `trader_adapter.DISABLED_login_and_connect_success_paths_deref_null_api` | `TraderAdapter.cpp:143/158-173/180-194`、`413-423` `refresh` | **未覆盖**：登录成功链路、`_date` 读取、refresh 命中 done 适配器 |
| `dumper.DISABLED_config_without_basefiles_or_traders_crashes` | `Dumper.cpp:34/70` | **未覆盖**：缺节的空指针解引用 |

## 四、本轮未进入的面（供后续 lane / shim 评估）

1. 三个模块的 `init()` 后半段都需要厂商 dylib（`thosttraderapi`、`USTPmduserapiAF`）与
   `CreateFtdcTraderApi/CreateFtdcMduserApi` 导出符号；本机只有头文件（`src/API/CTPMini1.5.8`、
   `src/API/Femas3.02`）没有可加载库 → 建议做一个「假厂商 dylib」shim 目标（导出同名 mangled 符号、
   返回一张可控的 vtable），才能进入下单/查询/登录的正向分支。
2. `TraderCTPMini::connect/disconnect` 与 `TraderDumper::Dumper::run` 必然起线程；
   本 lane 按「不起线程」规则整条 DISABLED。
3. `TraderCTPMini::authenticate/doLogin/orderInsert/orderAction(正向)/query* 的 lambda 体`
   要求 `m_pUserAPI` 非空；`TraderAdapter` 的 `login/query*` 要求注入 `ITraderApi`，
   但该字段是 private 且无 setter（`TraderAdapter.h:70-76`）→ 只能靠 shim 或产品侧开测试缝。
4. `ParserFemas::m_uTradingDate` 为 private 且仅由登录回报写入 → 夜盘时间修正分支
   （`ParserFemas.cpp:228-253`，还依赖本机时钟）本轮无法确定性地进入。
