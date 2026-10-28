# API 简报 T07

覆盖模块: TraderXTP, TraderXTPXAlgo, TraderYD。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `TraderXTP`

### `src/TraderXTP/TraderXTP.cpp` (952 行)
- L162: `static std::atomic<uint32_t>auto_refid(0)`
- L350: `boost::asio::post(_asyncio, [this]()`
- L352: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L662: `fs::create_directories(ss.str().c_str())`
- L670: `StdThreadPtrthrd(new StdThread([this]()`
- L671: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L699: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L782: `fs::create_directories(path.c_str())`
- L795: `fs::create_directories(path.c_str())`

### `src/TraderXTP/TraderXTP.h` (156 行)
- 类型: TraderXTP:28
- L32: `virtual ~TraderXTP()`
- L46: `virtual voidOnDisconnected(uint64_t session_id, int reason) override`
- L48: `virtual voidOnError(XTPRI *error_info) override`
- L50: `virtual voidOnOrderEvent(XTPOrderInfo *order_info, XTPRI *error_info, uint64_t session_id) override`
- L52: `virtual voidOnTradeEvent(XTPTradeReport *trade_info, uint64_t session_id) override`
- L54: `virtual voidOnCancelOrderError(XTPOrderCancelInfo *cancel_info, XTPRI *error_info, uint64_t session_id) override`
- L56: `virtual voidOnQueryOrder(XTPQueryOrderRsp *order_info, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L58: `virtual voidOnQueryTrade(XTPQueryTradeRsp *trade_info, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L60: `virtual voidOnQueryPosition(XTPQueryStkPositionRsp *position, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L62: `virtual voidOnQueryAsset(XTPQueryAssetRsp *asset, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L67: `virtual boolinit(WTSVariant *params) override`
- L69: `virtual voidrelease() override`
- L71: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L73: `virtual voidconnect() override`
- L75: `virtual voiddisconnect() override`
- L77: `virtual boolisConnected() override`
- L79: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L81: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L83: `virtual intlogout() override`
- L85: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L87: `virtual intorderAction(WTSEntrustAction* action) override`
- L89: `virtual intqueryAccount() override`
- L91: `virtual intqueryPositions() override`
- L93: `virtual intqueryOrders() override`
- L95: `virtual intqueryTrades() override`
- L98: `voidreconnect()`
- L99: `inline uint32_tgenRequestID()`
- L100: `voiddoLogin()`
- L102: `inline WTSOrderInfo*makeOrderInfo(XTPQueryOrderRsp* orderField)`
- L103: `inline WTSEntrust*makeEntrust(XTPOrderInfo *entrustField)`
- L104: `inline WTSTradeInfo*makeTradeInfo(XTPQueryTradeRsp *tradeField)`
- L106: `inline boolextractEntrustID(const char* entrustid, uint32_t &orderRef)`
- L107: `inline voidgenEntrustID(char* buffer, uint32_t orderRef)`

## `TraderXTPXAlgo`

### `src/TraderXTPXAlgo/TraderXTPXAlgo.cpp` (1237 行)
- L168: `static std::atomic<uint32_t>auto_refid(0)`
- L393: `boost::asio::post(_asyncio, [this]()`
- L395: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L875: `fs::create_directories(ss.str().c_str())`
- L883: `StdThreadPtrthrd(new StdThread([this]()`
- L884: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L915: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L999: `fs::create_directories(path.c_str())`
- L1012: `fs::create_directories(path.c_str())`
- L1063: `fs::create_directories(path.c_str())`
- L1076: `fs::create_directories(path.c_str())`

### `src/TraderXTPXAlgo/TraderXTPXAlgo.h` (248 行)
- 类型: TraderXTPXAlgo:20
- L24: `virtual ~TraderXTPXAlgo()`
- L42: `virtual voidOnDisconnected(uint64_t session_id, int reason) override`
- L47: `virtual voidOnError(XTPRI *error_info) override`
- L52: `virtual voidOnAlgoDisconnected(int reason) override`
- L55: `virtual voidOnAlgoConnected() override`
- L65: `virtual voidOnQueryStrategy(XTPStrategyInfoStruct* strategy_info, char* strategy_param, XTPRI *error_info, int32_t request_id, bool is_last, uint64_t session_id) override`
- L71: `virtual voidOnStrategyStateReport(XTPStrategyStateReportStruct* strategy_state, uint64_t session_id) override`
- L78: `virtual voidOnALGOUserEstablishChannel(char* user, XTPRI* error_info, uint64_t session_id) override`
- L85: `virtual voidOnInsertAlgoOrder(XTPStrategyInfoStruct* strategy_info, XTPRI *error_info, uint64_t session_id) override`
- L92: `virtual voidOnCancelAlgoOrder(XTPStrategyInfoStruct* strategy_info, XTPRI *error_info, uint64_t session_id) override`
- L99: `virtual voidOnOrderEvent(XTPOrderInfo *order_info, XTPRI *error_info, uint64_t session_id) override`
- L105: `virtual voidOnTradeEvent(XTPTradeReport *trade_info, uint64_t session_id) override`
- L123: `virtual voidOnQueryTrade(XTPQueryTradeRsp *trade_info, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L132: `virtual voidOnQueryPosition(XTPQueryStkPositionRsp *position, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L141: `virtual voidOnQueryAsset(XTPQueryAssetRsp *asset, XTPRI *error_info, int request_id, bool is_last, uint64_t session_id) override`
- L146: `virtual boolinit(WTSVariant *params) override`
- L148: `virtual voidrelease() override`
- L150: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L152: `virtual voidconnect() override`
- L154: `virtual voiddisconnect() override`
- L156: `virtual boolisConnected() override`
- L158: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L160: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L162: `virtual intlogout() override`
- L164: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L166: `virtual intorderAction(WTSEntrustAction* action) override`
- L168: `virtual intqueryAccount() override`
- L170: `virtual intqueryPositions() override`
- L172: `virtual intqueryOrders() override`
- L174: `virtual intqueryTrades() override`
- L177: `voidreconnect()`
- L178: `inline uint32_tgenRequestID()`
- L179: `voiddoLogin()`
- L181: `inline WTSOrderInfo*makeOrderInfo(XTPQueryOrderRsp* orderField)`
- L182: `inline WTSEntrust*makeEntrust(XTPOrderInfo *entrustField)`
- L183: `inline WTSEntrust*makeEntrust(XTPStrategyInfoStruct *stra_info)`
- L184: `inline WTSTradeInfo*makeTradeInfo(XTPQueryTradeRsp *tradeField)`
- L185: `inline WTSOrderInfo*makeOrderInfo(XTPStrategyInfoStruct *strategyState) // 算法单信息`
- L187: `inline boolextractEntrustID(const char* entrustid, uint32_t &orderRef)`
- L188: `inline voidgenEntrustID(char* buffer, uint32_t orderRef)`

## `TraderYD`

### `src/TraderYD/TraderYD.cpp` (1117 行)
- L40: `fmtutil::format_to(buffer, format, args...)`
- L278: `fs::create_directories(path.c_str())`
- L289: `fs::create_directories(path.c_str())`
- L697: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`
- L705: `StdUniqueLocklock(m_mtxQuery)`
- L876: `StdUniqueLocklock(m_mtxQuery)`
- L893: `StdUniqueLocklock(m_mtxQuery)`
- L923: `StdUniqueLocklock(m_mtxQuery)`
- L948: `StdUniqueLocklock(m_mtxQuery)`
- L1004: `convert::to_str(pRet->getOrderID(), 64, orderField->OrderSysID)`
- L1072: `convert::to_str(pRet->getTradeID(), 64, tradeField->TradeID)`
- L1084: `convert::to_str(pRet->getRefOrder(), 64, tradeField->OrderSysID)`

### `src/TraderYD/TraderYD.h` (181 行)
- 类型: TraderYD:29
- L33: `virtual ~TraderYD()`
- L36: `virtual voidnotifyReadyForLogin(bool hasLoginFailed) override`
- L39: `virtual voidnotifyLogin(int errorNo, int maxOrderRef, bool isMonitor) override`
- L42: `virtual voidnotifyFinishInit() override`
- L45: `virtual voidnotifyOrder(const YDOrder *pOrder, const YDInstrument *pInstrument, const YDAccount *pAccount) override`
- L48: `virtual voidnotifyTrade(const YDTrade *pTrade, const YDInstrument *pInstrument, const YDAccount *pAccount) override`
- L51: `virtual voidnotifyFailedOrder(const YDInputOrder *pFailedOrder, const YDInstrument *pInstrument, const YDAccount *pAccount) override`
- L54: `virtual voidnotifyFailedCancelOrder(const YDFailedCancelOrder *pFailedCancelOrder, const YDExchange *pExchange, const YDAccount *pAccount) override`
- L57: `virtual voidnotifyAccount(const YDAccount *pAccount) override`
- L59: `virtual voidnotifyCaughtUp() override`
- L75: `intdoLogin()`
- L80: `virtual boolinit(WTSVariant* params) override`
- L82: `virtual voidrelease() override`
- L84: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L86: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L88: `virtual voidconnect() override`
- L90: `virtual voiddisconnect() override`
- L92: `virtual boolisConnected() override`
- L94: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L96: `virtual intlogout() override`
- L98: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L100: `virtual intorderAction(WTSEntrustAction* action) override`
- L102: `virtual intqueryAccount() override`
- L104: `virtual intqueryPositions() override`
- L106: `virtual intqueryOrders() override`
- L108: `virtual intqueryTrades() override`
- L112: `inline WTSOrderInfo*makeOrderInfo(const YDOrder* orderField, const YDInstrument* instInfo) noexcept`
- L113: `inline WTSEntrust*makeEntrust(const YDInputOrder *entrustField, const YDInstrument* instInfo) noexcept`
- L114: `inline WTSTradeInfo*makeTradeRecord(const YDTrade *tradeField, const YDInstrument* instInfo) noexcept`
- L115: `inline WTSError*makeError(int errorno, WTSErroCode ec) noexcept`
- L117: `inline boolgenerateEntrustID(uint32_t orderRef, char* buffer) noexcept`
- L118: `inline boolextractEntrustID(const char* entrustid, uint32_t &orderRef) noexcept`
- L125: `voidon_kvcache_message(const char* message)`

