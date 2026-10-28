# API 简报 R6

测试目标: porter。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtPorter/WtRtRunner.cpp` (1272 行)
- L47: `fs::pathp(MODULE_NAME)`
- L110: `CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str())`
- L116: `WTSLogger::init(path.c_str(), true, this)`
- L120: `WTSLogger::init(logCfg, false, this)`
- L124: `WtHelper::setInstDir(getBinDir())`
- L125: `WtHelper::setGenerateDir(StrUtil::standardisePath(genDir).c_str())`
- L143: `WTSLogger::info("Callbacks of Extented Parser registration done")`
- L151: `WTSLogger::info("Callbacks of Extented Executer registration done")`
- L164: `WTSLogger::info("Callbacks of CTA engine registration done")`
- L176: `WTSLogger::info("Callbacks of SEL engine registration done")`
- L200: `WTSLogger::info("Callbacks of HFT engine registration done")`
- L205: `StdUniqueLocklock(_feed_mtx)`
- L222: `WTSLogger::error("Unsupported period of extended data loader")`
- L230: `StdUniqueLocklock(_feed_mtx)`
- L247: `WTSLogger::error("Unsupported period of extended data loader")`
- L255: `StdUniqueLocklock(_feed_mtx)`
- L267: `StdUniqueLocklock(_feed_mtx)`
- L287: `WTSLogger::error("Cannot feed bars because of no extented bar loader registered.")`
- L297: `ParserAdapterPtradapter(new ParserAdapter)`
- L301: `WTSLogger::info("Extended parser created")`
- L310: `WTSLogger::info("Extended Executer created")`
- L498: `WTSLogger::info("Trading sessions loaded")`
- L536: `WTSLogger::log_raw(LL_INFO, "Holidays loaded")`
- L542: `WTSLogger::log_raw(LL_INFO, "Hot rules loaded")`
- L548: `WTSLogger::log_raw(LL_INFO, "Second rules loaded")`
- L607: `WTSLogger::error("Loading parsers failed")`
- L640: `WTSLogger::error("Loading traders failed")`
- L678: `WTSLogger::error("Loading executers failed")`
- L862: `WTSLogger::info("Trading environment initialized, engine name: HFT")`
- L868: `WTSLogger::info("Trading environment initialized, engine name: SEL")`
- L874: `WTSLogger::info("Trading environment initialized, engine name: CTA")`
- L894: `WTSLogger::log_raw(LL_INFO, "Data manager initialized")`
- L921: `ParserAdapterPtradapter(new ParserAdapter)`
- L1079: `TraderAdapterPtradapter(new TraderAdapter(&_notifier))`
- L1104: `WTSLogger::error(message)`
- L1114: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L1121: `WTSLogger::error(message)`
- L1154: `WTSLogger::stop()`
- L1164: `WTSLogger::info("Action policies initialized")`

### `src/WtBtPorter/WtBtPorter.cpp` (2002 行)
- L289: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L293: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtPorter/WtPorter.cpp` (2269 行)
- L225: `WTSLogger::error("API not implemented")`
- L337: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L341: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtBtPorter/WtBtRunner.cpp` (634 行)
- L39: `fs::pathp(MODULE_NAME)`
- L98: `StdUniqueLocklock(_feed_mtx)`
- L115: `WTSLogger::error("Unsupported period of extended data loader")`
- L123: `StdUniqueLocklock(_feed_mtx)`
- L140: `WTSLogger::error("Unsupported period of extended data loader")`
- L148: `StdUniqueLocklock(_feed_mtx)`
- L160: `StdUniqueLocklock(_feed_mtx)`
- L172: `StdUniqueLocklock(_feed_mtx)`
- L186: `WTSLogger::error("Cannot feed bars because of no extented bar loader registered.")`
- L197: `WTSLogger::error("Cannot feed adjusting factors because of no extented adjusting factor loader registered.")`
- L208: `WTSLogger::error("Cannot feed ticks because of no extented tick loader registered.")`
- L227: `WTSLogger::info("Callbacks of CTA engine registration done")`
- L241: `WTSLogger::info("Callbacks of SEL engine registration done")`
- L263: `WTSLogger::info("Callbacks of HFT engine registration done")`
- L440: `CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str())`
- L443: `WTSLogger::init(logProfile, isFile)`
- L445: `WtHelper::setInstDir(getBinDir())`
- L446: `WtHelper::setOutputDir(outDir)`
- L453: `WTSLogger::error("WtBtEngine has already been inited")`
- L460: `WTSLogger::error("Loading config failed")`
- L538: `WTSLogger::error("Exception raised while worker running")`
- L543: `WTSLogger::debug("Worker thread of backtest finished")`
- L564: `WTSLogger::debug("Notify to finish last round")`
- L572: `WTSLogger::debug("Last round ended")`
- L580: `WTSLogger::freeAllDynLoggers()`
- L582: `WTSLogger::debug("Backtest stopped")`
- L587: `WTSLogger::stop()`

### `src/WtPorter/ExpHftContext.cpp` (242 行)
- L19: `extern WtRtRunner&getRunner()`
- L43: `HftStraBaseCtx::on_bar(code, period, times, newBar)`
- L56: `HftStraBaseCtx::on_channel_lost()`
- L69: `HftStraBaseCtx::on_channel_ready()`
- L86: `HftStraBaseCtx::on_entrust(localid, stdCode, bSuccess, message)`
- L96: `HftStraBaseCtx::on_init()`
- L110: `HftStraBaseCtx::on_session_begin(uTDate)`
- L127: `HftStraBaseCtx::on_session_end(uTDate)`
- L147: `HftStraBaseCtx::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled)`
- L187: `HftStraBaseCtx::on_tick(code, newTick)`
- L241: `HftStraBaseCtx::on_trade(localid, stdCode, isBuy, vol, price)`

### `src/WtBtPorter/WtBtRunner.h` (655 行)
- 类型: WTSTickData:21, WTSBarStruct:22, WTSVariant:23, SelMocker:35, CtaMocker:36, HftMocker:37, ExecMocker:38, WtBtRunner:47
- L74: `virtual boolloadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override`
- L85: `virtual boolloadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override`
- L94: `virtual boolloadAllAdjFactors(void* obj, FuncReadFactors cb) override`
- L104: `virtual boolloadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override`
- L115: `virtual boolloadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override`
- L133: `voidfeedRawBars(WTSBarStruct* bars, uint32_t count)`
- L140: `voidfeedRawTicks(WTSTickStruct* ticks, uint32_t count)`
- L149: `voidfeedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)`
- L238: `uint32_tinitCtaMocker(const char* name, int32_t slippage = 0, bool hook = false, bool persistData = true, bool bIncremental = false, bool isRatioSlp = false)`
- L247: `uint32_tinitHftMocker(const char* name, bool hook = false)`
- L272: `boolinitEvtNotifier(WTSVariant* cfg)`
- L281: `voidctx_on_init(uint32_t id, EngineType eType)`
- L291: `voidctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin = true, EngineType eType = ET_CTA)`
- L301: `voidctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType)`
- L311: `voidctx_on_calc(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType)`
- L322: `voidctx_on_calc_done(uint32_t id, uint32_t uDate, uint32_t uTime, EngineType eType)`
- L334: `voidctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType)`
- L347: `voidctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType = ET_CTA)`
- L357: `voidhft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)`
- L366: `voidhft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)`
- L375: `voidhft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTranns)`
- L384: `voidhft_on_channel_ready(uint32_t cHandle, const char* trader)`
- L399: `voidhft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)`
- L412: `voidhft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)`
- L424: `voidhft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)`
- L434: `voidinit(const char* logProfile = "", bool isFile = true, const char* outDir = "./outputs_bt")`
- L442: `voidconfig(const char* cfgFile, bool isFile = true)`
- L452: `voidrun(bool bNeedDump = false, bool bAsync = false)`
- L458: `voidrelease()`
- L465: `voidstop()`
- L475: `voidset_time_range(WtUInt64 stime, WtUInt64 etime)`
- L485: `voidenable_tick(bool bEnabled = true)`
- L493: `voidclear_cache()`
- L503: `const char*get_raw_stdcode(const char* stdCode)`
- L510: `inline CtaMocker*cta_mocker() return _cta_mocker; }`
- L517: `inline SelMocker*sel_mocker() return _sel_mocker; }`
- L524: `inline HftMocker*hft_mocker() return _hft_mocker; }`
- L531: `inline HisDataReplayer&replayer() return _replayer; }`
- L539: `inline boolisAsync() const { return _async; }`

### `src/WtPorter/WtRtRunner.h` (938 行)
- 类型: WTSVariant:39, WtDataStorage:40, WtRtRunner:62
- L87: `virtual boolloadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override`
- L98: `virtual boolloadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override`
- L107: `virtual boolloadAllAdjFactors(void* obj, FuncReadFactors cb) override`
- L117: `virtual boolloadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override`
- L125: `voidfeedRawBars(WTSBarStruct* bars, uint32_t count)`
- L135: `voidfeedAdjFactors(const char* stdCode, uint32_t* dates, double* factors, uint32_t count)`
- L146: `boolinit(const char* logCfg = "logcfg.prop", bool isFile = true, const char* genDir = "")`
- L155: `boolconfig(const char* cfgFile, bool isFile = true)`
- L162: `voidrun(bool bAsync = false)`
- L168: `voidrelease()`
- L180: `voidregisterCtaCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt, FuncStraCondTriggerCallback cbCondTrigger = NULL)`
- L190: `voidregisterSelCallbacks(FuncStraInitCallback cbInit, FuncStraTickCallback cbTick, FuncStraCalcCallback cbCalc, FuncStraBarCallback cbBar, FuncSessionEvtCallback cbSessEvt)`
- L216: `voidregisterEvtCallback(FuncEventCallback cbEvt)`
- L224: `voidregisterParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)`
- L232: `voidregisterExecuterPorter(FuncExecInitCallback cbInit, FuncExecCmdCallback cbExec)`
- L255: `boolcreateExtParser(const char* id)`
- L263: `boolcreateExtExecuter(const char* id)`
- L272: `uint32_tcreateCtaContext(const char* name, int32_t slippage)`
- L283: `uint32_tcreateHftContext(const char* name, const char* trader, bool bAgent, int32_t slippage)`
- L297: `uint32_tcreateSelContext(const char* name, uint32_t date, uint32_t time, const char* period, int32_t slippage, const char* trdtpl = "CHINA", const char* session="TRADING")`
- L304: `CtaContextPtrgetCtaContext(uint32_t id)`
- L311: `SelContextPtrgetSelContext(uint32_t id)`
- L318: `HftContextPtrgetHftContext(uint32_t id)`
- L324: `WtEngine*getEngine() return _engine; }`
- L332: `const char*get_raw_stdcode(const char* stdCode)`
- L343: `virtual voidhandleLogAppend(WTSLogLevel ll, const char* msg) override`
- L352: `voidparser_init(const char* id)`
- L358: `voidparser_connect(const char* id)`
- L364: `voidparser_release(const char* id)`
- L370: `voidparser_disconnect(const char* id)`
- L377: `voidparser_subscribe(const char* id, const char* code)`
- L384: `voidparser_unsubscribe(const char* id, const char* code)`
- L392: `voidon_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag)`
- L404: `voidexecuter_set_position(const char* id, const char* stdCode, double target)`
- L410: `voidexecuter_init(const char* id)`
- L455: `voidctx_on_init(uint32_t id, EngineType eType = ET_CTA)`
- L464: `voidctx_on_session_event(uint32_t id, uint32_t curTDate, bool isBegin = true, EngineType eType = ET_CTA)`
- L473: `voidctx_on_tick(uint32_t id, const char* stdCode, WTSTickData* newTick, EngineType eType = ET_CTA)`
- L482: `voidctx_on_calc(uint32_t id, uint32_t curDate, uint32_t curTime, EngineType eType = ET_CTA)`
- L492: `voidctx_on_bar(uint32_t id, const char* stdCode, const char* period, WTSBarStruct* newBar, EngineType eType = ET_CTA)`
- L503: `voidctx_on_cond_triggered(uint32_t id, const char* stdCode, double target, double price, const char* usertag, EngineType eType = ET_CTA)`
- L510: `voidhft_on_channel_ready(uint32_t cHandle, const char* trader)`
- L517: `voidhft_on_channel_lost(uint32_t cHandle, const char* trader)`
- L531: `voidhft_on_order(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag)`
- L543: `voidhft_on_trade(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag)`
- L554: `voidhft_on_entrust(uint32_t cHandle, WtUInt32 localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag)`
- L566: `voidhft_on_position(uint32_t cHandle, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)`
- L574: `voidhft_on_order_queue(uint32_t id, const char* stdCode, WTSOrdQueData* newOrdQue)`
- L582: `voidhft_on_order_detail(uint32_t id, const char* stdCode, WTSOrdDtlData* newOrdDtl)`
- L590: `voidhft_on_transaction(uint32_t id, const char* stdCode, WTSTransData* newTranns)`
- L598: `booladdExeFactories(const char* folder)`
- L606: `booladdCtaFactories(const char* folder)`
- L614: `booladdHftFactories(const char* folder)`
- L622: `booladdSelFactories(const char* folder)`
- L631: `boolinitTraders(WTSVariant* cfgTrader)`
- L639: `boolinitParsers(WTSVariant* cfgParser)`
- L647: `boolinitExecuters(WTSVariant* cfgExecuter)`
- L654: `boolinitDataMgr()`
- L661: `boolinitEvtNotifier()`
- L668: `boolinitCtaStrategies()`
- L675: `boolinitHftStrategies()`
- L682: `boolinitSelStrategies()`
- L689: `boolinitActionPolicy()`
- L696: `boolinitEngine()`

### `src/WtPorter/ExpCtaContext.cpp` (145 行)
- L20: `extern WtRtRunner&getRunner()`
- L51: `CtaStraBaseCtx::on_init()`
- L69: `CtaStraBaseCtx::on_session_begin(uDate)`
- L87: `CtaStraBaseCtx::on_session_end(uDate)`

### `src/WtPorter/ExpSelContext.cpp` (126 行)
- L18: `extern WtRtRunner&getRunner()`
- L51: `SelStraBaseCtx::on_init()`
- L65: `SelStraBaseCtx::on_session_begin(uDate)`
- L82: `SelStraBaseCtx::on_session_end(uDate)`

### `src/WtPorter/ExpParser.h` (104 行)
- 类型: ExpParser:21
- L33: `virtual ~ExpParser() }`
- L42: `virtual boolinit(WTSVariant* config) override`
- L49: `virtual voidrelease() override`
- L57: `virtual boolconnect() override`
- L65: `virtual booldisconnect() override`
- L73: `virtual boolisConnected() override { return true; }`
- L81: `virtual voidsubscribe(const CodeSet& setCodes) override`
- L89: `virtual voidunsubscribe(const CodeSet& setCodes) override`
- L97: `virtual voidregisterSpi(IParserSpi* listener) override`

### `src/WtPorter/ExpExecuter.h` (54 行)
- 类型: ExpExecuter:21
- L36: `voidinit()`
- L43: `virtual voidset_position(const wt_hashmap<std::string, double>& targets) override`
- L51: `virtual voidon_position_changed(const char* stdCode, double targetPos) override`

### `src/WtBtPorter/ExpHftMocker.h` (187 行)
- 类型: ExpHftMocker:22
- L38: `virtual ~ExpHftMocker() }`
- L50: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L58: `virtual voidon_channel_ready() override`
- L71: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag) override`
- L79: `virtual voidon_init() override`
- L89: `virtual voidon_session_begin(uint32_t uDate) override`
- L98: `virtual voidon_session_end(uint32_t uDate) override`
- L106: `virtual voidon_bactest_end() override`
- L123: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag) override`
- L135: `virtual voidon_tick_updated(const char* stdCode, WTSTickData* newTick) override`
- L147: `virtual voidon_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue) override`
- L158: `virtual voidon_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl) override`
- L170: `virtual voidon_trans_updated(const char* stdCode, WTSTransData* newTrans) override`
- L184: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag) override`

