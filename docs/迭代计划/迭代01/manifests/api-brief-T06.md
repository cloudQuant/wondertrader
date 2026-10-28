# API 简报 T06

覆盖模块: TraderHTS, TraderHuaX, TraderMocker, TraderOES。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `TraderHTS`

### `src/TraderHTS/TraderHTS.cpp` (1530 行)
- L56: `voidinst_hlp() }`
- L117: `boost::filesystem::pathp(_bin_dir)`
- L352: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L363: `boost::asio::post(m_asyncIO, [this]()`
- L675: `boost::asio::post(*m_strandIO, [this]()`
- L726: `boost::asio::post(*m_strandIO, [this]()`
- L761: `boost::filesystem::create_directories(path.c_str())`
- L817: `boost::asio::post(*m_strandIO, [this, entrust]()`
- L909: `boost::asio::post(*m_strandIO, [this, action]()`
- L1025: `StdUniqueLocklock(m_mtxQuery)`
- L1049: `std::vector<ITPDK_ZQGL>argTotalHolding(argHolding)`
- L1134: `StdUniqueLocklock(m_mtxQuery)`
- L1158: `std::vector<ITPDK_DRWT>argTotalOrders(argOrders)`
- L1277: `StdUniqueLocklock(m_mtxQuery)`
- L1301: `std::vector<ITPDK_SSCJ>argTotalTrades(argTrades)`
- L1383: `boost::asio::post(*m_strandIO, [this]()`
- L1403: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderHTS/TraderHTS.h` (177 行)
- 类型: TraderHTS:38, HTSCallMgr:163
- L44: `virtual ~TraderHTS()`
- L57: `voiddoLogin()`
- L58: `voidInitializeHTS(WTSVariant* params)`
- L59: `voidqryGDNo() //查询股东号`
- L63: `voidreconnect()`
- L65: `voidtriggerQuery()`
- L69: `voidhtsGetCallback(const char* pTime, const char* pMsg, int nType)`
- L71: `voidOnRtnOrder(const char* pMsg) // 委托信息`
- L72: `voidOnRtnTrade(const char* pMsg) // 成交信息`
- L79: `virtual boolinit(WTSVariant* params) override`
- L81: `virtual voidrelease() override`
- L83: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L85: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L87: `virtual voidconnect() override`
- L89: `virtual voiddisconnect() override`
- L91: `virtual boolisConnected() override`
- L93: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L95: `virtual intlogout() override`
- L97: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L99: `virtual intorderAction(WTSEntrustAction* action) override`
- L101: `virtual intqueryAccount() override`
- L103: `virtual intqueryPositions() override`
- L105: `virtual intqueryOrders() override`
- L107: `virtual intqueryTrades() override`
- L170: `boolinit(WTSVariant* params)`
- L171: `voidsetCallbackMsgFunc()`
- L172: `voidsetHTSCallPtr(std::string khh, TraderHTS* pTrader)`
- L175: `static voidcallbackMsgFunc(const char* pTime, const char* pMsg, int nType)`
- L176: `static voidcallbackConnFunc(const char* pKhh, const char* pConnKey, int nEvent, int nType)`

## `TraderHuaX`

### `src/TraderHuaX/TraderHuaX.cpp` (1086 行)
- L56: `static std::atomic<uint32_t>auto_refid(0)`
- L502: `StdThreadPtrthrd(new StdThread([this]()`
- L503: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L566: `fs::create_directories(path.c_str())`
- L579: `fs::create_directories(path.c_str())`
- L706: `fs::create_directories(ss.str().c_str())`
- L714: `StdThreadPtrthrd(new StdThread([this]()`
- L715: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L743: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`

### `src/TraderHuaX/TraderHuaX.h` (209 行)
- 类型: TraderHuaX:48
- L52: `virtual ~TraderHuaX()`
- L66: `virtual voidOnFrontConnected() override`
- L78: `virtual voidOnFrontDisconnected(int nReason) override`
- L80: `virtual voidOnRspError(CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L82: `virtual voidOnRspOrderInsert(CTORATstpInputOrderField* pInputOrderField, CTORATstpRspInfoField* pRspInfoField, int nRequestID) override`
- L84: `virtual voidOnRtnOrder(CTORATstpOrderField* pOrderField) override`
- L86: `virtual voidOnErrRtnOrderInsert(CTORATstpInputOrderField* pInputOrderField, CTORATstpRspInfoField* pRspInfoField, int nRequestID) override`
- L88: `virtual voidOnRtnTrade(CTORATstpTradeField* pTradeField) override`
- L90: `virtual voidOnErrRtnOrderAction(CTORATstpInputOrderActionField* pInputOrderActionField, CTORATstpRspInfoField* pRspInfoField, int nRequestID) override`
- L92: `virtual voidOnRspQryOrder(CTORATstpOrderField* pOrderField, CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L94: `virtual voidOnRspQryTrade(CTORATstpTradeField* pTradeField, CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L96: `virtual voidOnRspQryPosition(CTORATstpPositionField* pPositionField, CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L98: `virtual voidOnRspQryTradingAccount(CTORATstpTradingAccountField* pTradingAccountField, CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L100: `virtual voidOnRspUserLogin(CTORATstpRspUserLoginField* pRspUserLoginField, CTORATstpRspInfoField* pRspInfoField, int nRequestID) override`
- L102: `virtual voidOnRspGetConnectionInfo(CTORATstpConnectionInfoField* pConnectionInfoField, CTORATstpRspInfoField* pRspInfo, int nRequestID) override`
- L104: `virtual voidOnRspQryShareholderAccount(CTORATstpShareholderAccountField* pShareholderAccountField, CTORATstpRspInfoField* pRspInfoField, int nRequestID, bool bIsLast) override`
- L109: `virtual boolinit(WTSVariant *params) override`
- L111: `virtual voidrelease() override`
- L113: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L115: `virtual voidconnect() override`
- L117: `virtual voiddisconnect() override`
- L119: `virtual boolisConnected() override`
- L121: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L123: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L125: `virtual intlogout() override`
- L127: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L129: `virtual intorderAction(WTSEntrustAction* action) override`
- L131: `virtual intqueryAccount() override`
- L133: `virtual intqueryPositions() override`
- L135: `virtual intqueryOrders() override`
- L137: `virtual intqueryTrades() override`
- L140: `voidreconnect()`
- L141: `inline uint32_tgenRequestID()`
- L142: `voiddoLogin()`
- L144: `inline WTSOrderInfo*makeOrderInfo(CTORATstpOrderField* orderField)`
- L145: `inline WTSEntrust*makeEntrust(CTORATstpInputOrderField* entrustField)`
- L146: `inline WTSTradeInfo*makeTradeInfo(CTORATstpTradeField*tradeField)`
- L147: `inline WTSPositionItem*makePositionItem(CTORATstpPositionField* positionField)`
- L148: `inline WTSAccountInfo*makeAccountInfo(CTORATstpTradingAccountField* accountField)`
- L150: `inline WTSDirectionTypewrapDirectionType(TTORATstpDirectionType dirType)`
- L151: `inline WTSPriceTypewrapPriceType(TTORATstpDirectionType priceType)`
- L152: `inline WTSOrderStatewrapOrderState(TTORATstpOrderStatusType orderState)`
- L155: `inline boolextractEntrustID(const char* entrustid, int &orderRef)`
- L156: `inline voidgenEntrustID(char* buffer, uint32_t orderRef)`

## `TraderMocker`

### `src/TraderMocker/TraderMocker.cpp` (1007 行)
- 类型: UDPDataPacket:979
- L146: `boost::asio::post(_io_service, [this, entrust]()`
- L147: `StdUniqueLocklock(_mutex_api)`
- L353: `StdUniqueLocklock(_mtx_awaits)`
- L479: `StdUniqueLocklock(_mutex_api)`
- L541: `fs::create_directories(path.c_str())`
- L555: `StdFile::read_file_content(_pos_file.c_str(), json)`
- L594: `rj::Documentroot(rj::kObjectType)`
- L597: `rj::ValuejPos(rj::kArrayType)`
- L606: `rj::ValuepItem(rj::kObjectType)`
- L611: `rj::ValuedItem(rj::kObjectType)`
- L618: `rj::ValuedItem(rj::kObjectType)`
- L633: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L635: `StdFile::write_file_content(_pos_file.c_str(), sb.GetString())`
- L695: `boost::asio::post(_io_service, [this]()`
- L696: `StdUniqueLocklock(_mutex_api)`
- L731: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L735: `boost::asio::post(_io_service, [this]()`
- L736: `StdUniqueLocklock(_mutex_api)`
- L754: `boost::asio::post(_io_service, [this, action]()`
- L755: `StdUniqueLocklck(_mtx_awaits) //一定要把awaits锁起来,不然可能会导致一边撮合一边撤单`
- L815: `StdUniqueLocklock(_mutex_api)`
- L832: `boost::asio::post(_io_service, [this]()`
- L852: `StdUniqueLocklock(_mutex_api)`
- L864: `boost::asio::post(_io_service, [this]()`
- L902: `StdUniqueLocklock(_mutex_api)`
- L913: `boost::asio::post(_io_service, [this]()`
- L914: `StdUniqueLocklock(_mutex_api)`
- L925: `boost::asio::post(_io_service, [this]()`
- L926: `StdUniqueLocklock(_mutex_api)`
- L944: `std::this_thread::sleep_for(std::chrono::seconds(2))`

### `src/TraderMocker/TraderMocker.h` (150 行)
- 类型: WTSTickData:17, TraderMocker:25
- L35: `int32_tmatch_once()`
- L37: `uint32_tmakeTradeID()`
- L38: `uint32_tmakeOrderID()`
- L40: `voidload_positions()`
- L41: `voidsave_positions()`
- L112: `voidhandle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad)`
- L114: `voidextract_buffer(uint32_t length, bool isBroad)`
- L116: `voidreconn_udp()`
- L121: `virtual boolinit(WTSVariant *params) override`
- L123: `virtual voidrelease() override`
- L125: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L127: `virtual voidconnect() override`
- L129: `virtual voiddisconnect() override`
- L131: `virtual boolisConnected() override`
- L133: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L135: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L137: `virtual intlogout() override`
- L139: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L141: `virtual intorderAction(WTSEntrustAction* action) override`
- L143: `virtual intqueryAccount() override`
- L145: `virtual intqueryPositions() override`
- L147: `virtual intqueryOrders() override`
- L149: `virtual intqueryTrades() override`

## `TraderOES`

### `src/TraderOES/TraderOES.cpp` (1003 行)
- L229: `DLLHelper::load_library(dllpath.c_str())`
- L243: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`
- L276: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`
- L330: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L564: `fs::create_directories(path.c_str())`
- L577: `fs::create_directories(path.c_str())`

### `src/TraderOES/TraderOES.h` (129 行)
- 类型: TraderOES:28
- L32: `virtual ~TraderOES()`
- L44: `voidhandle_ord_connected(OesAsyncApiChannelT *pAsyncChannel)`
- L45: `voidhandle_ord_disconnected(OesAsyncApiChannelT *pAsyncChannel)`
- L46: `voidhandle_rpt_connected(OesAsyncApiChannelT *pAsyncChannel)`
- L47: `voidhandle_rpt_disconnected(OesAsyncApiChannelT *pAsyncChannel)`
- L48: `voidhandle_rpt_message(SMsgHeadT *pMsgHead, void *pMsgItem)`
- L49: `voidhandle_ord_message(SMsgHeadT *pMsgHead, void *pMsgItem)`
- L51: `voidhandle_rsp_account(SMsgHeadT *pMsgHead, void *pMsgItem, OesQryCursorT *pQryCursor)`
- L52: `voidhandle_rsp_orders(SMsgHeadT *pMsgHead, void *pMsgItem, OesQryCursorT *pQryCursor)`
- L53: `voidhandle_rsp_trades(SMsgHeadT *pMsgHead, void *pMsgItem, OesQryCursorT *pQryCursor)`
- L54: `voidhandle_rsp_positions(SMsgHeadT *pMsgHead, void *pMsgItem, OesQryCursorT *pQryCursor)`
- L59: `virtual boolinit(WTSVariant *params) override`
- L61: `virtual voidrelease() override`
- L63: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L65: `virtual voidconnect() override`
- L67: `virtual voiddisconnect() override`
- L69: `virtual boolisConnected() override`
- L71: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L73: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L75: `virtual intlogout() override`
- L77: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L79: `virtual intorderAction(WTSEntrustAction* action) override`
- L81: `virtual intqueryAccount() override`
- L83: `virtual intqueryPositions() override`
- L85: `virtual intqueryOrders() override`
- L87: `virtual intqueryTrades() override`
- L90: `voidreconnect()`
- L91: `voiddoLogin()`
- L93: `inline WTSOrderInfo*makeOrderInfo(OesOrdCnfmT* orderField)`
- L94: `inline WTSEntrust*makeEntrust(OesOrdRejectT *entrustField)`
- L95: `inline WTSTradeInfo*makeTradeInfo(OesTrdCnfmT *tradeField)`
- L97: `inline boolextractEntrustID(const char* entrustid, uint32_t &orderRef)`
- L98: `inline voidgenEntrustID(char* buffer, uint32_t orderRef)`

