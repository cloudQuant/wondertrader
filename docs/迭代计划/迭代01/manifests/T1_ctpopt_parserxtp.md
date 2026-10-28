# lane T1 交付清单 — TraderCTPOpt / ParserXTP（第六波·收发族）

- 用例文件（各目录一个测试目标，生产 TU 由 `dir:` 通配编入）：
  - `src/TestUnits/coverage/t_TraderCTPOpt/cov_traderctpopt_spi_test.cpp`（门禁 `gate_flags/t_TraderCTPOpt.sh` exit 0）
  - `src/TestUnits/coverage/t_ParserXTP/cov_parserxtp_spi_test.cpp`（门禁 `gate_flags/t_ParserXTP.sh` exit 0）
- 启用用例 30 条（TraderCTPOpt 18 / ParserXTP 12），DISABLED 3 条（2+1），缺陷登记 12 条。
- 行号均为本轮直读 `src/TraderCTPOpt/TraderCTPOpt.cpp(.h)`、`src/ParserXTP/ParserXTP.cpp(.h)` 实录。
- 访问缝：`CtpProbe : TraderCTPOpt` 暴露 protected 成员（规则 2 允许的公开入口继承），无 `#define private public`。

## TraderCTPOpt → `src/TraderCTPOpt/TraderCTPOpt.cpp`

| 用例（TEST 名） | 覆盖行段 |
|---|---|
| `factory_entries_create_and_delete_trader` | L59-75（createTrader/deleteTrader） |
| `init_reads_config_with_documented_defaults` | L243-281（缺省 flowdir L255-258、缺省 ctpmodule L260-264、L266-276 符号加载失败仍返回 true、L278 quick=false） |
| `init_honours_custom_flowdir_module_and_quickstart` | L243-281（显式 flowdir L253-258、自定义 module L260-264、L278 quick=true） |
| `make_entrust_id_writes_zero_padded_triple_and_increments` | L378-395（含 NULL/0 长度负分支 L380-381）、L1664-1667（generateEntrustID）、L1669-1692（extractEntrustID 三个返回点） |
| `register_spi_wires_basedata_mgr_and_opt_spi` | L397-406 |
| `unlogged_state_rejects_every_request_with_minus_one` | L413-428（login 判空）、L447-452（logout）、L467-475、L531-534、L576-581、L604-613、L704-711、L742-747、L763-768、L784-789、L806-811、L828-833、L1726-1729（isConnected）、L1757-1762（经回调间接入 confirm 守卫，见用例 settlement） |
| `request_ids_increment_monotonically` | L408-411（genRequestID） |
| `front_lifecycle_events_forward_to_sink` | L852-856、L858-863、L865-868、L1375-1378（OnRspError 空桩钉死）、L1726-1729 |
| `authentication_and_login_failure_branches_report_to_sink` | L870-885（认证失败分支）、L928-935（登录失败分支）、L938-943（登出回报）、L1726-1729 |
| `settlement_confirmation_state_machine_reaches_allready` | L945-998（L953-979 NULL 数据→CONFIRM_QRYED + L975-979；L984-997 确认回报→ALLREADY；带日期直达分支 L955-966）、L1757-1762（confirm 判空 -1）、L1726-1729 |
| `wrap_tables_convert_wts_enums_to_ctp_codes` | L77-219（13 个换算函数全表，含 L145/L157 兜底、L163 CFFEX 分支、L206-209 Unknown→Submitting） |
| `make_entrust_converts_input_order_with_ioc_variants` | L1475-1517（GFD/IOC×AV/MV/CV 三组合 L1492-1502、未知合约 NULL L1478-1479）、L1000-1012（OnRspOrderInsert）、L1702-1714（OnErrRtnOrderInsert）、L1598-1602（makeError）、L1694-1700（IsErrorRspInfo 三点） |
| `make_order_info_maps_ctp_order_fields_to_wts` | L1407-1473（L1421-1431 时态条件、L1439-1443 日期时间、L1445-1447 状态+拒单位、L1450 entrustID、L1456-1470 userTag 回退） |
| `make_trade_record_and_error_conversions` | L1604-1662（L1618-1621 时间、L1642-1650 方向/平昨仓换算与 amount）、L1598-1602、L1694-1700 |
| `rtn_order_and_trade_push_converted_records` | L1380-1392（OnRtnOrder）、L1394-1404（OnRtnTrade）、L1716-1724（OnRtnInstrumentStatus） |
| `exec_order_paths_route_to_opt_spi` | L1014-1026（OnRspExecOrderInsert）、L1054-1066（OnErrRtnExecOrderInsert）、L1042-1052（OnRtnExecOrder）、L1519-1573（makeOrderInfo(Exec)：L1542-1547 Nottouched/Canceled 两态）、L1575-1595（makeEntrust(Exec)）、L576-581（queryOrdersOpt 守卫） |
| `query_rsp_streams_fill_arrays_and_reset_state` | L1345-1373（OnRspQryOrder 错误/入队/清空）、L1315-1343（OnRspQryTrade）、L1068-1096（OnRspQryExecOrder，含 L1078 数组判空、L1088-1095 末帧转发）、L1112-1143（OnRspQryTradingAccount 含 L1133 balance 算式）、L1145-1162（结算单文本拼接+末帧透传） |
| `position_query_aggregates_today_cover_mode` | L1164-1313（L1174-1191 map 建/查、L1193-1198 CoverToday+Today 分支、L1206-1217 margin/avg、L1221-1236 可用扣减、L1280-1285 组合仓兜底、L1289-1312 末帧释放 map） |

DISABLED（不执行，触发条件与根因见 bug 文件）：
- `DISABLED_TraderCTPOpt.login_success_and_connect_require_real_ctp_lib` → 对应未覆盖段 L305-363（connect）、L430-445（doLogin）、L887-926（OnRspUserLogin 成功分支）、L1731-1755（queryConfirm 成功路径）、L1783-1795（authenticate）。
- `DISABLED_TraderCTPOpt.order_action_rsp_null_rspinfo_derefs_errormsg` → L1098-1110 / L1028-1040 的 NULL 解引用面。

## ParserXTP → `src/ParserXTP/ParserXTP.cpp`

| 用例（TEST 名） | 覆盖行段 |
|---|---|
| `factory_entries_create_and_destroy_parser` | L39-55（createParser/deleteParser + NULL 幂等） |
| `state_queries_before_init_answer_deterministically` | L81-92（ctor/dtor）、L137-140（release）、L163-173（disconnect 判空两分支）、L685-688（isConnected）、L690-696（registerSpi） |
| `register_spi_accepts_null_sink_without_touching_callbacks` | L690-696、L175-181（m_sink 空守卫经 write_log L29-30） |
| `on_error_filters_zero_error_id_and_logs_nonzero` | L175-181、L584-590（IsErrorRspInfo 三点）、L27-37（write_log） |
| `unsubscribe_marketdata_callback_is_documented_noop` | L198-201 |
| `sub_marketdata_error_path_formats_exchange_and_ticker` | L419-430、L432-443（SH/SZSE 三元表达式 L428/L441、成功空分支） |
| `on_disconnected_delivers_close_event_without_running_relogin_post` | L183-190（同步段；L191-195 posted 重登录 lambda 不执行，由事件计数钉死） |
| `depth_market_data_without_basedata_or_contract_bails_out` | L285-290（NULL mgr 早退）、L307-313（未知合约日志） |
| `depth_market_data_converts_snapshot_and_level1_queues` | L292-357（日期时间 L292-293、交易所分支 L296-305、checkValid 面 L324-354、期货分支 L332-339、十档复制 L347-354）、L356-357（handleQuote）、L360-414（买/卖一方队列，qsize=min(count,50)）、L416（release） |
| `tickbytick_converts_entrust_and_trade_branches` | L203-283（L205-226 合约解析/缺失日志、L229-231 日期时间、L233-256 逐笔委托、L257-282 逐笔成交含 TT_Cancel 判定 L272）、NULL mgr 早退 L205-208 |
| `subscribe_before_login_only_buffers_without_api_calls` | L592-607（未登录缓存分支：SSE./SZSE./其它前缀）、L681-683（unsubscribe 空） |
| `time_and_price_helpers_strip_colons_and_guard_sentinels` | L57-79（strToTime/checkValid 全函数） |

DISABLED：
- `DISABLED_ParserXTP.init_and_connect_require_vendor_lib_and_leak_thread` → 对应未覆盖段 L94-135（init）、L142-161（connect+worker）、L445-495（DoLogin）、L497-582（DoSubscribeMD）、L608-678（subscribe 登录后分支）。

## 已尝试但进不去的面（留给主控裁定）
- ParserXTP `subscribe` 登录后直订分支 L608-678 与 `DoSubscribeMD` L497-582：状态变量 `m_uTradingDate` 仅能由 `DoLogin`→真实 api 设定，私有且无窄缝。
- TraderCTPOpt 私有登录链 `authenticate/doLogin/queryConfirm/confirm`（TraderCTPOpt.h L49-55）：private，非本轮缝；见 bug T1-CTPOpt-06。
- 两个模块的 `OnRspExecOrderAction`（L1028-1040）仅 NULL 面可达（正常 pRspInfo 会走 bug T1-CTPOpt-03 描述的语义反转路径，已用 DISABLED 钉住未测）。
