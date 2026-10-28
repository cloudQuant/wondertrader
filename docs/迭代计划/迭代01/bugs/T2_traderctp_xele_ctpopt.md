# lane T2 缺陷登记 — TraderCTP / ParserXeleSkt / ParserCTPOpt

只登记，不修改 `src/**`，不改断言凑绿。用例文件里凡是被钉死的当前行为都在「现状」一栏注明。

---

## TraderCTP（`src/TraderCTP/TraderCTP.cpp`）

- **T2-CTP-01 构造函数漏初始化会话字段，登录前 `makeEntrustID` 打印未初始化值（UB）**
  - 位置：`TraderCTP.cpp:203-217`（`m_frontID / m_sessionID / m_orderRef / m_lDate` 不在初始化列表里）、`TraderCTP.cpp:381-399`
  - 现状：`makeEntrustID` 无条件用 `fmt::format_to("{:06d}#{:010d}#{:06d}", m_frontID, m_sessionID, orderref)`，
    登录前调用会输出栈/堆垃圾；`m_orderRef` 是无默认值的 `std::atomic<uint32_t>`，`fetch_add` 读的是未初始化对象。
  - 用例侧规避：`make_entrust_id_is_fixed_width_and_increments_ref` 先跑完整登录链（把三值置成 12345/67/1000）再断言精确串，
    因此本轮**没有**在「未登录」状态下调用 `makeEntrustID`（否则断言不可复现）。
  - 状态: 待审核

- **T2-CTP-02 `connect()` 对 API 指针的判空顺序错 + `m_sink` 未判空**
  - 位置：`TraderCTP.cpp:305-366`
  - 现状：L310 `m_pUserAPI = m_funcCreator(...)` 之后 **L311 立刻** `m_pUserAPI->RegisterSpi(this)`，而 L329 才做
    `if (m_pUserAPI)`；`m_funcCreator` 为空（Windows dll 缺失）或 `CreateFtdcTraderApi` 返回 NULL 时空指针解引用。
    另外 L326 `m_sink->handleTraderLog(...)` 在 `registerSpi` 之前被调用时同样解引用 NULL。
  - 该路径同时会起常驻 worker 线程，用例改名 `DISABLED_connect_starts_worker_thread_and_needs_real_front`，未实测崩溃。
  - 状态: 待审核

- **T2-CTP-03 `disconnect()` 在 worker 未启动时把 `release()` 永久留在队列里**
  - 位置：`TraderCTP.cpp:368-379`
  - 现状：只有 `m_thrdWorker` 存在才 `join`；未 `connect` 直接 `disconnect` 时，`release()` 这个 lambda 既不执行也不丢弃，
    `m_bStopped` 保持 false，`m_pUserAPI` 不释放。
  - 状态: 待审核

- **T2-CTP-04 结算确认比较在 `m_lDate==0` 时恒真，未确认也会置「全部就绪」**
  - 位置：`TraderCTP.cpp:753`（`m_lDate = atoi(m_pUserAPI->GetTradingDay())`，无失败处理）、`TraderCTP.cpp:818-827`
  - 现状：`uConfirmDate >= m_lDate` ⇒ 当柜台返回空 TradingDay（`m_lDate=0`）时任何确认日期都通过，直接 `WS_ALLREADY` 并 `onLoginResult(true)`。
  - 状态: 待审核

- **T2-CTP-05 错误回报不判空：`OnRspOrderInsert/OnErrRtnOrderInsert/OnRspOrderAction` 把可能为 NULL 的字段直接喂 `makeEntrust/makeAction`**
  - 位置：`TraderCTP.cpp:861-897`、`TraderCTP.cpp:1397-1409`、`TraderCTP.cpp:1263-1301`、`TraderCTP.cpp:1303-1315`
  - 现状：`makeEntrust(pInputOrder)` 立刻读 `entrustField->InstrumentID`；CTP 在部分拒绝回报里允许 `pInputOrder==NULL`。
    `OnRtnOrder/OnRtnTrade`（L1164-1188）同样不判空。`OnRtnDepthMarketData` 那类判空在 Xele/CTP 里都不一致。
  - 用例侧规避：本轮只喂**非空**结构体，NULL 面未测。
  - 状态: 待审核

- **T2-CTP-06 `OnRspQryTradingAccount` 在 `bIsLast && 无错误` 时无条件解引用 `pTradingAccount`**
  - 位置：`TraderCTP.cpp:899-930`（L909-921）
  - 现状：CTP 空结果时会带 `pTradingAccount==NULL, ErrorID==0, bIsLast==true` 收尾 ⇒ 空指针读字段。
    同族 `OnRspQryInvestorPosition`(L940) 与 `OnRspQrySettlementInfo`(L1088) 都做了 `&& 指针` 判空，唯此处与 L1093-1095
    （`bIsLast && !m_strSettleInfo.empty()` 里读 `pSettlementInfo->TradingDay`）漏判。
  - 状态: 待审核

- **T2-CTP-07 合约状态/订单状态用整数强转跨枚举，映射依赖字面值巧合**
  - 位置：`TraderCTP.cpp:1411-1415`（`(WTSTradeStatus)pInstrumentStatus->InstrumentStatus`，CTP 侧是 `'1'..'A'` 字符枚举）、
    `TraderCTP.cpp:167-178`（`wrapOrderState` 的 default 分支 `(WTSOrderState)orderState`）
  - 现状：订单侧恰好 `'0'..'5'` 与 `WOS_*` 对齐所以看起来正确；`THOST_FTDC_OST_NotTouched/Touched`('7'/'8') 会落成越界枚举；
    合约状态 `THOST_FTDC_IS_*` 是字符值，`WTSTradeStatus` 是 0..n，**当前实现必然错位**。
    用例 `front_events_and_logout_and_status_pushes` 把「强转后等于原字符值」这一现状钉死（不是正确性背书）。
  - 状态: 待审核

- **T2-CTP-08 `orderInsert` 对无 usertag 的委托用 `m_orderRef.fetch_add(0)`，与 `makeEntrustID` 的自增序列不同步**
  - 位置：`TraderCTP.cpp:485-496` vs `TraderCTP.cpp:386-391`
  - 现状：不带 userTag 时 OrderRef 取当前值但**不自增** ⇒ 连续两笔裸委托会拿到同一个 OrderRef（柜台会拒 or 回报串号）。
    用例走的是带 userTag 的分支（entrustID 回填），未把该分支的重复号写进断言。
  - 状态: 待审核

- **T2-CTP-09 未进入的 private 面（留给主控裁定）**
  - `TraderCTP.h:51-60, 142-151` 是 private：`confirm/queryConfirm/authenticate/doLogin/makeOrderInfo/makeEntrust/makeAction/makeError/makeTradeInfo/generateEntrustID/extractEntrustID/genRequestID`。
    本轮通过 public 回调**间接**进入其中除 `makeAction` 正常分支外的全部；`wrapTimeCondition`（`TraderCTP.cpp:141-165`）
    在源码里带 `[[maybe_unused]]` 且没有任何 public 入口调用，属**死代码**，未覆盖。
  - `createTrader/deleteTrader`（`TraderCTP.cpp:185-201`）本轮未建 C 入口用例（ParserXeleSkt/ParserCTPOpt 两个目录已建，可照搬）。
  - 状态: 待审核

---

## ParserXeleSkt（`src/ParserXeleSkt/ParserXeleSkt.cpp`）

- **T2-XELE-01 `init()` 不校验地址，非法 `tcp_host` 抛异常穿透 C++ 插件边界**
  - 位置：`ParserXeleSkt.cpp:110-114`（`ip::make_address(_tcp_host)` 裸调用）
  - 现状：配置缺失或写错时 `boost::system::system_error` 直接飞出 `createParser()` 的调用方（WtDtServo/WtLoader 都没 try）。
    用例 `init_throws_on_unparsable_tcp_host` / `init_throws_when_tcp_host_missing` 用 `EXPECT_THROW` 把当前行为钉死（不是期望行为）。
  - 状态: 待审核

- **T2-XELE-02 `release()` 是空实现，既不关口也不停线程；`disconnect()` 同样不 join `_thrd_parser`**
  - 位置：`ParserXeleSkt.cpp:119-122`、`ParserXeleSkt.cpp:279-292`、`ParserXeleSkt.cpp:265-277`、析构 `ParserXeleSkt.cpp:95-97`
  - 现状：`_stopped=true` 从没被任何循环读取（唯一读点在未覆盖的 worker 里）；`connect()` 起的 `StdThread` 在 `disconnect`/析构时都不 join，
    而 `~StdThread` 对 joinable 线程会 `std::terminate` —— 这是挂死/崩溃风险面，因此 `connect()` 整条改名
    `DISABLED_connect_joins_multicast_and_starts_parser_thread`，未实测。
  - 状态: 待审核

- **T2-XELE-03 `_bd_mgr` 在 `registerSpi(NULL)` 后仍保留旧指针；`prepare()/extract_buffer` 直接解引用**
  - 位置：`ParserXeleSkt.cpp:318-329`、`ParserXeleSkt.cpp:202`（`_bd_mgr->getContract(...)`）
  - 现状：`registerSpi(NULL)` 只把 `_sink` 置空，`_bd_mgr` 不清空；反之若从未注册就 `connect()`，L202 解引用 NULL。
  - 状态: 待审核

- **T2-XELE-04 行情解析主体不可达（留给主控裁定）**
  - `handle_udp_read`（L343 起）、`prepare()`（L156-262）、`extract_buffer()`（L392-540）以及 `md_struct.h` 的
    `CXeleShfeDepthMarketData/CXeleShfeSnapShot` → `WTSTickData` 全部转换、`_price_scales`/`_tick_cache` 逻辑都是
    **private**（`ParserXeleSkt.h:50-62`），且数据成员也全 private（`ParserXeleSkt.h:64-93`），
    没有 protected seam、没有可注入的 socket/回调指针，禁用 `#define private public` 的前提下本轮只能覆盖公开生命周期面。
    若主控愿意开一个「测试专用 friend/protected」的口子，`extract_buffer` 的报文解析（约 150 行，含夜盘日期与价格缩放）可以成批拿下。
  - 状态: 待审核

---

## ParserCTPOpt（`src/ParserCTPOpt/ParserCTPOpt.cpp`）

- **T2-OPT-01 `init()` 在期权 CTP 库缺失时空指针调用 creator（段错误）**
  - 位置：`ParserCTPOpt.cpp:114-132`
  - 现状：`DLLHelper::load_library` 失败返回无效句柄，`get_symbol` 返回 NULL，紧接 L130 `m_pUserAPI = m_funcCreator(path,false,false)`
    是**空函数指针调用**；L131-132 又对结果不判空。macOS 本机没有 `soptthostmduserapi_se`，所以 `init()` 必崩。
    用例改名 `DISABLED_init_and_login_rsp_dereference_missing_ctpopt_library`；本目录全部用例都在**未调用 init()** 的裸构造对象上跑。
  - 状态: 待审核

- **T2-OPT-02 `OnRspUserLogin(bIsLast=true)` 解引用可能为 NULL 的 `m_pUserAPI`**
  - 位置：`ParserCTPOpt.cpp:180-193`（L184 `m_pUserAPI->GetTradingDay()`）
  - 现状：同文件 `ReqUserLogin()`（L344-349）对 `m_pUserAPI` 做了判空，这里没有 ⇒ 未 init 成功却收到登录回报即崩。
    用例只喂 `bIsLast=false`（`login_rsp_without_is_last_is_ignored`）。
  - 状态: 待审核

- **T2-OPT-03 `IsErrorRspInfo` 恒返回 false，柜台的错误回报被整条吞掉**
  - 位置：`ParserCTPOpt.cpp:402-405`（参数被注释掉、直接 `return false`）
  - 现状：`OnRspError`（L164-167）因此是空操作；`OnRspUserLogin` 的错误分支（L182）永不进入；
    `OnRspSubMarketData`（L326-336）/`OnRspUnSubMarketData`（L213-216）两个分支都是空体 ⇒ 订阅失败无任何日志/事件。
    用例 `heartbeat_and_error_callbacks_do_not_reach_core` 以「日志与事件计数不变」钉死该现状。
  - 状态: 待审核

- **T2-OPT-04 `subscribe()` 用固定 500 的栈数组承载合约数**
  - 位置：`ParserCTPOpt.cpp:416-425`（`char *subscribe[500]`），同样问题在 `SubscribeMarketData()` L373 用 `new char*[size]` 是对的
  - 现状：订阅集合 >500 个代码时越界写栈。ETF 期权全量合约数已在同一量级，风险实际存在。
  - 状态: 待审核

- **T2-OPT-05 `strToTime` 用函数级 `static` 缓冲，非线程安全**
  - 位置：`ParserCTPOpt.cpp:59-77`（`static char tmp_timestr[10]`）
  - 现状：多个解析器实例或柜台多线程回报会互相覆盖；`TraderCTP.cpp:40-54` 的同名函数已改成局部 `std::string`，两处不同源。
  - 状态: 待审核

- **T2-OPT-06 私有面不可达（留给主控裁定）**
  - `m_pUserAPI / m_uTradingDate / m_filterSubs / m_iRequestID`（`ParserCTPOpt.h:91-106`）全 private，
    `ReqUserLogin/SubscribeMarketData/IsErrorRspInfo` 也 private ⇒
    `subscribe()` 的 `m_uTradingDate!=0` 分支（L413-441）、`SubscribeMarketData()` 全段（L365-400）、
    `OnRspUserLogin` 的成功分支（L184-193）本轮不可达。若给一个 protected/注入点，这三段（约 60 行）可补。
  - 状态: 待审核

---

## 分级汇总

| 级别 | 条数 | 编号 |
| --- | --- | --- |
| 高（崩溃/UB，需尽早定论） | 3 | T2-CTP-02, T2-OPT-01, T2-OPT-02 |
| 中（逻辑/映射错误、资源不释放） | 8 | T2-CTP-01, T2-CTP-04, T2-CTP-05, T2-CTP-06, T2-CTP-07, T2-XELE-01, T2-XELE-02, T2-OPT-03 |
| 低（一致性/容量/健壮性） | 5 | T2-CTP-03, T2-CTP-08, T2-XELE-03, T2-OPT-04, T2-OPT-05 |
| 留白（不可达面，待主控裁定 seam） | 3 | T2-CTP-09, T2-XELE-04, T2-OPT-06 |
