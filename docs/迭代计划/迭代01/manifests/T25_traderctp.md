# lane R25 用例清单 — TraderCTP 留白补写（第二刀）

生产文件：`src/TraderCTP/TraderCTP.cpp`（1519 行，可执行行 1103）。
本轮唯一目标二进制：`CoverageTests_t_TraderCTP`（由 `src/TestUnits/coverage/t_TraderCTP/` 目录名自动派生，未改 `src/TestUnits/coverage/CMakeLists.txt`）。

> 说明：主控派单里写的「`src/TraderCTP/TraderSpi.cpp`」在本仓库不存在（`ls src/TraderCTP/` 只有
> `TraderCTP.cpp/.h/CMakeLists.txt/vcxproj*`）。`TraderSpi` 只在 `src/CTPLoader/` 与 `src/CTPOptLoader/`
> 有实体文件，属于别的 target（`t_CTPLoader` / `t_CTPOptLoader`），不在本 lane 范围。
> `TraderCTP` 的 SPI 回调就是 `TraderCTP` 自己（`TraderCTP.h:32` 多重继承 `CThostFtdcTraderSpi`），
> 所以本轮覆盖对象 = `TraderCTP.cpp` 全量。

## 交付物

| 文件 | 启用 | DISABLED | 说明 |
| --- | --- | --- | --- |
| `src/TestUnits/coverage/t_TraderCTP/t25_traderctp_gap_test.cpp` | 41 | 5 | 本轮全部用例 |
| `src/TestUnits/coverage/t_TraderCTP/t25_ctp_api_stub.hpp` | — | — | R25 自带 `CThostFtdcTraderApi` 空实现（107 个纯虚函数，计数器为 `std::atomic`，可观察 RegisterFront/Subscribe*Topic/Release/Init），**不复用** lane T2 的 `t2_ctp_api_stub.hpp`，两文件互不依赖 |
| `src/TestUnits/coverage/t_TraderCTP/gate_placeholder_test.cpp` | 1 | — | 未改动 |
| `src/TestUnits/coverage/t_TraderCTP/t2_traderctp_conversion_test.cpp` | 19 | 1 | lane T2 原有，未改动（见下面「与 lane T2 的关系」） |

门禁（`docs/迭代计划/迭代01/tools/case_runner.sh`，CASE_TIMEOUT=60，`-P 4`，运行目录新建）：
`cmake --build build-cov --target CoverageTests_t_TraderCTP` → exit 0；
枚举 61 条（R25 41 + T2 19 + 占位 1）→ **50 PASS / 11 FAIL / 0 CRASH / 0 HANG / 0 RUNDIR**；
11 条 FAIL 全部是 lane T2 的既有用例（与本轮开工前的基线逐条同名同判，见「回归控件」）。

## 覆盖率账（只算 `src/TraderCTP/TraderCTP.cpp`，用同一套 llvm-cov 度量）

| 阶段 | 可执行行 | 已覆盖 | 未覆盖 |
| --- | --- | --- | --- |
| 本轮开工前（lane T2 的 20 条跑完） | 1103 | 818 | **285** |
| 本轮交付后（61 条全跑） | 1103 | 1075 | **28** |

净增 **+257 行**。度量口径：`xcrun llvm-profdata merge -sparse` + `xcrun llvm-cov export -format=lcov
-object build-cov/TestUnits/coverage/CMakeFiles/CoverageTests_t_TraderCTP.dir/__/__/TraderCTP/TraderCTP.cpp.o`，
只取 `DA:` 行；本轮基线与终值同法同对象，可直接对账。百分比留给主控统一度量。

## 关键 seam（不改产品代码、不 `#define private public`）

- `TraderCTP.h:153-212` 是 **protected** 段 ⇒ `t25ctp::Probe : public TraderCTP` 可读写
  `m_pUserAPI / m_wrapperState / m_funcCreator / m_strFront / m_bQuickStart / m_queQuery / m_mtxQuery /
  m_bInQuery / m_bStopped / m_eidCache / m_oidCache / m_ayOrders / m_ayTrades / m_ayPosDetail / m_mapPosition`。
- 柜台指针指向 `t25ctp::StubApi`：只 memcpy 抓 12 个 `Req*` 的请求结构 + 返回可编程的 `ret_`，
  不起线程、不开 socket、不调厂商实现。
- **`connect()` 这一刀是本轮新开的**：`m_funcCreator` 被换成 `t25ctp::fakeCreator`（L206-207 的函数指针成员），
  所以 `TraderCTP.cpp:310` 拿到的是替身指针，`CreateFtdcTraderApi`（厂商真实现）一次都没执行；
  worker 线程（L336-364）被真实创建并在 `disconnect()` 里被 join，用例内不出现无 join 的 `std::thread`
  （`TearDown` 有 `killWorker()` 安全网：先置 `m_bStopped`、再 join、再置空 —— `StdThread` 就是 `std::thread`，
  带着 joinable 线程析构会 `std::terminate`，这条是本轮实测踩到的）。
- 查询类接口压 `m_queQuery`：无 worker 的用例用 `drainQueries()` 在调用线程上执行；
  有 worker 的用例只允许**有限轮询**（`waitUntil`，默认上限 6s），绝不 sleep 等真柜台。
- 状态机前置（照 FAIL裁定记录 A-07 实测结论写死）：
  `login()` → `OnRspAuthenticate`（非错误分支 ⇒ 内部 `doLogin()`，只到 `WS_LOGINING`）→
  `OnRspUserLogin`（`WS_LOGINED` 唯一置位点在 L746，并且是 `queryConfirm()` 的唯一入队点 L787）→
  drain ⇒ `ReqQrySettlementInfoConfirm` → `OnRspQrySettlementInfoConfirm`（`ConfirmDate>=m_lDate` ⇒ `WS_ALLREADY`；
  否则 `WS_CONFIRM_QRYED` + `confirm()`）→ `OnRspSettlementInfoConfirm`（仅当状态是 `WS_CONFIRM_QRYED` ⇒ `WS_ALLREADY`）。
  `bringToLogin()/bringToReady()` 就是这两条链，前置里带的 `FrontID=12345 / SessionID=67 / MaxOrderRef=1000 /
  GetTradingDay()="20240115"` 让委托号固定为 `012345#0000000067#001001`。
- 中文字段：本轮实测产品侧全是**字节比较**（`fmtutil::format` + `strncpy`/`wt_strcpy` 原样搬运），
  UTF-8 的「资金不足(15)」「撤单被拒」「已受理」在断言里逐字命中，不涉及 GBK 转码，
  所以 lane T2 那条 `资金不足(15)` 红是**断言选错了通道**（`OnRspOrderAction` 走 `WTSError::create(ec, ErrorMsg)`，不带 `(ErrorID)` 后缀），不是编码缺陷；本轮把两条通道的差异分别钉死。

## t_TraderCTP 逐用例（生产文件 `src/TraderCTP/TraderCTP.cpp`）

「生产行段」是本轮实测进入的行（llvm-cov 计数 > 0 的行），不是猜测。

| 用例（`t25_traderctp_gap_test.cpp` 行号） | 证伪点（期望） | 生产行段 |
| --- | --- | --- |
| `plugin_entry_points_build_and_destroy_trader` (L595) | `createTrader()` 产出真 `TraderCTP`；未连柜台时 `isConnected()==false`；`release()` 不炸；`deleteTrader` 把入参置空且二次调用安全 | L185-201（本轮首次进入） |
| `init_falls_back_to_ctptdflow_and_ignores_non_string_front` (L613) | `front` 是整数时**不**入表（0 个）；缺 `flowdir` ⇒ `"CTPTDFlow/"`；缺 `user/pass/appid/authcode` ⇒ 空串；`quick=true` 生效 | L226-252, L254-255, L276-278 |
| `init_keeps_trailing_slash_of_given_flowdir` (L630) | front 数组两元素按序入表；flowdir 规范化后 = `tmp + "/"` | L233-239, L247-252 |
| `str_to_time_strips_colons_and_parses_to_uint` (L650) | `strToTime("09:30:15")==93015`、`"21:30:05"==213005`、`""==0`、无冒号原样解析 | L40-54（本轮首次进入；产品内 0 调用点，经外部链接声明直连） |
| `login_rsp_with_error_marks_login_failed_and_never_queries_confirm` (L661) | ErrorID≠0 ⇒ `WS_LOGINFAILED`；FrontID/SessionID/交易日/委托号**一个都不落库**（0/0/0）；不压确认查询（depth 0）；`onLoginResult(false,"CTP:登录失败",0)` | L742-744, L789-796 |
| `login_rsp_with_null_rspinfo_is_taken_as_success` (L692) | `pRspInfo==NULL` 被 `IsErrorRspInfo` 当成功 ⇒ 会话参数落库（7/8/50）、`WS_LOGINED`、压 1 条确认查询、但**不**报登录成功 | L56-59, L742-753, L784-787, L1422-1431 |
| `do_login_send_failure_is_logged_and_state_stays_logining` (L719) | `ReqUserLogin` 返回 5 ⇒ 日志含 `Sending login request failed: 5`，状态停在 `WS_LOGINING` | L432-447 |
| `order_insert_and_action_send_failures_are_logged_but_still_return_zero` (L733) | `ret_=9` ⇒ `orderInsert/orderAction` 仍返回 0，日志分别是 `Order inserting failed: 9` / `Sending cancel request failed: 9` | L542-546, L580-584 |
| `authenticate_return_code_is_swallowed_and_logs_nothing` (L753) | `ReqAuthenticate` 返回 7 ⇒ **0 条日志**、状态仍 `WS_LOGINING`（与其余 4 个 Req* 不一致，见 BUG-T25-004） | L1476-1488 |
| `order_insert_without_usertag_reuses_current_order_ref_and_skips_cache` (L767) | 无 usertag ⇒ `req.OrderRef=="1000"`（`fetch_add(0)` 不自增）、eid 缓存条目数不变、默认价型/`GFD+AV` | L485-489, L504-520, L534-540 |
| `order_insert_ignores_malformed_entrust_id_when_usertag_set` (L785) | usertag 非空但 entrustID 无 `#` ⇒ 返回值被丢弃：**请求照发**（`n_insert_==1`）且畸形 key 被写进 eid 缓存（见 BUG-T25-002） | L490-501 |
| `order_insert_wraps_bestprice_lastprice_and_every_offset` (L800) | `WPT_BESTPRICE⇒'3'`、`WPT_LASTPRICE/未知⇒'4'`、`WOT_CLOSETODAY⇒'3'`、`WOT_CLOSEYESTERDAY⇒'1'`、`WOT_FORCECLOSE⇒'2'`；FOK⇒`IOC+CV`、FAK⇒`IOC+AV`、NOR⇒`GFD+AV`；`HF_Speculation/CC_Immediately/FCC_NotForceClose/IsAutoSuspend=0` | L76-91, L110-123, L504-530 |
| `order_insert_direction_flag_is_swapped_for_short_close` (L839) | 方向四象限：Long+Open⇒Buy、Long+Close⇒Sell、Short+Open⇒Sell、**Short+Close⇒Buy**；非 CFFEX 市价⇒`AnyPrice`，CFFEX 市价⇒`FiveLevelPrice`；合约/交易所原样入请求 | L61-64, L110-115, L479-483, L504 |
| `order_action_rejects_entrust_id_with_only_one_hash` (L883) | 只有一个 `#` ⇒ `-1` 且**不发**请求；合法三段 ⇒ `FrontID=12345 / SessionID=67 / OrderRef="42" / AF_Delete` | L560-573, L1373-1395（L1385-1387 本轮首次进入） |
| `disconnect_without_worker_only_queues_release` (L906) | 没有 worker 时 `disconnect()` 只压队列、不 join、不自检：`queryDepth==1`、`m_bStopped` 仍 false、`Release()` 未调；手动 drain 后才落地（钉住 lane T2 的 T2-CTP-03 现状） | L368-379 |
| `release_after_ready_clears_api_and_every_container` (L923) | 有 API 时 `Release()` 恰好 1 次且 `m_pUserAPI` 置 NULL；orders/trades/map/posDetail 四个容器被 `clear()`（对象仍在、size 归 0）；`m_bStopped` 翻转 | L281-303（L285-303 的 true 支本轮首次进入） |
| `connect_with_stub_creator_wires_spi_fronts_and_worker_sends_query` (L973) | creator 收到 `flowdir+"flows/9999/88880001/"` 且目录真被创建；`RegisterSpi` 的指针 == TraderCTP 自身；`RegisterFront` 逐条 front + `registerFront: ...` 日志；`quick=false⇒RESUME/RESUME`、`quick=true⇒QUICK/QUICK`；第二次 `connect()` 不重复起线程；worker 真的把 `ReqQryTradingAccount` 发出去（BrokerID/InvestorID 正确）；发出后 `m_bInQuery` 必须仍是 true，只有 `OnRspQryTradingAccount(bIsLast=true)` 才复位；`disconnect()` join 后线程消失、`Release()` 落地 | L305-366（全段本轮首次进入）, L589-610, L899-905 |
| `settlement_confirm_older_than_trading_day_goes_through_confirm` (L1037) | 确认查询在 drain 前 depth=1、`n_qry_confirm_` 从 0 起；`ConfirmDate=20240112 < m_lDate` ⇒ 只到 `WS_CONFIRM_QRYED`、不报就绪；`confirm()` 请求体 BrokerID/InvestorID 正确、`ConfirmDate` 是**当天真实日期**（8 位）、`ConfirmTime` 恰 8 字符且形如 `HH:MM:SS`；随后 `OnRspSettlementInfoConfirm` ⇒ `WS_ALLREADY` + `onLoginResult(true,"",20240115)` | L806-843, L845-857, L1450-1474 |
| `settlement_confirm_query_with_null_field_or_error_keeps_or_skips_state` (L1078) | `bIsLast=false` ⇒ `m_bInQuery` 不复位；带 ErrorID 时（无论 bIsLast）**整个分支都不进** ⇒ 状态不变、`n_confirm_` 不涨；`bIsLast=true`+错误 ⇒ 只复位 `m_bInQuery`；对象为 NULL 且无错误 ⇒ `WS_CONFIRM_QRYED` + 发 1 次确认 | L806-843（L814 false 支、L836-840 本轮首次进入） |
| `settlement_confirm_rsp_only_acts_in_confirm_qryed_state` (L1103) | 状态不是 `WS_CONFIRM_QRYED`／带错误／回报对象为 NULL —— 三种都完全不动（0 次 `onLoginResult`）；三者齐备才 `WS_ALLREADY` | L845-859 |
| `confirm_send_failure_logs_error_and_state_stays_confirm_qryed` (L1135) | `ret_=8` ⇒ 日志 `Sending confirming of settlement data failed: 8`、状态停在 `WS_CONFIRM_QRYED`、不报就绪；随后柜台补一个确认成功回报仍能进 `WS_ALLREADY`（发送失败不回滚，见 BUG-T25-009） | L1458-1471 |
| `query_confirm_send_failure_is_logged_inside_worker_item` (L1156) | drain 后 `n_qry_confirm_==1`、日志 `Sending query of settlement data confirming state failed: 7`、且**没有**发确认请求 | L1437-1441 |
| `position_cover_today_short_today_row_subtracts_long_frozen` (L1191) | 空头今仓 5、多头冻结 2 ⇒ 可平今 3、可平昨 0、均价 35000/5/1=7000、方向 `WDT_SHORT`、币种/交易所取自 commInfo；同批 ag2602 多头今仓 1、空头冻结 3 ⇒ 1-3<0 ⇒ 可平今夹零；`bIsLast` 后 `m_mapPosition` 被释放并置 NULL | L945-1001（L995-1001 本轮首次进入）, L1054-1077 |
| `position_cover_today_history_rows_clamp_negative_availability` (L1228) | 昨仓那一支：多头昨仓 4、空头冻结 1 ⇒ 可平昨 3；空头昨仓 3、多头冻结 5 ⇒ 3-5<0 ⇒ 夹零；均价 0（成本 0）；非 bIsLast 一条都不推 | L962-963, L1003-1017（本轮首次进入） |
| `position_direct_cover_splits_new_pre_and_both_freeze_sides` (L1261) | 非 `CM_CoverToday`：今仓=`TodayPosition`、昨仓=`Position-TodayPosition`；多头今仓 4/总 10/空冻 1 ⇒ 可平今 3、可平昨 10-0-1-3=6、均价 350000/10/50=700；空头 ⇒ 可平今 2-1=1、可平昨 5-1-0-1=3；IF2403 空头今仓 1、多头冻结 4 ⇒ 可平今夹零而**可平昨落 -3**（这一支只夹今）；未知合约行被忽略但 `bIsLast` 仍推 3 条 | L965-969, L1019-1038（本轮首次进入）, L1059-1068 |
| `position_combination_skips_availability_and_zero_margin_clears_it` (L1310) | `CC_Combination` ⇒ 完全不算可用持仓；同 key 两行累加成 `今 5 + 昨 -8 = -3`、保证金 0 ⇒ 命中「有仓位但保证金为 0」兜底 ⇒ 可平今/可平昨都归 0 | L984-1050（L1040-1043、L1045-1050 本轮首次进入） |
| `position_zero_total_sets_avg_price_to_zero_and_still_pushes` (L1334) | 总持仓 0 ⇒ 均价强制 0（不做除法）；保证金与浮动盈亏仍照累加；0 持仓也照样推 1 条 | L975-982 |
| `position_error_last_row_pushs_accumulated_then_map_is_reset` (L1351) | 带错误的 `bIsLast`：不累加、仍推已攒的 1 条、map 释放；第二次 `bIsLast`（map 已空）⇒ 推 0 条 | L940, L1054-1077 |
| `usertag_round_trips_from_eid_cache_into_order_then_trade` (L1379) | 委托号定宽串精确等于 `012345#0000000067#001001`；带 usertag 下单 ⇒ `req.OrderRef=="1001"` + eid 缓存 1 条 + oid 缓存 0 条；订单回报（同 front/session/ref）⇒ usertag 反查回 `TAG-25`、`OrderSysID` 非空时写 oid 缓存（值 `TAG-25`）；成交回报（同 OrderSysID）⇒ usertag `TAG-25`、金额 `1*1*7000`、方向/开平/RefOrder/日期齐备 | L381-399, L490-501, L1191-1261（L1250-1258 本轮首次进入）, L1323-1366（L1361-1363 本轮首次进入） |
| `order_rsp_maps_rejected_submit_status_and_mv_volume_and_reverse_types` (L1455) | `OrderSubmitStatus>=InsertRejected` ⇒ `isError()` 为真，`< InsertRejected` ⇒ 假；`IOC+MV` ⇒ FAK（与 `IOC+CV`⇒FOK 区分）；`BestPrice⇒WPT_BESTPRICE`、`AskPrice1⇒WPT_LASTPRICE`、`CloseToday/CloseYesterday/ForceClose⇒WOT_*`；`Sell+CloseToday⇒WDT_LONG`；`InsertDate!=m_lDate` 时夜盘修正不触发；委托号按回报里的 front/session/ref 生成，未命中缓存时 usertag 回落自身 | L93-108, L125-139, L1205-1237（L1212、L1236-1237 本轮首次进入）, L1245-1258 |
| `entrust_rsp_maps_ioc_cv_to_fok_and_usertag_from_cache` (L1508) | `OnErrRtnOrderInsert`：合约/交易所/委托号（由 `m_frontID/m_sessionID` 生成）/usertag/量价全部反查正确；`IOC+CV⇒FOK`、`IOC+MV⇒FAK`、`IOC+AV⇒FAK`、`GFD⇒NOR`；`FiveLevelPrice⇒WPT_ANYPRICE`、`Sell+Close⇒WDT_LONG`；错误文本 `资金不足(15)`（带括号后缀，和 `OnRspOrderAction` 通道区分） | L1263-1301（L1285-1291、L1296-1298 本轮首次进入）, L1317-1321, L1397-1409 |
| `rsp_order_insert_with_unknown_contract_pushes_only_trader_error` (L1566) | 未知合约 + 错误 ⇒ 只走 `onTraderError("不存在合约(20)")`，entrust 通道 0 条；未知合约 + 无错误 ⇒ 两个分支都不进（完全静默）；`OnErrRtnOrderInsert` **没有** else-if 兜底 ⇒ 同样的输入两条通道都是 0 条（错误被丢掉） | L861-880（L873-879 本轮首次进入）, L1263-1267, L1397-1409 |
| `rsp_order_action_error_carries_action_fields_and_bare_message` (L1595) | 撤单被拒：`WTSError` 文本是**裸 ErrorMsg**（`撤单被拒`，无 `(30)`）；附带的 action 有合约/交易所/委托号（`generateEntrustID` 用回报里的 front/session/ref）/usertag `TAG-C`/`OrderSysID`；无错误码时一条回报都不发 | L882-897, L1303-1315（L1310-1312 本轮首次进入） |
| `trade_with_unknown_contract_is_dropped_by_both_channels` (L1635) | 未知合约 ⇒ `OnRtnTrade` 不推（`makeTradeInfo` 返回 NULL），`OnRspQryTrade` 不累加但仍回调（size 0） | L1178-1188, L1323-1327 |
| `qry_order_and_trade_rows_accumulate_and_clear_after_last` (L1655) | 订单：非 bIsLast 两行 ⇒ 数组攒到 2；带错误的 bIsLast ⇒ 不追加、推 2 条、随后 `clear()`（数组对象仍在、size 0）、`m_bInQuery` 复位；成交：夜盘 `21:10:00` 且成交日==交易日 ⇒ 日期回退到 `20240114`；bIsLast 那一行**也先进数组** ⇒ 推出 2 条 | L1099-1157（含 L1119-1126 的 `m_ayTrades` 支）, L1323-1350 |
| `settlement_info_concatenates_rows_and_pushes_only_when_non_empty` (L1708) | 两行 Content 串接成 `line-Aline-B`、日期取最后一行的 `20240115`；`querySettlement(20240116)` 先清空缓冲并把 `TradingDay="20240116"` 装进请求；随后带错误的回报不拼进内容 ⇒ 缓冲为空时**不推**（`n_settlement_` 保持 1） | L683-705, L1080-1097 |
| `order_flag_stays_zero_when_time_condition_is_neither_gfd_nor_ioc` (L1736) | `TimeCondition='4'(GTD)` ⇒ 订单通道 `orderFlag==0`（既不是 NOR/FAK/FOK），委托通道 `orderFlag==WOF_NOR` ⇒ 同一回报两条通道给出不同标志（见 BUG-T25-006）；同函数其它字段都被显式覆盖 | L1205-1215（无 else 支）, L1281-1291 |
| `front_and_heartbeat_paths_are_quiet_when_sink_is_unregistered` (L1793) | `registerSpi(NULL)` 之后：`orderInsert(NULL)` 返回 -1 且 0 条日志（`write_log` 的 `sink==NULL` 早退，L32-33 本轮首次进入）；`OnFrontConnected/OnFrontDisconnected/OnRspUserLogout/OnHeartBeatWarning` 都不发事件、不打日志；认证失败仍落 `WS_LOGINFAILED` 但不发 `onLoginResult`；重新登记后 `WTE_Connect` 恢复 | L26-38, L469-475, L707-723, L799-804, L725-740 |
| `front_disconnect_resets_state_and_logout_reports_reason` (L1825) | 断线 ⇒ `WS_NOTLOGIN` + `WTE_Close(200)`；**在途的确认查询回报仍能把状态直接抬到 `WS_ALLREADY`**（该回调不看前置状态，见 BUG-T25-008）；`logout()` 组包 BrokerID/UserID 正确；登出回报 ⇒ `WS_NOTLOGIN` + `WTE_Logout(0)`，且就绪链本身不发事件（事件总数恰 2）；心跳只 1 条日志、状态不动；`OnRspError` 文本精确 `4211 rsp error: 15 : 资金不足`；合约状态回报原样强转 `TS_Continous` | L449-467, L707-723, L799-804, L806-843, L1159-1162, L1411-1415 |
| `confirm_guard_rejects_when_api_is_gone_or_state_moved_on` (L1886) | 已 `WS_CONFIRM_QRYED` 但 `m_pUserAPI==NULL` ⇒ `confirm()` 被 L1452-1455 挡住：**不多发**确认请求、状态原地、不报就绪 | L1451-1455（本轮首次进入） |
| `login_warns_and_continues_when_cache_dir_is_not_writable` (L1911) | `flowdir/local/<broker>/` 只读 ⇒ 两个 kv 缓存各打 1 条 `Mapping cache file failed`（共 ≥2）、`m_lDate`/状态照常落、确认查询照压、`eidSize/oidSize==0`、`eidCache.get()==""`（只查索引所以安全） | L758-782（L767-768、L780-781 本轮首次进入） |

## DISABLED 用例（不能启用的原因 + 实测证据）

启用面里不留任何崩溃/挂死。每条都在本轮单独跑过（`--gtest_also_run_disabled_tests`，
`/tmp/t25dis` 隔离目录，25s 看门狗），判决是**实测**不是推测：

| 用例（L1954 起） | 实测 | 为什么不能启用 |
| --- | --- | --- |
| `DISABLED_settlement_info_last_row_without_payload_crashes` | SIGSEGV，`rc=139` | `OnRspQrySettlementInfo` 的 `bIsLast` 分支（L1093-1096）无条件解引用 `pSettlementInfo`：只要「前面攒到过内容」且「末行是 NULL」（柜台以无更多数据收尾就是这形状）就是空指针；启用即 CRASH，会被门禁记成非通过 |
| `DISABLED_rsp_error_with_null_sink_crashes` | SIGSEGV，`rc=139` | `OnRspError`（L1161）无条件 `m_sink->handleTraderLog`，而同文件的 `write_log` 与其余 6 处回调都判空；用例先用 `registerSpi(NULL)` 把 `m_sink` 明确置 NULL，所以崩得确定、不依赖内存残留 |
| `DISABLED_order_insert_after_failed_cache_init_crashes` | SIGSEGV，`rc=139` | 承接 `login_warns_...`：缓存 `init` 失败只是「记日志 + 返回 false」，返回值被 L766/L779 丢掉；此时 `_cache._block` 仍为 NULL，而 `WtKVCache::put`（`WtKVCache.hpp:211-232`）索引未命中时直接读 `_cache._block->_size` ⇒ flowdir 不可写时**第一笔带 usertag 的委托**就把进程打死 |
| `DISABLED_disconnect_hangs_when_no_response_clears_in_query` | HANG（>22s 仍存活，被看门狗 KILL） | `connect()` 的 worker（L336-364）把 `m_bInQuery` 置 true 后自己永不复位，只有 `OnRsp*` 的 `bIsLast` 才清零；查询发出而回报不到（断流/丢包）时 worker 在 L339 无限自旋，`disconnect()` 的 `m_thrdWorker->join()`（L376）永不返回 ⇒ 启用即 HANG |
| `DISABLED_plugin_created_trader_has_indeterminate_sink` | **未崩**，`rc=0`（3 次） | 这条**不是**崩溃证据：构造表（L203-217）漏 `m_sink/m_bdMgr` 是源码判据（裸指针 + 无类内初值），但把归还的堆块涂脏后实测仍未命中非空垃圾值（新页面是 0），崩不崩取决于堆残留 ⇒ 只作为 BUG-T25-001 的复现脚手架保留，不能当「稳定复现」。真正可复现的 NULL 面已单独立在 `DISABLED_rsp_error_with_null_sink_crashes` |

## 留白面（28 行，逐段带原因与量级）

| 行段 | 行数 | 为什么进不去 |
| --- | --- | --- |
| `L141-152`、`L154-165` | 22 | `wrapTimeCondition` 两个重载：`constexpr static inline` + `[[maybe_unused]]`，**内部链接**且全产品 0 调用点（`orderInsert` 直接写 `THOST_FTDC_TC_*` 常量，L518-529）。没有外部符号可声明、也不能 `#include` 生产 .cpp（会 duplicate `createTrader` 等 extern "C" 入口），任何测试 seam 都到不了 ⇒ 只能由评审决定是否删除（删除即可清零） |
| `L396`、`L398` | 2 | `makeEntrustID` 的 `catch(...)` 与末尾 `return false`：`fmt::format_to(buffer, ...)` 对裸 `char*` 输出迭代器**没有边界**，不会抛异常（写超界是越界写，见下方「未成条目」），所以 try 体不产生异常 ⇒ catch 不可达。要触发只能改产品（换 `format_to_n`），本轮不改代码 |
| `L777` | 1 | 订单标记缓存的 `fs::create_directories(path)`：path 与 L764 完全同串（`flowdir/local/<broker>/`），前一块刚建过（或已存在）⇒ 这里的 `!exists()` 恒 false，单靠一次登录链无法让它为真 |
| `L1425-1427` | 3 | `queryConfirm()` 的 `-1` 守卫：唯一调用点是 `OnRspUserLogin`（L787），而它在 L746 已把状态置成 `WS_LOGINED`（守卫的第一个条件恒不成立）、并在 L753 已经无条件解引用 `m_pUserAPI->GetTradingDay()`（API 为空时先崩，走不到守卫）⇒ 逻辑死路 |
| `L1490-1521` | 0 | `triggerQuery()` 整段被源码注释掉（`TraderCTP.cpp:1490` 起 `/* ... */`），llvm-cov 里根本不出现 ⇒ 不计入分母 |

另有两处**本轮已按现状钉死、但没有登记为缺陷**的行为（避免与 lane T2 重复计数）：
未 `connect` 直接 `disconnect()` 会把 `release()` 永久留在队列（已由 `disconnect_without_worker_only_queues_release` 钉死，同 lane T2 的 T2-CTP-03）；
`m_ayPosDetail` 在生产里没有任何分配点（只有 L212 置 NULL 与 L296 `clear()`），本轮由用例自建把它走通。

## 与 lane T2 的关系（回归控件）

- 本轮**没有**改动 lane T2 的任何文件（`t2_ctp_api_stub.hpp`、`t2_traderctp_conversion_test.cpp` 原样）。
- 开工前基线（同一二进制、同一 runner、运行目录新建）：20 条枚举 = 占位 1 PASS + T2 用例 8 PASS / 11 FAIL。
- 交付后：61 条枚举 = 占位 1 PASS + T2 用例 **8 PASS / 11 FAIL（逐条同名同判）** + R25 用例 41 PASS。
  ⇒ lane T2 的红一条没变、也没有新增红，本轮对兄弟 lane 零影响。
- 那 11 条红的成因本轮顺带量到了（**不代 lane T2 改断言，只登记线索**）：
  委托号实际格式是 `{:06d}#{:010d}#{:06d}`（`000001#0000123456#000001`，T2 断言写成了位数互调）；
  `Short+Close ⇒ THOST_FTDC_D_Buy`（T2 断言 Sell）；
  `bringToReady()` 后队列里是 6 条而不是 5 条（`OnRspUserLogin` 自己压了 1 条确认查询）；
  `OnRspOrderAction` 的错误文本不带 `(ErrorID)`；
  `trade` 通道的 usertag 没有「回落自身」这一步（只有 oid 命中才写，未命中是空串）。
