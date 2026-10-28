# API 简报 R9

测试目标: btporter。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtBtPorter/WtBtPorter.cpp` (2002 行)
- L289: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L293: `WTSLogger::log_raw((WTSLogLevel)level, message)`

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

### `src/WtBtPorter/ExpHftMocker.cpp` (111 行)
- L6: `extern WtBtRunner&getRunner()`
- L25: `HftMocker::on_bar(stdCode, period, times, newBar)`
- L32: `HftMocker::on_channel_ready()`
- L39: `HftMocker::on_entrust(localid, stdCode, bSuccess, message, userTag)`
- L46: `HftMocker::on_init()`
- L55: `HftMocker::on_session_begin(uDate)`
- L63: `HftMocker::on_session_end(uDate)`
- L71: `HftMocker::on_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag)`
- L82: `HftMocker::on_tick_updated(stdCode, newTick)`
- L103: `HftMocker::on_trade(localid, stdCode, isBuy, vol, price, userTag)`

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

### `src/WtBtPorter/ExpCtaMocker.cpp` (204 行)
- L21: `extern WtBtRunner&getRunner()`
- L60: `CtaMocker::on_init()`
- L79: `CtaMocker::on_session_begin(uCurDate)`
- L100: `CtaMocker::on_session_end(uCurDate)`
- L119: `CtaMocker::on_tick_updated(stdCode, newTick)`
- L135: `CtaMocker::on_bar_close(code, period, newBar)`
- L153: `CtaMocker::on_calculate(curDate, curTime)`
- L170: `CtaMocker::on_calculate_done(curDate, curTime)`

### `src/WtBtPorter/ExpSelMocker.cpp` (157 行)
- L20: `extern WtBtRunner&getRunner()`
- L54: `SelMocker::on_init()`
- L72: `SelMocker::on_session_begin(uDate)`
- L88: `SelMocker::on_session_end(uDate)`
- L124: `SelMocker::on_bar_close(stdCode, period, newBar)`
- L140: `SelMocker::on_strategy_schedule(curDate, curTime)`

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

