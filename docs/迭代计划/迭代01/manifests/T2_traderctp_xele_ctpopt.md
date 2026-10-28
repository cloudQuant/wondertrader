# lane T2 用例清单 — 收发族第六波（TraderCTP / ParserXeleSkt / ParserCTPOpt）

行号来源：本轮实测读取的 `src/TraderCTP/TraderCTP.cpp`(1519)、`src/ParserXeleSkt/ParserXeleSkt.cpp`(540)、
`src/ParserCTPOpt/ParserCTPOpt.cpp`(459)。三套用例目录各自对应一个 `CoverageTests_t_<模块>` 目标。

## 交付物

| 文件 | 启用 | DISABLED | 门禁 |
| --- | --- | --- | --- |
| `src/TestUnits/coverage/t_TraderCTP/t2_traderctp_conversion_test.cpp` | 19 | 1 | `t_TraderCTP.sh` exit 0 |
| `src/TestUnits/coverage/t_TraderCTP/t2_ctp_api_stub.hpp` | 生成物：`CThostFtdcTraderApi` 107 个纯虚函数的机械空实现（供派生 seam 注入） | — | 随上面的 TU 一起过 |
| `src/TestUnits/coverage/t_ParserXeleSkt/t2_parserxele_lifecycle_test.cpp` | 11 | 1 | `t_ParserXeleSkt.sh` exit 0 |
| `src/TestUnits/coverage/t_ParserCTPOpt/t2_parserctpopt_tick_test.cpp` | 13 | 1 | `t_ParserCTPOpt.sh` exit 0 |

每个目录原有的 `gate_placeholder_test.cpp` 未改动、未删除。

## 关键 seam（不改产品代码、不 `#define private public`）

- `TraderCTP.h:153-212` 是 **protected** 段 ⇒ 测试里的 `t2ctp::Probe : public TraderCTP`
  可直接读写 `m_pUserAPI / m_wrapperState / m_frontID / m_sessionID / m_orderRef / m_lDate / m_bInQuery / m_str* / m_queQuery / m_mtxQuery`。
- 柜台指针 `m_pUserAPI` 指向 `CtpApiStub`（`t2_ctp_api_stub.hpp`）的派生 `FakeCtpApi`：
  只做 memcpy 抓请求结构 + 返回可编程的 `stub_ret_`，不起线程、不开 socket、不链 `libthosttraderapi_se.a` 的调用。
- `TraderCTP::connect()` 起的 worker 线程不启动，用例用 `Probe::drainQueries()` 在**调用线程**上执行
  `m_queQuery` 里的 lambda（`TraderCTP.cpp:597-604, 620-627, 643-651, 667-675, 691-700, 1430-1442`），
  等价复现 worker 的那段逻辑而不需要线程/`sleep`。
- `ParserCTPOpt` / `ParserXeleSkt` 的成员全是 private（`ParserCTPOpt.h:90-114`、`ParserXeleSkt.h:64-93`），
  没有 protected 段 ⇒ 只能走 public 面：`ParserCTPOpt` 的厂商回调恰好是 public（`ParserCTPOpt.h:56-73`），
  行情转换主体可达；`ParserXeleSkt` 只有 `IParserApi` 那 8 个 public 方法可达。

## t_TraderCTP（生产文件：`src/TraderCTP/TraderCTP.cpp`）

| 用例（`t2_traderctp_conversion_test.cpp` 行号） | 证伪点（期望） | 生产行段 |
| --- | --- | --- |
| `init_reads_broker_user_and_defaults` (L424) | front 字符串入表、broker/user 落库、flowdir 规范化后带尾斜杠、quick 默认 false | L224-279 |
| `init_accepts_front_array_and_quick_flag` (L437) | front 数组两元素、`quick=true` 生效、缺省 flowdir 回落 `CTPTDFlow` | L226-252 |
| `release_without_api_only_marks_stopped` (L458) | `m_bStopped` 翻转、四个 NULL 容器/无 API 时安全 | L281-303 |
| `login_logout_and_queries_rejected_without_api` (L466) | `login/logout` 返回 -1，但参数已先吃进内存；状态仍 WS_NOTLOGIN | L415-430, L449-454, L1417-1420 |
| `order_and_query_rejected_when_state_not_ready` (L479) | 非 WS_ALLREADY 时 5 个 query + orderInsert/orderAction 全部 -1 且队列为空 | L469-475, L551-558, L589-705 |
| `authenticate_failure_marks_login_failed_and_reports` (L502) | ErrorID!=0 ⇒ WS_LOGINFAILED、不再发 ReqUserLogin、`onLoginResult(false,msg)` | L725-740, L56-59, L1476-1488 |
| `login_rsp_carries_credentials_and_session_params` (L527) | 认证通过后 ReqUserLogin 带 Broker/User/Pwd/ProductInfo；FrontID/SessionID/MaxOrderRef/TradingDay 落库；就绪事件 | L432-447, L742-797, L1422-1448 |
| `settlement_unconfirmed_goes_through_confirm` (L548) | 队列 1 条 ⇒ drain 后发 ReqQrySettlementInfoConfirm；ConfirmDate<交易日 ⇒ WS_CONFIRM_QRYED 并发确认；确认后 WS_ALLREADY | L1430-1442, L806-843, L1450-1474, L845-859 |
| `make_entrust_id_is_fixed_width_and_increments_ref` (L584) | NULL/0 长度返回 false；就绪后精确输出 `012345#0000000067#001001`，再调自增 | L381-399 |
| `order_insert_maps_types_and_ref_for_shfe` (L602) | 限价/买/开/IOC-AV/价格量正确，OrderRef 由 entrustID 尾段回填，usertag 进 eid 缓存并被订单回报反查 | L469-549, L110-139, L61-64, L76-91, L1373-1395, L1368-1371, L500, L1245-1258 |
| `order_insert_cffex_anyprice_and_close_offsets` (L653) | CFFEX 市价 ⇒ FiveLevelPrice；FOK ⇒ IOC+CV；昨仓 ⇒ `OF_Close`；强平 ⇒ `OF_ForceClose`；NOR ⇒ GFD+AV | L110-123, L141-152(未用), L504-530 |
| `order_action_rejects_bad_entrust_id_and_builds_cancel` (L688) | 无 `#` ⇒ -1 且不发请求；正常 ⇒ FrontID/SessionID/OrderRef/OrderSysID + `AF_Delete`/`AF_Modify` | L551-587, L1373-1395, L180-183, L1303-1315 |
| `queries_enqueue_then_send_on_drain` (L718) | 5 次查询 ⇒ 队列深度 5 ⇒ drain 后 5 个 ReqQry* 各 1 次，BrokerID/InvestorID/TradingDay 正确，请求号递增 | L589-705, L410-413 |
| `rtn_order_conversion_maps_state_and_night_date` (L743) | 未知合约不推送；量价/已成交/剩余、方向(Buy+Close⇒SHORT)、AnyPrice⇒ANY、IOC+CV⇒FOK、`PartTradedNotQueueing⇒WOS_Canceled`、夜盘 21:30 ⇒ 日期回退 1 天、`Unknown⇒WOS_Submitting` | L1164-1176, L1191-1261, L66-69, L93-108, L125-139, L167-178, L40-54, L1368-1371 |
| `trade_conversion_amount_and_usertag_chain` (L810) | 额=volScale*量*价=14000、Sell+Open⇒WDT_SHORT、成交日期夜盘修正、oid 缓存 usertag 反查回落 | L1178-1188, L1323-1366, L71-74 |
| `rsp_order_insert_and_action_error_paths` (L862) | 错误文本格式 `msg(ErrorID)`、entrust 通道计数、`OnRspOrderAction` 无错误码时不推错误 | L861-897, L1263-1301, L1317-1321, L1397-1409 |
| `rsp_account_position_and_settlement` (L917) | 非 bIsLast 不推送；余额=prev+平盈-手续费+入-出=103400；同 key 累加保证金、avgPrice、可用=持仓-冻结；未知合约推送空数组；结算单串接 | L899-930, L932-1078, L1080-1097, L93-108, L71-74 |
| `rsp_qry_order_and_trade_arrays` (L990) | 订单数组累积、成交数组累积 2 笔、错误回报只清不追加 | L1099-1157, L1129-1157 |
| `front_events_and_logout_and_status_pushes` (L1042) | WTE_Connect/WTE_Close(reason)/WTE_Logout 三类事件、心跳日志、`OnRspError` 文本、合约状态强转、logout 组包与返回码!=0 只写日志 | L707-723, L799-804, L1159-1162, L1411-1415, L449-467 |
| `DISABLED_connect_starts_worker_thread_and_needs_real_front` (L1099) | 不进：`connect()` L305-366 会 `CreateFtdcTraderApi`+`Init`+起 StdThread 常驻轮询（挂死风险），见 bugs 文件 | L305-366 |

`TraderCTP.cpp` 中本轮**未进入**的行段（原因见 bugs 文件）：L185-201（`createTrader/deleteTrader`，本轮没建 C 入口用例）、
L305-366（connect 全段）、L1491-1519（`triggerQuery` 已被源码注释掉）、L52-54（`strToTime` 仅在 makeOrderInfo/makeTradeInfo 经 `StrUtil::replace` 路径旁路）。

## t_ParserXeleSkt（生产文件：`src/ParserXeleSkt/ParserXeleSkt.cpp`）

| 用例（文件行号） | 证伪点 | 生产行段 |
| --- | --- | --- |
| `plugin_entry_points_build_and_destroy_parser` (L117) | `createParser()` 产出真 `ParserXeleSkt`；`deleteParser` 置空且二次调用安全 | L64-80, L84-97 |
| `init_builds_endpoints_without_opening_sockets` (L128) | 合法地址 init 返回 true 且**不开组播口**（isConnected 仍 false） | L99-117, L294-297 |
| `init_throws_on_unparsable_tcp_host` (L141) | `make_address` 抛 `boost::system::system_error` 穿透插件边界 | L110-114 |
| `init_throws_when_tcp_host_missing` (L150) | 缺 key 时同上（空串不可解析） | L101-111 |
| `register_spi_wires_sink_and_base_datamgr` (L158) | 首次注册取 `getBaseDataMgr()` 一次、无日志 | L318-329 |
| `register_second_sink_logs_replacement` (L164) | 替换监听时只有**新** sink 收到 “Listener is replaced” | L320-325 |
| `unregistering_sink_keeps_public_calls_crash_free` (L174) | `_sink=NULL` 后公开调用不崩（`write_log` 判空） | L26-35, L119-122, L300-316 |
| `subscribe_is_additive_and_repeatable` (L186) | 重复订阅幂等、不开口、不推行情 | L300-311 |
| `unsubscribe_is_an_empty_stub_today` (L198) | 空实现：不关口/不推事件/不写日志 | L313-316 |
| `release_body_is_empty` (L210) | 空实现可重复调用，状态不变 | L119-122 |
| `disconnect_without_connect_closes_nothing` (L218) | `_udp_socket==NULL` 跳过关口、返回 true、幂等、`doOnDisconnected` 不执行（无人 run io_context） | L279-292 |
| `DISABLED_connect_joins_multicast_and_starts_parser_thread` (L229) | 不进：`connect()` L265-277 → `reconnect()` L124-154 会 bind/join_group 并 new StdThread | L124-154, L265-277 |

**本轮无法覆盖的主体**：`prepare()` L156-262、`handle_udp_read` L343-390、`extract_buffer` L392-540
（`CXeleShfe*` 结构 → `WTSTickData` 的全部转换、价格缩放、`_tick_cache` 合并）——这些成员与数据全是
`private`（`ParserXeleSkt.h:50-62, 64-93`），无 protected seam，已写「留给主控裁定」。

## t_ParserCTPOpt（生产文件：`src/ParserCTPOpt/ParserCTPOpt.cpp`）

| 用例（文件行号） | 证伪点 | 生产行段 |
| --- | --- | --- |
| `plugin_entry_points_build_and_destroy_parser` (L188) | `createParser()` 产出 `ParserCTPOpt`，`deleteParser` 置空/二次安全 | L40-56, L84-95 |
| `fresh_parser_is_not_connected_but_lifecycle_is_safe` (L200) | `isConnected()==false`、`connect/disconnect` 只走判空分支返回 true、`release→disconnect` | L137-162, L448-451 |
| `register_spi_pulls_base_datamgr_once` (L209) | 未注册不取 datamgr；`registerSpi(NULL)` 安全 | L453-459 |
| `front_connected_and_close_events` (L218) | WPE_Connect/Close/Logout 三类事件 + 日志文本；`ReqUserLogin` 在 API 为空时早退 | L169-178, L344-349, L196-211 |
| `heartbeat_and_error_callbacks_do_not_reach_core` (L241) | 心跳只写日志；`IsErrorRspInfo` 恒 false ⇒ `OnRspError`/`OnRspSubMarketData`/`OnRspUnSubMarketData` 全空（错误被吞） | L338-342, L164-167, L402-405, L213-216, L326-336 |
| `login_rsp_without_is_last_is_ignored` (L270) | bIsLast=false ⇒ 不取 TradingDay、不推事件 | L180-194 |
| `depth_tick_without_datamgr_is_dropped` (L281) | `m_pBaseDataMgr==NULL` 早退；未登记合约早退（不产生 tick） | L218-223, L251-253 |
| `depth_tick_maps_prices_and_levels` (L295) | action_date/time=`103000100`、价/开/高/低/量/持仓/昨仓/涨跌停/昨收/昨结、五档买卖价量、`SettlementPrice==DBL_MAX` 不覆盖、`trading_date==0` | L225-324, L60-82, L126-139(数据类型头) |
| `depth_tick_invalid_values_are_zeroed` (L329) | DBL_MAX/FLT_MAX ⇒ 0；非 CZCE 且 Turnover==DBL_MAX ⇒ 保持 0 | L79-82, L265-281 |
| `czce_turnover_is_scaled_by_volscale` (L356) | CZCE ⇒ 换手值 × volScale(5)=5000；settle_price 正常写入；交易所串取 commInfo | L273-281, L260, L271-272 |
| `stale_tick_time_is_lifted_to_latest` (L372) | 时间戳倒退/为 0 时用 `m_uLatestTime + 本地增量` 抬升，绝不早于上一笔 | L235-249 |
| `first_zero_time_tick_falls_back_to_local_clock` (L399) | 首笔且时间戳为 0 ⇒ `TimeUtils::getDateTime` 覆盖成本地日期（≠ 报文 ActionDay） | L236-238 |
| `subscribe_and_unsubscribe_without_api_are_harmless` (L413) | 未登录时只填 `m_filterSubs`；API 为空 ⇒ 不发订阅；`unsubscribe` 空实现 | L407-412, L444-446 |
| `DISABLED_init_and_login_rsp_dereference_missing_ctpopt_library` (L427) | 不进：`init` L129-132 与 `OnRspUserLogin` L184 对 NULL API 解引用 ⇒ 段错误 | L110-135, L184 |

**本轮未进入**：`init()` L97-135 整体、`OnRspUserLogin` 的 bIsLast 分支 L184-193、`SubscribeMarketData()` L365-400、
`subscribe()` 的 `m_uTradingDate!=0` 分支 L413-441（全依赖 private `m_pUserAPI/m_uTradingDate`，无 seam）。
