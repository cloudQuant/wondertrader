# API 简报 T01

覆盖模块: CTPLoader, CTPOptLoader, ParserCTP, ParserCTPMini。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `CTPLoader`

### `src/CTPLoader/CTPLoader.cpp` (291 行)
- L71: `EXPORT_FLAG intrun(const char* cfgfile, bool bAsync, bool isFile)`
- L277: `boost::filesystem::create_directories(flowPath.c_str())`

### `src/CTPLoader/TraderSpi.cpp` (867 行)
- L233: `SpinLocklock(_mtx)`
- L245: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L252: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L262: `SpinLocklock(_mtx)`
- L299: `throw std::runtime_error("non implemented category")`
- L686: `rj::Documentroot(rj::kObjectType)`
- L701: `rj::ValuejFees(rj::kObjectType)`
- L726: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L738: `rj::DocumentjComms(rj::kObjectType)`
- L750: `rj::ValuejComm(rj::kObjectType)`
- L770: `rj::DocumentjContracts(rj::kObjectType)`
- L782: `rj::ValuejcInfo(rj::kObjectType)`
- L807: `rj::ValueoptInfo(rj::kObjectType)`
- L826: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L838: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`

### `src/CTPLoader/TraderSpi.h` (82 行)
- 类型: CTraderSpi:20
- L33: `virtual voidOnFrontConnected() override`
- L35: `virtual voidOnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L38: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L41: `virtual voidOnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L43: `virtual voidOnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L45: `virtual voidOnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField *pInstrumentMarginRate, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L48: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L51: `virtual voidOnFrontDisconnected(int nReason) override`
- L54: `voidReqAuth()`
- L56: `voidReqUserLogin()`
- L58: `voidReqQryInstrument()`
- L60: `voidReqQryCommission(const Contract& cInfo)`
- L62: `voidReqQryMargin(const Contract& cInfo)`
- L65: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`
- L67: `voidDumpToJson()`
- L69: `voidDumpFees()`
- L71: `voidLoadFromJson()`
- L73: `voidAppendQuery(const QueryTask& task)`

## `CTPOptLoader`

### `src/CTPOptLoader/CTPOptLoader.cpp` (268 行)
- L62: `EXPORT_FLAG intrun(const char* cfgfile, bool bAsync, bool isFile)`
- L254: `fs::create_directories(flowPath.c_str())`

### `src/CTPOptLoader/TraderSpi.cpp` (623 行)
- L211: `throw std::runtime_error("non implemented category")`
- L492: `rj::DocumentjComms(rj::kObjectType)`
- L504: `rj::ValuejComm(rj::kObjectType)`
- L524: `rj::DocumentjContracts(rj::kObjectType)`
- L536: `rj::ValuejcInfo(rj::kObjectType)`
- L563: `rj::ValueoptInfo(rj::kObjectType)`
- L582: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L594: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`

### `src/CTPOptLoader/TraderSpi.h` (42 行)
- 类型: CTraderSpi:5
- L9: `virtual voidOnFrontConnected()`
- L11: `virtual voidOnRspAuthenticate(CThostFtdcRspAuthenticateField *pRspAuthenticateField, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)`
- L14: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)`
- L17: `virtual voidOnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)`
- L20: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)`
- L23: `virtual voidOnFrontDisconnected(int nReason)`
- L26: `voidReqAuth()`
- L28: `voidReqUserLogin()`
- L30: `voidReqQryInstrument()`
- L33: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`
- L35: `voidDumpToJson()`
- L37: `voidLoadFromJson()`

## `ParserCTP`

### `src/ParserCTP/ParserCTP.cpp` (459 行)
- L35: `fmtutil::format_to(buffer, format, args...)`
- L117: `fs::create_directories(fs::path(path))`
- L245: `TimeUtils::getDateTime(actDate, actTime)`
- L258: `TimeUtils::getDateTime(curDate, curTime)`

### `src/ParserCTP/ParserCTP.h` (116 行)
- 类型: WTSTickData:21, ParserCTP:26
- L30: `virtual ~ParserCTP()`
- L42: `virtual boolinit(WTSVariant* config) override`
- L44: `virtual voidrelease() override`
- L46: `virtual boolconnect() override`
- L48: `virtual booldisconnect() override`
- L50: `virtual boolisConnected() override`
- L52: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L53: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L55: `virtual voidregisterSpi(IParserSpi* listener) override`
- L60: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L62: `virtual voidOnFrontConnected() override`
- L64: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L67: `virtual voidOnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L69: `virtual voidOnFrontDisconnected(int nReason) override`
- L71: `virtual voidOnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L73: `virtual voidOnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData) override`
- L75: `virtual voidOnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L77: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L83: `voidReqUserLogin()`
- L87: `voidDoSubscribeMD()`
- L91: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`

## `ParserCTPMini`

### `src/ParserCTPMini/ParserCTPMini.cpp` (467 行)
- L35: `fmtutil::format_to(buffer, format, args...)`
- L117: `fs::create_directories(fs::path(path))`
- L237: `TimeUtils::getDateTime(curDate, curTime)`

### `src/ParserCTPMini/ParserCTPMini.h` (112 行)
- 类型: WTSTickData:17, ParserCTPMini:22
- L26: `virtual ~ParserCTPMini()`
- L38: `virtual boolinit(WTSVariant* config) override`
- L40: `virtual voidrelease() override`
- L42: `virtual boolconnect() override`
- L44: `virtual booldisconnect() override`
- L46: `virtual boolisConnected() override`
- L48: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L49: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L51: `virtual voidregisterSpi(IParserSpi* listener) override`
- L56: `virtual voidOnRspError(CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L58: `virtual voidOnFrontConnected() override`
- L60: `virtual voidOnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L63: `virtual voidOnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L65: `virtual voidOnFrontDisconnected(int nReason) override`
- L67: `virtual voidOnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L69: `virtual voidOnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData) override`
- L71: `virtual voidOnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument, CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L73: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L79: `voidReqUserLogin()`
- L83: `voidSubscribeMarketData()`
- L87: `boolIsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)`

