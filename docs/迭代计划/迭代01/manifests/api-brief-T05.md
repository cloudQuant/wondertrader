# API 简报 T05

覆盖模块: TraderCTPOpt, TraderDD, TraderDumper, TraderFemas。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `TraderCTPOpt`

### `src/TraderCTPOpt/TraderCTPOpt.cpp` (1827 行)
- L310: `fs::create_directories(ss.str().c_str())`
- L338: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`
- L345: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L355: `StdUniqueLocklock(m_mtxQuery)`
- L589: `StdUniqueLocklock(m_mtxQuery)`
- L749: `StdUniqueLocklock(m_mtxQuery)`
- L770: `StdUniqueLocklock(m_mtxQuery)`
- L791: `StdUniqueLocklock(m_mtxQuery)`
- L813: `StdUniqueLocklock(m_mtxQuery)`
- L836: `StdUniqueLocklock(m_mtxQuery)`
- L907: `fs::create_directories(path.c_str())`
- L1441: `StrUtil::replace(strTime, ":", "")`
- L1538: `StrUtil::replace(strTime, ":", "")`
- L1619: `StrUtil::replace(strTime, ":", "")`
- L1738: `StdUniqueLocklock(m_mtxQuery)`
- L1800: `boost::asio::post(*m_strandIO, [this]()`
- L1807: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1808: `boost::asio::post(*m_strandIO, [this]()`
- L1820: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderCTPOpt/TraderCTPOpt.h` (245 行)
- 类型: TraderCTPOpt:28
- L32: `virtual ~TraderCTPOpt()`
- L49: `intconfirm()`
- L51: `intqueryConfirm()`
- L53: `intauthenticate()`
- L55: `intdoLogin()`
- L60: `virtual boolinit(WTSVariant* params) override`
- L62: `virtual voidrelease() override`
- L64: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L66: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L68: `virtual voidconnect() override`
- L70: `virtual voiddisconnect() override`
- L72: `virtual boolisConnected() override`
- L74: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L76: `virtual intlogout() override`
- L78: `virtual intorderInsert(WTSEntrust* entrust) override`
- L80: `virtual intorderAction(WTSEntrustAction* action) override`
- L82: `virtual intqueryAccount() override`
- L84: `virtual intqueryPositions() override`
- L86: `virtual intqueryOrders() override`
- L88: `virtual intqueryTrades() override`
- L90: `virtual intquerySettlement(uint32_t uDate) override`
- L95: `virtual intorderInsertOpt(WTSEntrust* entrust) override`
- L97: `virtual intorderActionOpt(WTSEntrustAction* action) override`
- L99: `virtual intqueryOrdersOpt(WTSBusinessType bType) override`
- L105: `virtual voidOnFrontConnected() override`
- L107: `virtual voidOnFrontDisconnected(int nReason) override`
- L109: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L111: `virtual voidOnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L113: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L115: `virtual voidOnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L117: `virtual voidOnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L119: `virtual voidOnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *pSettlementInfoConfirm, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L121: `virtual voidOnRspQryTradingAccount(CThostFtdcTradingAccountField *pTradingAccount, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L123: `virtual voidOnRspOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L125: `virtual voidOnRspOrderAction(CThostFtdcInputOrderActionField *pInputOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L127: `virtual voidOnRspQryInvestorPosition(CThostFtdcInvestorPositionField *pInvestorPosition, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L129: `virtual voidOnRspQrySettlementInfo(CThostFtdcSettlementInfoField *pSettlementInfo, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L132: `virtual voidOnRspQryTrade(CThostFtdcTradeField *pTrade, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L134: `virtual voidOnRspQryOrder(CThostFtdcOrderField *pOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L136: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L138: `virtual voidOnRtnOrder(CThostFtdcOrderField *pOrder) override`
- L140: `virtual voidOnRtnTrade(CThostFtdcTradeField *pTrade) override`
- L142: `virtual voidOnErrRtnOrderInsert(CThostFtdcInputOrderField *pInputOrder, CThostFtdcRspInfoField *pRspInfo) override`
- L144: `virtual voidOnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *pInstrumentStatus) override`
- L148: `virtual voidOnRspExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L151: `virtual voidOnRspExecOrderAction(CThostFtdcInputExecOrderActionField *pInputExecOrderAction, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L154: `virtual voidOnRtnExecOrder(CThostFtdcExecOrderField *pExecOrder) override`
- L157: `virtual voidOnErrRtnExecOrderInsert(CThostFtdcInputExecOrderField *pInputExecOrder, CThostFtdcRspInfoField *pRspInfo) override`
- L159: `virtual voidOnRspQryExecOrder(CThostFtdcExecOrderField *pExecOrder, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L165: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`
- L168: `WTSOrderInfo*makeOrderInfo(CThostFtdcOrderField* orderField)`
- L169: `WTSOrderInfo*makeOrderInfo(CThostFtdcExecOrderField* orderField)`
- L170: `WTSEntrust*makeEntrust(CThostFtdcInputOrderField *entrustField)`
- L171: `WTSError*makeError(CThostFtdcRspInfoField* rspInfo)`
- L172: `WTSTradeInfo*makeTradeRecord(CThostFtdcTradeField *tradeField)`
- L173: `WTSEntrust*makeEntrust(CThostFtdcInputExecOrderField *entrustField)`
- L175: `voidgenerateEntrustID(char* buffer, uint32_t frontid, uint32_t sessionid, uint32_t orderRef)`
- L176: `boolextractEntrustID(const char* entrustid, uint32_t &frontid, uint32_t &sessionid, uint32_t &orderRef)`
- L181: `uint32_tgenRequestID()`

## `TraderDD`

### `src/TraderDD/TraderDD.cpp` (1621 行)
- L54: `voidinst_hlp() }`
- L124: `boost::filesystem::pathp(_bin_dir)`
- L219: `boolcb_rtn_order(HANDLE_CONN hconn, HANDLE_SESSION hsess, int64_t subid, void *pData)`
- L417: `StdThreadPtrthrd(new StdThread([this]()`
- L418: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L482: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L493: `boost::asio::post(m_asyncIO, [this]()`
- L557: `boost::asio::post(*m_strandIO, [this]()`
- L611: `boost::asio::post(*m_strandIO, [this]()`
- L652: `boost::asio::post(*m_strandIO, [this]()`
- L719: `boost::filesystem::create_directories(path.c_str())`
- L816: `boost::asio::post(*m_strandIO, [this, entrust]()`
- L902: `boost::asio::post(*m_strandIO, [this, action]()`
- L945: `StdUniqueLocklock(m_mtxQuery)`
- L1001: `StdUniqueLocklock(m_mtxQuery)`
- L1339: `StdUniqueLocklock(m_mtxQuery)`
- L1482: `StdUniqueLocklock(m_mtxQuery)`
- L1595: `boost::asio::post(*m_strandIO, [this]()`
- L1615: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderDD/TraderDD.h` (158 行)
- 类型: TraderDD:36
- L40: `virtual ~TraderDD()`
- L55: `voiddoLogin()`
- L56: `voidqryGDNo() //查询股东号`
- L57: `voidqryZJZH() //查询资金账户`
- L59: `voidreconnect()`
- L61: `voidtriggerQuery()`
- L64: `voidOnRtnOrder(HANDLE_CONN hconn, HANDLE_SESSION hsess)`
- L65: `voidOnRtnTrade(HANDLE_CONN hconn, HANDLE_SESSION hsess)`
- L70: `virtual boolinit(WTSVariant* params) override`
- L72: `virtual voidrelease() override`
- L74: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L76: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L78: `virtual voidconnect() override`
- L80: `virtual voiddisconnect() override`
- L82: `virtual boolisConnected() override`
- L84: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L86: `virtual intlogout() override`
- L88: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L90: `virtual intorderAction(WTSEntrustAction* action) override`
- L92: `virtual intqueryAccount() override`
- L94: `virtual intqueryPositions() override`
- L96: `virtual intqueryOrders() override`
- L98: `virtual intqueryTrades() override`

## `TraderDumper`

### `src/TraderDumper/Dumper.cpp` (163 行)
- L18: `WTSLogger::init(logProfile)`
- L23: `WtHelper::set_module_dir(modDir)`
- L46: `WTSLogger::info("Trading sessions loaded")`
- L90: `TraderAdapterPtradapter(new TraderAdapter(&g_adapterMgr))`
- L111: `std::this_thread::sleep_for(std::chrono::seconds(1))`
- L122: `std::this_thread::sleep_for(std::chrono::seconds(_refresh_span))`

### `src/TraderDumper/Dumper.h` (54 行)
- 类型: Dumper:5
- L21: `voidinit(const char* logProfile)`
- L23: `boolconfig(const char* cfgfile, bool isFile, const char* modDir)`
- L25: `voidrun(bool bOnce = true)`
- L27: `voidrelease()`

### `src/TraderDumper/TraderAdapter.cpp` (423 行)
- L34: `extern Dumper&getDumper()`
- L35: `extern std::stringgetBaseFolder()`
- L336: `WTSLogger::log_raw(ll, message)`

### `src/TraderDumper/TraderAdapter.h` (126 行)
- 类型: WTSVariant:20, ActionPolicyMgr:21, WTSContractInfo:22, WTSCommodityInfo:23, WtExecuter:24, EventNotifier:25, ITrdNotifySink:27, TraderAdapterMgr:29, TraderAdapter:31, TraderAdapterMgr:97
- L35: `virtual ~TraderAdapter()`
- L38: `boolinit(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr)`
- L40: `voidrelease()`
- L42: `boolrun()`
- L44: `inline const char*id() }`
- L46: `boolisDone() const { return _done; }`
- L48: `voidqueryFund()`
- L49: `voidqueryPosition()`
- L54: `virtual voidhandleEvent(WTSTraderEvent e, int32_t ec) override`
- L56: `virtual voidonLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override`
- L58: `virtual voidonLogout() override`
- L60: `virtual voidonRspAccount(WTSArray* ayAccounts) override`
- L62: `virtual voidonRspPosition(const WTSArray* ayPositions) override`
- L64: `virtual voidonRspTrades(const WTSArray* ayTrades) override`
- L66: `virtual voidonRspOrders(const WTSArray* ayOrders) override`
- L68: `virtual voidonPushTrade(WTSTradeInfo* tradeRecord) override`
- L70: `virtual voidonPushOrder(WTSOrderInfo* orderInfo) override`
- L72: `virtual voidonTraderError(WTSError* err, void* pData = NULL) override`
- L74: `virtual IBaseDataMgr*getBaseDataMgr() override`
- L76: `virtual voidhandleTraderLog(WTSLogLevel ll, const char* message) override`
- L100: `voidrelease()`
- L102: `voidrun()`
- L104: `const TraderAdapterMap&getAdapters() const { return _adapters; }`
- L106: `TraderAdapterPtrgetAdapter(const char* tname)`
- L108: `booladdAdapter(const char* tname, TraderAdapterPtr& adapter)`
- L110: `boolisAnyAlive() const`
- L114: `std::size_tsize() }`
- L116: `voiddecAlive()`
- L118: `voidrefresh()`

### `src/TraderDumper/TraderDumper.h` (21 行)
- L9: `EXPORT_FLAG voidregister_callbacks(FuncOnAccount cbAccount, FuncOnOrder cbOrder, FuncOnTrade cbTrade, FuncOnPosition cbPosition)`
- L11: `EXPORT_FLAG voidinit(const char* logProfile)`
- L13: `EXPORT_FLAG boolconfig(const char* cfgfile, bool isFile)`
- L15: `EXPORT_FLAG voidrun(bool bOnce)`
- L17: `EXPORT_FLAG voidrelease()`

### `src/TraderDumper/WtHelper.h` (26 行)
- 类型: WtHelper:14
- L17: `static const char*get_cwd()`
- L19: `static const char*get_module_dir() }`
- L21: `static voidset_module_dir(const char* mod_dir) _bin_dir = mod_dir; }`

## `TraderFemas`

### `src/TraderFemas/TraderFemas.cpp` (1180 行)
- L174: `fs::create_directories(path.c_str())`
- L206: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L216: `boost::asio::post(m_asyncIO, [this]()`
- L431: `StdUniqueLocklock(m_mtxQuery)`
- L453: `StdUniqueLocklock(m_mtxQuery)`
- L473: `StdUniqueLocklock(m_mtxQuery)`
- L494: `StdUniqueLocklock(m_mtxQuery)`
- L575: `StrUtil::trim(m_strSessionID, "0", false, true)`
- L997: `StrUtil::replace(strTime, ":", "")`
- L1074: `StrUtil::replace(strTime, ":", "")`
- L1154: `boost::asio::post(*m_strandIO, [this]()`
- L1161: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1162: `boost::asio::post(*m_strandIO, [this]()`
- L1174: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderFemas/TraderFemas.h` (247 行)
- 类型: TraderFemas:42
- L46: `virtual ~TraderFemas()`
- L62: `voidonInitialized()`
- L64: `intauthenticate()`
- L66: `intdoLogin()`
- L71: `virtual boolinit(WTSVariant* params) override`
- L73: `virtual voidrelease() override`
- L75: `virtual voidsetTag(const char* tag) m_strTag = tag;}`
- L76: `virtual const char*getTag() }`
- L78: `virtual voidregisterSpi(ITraderSpi *listener) override`
- L80: `virtual boolmakeEntrustID(char* buffer, int length) override`
- L82: `virtual voidconnect() override`
- L84: `virtual voiddisconnect() override`
- L86: `virtual boolisConnected() override`
- L88: `virtual intlogin(const char* user, const char* pass, const char* productInfo) override`
- L90: `virtual intlogout() override`
- L92: `virtual intorderInsert(WTSEntrust* eutrust) override`
- L94: `virtual intorderAction(WTSEntrustAction* action) override`
- L96: `virtual intqueryAccount() override`
- L98: `virtual intqueryPositions() override`
- L100: `virtual intqueryOrders() override`
- L102: `virtual intqueryTrades() override`
- L108: `virtual voidOnFrontConnected() override`
- L110: `virtual voidOnQryFrontConnected() override`
- L112: `virtual voidOnFrontDisconnected(int nReason) override`
- L114: `virtual voidOnQryFrontDisconnected(int nReason) override`
- L116: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L118: `virtual voidOnRspError(CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L120: `virtual voidOnRspUserLogin(CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L123: `virtual voidOnRspQueryUserLogin(CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L125: `virtual voidOnRspUserLogout(CUstpFtdcRspUserLogoutField *pRspUserLogout, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L127: `virtual voidOnRspDSUserCertification(CUstpFtdcDSUserCertRspDataField *pDSUserCertRspData, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L130: `virtual voidOnRtnTrade(CUstpFtdcTradeField *pTrade) override`
- L133: `virtual voidOnRtnOrder(CUstpFtdcOrderField *pOrder) override`
- L136: `virtual voidOnRspOrderInsert(CUstpFtdcInputOrderField *pInputOrder, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L139: `virtual voidOnRspOrderAction(CUstpFtdcOrderActionField *pOrderAction, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L141: `virtual voidOnRspQryTrade(CUstpFtdcTradeField *pTrade, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L143: `virtual voidOnRspQryOrder(CUstpFtdcOrderField *pOrder, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L145: `virtual voidOnRspQryInvestorAccount(CUstpFtdcRspInvestorAccountField *pRspInvestorAccount, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L148: `virtual voidOnRspQryInvestorPosition(CUstpFtdcRspInvestorPositionField *pRspInvestorPosition, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L151: `virtual voidOnErrRtnOrderInsert(CUstpFtdcInputOrderField *pInputOrder, CUstpFtdcRspInfoField *pRspInfo) override`
- L154: `virtual voidOnErrRtnOrderAction(CUstpFtdcOrderActionField *pOrderAction, CUstpFtdcRspInfoField *pRspInfo) override`
- L156: `virtual voidOnRtnInstrumentStatus(CUstpFtdcInstrumentStatusField *pInstrumentStatus) override`
- L159: `boolIsErrorRspInfo(CUstpFtdcRspInfoField *pRspInfo)`
- L161: `intwrapPriceType(WTSPriceType priceType, bool isCFFEX = false)`
- L162: `intwrapDirectionType(WTSDirectionType dirType, WTSOffsetType offType)`
- L163: `intwrapOffsetType(WTSOffsetType offType)`
- L164: `intwrapTimeCondition(WTSTimeCondition timeCond)`
- L165: `intwrapActionFlag(WTSActionFlag actionFlag)`
- L167: `const char*wrapExchg(const char* exchg)`
- L170: `WTSPriceTypewrapPriceType(TUstpFtdcPriceType priceType)`
- L171: `WTSDirectionTypewrapDirectionType(TUstpFtdcDirectionType dirType, TUstpFtdcOffsetFlagType offType)`
- L172: `WTSDirectionTypewrapPosDirection(TUstpFtdcDirectionType dirType)`
- L173: `WTSOffsetTypewrapOffsetType(TUstpFtdcOffsetFlagType offType)`
- L174: `WTSTimeConditionwrapTimeCondition(TUstpFtdcTimeConditionType timeCond)`
- L175: `WTSOrderStatewrapOrderState(TUstpFtdcOrderStatusType orderState)`
- L177: `WTSOrderInfo*makeOrderInfo(CUstpFtdcOrderField* orderField)`
- L178: `WTSEntrust*makeEntrust(CUstpFtdcInputOrderField *entrustField)`
- L179: `WTSError*makeError(CUstpFtdcRspInfoField* rspInfo)`
- L180: `WTSTradeInfo*makeTradeRecord(CUstpFtdcTradeField *tradeField)`
- L183: `uint32_tgenRequestID()`
- L184: `uint32_tgenLocalOrdID()`
- L186: `voidtriggerQuery()`

