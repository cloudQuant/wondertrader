# API 简报 T02

覆盖模块: ParserCTPOpt, ParserFemas, ParserHuaX, ParserOES。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `ParserCTPOpt`

### `src/ParserCTPOpt/ParserCTPOpt.cpp` (459 行)
- L35: `fmtutil::format_to(buffer, format, args...)`
- L118: `fs::create_directories(fs::path(path))`
- L238: `TimeUtils::getDateTime(actDate, actTime)`

### `src/ParserCTPOpt/ParserCTPOpt.h` (115 行)
- 类型: WTSTickData:17, ParserCTPOpt:22
- L26: `virtual ~ParserCTPOpt()`
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

## `ParserFemas`

### `src/ParserFemas/ParserFemas.cpp` (462 行)
- L35: `fmtutil::format_to(buffer, format, args...)`
- L116: `fs::create_directories(fs::path(path))`
- L233: `TimeUtils::getDateTime(curDate, curTime)`

### `src/ParserFemas/ParserFemas.h` (112 行)
- 类型: WTSTickData:17, ParserFemas:22
- L26: `virtual ~ParserFemas()`
- L38: `virtual boolinit(WTSVariant* config) override`
- L40: `virtual voidrelease() override`
- L42: `virtual boolconnect() override`
- L44: `virtual booldisconnect() override`
- L46: `virtual boolisConnected() override`
- L48: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L49: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L51: `virtual voidregisterSpi(IParserSpi* listener) override`
- L56: `virtual voidOnRspError(CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L58: `virtual voidOnFrontConnected() override`
- L60: `virtual voidOnRspUserLogin(CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L63: `virtual voidOnRspUserLogout(CUstpFtdcRspUserLogoutField *pUserLogout, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L65: `virtual voidOnFrontDisconnected(int nReason) override`
- L67: `virtual voidOnRspUnSubMarketData(CUstpFtdcSpecificInstrumentField *pSpecificInstrument, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L69: `virtual voidOnRtnDepthMarketData(CUstpFtdcDepthMarketDataField *pDepthMarketData) override`
- L71: `virtual voidOnRspSubMarketData(CUstpFtdcSpecificInstrumentField *pSpecificInstrument, CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast) override`
- L73: `virtual voidOnHeartBeatWarning(int nTimeLapse) override`
- L79: `voidReqUserLogin()`
- L83: `voidSubscribeMarketData()`
- L87: `boolIsErrorRspInfo(CUstpFtdcRspInfoField *pRspInfo)`

## `ParserHuaX`

### `src/ParserHuaX/ParserHuaX.cpp` (479 行)
- L25: `fmt::format_to(buffer, format, args...)`
- L94: `fs::create_directories(path.c_str())`

### `src/ParserHuaX/ParserHuaX.h` (124 行)
- 类型: WTSTickData:27, ParserHuaX:39
- L43: `virtual ~ParserHuaX()`
- L55: `virtual boolinit(WTSVariant* config) override`
- L57: `virtual voidrelease() override`
- L59: `virtual boolconnect() override`
- L61: `virtual booldisconnect() override`
- L63: `virtual boolisConnected() override`
- L65: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L66: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L68: `virtual voidregisterSpi(IParserSpi* listener) override`
- L73: `virtual voidOnFrontConnected() override`
- L83: `virtual voidOnFrontDisconnected(int nReason) override`
- L85: `virtual voidOnRspUserLogin(CTORATstpRspUserLoginField* pRspUserLoginField, CTORATstpRspInfoField* pRspInfoField, int nRequestID) override`
- L87: `virtual voidOnRspSubMarketData(CTORATstpSpecificSecurityField* pSpecificSecurityField, CTORATstpRspInfoField* pRspInfoField) override`
- L89: `virtual voidOnRtnMarketData(CTORATstpMarketDataField* pMarketDataField) override`
- L95: `voidDoLogin()`
- L99: `voidDoSubscribeMD()`

## `ParserOES`

### `src/ParserOES/ParserOES.cpp` (871 行)
- L36: `fmtutil::format_to(buffer, format, args...)`
- L140: `DLLHelper::load_library(dllpath.c_str())`
- L154: `std::this_thread::sleep_for(std::chrono::milliseconds(100))`

### `src/ParserOES/ParserOES.h` (68 行)
- 类型: ParserOES:20
- L28: `virtual boolinit(WTSVariant* config) override`
- L30: `virtual voidrelease() override`
- L32: `virtual boolconnect() override`
- L34: `virtual booldisconnect() override`
- L36: `virtual boolisConnected() override`
- L38: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L39: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L41: `virtual voidregisterSpi(IParserSpi* listener) override`
- L45: `voiddoSubscribe()`
- L48: `voiddoOnConnected(MdsAsyncApiChannelT *pAsyncChannel)`
- L49: `voiddoOnDisconnected(MdsAsyncApiChannelT *pAsyncChannel)`
- L50: `voiddoOnMessage(SMsgHeadT *pMsgHead, void *pMsgItem)`

