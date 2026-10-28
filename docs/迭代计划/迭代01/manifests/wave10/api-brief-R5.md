# API 简报 R5

测试目标: dtservo。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtDtServo/WtDataManager.cpp` (617 行)
- L65: `WTSLogger::error("Initializing of random data reader failed: function createRdmDtReader not found...")`
- L71: `WTSLogger::error("Initializing of random data reader failed: function deleteRdmDtReader not found...")`
- L109: `WTSLogger::log_raw(ll, message)`
- L530: `StdUniqueLocklock(_mtx_rtbars)`
- L548: `StdUniqueLocklock(_mtx_rtbars)`
- L562: `StdUniqueLocklock(_mtx_rtbars)`
- L572: `StdUniqueLocklock(_mtx_rtbars)`
- L611: `WTSLogger::warn("DataReader not initialized, clearing canceled")`
- L616: `WTSLogger::warn("All cache cleared")`

### `src/WtDtServo/WtDtRunner.cpp` (648 行)
- L40: `extern const char*getModuleName()`
- L48: `WTSLogger::error("WtDtServo has already been initialized")`
- L55: `WTSLogger::init(logCfg)`
- L56: `WtHelper::set_module_dir(modDir)`
- L61: `WTSLogger::error("Loading config failed")`
- L62: `WTSLogger::log_raw(LL_INFO, cfgFile)`
- L69: `CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd())`
- L77: `WTSLogger::info("Trading sessions loaded")`
- L115: `WTSLogger::info("Holidays loaded")`
- L121: `WTSLogger::info("Hot rules loaded")`
- L127: `WTSLogger::info("Second rules loaded")`
- L183: `WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers")`
- L199: `WTSLogger::info("Data manager initialized")`
- L206: `WTSLogger::error("WtDtServo not initialized")`
- L246: `WTSLogger::error("WtDtServo not initialized")`
- L272: `WTSLogger::log_raw(LL_ERROR, "get_bars_by_date only supports minute period")`
- L288: `WTSLogger::error("WtDtServo not initialized")`
- L304: `WTSLogger::error("WtDtServo not initialized")`
- L315: `WTSLogger::error("WtDtServo not initialized")`
- L355: `WTSLogger::error("WtDtServo not initialized")`
- L371: `WTSLogger::error("WtDtServo not initialized")`
- L397: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, this))`
- L465: `StdUniqueLocklock(_mtx_subs)`
- L511: `StdUniqueLocklock(_mtx_innersubs)`
- L561: `StdUniqueLocklock(_mtx_innersubs)`
- L581: `StdUniqueLocklock(_mtx_subs)`

### `src/WtDtServo/ParserAdapter.cpp` (347 行)
- L290: `WTSLogger::log_raw_by_cat("parser", ll, message)`

### `src/WtDtServo/WtDtServo.cpp` (241 行)
- L41: `fs::pathp(MODULE_NAME)`

### `src/WtDtServo/WtDataManager.h` (333 行)
- 类型: WtDtRunner:27, WTSVariant:30, WTSTickData:31, WTSKlineSlice:32, WTSKlineData:33, WTSTickSlice:34, IBaseDataMgr:35, IHotMgr:36, WTSSessionInfo:37, WTSBarStruct:38, WTSCommodityInfo:39, WtDataManager:52
- L77: `boolinitStore(WTSVariant* cfg)`
- L87: `WTSSessionInfo*get_session_info(const char* sid, bool isCode = false)`
- L95: `virtual IBaseDataMgr*get_basedata_mgr() override { return _bd_mgr; }`
- L100: `virtual IHotMgr*get_hot_mgr() override { return _hot_mgr; }`
- L105: `virtual voidreader_log(WTSLogLevel ll, const char* message) override`
- L117: `boolinit(WTSVariant* cfg, WtDtRunner* runner)`
- L129: `WTSOrdQueSlice*get_order_queue_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0)`
- L141: `WTSOrdDtlSlice*get_order_detail_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0)`
- L153: `WTSTransSlice*get_transaction_slice(const char* stdCode, uint64_t stime, uint64_t etime = 0)`
- L164: `WTSTickSlice*get_tick_slice_by_date(const char* stdCode, uint32_t uDate = 0)`
- L176: `WTSKlineSlice*get_skline_slice_by_date(const char* stdCode, uint32_t secs, uint32_t uDate = 0)`
- L189: `WTSKlineSlice*get_kline_slice_by_date(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t uDate = 0)`
- L201: `WTSTickSlice*get_tick_slices_by_range(const char* stdCode, uint64_t stime, uint64_t etime = 0)`
- L215: `WTSKlineSlice*get_kline_slice_by_range(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint64_t stime, uint64_t etime = 0)`
- L228: `WTSTickSlice*get_tick_slice_by_count(const char* stdCode, uint32_t count, uint64_t etime = 0)`
- L243: `WTSKlineSlice*get_kline_slice_by_count(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0)`
- L250: `doubleget_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo = NULL)`
- L261: `voidsubscribe_bar(const char* stdCode, WTSKlinePeriod period, uint32_t times)`
- L268: `voidclear_subbed_bars()`
- L279: `voidupdate_bars(const char* stdCode, WTSTickData* newTick)`
- L287: `voidclear_cache()`

### `src/WtDtServo/WtDtRunner.h` (350 行)
- 类型: WTSVariant:24, WtDataStorage:25, WTSKlineSlice:26, WTSTickSlice:27, WtDtRunner:37
- L75: `voidstart()`
- L83: `inline WTSBaseDataMgr&getBaseDataMgr() return _bd_mgr; }`
- L91: `inline WTSHotMgr&getHotMgr() return _hot_mgr; }`
- L101: `voidproc_tick(WTSTickData* curTick)`
- L110: `voidtrigger_tick(const char* stdCode, WTSTickData* curTick)`
- L120: `voidtrigger_bar(const char* stdCode, const char* period, WTSBarStruct* lastBar)`
- L131: `voidsub_tick(const char* stdCode, bool bReplace, bool bInner = false)`
- L141: `voidsub_bar(const char* stdCode, const char* period)`
- L150: `voidclear_cache()`
- L165: `WTSKlineSlice*get_bars_by_range(const char* stdCode, const char* period, uint64_t beginTime, uint64_t endTime = 0)`
- L178: `WTSKlineSlice*get_bars_by_date(const char* stdCode, const char* period, uint32_t uDate = 0)`
- L191: `WTSTickSlice*get_ticks_by_range(const char* stdCode, uint64_t beginTime, uint64_t endTime = 0)`
- L205: `WTSKlineSlice*get_bars_by_count(const char* stdCode, const char* period, uint32_t count, uint64_t endTime = 0)`
- L218: `WTSTickSlice*get_ticks_by_count(const char* stdCode, uint32_t count, uint64_t endTime = 0)`
- L230: `WTSTickSlice*get_ticks_by_date(const char* stdCode, uint32_t uDate = 0)`
- L244: `WTSKlineSlice*get_sbars_by_date(const char* stdCode, uint32_t secs, uint32_t uDate = 0)`
- L254: `voidinitDataMgr(WTSVariant* config)`
- L263: `voidinitParsers(WTSVariant* cfg)`

### `src/WtDtServo/ParserAdapter.h` (90 行)
- 类型: WTSVariant:18, WTSBaseDataMgr:22, WtDtRunner:23, ParserAdapter:25, ParserAdapterMgr:73
- L29: `virtual ~ParserAdapter()`
- L32: `boolinit(const char* id, WTSVariant* cfg)`
- L34: `boolinitExt(const char* id, IParserApi* api)`
- L36: `voidrelease()`
- L38: `boolrun()`
- L40: `const char*id() }`
- L43: `virtual voidhandleSymbolList(const WTSArray* aySymbols) override`
- L45: `virtual voidhandleQuote(WTSTickData *quote, uint32_t procFlag) override`
- L47: `virtual voidhandleOrderQueue(WTSOrdQueData* ordQueData) override`
- L49: `virtual voidhandleTransaction(WTSTransData* transData) override`
- L51: `virtual voidhandleOrderDetail(WTSOrdDtlData* ordDetailData) override`
- L53: `virtual voidhandleParserLog(WTSLogLevel ll, const char* message) override`
- L55: `virtual IBaseDataMgr*getBaseDataMgr() override`
- L76: `voidrelease()`
- L78: `voidrun()`
- L80: `ParserAdapterPtrgetAdapter(const char* id)`
- L82: `booladdAdapter(const char* id, ParserAdapterPtr& adapter)`
- L84: `uint32_tsize() }`

### `src/WtDtServo/WtHelper.h` (70 行)
- 类型: WtHelper:24
- L36: `static const char*get_cwd()`
- L47: `static const char*get_module_dir() }`
- L58: `static voidset_module_dir(const char* mod_dir) _bin_dir = mod_dir; }`

