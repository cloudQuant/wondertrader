# API 简报 T04

覆盖模块: ParserYD, TraderATP, TraderCTP, TraderCTPMini。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `ParserYD`

### `src/ParserYD/ParserYD.cpp` (291 行)
- L31: `fmt::format_to(buffer, format, args...)`

### `src/ParserYD/ParserYD.h` (87 行)
- 类型: WTSTickData:17, ParserYD:22
- L26: `virtual ~ParserYD()`
- L30: `virtual voidnotifyReadyForLogin(bool hasLoginFailed) override`
- L32: `virtual voidnotifyLogin(int errorNo, int maxOrderRef, bool isMonitor) override`
- L34: `virtual voidnotifyFinishInit(void) override`
- L36: `virtual voidnotifyMarketData(const YDMarketData *pMarketData) override`
- L40: `virtual boolinit(WTSVariant* config) override`
- L42: `virtual voidrelease() override`
- L44: `virtual boolconnect() override`
- L46: `virtual booldisconnect() override`
- L48: `virtual boolisConnected() override`
- L50: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L51: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L53: `virtual voidregisterSpi(IParserSpi* listener) override`
- L61: `voidDoLogin()`
- L65: `voidDoSubscribe()`

## `TraderATP`

### `src/TraderATP/TraderATP.cpp` (1433 行)
- L66: `static std::atomic<uint32_t>auto_refid(0)`
- L237: `const std::stringcode(order_info->security_id)`
- L266: `const std::stringcode(order_info->security_id)`
- L322: `const std::stringcode(order_status_ack->security_id)`
- L379: `const std::stringcode(trade_info->security_id)`
- L415: `const std::stringcode(cash_auction_trade_er->security_id)`
- L536: `fs::create_directories(path.c_str())`
- L549: `fs::create_directories(path.c_str())`
- L968: `std::this_thread::sleep_for(std::chrono::seconds(1))`
- L1000: `std::this_thread::sleep_for(std::chrono::seconds(1))`
- L1041: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L1281: `boost::asio::post(_asyncio, [this]()`
- L1302: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`
- L1321: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1334: `boost::asio::post(_asyncio, [this]()`
- L1353: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`
- L1373: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1384: `boost::asio::post(_asyncio, [this]()`
- L1403: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`

### `src/TraderATP/TraderATP.h` (220 行)
- 类型: TraderATP:31
- L35: `virtual ~TraderATP()`
- L49: `virtual voidOnLogin(const std::string& reason) override`
- L52: `virtual voidOnLogout(const std::string& reason) override`
- L55: `virtual voidOnConnectFailure(const std::string &reason) override`
- L58: `virtual voidOnConnectTimeOut(const std::string &reason) override`
- L61: `virtual voidOnHeartbeatTimeout(const std::string &reason) override`
- L64: `virtual voidOnClosed(const std::string &reason) override`
- L67: `virtual voidOnEndOfConnection(const std::string& reason) override`
- L70: `virtual voidOnRspCustLoginResp(const ATPRspCustLoginRespOtherMsg &cust_login_resp) override`
- L73: `virtual voidOnRspCustLogoutResp(const ATPRspCustLogoutRespOtherMsg &cust_logout_resp) override`
- L76: `virtual voidOnRspOrderStatusInternalAck(const ATPRspOrderStatusAckMsg& order_status_ack) override`
- L79: `virtual voidOnRspOrderStatusAck(const ATPRspOrderStatusAckMsg& order_status_ack) override`
- L82: `virtual voidOnRspCashAuctionTradeER(const ATPRspCashAuctionTradeERMsg& cash_auction_trade_er) override`
- L85: `virtual voidOnRspBizRejection(const ATPRspBizRejectionOtherMsg& biz_rejection) override`
- L92: `virtual voidOnRspFundQueryResult(const ATPRspFundQueryResultMsg &msg) override`
- L99: `virtual voidOnRspShareQueryResult(const ATPRspShareQueryResultMsg &msg) override`
- L106: `virtual voidOnRspTradeOrderQueryResult(const ATPRspTradeOrderQueryResultMsg &msg) override`
- L113: `virtual voidOnRspOrderQueryResult(const ATPRspOrderQueryResultMsg &msg) override`
- L118: `virtual boolinit(WTSVariant *params) override`
- L120: `virtual voidrelease() override`
- L122: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L124: `virtual voidconnect() override`
- L126: `virtual voiddisconnect() override`
- L128: `virtual boolisConnected() override`
- L130: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L132: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L134: `virtual intlogout() override`
- L136: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L138: `virtual intorderAction(WTSEntrustAction* action) override`
- L140: `virtual intqueryAccount() override`
- L142: `virtual intqueryPositions() override`
- L144: `virtual intqueryOrders() override`
- L146: `virtual intqueryTrades() override`
- L149: `voidreconnect()`
- L150: `inline uint32_tgenRequestID()`
- L151: `voiddoLogin(const char* productid)`
- L153: `inline WTSOrderInfo*makeOrderInfo(const APIOrderUnit* orderField)`
- L154: `inline WTSEntrust*makeEntrust(const ATPRspOrderStatusAckMsg *entrustField)`
- L155: `inline WTSTradeInfo*makeTradeInfo(const APITradeOrderUnit *tradeField)`
- L156: `inline WTSOrderInfo*makeOrderInfo(const ATPRspOrderStatusAckMsg *order_status_ack)`
- L157: `inline WTSTradeInfo*makeTradeRecord(const ATPRspCashAuctionTradeERMsg *cash_auction_trade_er)`
- L159: `inline boolextractEntrustID(const char* entrustid, uint32_t &orderRef)`
- L160: `inline voidgenEntrustID(char* buffer, uint32_t orderRef)`

## `TraderCTP`

### `src/TraderCTP/TraderCTP.cpp` (1520 行)
- L309: `fs::create_directories(ss.str().c_str())`
- L341: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`
- L348: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L358: `StdUniqueLocklock(m_mtxQuery)`
- L597: `StdUniqueLocklock(m_mtxQuery)`
- L620: `StdUniqueLocklock(m_mtxQuery)`
- L643: `StdUniqueLocklock(m_mtxQuery)`
- L667: `StdUniqueLocklock(m_mtxQuery)`
- L691: `StdUniqueLocklock(m_mtxQuery)`
- L764: `fs::create_directories(path.c_str())`
- L777: `fs::create_directories(path.c_str())`
- L1225: `StrUtil::replace(strTime, ":", "")`
- L1336: `StrUtil::replace(strTime, ":", "")`
- L1430: `StdUniqueLocklock(m_mtxQuery)`
- L1493: `boost::asio::post(*m_strandIO, [this]()`
- L1500: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1501: `boost::asio::post(*m_strandIO, [this]()`
- L1513: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderCTP/TraderCTP.h` (214 行)
- 类型: TraderCTP:32
- L36: `virtual ~TraderCTP()`
- L53: `intconfirm()`
- L55: `intqueryConfirm()`
- L57: `intauthenticate()`
- L59: `intdoLogin()`
- L64: `virtual boolinit(WTSVariant* params) override`
- L66: `virtual voidrelease() override`
- L68: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L70: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L72: `virtual voidconnect() override`
- L74: `virtual voiddisconnect() override`
- L76: `virtual boolisConnected() override`
- L78: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L80: `virtual intlogout() override`
- L82: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L84: `virtual intorderAction(WTSEntrustAction* action) override`
- L86: `virtual intqueryAccount() override`
- L88: `virtual intqueryPositions() override`
- L90: `virtual intqueryOrders() override`
- L92: `virtual intqueryTrades() override`
- L94: `virtual intquerySettlement(uint32_t uDate) override`
- L100: `virtual voidOnFrontConnected() override`
- L102: `virtual voidOnFrontDisconnected(int nReason) override`
- L104: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L106: `virtual voidOnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L108: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L110: `virtual voidOnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L112: `virtual voidOnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L114: `virtual voidOnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L116: `virtual voidOnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L118: `virtual voidOnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L120: `virtual voidOnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L122: `virtual voidOnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L124: `virtual voidOnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L127: `virtual voidOnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L129: `virtual voidOnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L131: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L133: `virtual voidOnRtnOrder(CThostFtdcOrderField *pOrder) override`
- L135: `virtual voidOnRtnTrade(CThostFtdcTradeField *pTrade) override`
- L137: `virtual voidOnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override`
- L139: `virtual voidOnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override`
- L142: `WTSOrderInfo*makeOrderInfo(CThostFtdcOrderField* orderField)`
- L143: `WTSEntrust*makeEntrust(CThostFtdcInputOrderField *entrustField)`
- L144: `WTSEntrustAction*makeAction(CThostFtdcInputOrderActionField *entrustField)`
- L145: `WTSError*makeError(CThostFtdcRspInfoField* rspInfo, WTSErroCode ec = WEC_NONE)`
- L146: `WTSTradeInfo*makeTradeInfo(CThostFtdcTradeField *tradeField)`
- L148: `voidgenerateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)`
- L149: `boolextractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)`
- L151: `uint32_tgenRequestID()`

## `TraderCTPMini`

### `src/TraderCTPMini/TraderCTPMini.cpp` (1377 行)
- L168: `fs::create_directories(ss.str().c_str())`
- L196: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`
- L203: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L213: `StdUniqueLocklock(m_mtxQuery)`
- L468: `StdUniqueLocklock(m_mtxQuery)`
- L491: `StdUniqueLocklock(m_mtxQuery)`
- L513: `StdUniqueLocklock(m_mtxQuery)`
- L537: `StdUniqueLocklock(m_mtxQuery)`
- L611: `fs::create_directories(path.c_str())`
- L624: `fs::create_directories(path.c_str())`
- L1120: `StrUtil::replace(strTime, ":", "")`
- L1230: `StrUtil::replace(strTime, ":", "")`
- L1350: `boost::asio::post(*m_strandIO, [this]()`
- L1357: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1358: `boost::asio::post(*m_strandIO, [this]()`
- L1370: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderCTPMini/TraderCTPMini.h` (218 行)
- 类型: TraderCTPMini:28
- L32: `virtual ~TraderCTPMini()`
- L49: `intauthenticate()`
- L51: `intdoLogin()`
- L56: `virtual boolinit(WTSVariant* params) override`
- L58: `virtual voidrelease() override`
- L60: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L62: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L64: `virtual voidconnect() override`
- L66: `virtual voiddisconnect() override`
- L68: `virtual boolisConnected() override`
- L70: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L72: `virtual intlogout() override`
- L74: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L76: `virtual intorderAction(WTSEntrustAction* action) override`
- L78: `virtual intqueryAccount() override`
- L80: `virtual intqueryPositions() override`
- L82: `virtual intqueryOrders() override`
- L84: `virtual intqueryTrades() override`
- L90: `virtual voidOnFrontConnected() override`
- L92: `virtual voidOnFrontDisconnected(int nReason) override`
- L94: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L96: `virtual voidOnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L98: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L100: `virtual voidOnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L102: `virtual voidOnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L104: `virtual voidOnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L106: `virtual voidOnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L108: `virtual voidOnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L111: `virtual voidOnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L113: `virtual voidOnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L115: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L117: `virtual voidOnRtnOrder(CThostFtdcOrderField *pOrder) override`
- L119: `virtual voidOnRtnTrade(CThostFtdcTradeField *pTrade) override`
- L121: `virtual voidOnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override`
- L123: `virtual voidOnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override`
- L129: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`
- L131: `intwrapPriceType(WTSPriceType priceType, bool isCFFEX = false)`
- L132: `intwrapDirectionType(WTSDirectionType dirType, WTSOffsetType offType)`
- L133: `intwrapOffsetType(WTSOffsetType offType)`
- L134: `intwrapTimeCondition(WTSTimeCondition timeCond)`
- L135: `intwrapActionFlag(WTSActionFlag actionFlag)`
- L137: `WTSPriceTypewrapPriceType(TThostFtdcOrderPriceTypeType priceType)`
- L138: `WTSDirectionTypewrapDirectionType(TThostFtdcDirectionType dirType, TThostFtdcOffsetFlagType offType)`
- L139: `WTSDirectionTypewrapPosDirection(TThostFtdcPosiDirectionType dirType)`
- L140: `WTSOffsetTypewrapOffsetType(TThostFtdcOffsetFlagType offType)`
- L141: `WTSTimeConditionwrapTimeCondition(TThostFtdcTimeConditionType timeCond)`
- L142: `WTSOrderStatewrapOrderState(TThostFtdcOrderStatusType orderState)`
- L144: `WTSOrderInfo*makeOrderInfo(CThostFtdcOrderField* orderField)`
- L145: `WTSEntrust*makeEntrust(CThostFtdcInputOrderField *entrustField)`
- L146: `WTSError*makeError(CThostFtdcRspInfoField* rspInfo)`
- L147: `WTSTradeInfo*makeTradeRecord(CThostFtdcTradeField *tradeField)`
- L149: `voidgenerateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)`
- L150: `boolextractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)`
- L155: `uint32_tgenRequestID()`

