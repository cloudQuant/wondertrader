# API 简报 N03

覆盖模块: WtIndexFactory, WtLatencyHFT, WtLatencyUFT, WtUftCore。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WtIndexFactory`

### `src/WtIndexFactory/WtBlkIdxWorker.cpp` (342 行)
- L188: `SpinLocklock(_mtx_data)`
- L216: `StdUniqueLocklck(_mtx_trigger)`
- L227: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L261: `SpinLocklock(_mtx_data)`
- L304: `TimeUtils::Time32tm32(maxTime)`

### `src/WtIndexFactory/WtBlkIdxWorker.h` (61 行)
- 类型: WTSVariant:12, WTSTickData:13, IHotMgr:14, IBaseDataMgr:15, WTSContractInfo:16, WtBlkIdxWorker:18
- L24: `virtual boolinit(WTSVariant* config) override`
- L25: `virtual voidhandle_quote(WTSTickData* newTick) override`
- L26: `virtual const char*get_fact_name() override`
- L29: `voidgenerate_tick()`

### `src/WtIndexFactory/WtIndexFactory.h` (19 行)
- 类型: WtIdxFactory:6
- L10: `virtual ~WtIdxFactory()`
- L13: `virtual const char*get_name() override`
- L15: `virtual IIndexWorker*create_worker(const char* name, const char* id) override`
- L17: `virtual booldelete_worker(IIndexWorker* worker) override`

## `WtLatencyHFT`

### `src/WtLatencyHFT/HftLatencyTool.cpp` (507 行)
- 类型: TestParser:89, TestTrader:223, TestStrategy:272
- L287: `virtual const char*getName() override { return "TestStrategy"; }`
- L294: `virtual const char*getFactName() override { return "TestStrategyFact"; }`
- L350: `WTSLogger::init("logcfg.yaml")`
- L356: `WTSLogger::log_raw(LL_ERROR, "Loading config file config.yaml failed")`
- L401: `WTSLogger::log_raw(LL_INFO, "Hot rules loades")`
- L448: `WTSLogger::warn("Trading enviroment initialzied with engine: HFT")`
- L465: `ParserAdapterPtradapter(new ParserAdapter)`
- L472: `TraderAdapterPtradapter(new TraderAdapter())`

### `src/WtLatencyHFT/HftLatencyTool.h` (98 行)
- 类型: WTSVariant:24, HftLatencyTool:33
- L52: `boolinit()`
- L58: `voidrun()`
- L66: `boolinitModules()`
- L73: `boolinitStrategies()`
- L81: `boolinitEngine(WTSVariant* cfg)`

### `src/WtLatencyHFT/main.cpp` (34 行)
- L18: `extern voidtest_hft()`

## `WtLatencyUFT`

### `src/WtLatencyUFT/UftLatencyTool.cpp` (328 行)
- 类型: TestParser:38, TestTrader:134, TestStrategy:159
- L167: `virtual const char*getName() override { return "TestStrategy"; }`
- L172: `virtual const char*getFactName() override { return "TestStrategyFact"; }`
- L200: `WTSLogger::init("logcfg.yaml")`
- L278: `WTSLogger::warn("Trading enviroment initialzied with engine: UFT")`
- L290: `ParserAdapterPtradapter(new ParserAdapter)`
- L297: `TraderAdapterPtradapter(new TraderAdapter())`

### `src/WtLatencyUFT/UftLatencyTool.h` (91 行)
- 类型: WTSVariant:20, UftLatencyTool:29
- L48: `boolinit()`
- L54: `voidrun()`
- L62: `boolinitModules()`
- L69: `boolinitStrategies()`
- L77: `boolinitEngine(WTSVariant* cfg)`

### `src/WtLatencyUFT/main.cpp` (34 行)
- L18: `extern voidtest_uft()`

## `WtUftCore`

### `src/WtUftCore/ActionPolicyMgr.h` (118 行)
- 类型: WTSVariant:22, ActionPolicyMgr:75
- L97: `boolinit(const char* filename)`
- L107: `const ActionRuleGroup&getActionRules(const char* pid)`

### `src/WtUftCore/EventNotifier.cpp` (303 行)
- L81: `DLLHelper::free_library(dllInst)`
- L104: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L121: `boost::asio::post(_asyncio, [this, strTag, strMsg]()`
- L124: `rj::Documentroot(rj::kObjectType)`
- L132: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L149: `boost::asio::post(_asyncio, [this, strMsg]()`
- L152: `rj::Documentroot(rj::kObjectType)`
- L159: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L176: `boost::asio::post(_asyncio, [this, strTrader, strMsg]()`
- L179: `rj::Documentroot(rj::kObjectType)`
- L187: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L205: `boost::asio::post(_asyncio, [this, strTrader, strCode, localid, trdInfo]()`
- L222: `boost::asio::post(_asyncio, [this, strTrader, strCode, localid, ordInfo]()`
- L243: `rj::Documentroot(rj::kObjectType)`
- L258: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L279: `rj::Documentroot(rj::kObjectType)`
- L298: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`

### `src/WtUftCore/EventNotifier.h` (65 行)
- 类型: WTSTradeInfo:27, WTSOrderInfo:28, WTSVariant:29, EventNotifier:31
- L18: `typedef unsignedlong(*FuncCreateMQServer)(const char*)`
- L19: `typedefvoid(*FuncDestroyMQServer)(unsigned long)`
- L20: `typedefvoid(*FundPublishMessage)(unsigned long, const char*, const char*, unsigned long)`
- L22: `typedefvoid(*FuncLogCallback)(unsigned long, const char*, bool)`
- L23: `typedefvoid(*FuncRegCallbacks)(FuncLogCallback)`
- L38: `voidtradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output)`
- L39: `voidorderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output)`
- L42: `boolinit(WTSVariant* cfg)`
- L44: `voidnotify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)`
- L45: `voidnotify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)`
- L46: `voidnotify(const char* trader, const char* message)`
- L48: `voidnotify_log(const char* tag, const char* message)`
- L50: `voidnotify_event(const char* message)`

### `src/WtUftCore/ITrdNotifySink.h` (102 行)
- 类型: ITrdNotifySink:25
- L40: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price) = 0`
- L56: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled = false) = 0`
- L71: `virtual voidon_position(const char* /*stdCode*/, bool /*isLong*/, double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/, uint32_t /*tradingday*/) }`
- L80: `virtual voidon_channel_ready(uint32_t tradingday) = 0`
- L87: `virtual voidon_channel_lost() = 0`
- L99: `virtual voidon_entrust(uint32_t /*localid*/, const char* /*stdCode*/, bool /*bSuccess*/, const char* /*message*/) }`

### `src/WtUftCore/ParserAdapter.cpp` (388 行)
- L335: `WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message)`

### `src/WtUftCore/ParserAdapter.h` (114 行)
- 类型: WTSVariant:19, IParserStub:21, ParserAdapter:31, ParserAdapterMgr:98
- L24: `virtual voidhandle_push_quote(WTSTickData* /*curTick*/) }`
- L26: `virtual voidhandle_push_order_detail(WTSOrdDtlData* /*curOrdDtl*/) }`
- L27: `virtual voidhandle_push_order_queue(WTSOrdQueData* /*curOrdQue*/) }`
- L28: `virtual voidhandle_push_transaction(WTSTransData* /*curTrans*/) }`
- L36: `virtual ~ParserAdapter()`
- L39: `boolinit(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr)`
- L40: `boolinitExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr)`
- L42: `voidrelease()`
- L44: `boolrun()`
- L46: `const char*id() }`
- L49: `virtual voidhandleSymbolList(const WTSArray* /*aySymbols*/) override {}`
- L56: `virtual voidhandleQuote(WTSTickData *quote, uint32_t procFlag) override`
- L62: `virtual voidhandleOrderQueue(WTSOrdQueData* ordQueData) override`
- L68: `virtual voidhandleOrderDetail(WTSOrdDtlData* ordDetailData) override`
- L74: `virtual voidhandleTransaction(WTSTransData* transData) override`
- L76: `virtual voidhandleParserLog(WTSLogLevel ll, const char* message) override`
- L78: `virtual IBaseDataMgr*getBaseDataMgr() override { return _bd_mgr; }`
- L101: `voidrelease()`
- L103: `voidrun()`
- L105: `ParserAdapterPtrgetAdapter(const char* id)`
- L107: `booladdAdapter(const char* id, ParserAdapterPtr& adapter)`

### `src/WtUftCore/ShareManager.cpp` (507 行)
- L107: `std::this_thread::sleep_for(std::chrono::microseconds(microsecs))`
- L111: `WTSLogger::info("Share domain is on watch")`

### `src/WtUftCore/ShareManager.h` (585 行)
- 类型: WtUftEngine:17, ShareManager:236
- L27: `typedefbool(*func_init_master)(const char*, const char*)`
- L35: `typedefuint64_t(*func_get_section_updatetime)(const char*, const char*)`
- L43: `typedefbool(*func_commit_section)(const char*, const char*)`
- L119: `typedefbool(*func_set_string)(const char*, const char*, const char*, const char*)`
- L129: `typedefbool(*func_set_int32)(const char*, const char*, const char*, int32_t)`
- L139: `typedefbool(*func_set_int64)(const char*, const char*, const char*, int64_t)`
- L149: `typedefbool(*func_set_uint32)(const char*, const char*, const char*, uint32_t)`
- L159: `typedefbool(*func_set_uint64)(const char*, const char*, const char*, uint64_t)`
- L169: `typedefbool(*func_set_double)(const char*, const char*, const char*, double)`
- L188: `typedefint32_t(*func_get_int32)(const char*, const char*, const char*, int32_t)`
- L198: `typedefint64_t(*func_get_int64)(const char*, const char*, const char*, int64_t)`
- L208: `typedefuint32_t(*func_get_uint32)(const char*, const char*, const char*, uint32_t)`
- L218: `typedefuint64_t(*func_get_uint64)(const char*, const char*, const char*, uint64_t)`
- L228: `typedefdouble(*func_get_double)(const char*, const char*, const char*, double)`
- L277: `voidset_engine(WtUftEngine* engine) _engine = engine; }`
- L287: `boolinitialize(const char* module)`
- L297: `boolstart_watching(uint32_t microsecs)`
- L307: `boolinit_domain(const char* id)`
- L317: `boolcommit_param_watcher(const char* section)`
- L329: `boolset_value(const char* section, const char* key, const char* val)`
- L341: `boolset_value(const char* section, const char* key, int32_t val)`
- L353: `boolset_value(const char* section, const char* key, int64_t val)`
- L365: `boolset_value(const char* section, const char* key, uint32_t val)`
- L377: `boolset_value(const char* section, const char* key, uint64_t val)`
- L389: `boolset_value(const char* section, const char* key, double val)`
- L401: `const char*get_value(const char* section, const char* key, const char* defVal = "")`
- L413: `int32_tget_value(const char* section, const char* key, int32_t defVal = 0)`
- L425: `int64_tget_value(const char* section, const char* key, int64_t defVal = 0)`
- L437: `uint32_tget_value(const char* section, const char* key, uint32_t defVal = 0)`
- L449: `uint64_tget_value(const char* section, const char* key, uint64_t defVal = 0)`
- L461: `doubleget_value(const char* section, const char* key, double defVal = 0)`
- L475: `const char*allocate_value(const char* section, const char* key, const char* initVal = "", bool bForceWrite = false, bool isExchg = false)`
- L489: `int32_t*allocate_value(const char* section, const char* key, int32_t initVal = 0, bool bForceWrite = false, bool isExchg = false)`
- L503: `int64_t*allocate_value(const char* section, const char* key, int64_t initVal = 0, bool bForceWrite = false, bool isExchg = false)`
- L517: `uint32_t*allocate_value(const char* section, const char* key, uint32_t initVal = 0, bool bForceWrite = false, bool isExchg = false)`
- L531: `uint64_t*allocate_value(const char* section, const char* key, uint64_t initVal = 0, bool bForceWrite = false, bool isExchg = false)`
- L545: `double*allocate_value(const char* section, const char* key, double initVal = 0, bool bForceWrite = false, bool isExchg = false)`

### `src/WtUftCore/TraderAdapter.cpp` (2037 行)
- L291: `SpinLocklock(_mtx_orders)`
- L383: `SpinLocklock(_mtx_orders)`
- L1158: `WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,err->getMessage())`
- L1375: `SpinLocklock(_mtx_orders)`
- L1707: `SpinLocklock(_mtx_orders)`
- L1854: `WTSLogger::log_dyn_raw("trader", _id.c_str(), ll, message)`

### `src/WtUftCore/TraderAdapter.h` (290 行)
- 类型: WTSVariant:19, WTSContractInfo:20, WTSCommodityInfo:21, ITrdNotifySink:22, ActionPolicyMgr:23, TraderAdapter:28, TraderAdapterMgr:273
- L32: `virtual ~TraderAdapter()`
- L100: `boolinit(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)`
- L102: `boolinitExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)`
- L104: `voidrelease()`
- L106: `boolrun()`
- L108: `inline const char*id() }`
- L110: `AdapterStatestate() const{ return _state; }`
- L118: `uint32_tdoEntrust(WTSEntrust* entrust)`
- L119: `booldoCancel(WTSOrderInfo* ordInfo)`
- L121: `inline voidprintPosition(const char* stdCode, const PosItem& pItem)`
- L123: `inline WTSContractInfo*getContract(const char* stdCode)`
- L125: `inline voidupdateUndone(const char* stdCode, double qty)`
- L127: `const RiskParams*getRiskParams(const char* stdCode)`
- L130: `doublegetPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3)`
- L131: `doubleenumPosition(const char* stdCode = "")`
- L132: `OrderMap*getOrders(const char* stdCode)`
- L142: `uint32_tgetInfos(const char* stdCode)`
- L144: `OrderIDsbuy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL)`
- L145: `OrderIDssell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL)`
- L155: `uint32_topenLong(const char* stdCode, double price, double qty, int flag)`
- L165: `uint32_topenShort(const char* stdCode, double price, double qty, int flag)`
- L176: `uint32_tcloseLong(const char* stdCode, double price, double qty, bool isToday, int flag)`
- L187: `uint32_tcloseShort(const char* stdCode, double price, double qty, bool isToday, int flag)`
- L189: `boolcancel(uint32_t localid)`
- L190: `OrderIDscancelAll(const char* stdCode)`
- L192: `inline boolisTradeEnabled(const char* stdCode) const`
- L194: `boolcheckCancelLimits(const char* stdCode)`
- L195: `boolcheckOrderLimits(const char* stdCode)`
- L200: `virtual voidhandleEvent(WTSTraderEvent e, int32_t ec) override`
- L202: `virtual voidonLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override`
- L204: `virtual voidonLogout() override`
- L206: `virtual voidonRspEntrust(WTSEntrust* entrust, WTSError *err) override`
- L208: `virtual voidonRspAccount(WTSArray* ayAccounts) override`
- L210: `virtual voidonRspPosition(const WTSArray* ayPositions) override`
- L212: `virtual voidonRspOrders(const WTSArray* ayOrders) override`
- L214: `virtual voidonRspTrades(const WTSArray* ayTrades) override`
- L216: `virtual voidonPushOrder(WTSOrderInfo* orderInfo) override`
- L218: `virtual voidonPushTrade(WTSTradeInfo* tradeRecord) override`
- L220: `virtual voidonTraderError(WTSError* err, void* pData = NULL) override`
- L222: `virtual IBaseDataMgr*getBaseDataMgr() override`
- L224: `virtual voidhandleTraderLog(WTSLogLevel ll, const char* message) override`
- L276: `voidrelease()`
- L278: `voidrun()`
- L280: `const TraderAdapterMap&getAdapters() const { return _adapters; }`
- L282: `TraderAdapterPtrgetAdapter(const char* tname)`
- L284: `booladdAdapter(const char* tname, TraderAdapterPtr& adapter)`

### `src/WtUftCore/UftStraContext.cpp` (1359 行)
- L179: `SpinLocklock(_rnd_blk._mutex)`
- L200: `SpinLocklock(_trd_blk._mutex)`
- L225: `SpinLocklock(_pos_blk._mutex)`
- L249: `SpinLocklock(_trd_blk._mutex)`
- L299: `SpinLocklock(_rnd_blk._mutex)`
- L320: `SpinLocklock(_trd_blk._mutex)`
- L345: `SpinLocklock(_pos_blk._mutex)`
- L369: `SpinLocklock(_trd_blk._mutex)`
- L392: `SpinLocklock(_pos_blk._mutex)`
- L414: `SpinLocklock(_trd_blk._mutex)`
- L451: `SpinLocklock(_rnd_blk._mutex)`
- L474: `SpinLocklock(_trd_blk._mutex)`
- L496: `SpinLocklock(_pos_blk._mutex)`
- L518: `SpinLocklock(_trd_blk._mutex)`
- L552: `SpinLocklock(_rnd_blk._mutex)`
- L575: `SpinLocklock(_trd_blk._mutex)`
- L606: `SpinLocklock(_ord_blk._mutex)`
- L731: `ShareManager::self().commit_param_watcher(_name.c_str())`
- L1018: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L1023: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L1028: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L1038: `fs::create_directories(folder.c_str())`
- L1070: `SpinLocklock(_pos_blk._mutex)`
- L1130: `SpinLocklock(_pos_blk._mutex)`
- L1218: `SpinLocklock(_ord_blk._mutex)`
- L1266: `SpinLocklock(_trd_blk._mutex)`
- L1314: `SpinLocklock(_rnd_blk._mutex)`

### `src/WtUftCore/UftStraContext.h` (890 行)
- 类型: UftStrategy:21, WtUftEngine:24, TraderAdapter:25, UftStraContext:32
- L45: `virtual ~UftStraContext()`
- L51: `voidset_strategy(UftStrategy* stra) _strategy = stra; }`
- L57: `UftStrategy*get_stragety() return _strategy; }`
- L63: `voidsetTrader(TraderAdapter* trader)`
- L70: `virtual uint32_tid() override { return _context_id; }`
- L76: `virtual voidon_init() override`
- L83: `virtual voidon_tick(const char* code, WTSTickData* newTick) override`
- L90: `virtual voidon_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override`
- L97: `virtual voidon_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override`
- L104: `virtual voidon_transaction(const char* stdCode, WTSTransData* newTrans) override`
- L113: `virtual voidon_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L124: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price) override`
- L137: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled = false) override`
- L143: `virtual voidon_channel_ready(uint32_t tradingday) override`
- L148: `virtual voidon_channel_lost() override`
- L157: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L169: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`
- L175: `virtual voidon_session_begin(uint32_t uTDate) override`
- L181: `virtual voidon_session_end(uint32_t uTDate) override`
- L186: `virtual voidon_params_updated() override`
- L203: `virtual const char*watch_param(const char* name, const char* initVal = "") override`
- L211: `virtual doublewatch_param(const char* name, double initVal = 0) override`
- L219: `virtual uint32_twatch_param(const char* name, uint32_t initVal = 0) override`
- L227: `virtual uint64_twatch_param(const char* name, uint64_t initVal = 0) override`
- L235: `virtual int32_twatch_param(const char* name, int32_t initVal = 0) override`
- L243: `virtual int64_twatch_param(const char* name, int64_t initVal = 0) override`
- L249: `virtual voidcommit_param_watcher() override`
- L257: `virtual const char*read_param(const char* name, const char* defVal = "") override`
- L265: `virtual doubleread_param(const char* name, double defVal = 0) override`
- L273: `virtual uint32_tread_param(const char* name, uint32_t defVal = 0) override`
- L281: `virtual uint64_tread_param(const char* name, uint64_t defVal = 0) override`
- L289: `virtual int32_tread_param(const char* name, int32_t defVal = 0) override`
- L297: `virtual int64_tread_param(const char* name, int64_t defVal = 0) override`
- L306: `virtual const char*sync_param(const char* name, const char* initVal = "", bool bForceWrite = false) override`
- L315: `virtual double*sync_param(const char* name, double initVal = 0, bool bForceWrite = false) override`
- L324: `virtual uint32_t*sync_param(const char* name, uint32_t initVal = 0, bool bForceWrite = false) override`
- L333: `virtual uint64_t*sync_param(const char* name, uint64_t initVal = 0, bool bForceWrite = false) override`
- L342: `virtual int32_t*sync_param(const char* name, int32_t initVal = 0, bool bForceWrite = false) override`
- L351: `virtual int64_t*sync_param(const char* name, int64_t initVal = 0, bool bForceWrite = false) override`
- L360: `virtual uint32_tstra_get_date() override`
- L366: `virtual uint32_tstra_get_time() override`
- L372: `virtual uint32_tstra_get_secs() override`
- L379: `virtual boolstra_cancel(uint32_t localid) override`
- L386: `virtual OrderIDsstra_cancel_all(const char* stdCode) override`
- L404: `virtual OrderIDsstra_buy(const char* stdCode, double price, double qty, int flag = 0) override`
- L422: `virtual OrderIDsstra_sell(const char* stdCode, double price, double qty, int flag = 0) override`
- L440: `virtual uint32_tstra_enter_long(const char* stdCode, double price, double qty, int flag = 0) override`
- L458: `virtual uint32_tstra_enter_short(const char* stdCode, double price, double qty, int flag = 0) override`
- L478: `virtual uint32_tstra_exit_long(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override`
- L498: `virtual uint32_tstra_exit_short(const char* stdCode, double price, double qty, bool isToday = false, int flag = 0) override`
- L505: `virtual WTSCommodityInfo*stra_get_comminfo(const char* stdCode) override`
- L514: `virtual WTSKlineSlice*stra_get_bars(const char* stdCode, const char* period, uint32_t count) override`
- L522: `virtual WTSTickSlice*stra_get_ticks(const char* stdCode, uint32_t count) override`
- L530: `virtual WTSOrdDtlSlice*stra_get_order_detail(const char* stdCode, uint32_t count) override`
- L538: `virtual WTSOrdQueSlice*stra_get_order_queue(const char* stdCode, uint32_t count) override`
- L546: `virtual WTSTransSlice*stra_get_transaction(const char* stdCode, uint32_t count) override`
- L553: `virtual WTSTickData*stra_get_last_tick(const char* stdCode) override`
- L559: `virtual voidstra_log_info(const char* message) override`
- L565: `virtual voidstra_log_debug(const char* message) override`
- L571: `virtual voidstra_log_error(const char* message) override`
- L580: `virtual doublestra_get_position(const char* stdCode, bool bOnlyValid = false, int32_t iFlag = 3) override`
- L587: `virtual doublestra_get_local_position(const char* stdCode) override`
- L594: `virtual doublestra_get_local_posprofit(const char* stdCode) override`
- L601: `virtual doublestra_get_local_closeprofit(const char* stdCode) override`
- L608: `virtual doublestra_enum_position(const char* stdCode) override`
- L615: `virtual doublestra_get_price(const char* stdCode) override`
- L622: `virtual doublestra_get_undone(const char* stdCode) override`
- L629: `virtual uint32_tstra_get_infos(const char* stdCode) override`
- L635: `virtual voidstra_sub_ticks(const char* stdCode) override`
- L641: `virtual voidstra_sub_order_details(const char* stdCode) override`
- （另有 3 个声明，按需用 Read 定位）

### `src/WtUftCore/UftStrategyMgr.cpp` (132 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L101: `UftStrategyPtrret(new UftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L120: `UftStrategyPtrret(new UftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

### `src/WtUftCore/UftStrategyMgr.h` (163 行)
- 类型: UftStraWrapper:24, UftStrategyMgr:68
- L50: `UftStrategy*self() return _stra; }`
- L87: `boolloadFactories(const char* path)`
- L95: `UftStrategyPtrcreateStrategy(const char* name, const char* id)`
- L104: `UftStrategyPtrcreateStrategy(const char* factname, const char* unitname, const char* id)`
- L111: `UftStrategyPtrgetStrategy(const char* id)`

### `src/WtUftCore/WtHelper.cpp` (93 行)
- L59: `fs::create_directories(folder)`
- L67: `fs::create_directories(folder)`
- L75: `fs::create_directories(folder)`
- L83: `fs::create_directories(folder)`
- L91: `fs::create_directories(folder)`

### `src/WtUftCore/WtHelper.h` (133 行)
- 类型: WtHelper:19
- L26: `static std::stringgetCWD()`
- L35: `static std::stringgetModulePath(const char* moduleName, const char* subDir, bool isCWD = true)`
- L41: `static const char*getBaseDir()`
- L46: `static const char*getOutputDir()`
- L51: `static const char*getStraDataDir()`
- L56: `static const char*getStraUsrDatDir()`
- L62: `static const char*getPortifolioDir()`
- L81: `static inline voidsetTDate(uint32_t tDate) _cur_tdate = tDate; }`
- L87: `static inline uint32_tgetDate() return _cur_date; }`
- L93: `static inline uint32_tgetTime() return _cur_time; }`
- L99: `static inline uint32_tgetSecs() return _cur_secs; }`
- L105: `static inline uint32_tgetTradingDate() return _cur_tdate; }`
- L111: `static const std::string&getInstDir() return _inst_dir; }`
- L117: `static voidsetInstDir(const char* inst_dir) _inst_dir = inst_dir; }`
- L123: `static voidsetGenerateDir(const char* gen_dir) _gen_dir = gen_dir; }`

### `src/WtUftCore/WtUftDtMgr.h` (149 行)
- 类型: WTSVariant:20, WTSTickData:21, WTSKlineSlice:22, WTSTickSlice:23, IBaseDataMgr:24, IBaseDataMgr:25, WtUftEngine:26, WtUftDtMgr:33
- L55: `boolinit(WTSVariant* cfg, WtUftEngine* engine)`
- L62: `voidhandle_push_quote(const char* stdCode, WTSTickData* newTick)`
- L73: `virtual WTSTickSlice*get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L82: `virtual WTSOrdQueSlice*get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L91: `virtual WTSOrdDtlSlice*get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L100: `virtual WTSTransSlice*get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L111: `virtual WTSKlineSlice*get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override`
- L118: `virtual WTSTickData*grab_last_tick(const char* stdCode) override`

### `src/WtUftCore/WtUftEngine.cpp` (687 行)
- L41: `TimeUtils::getDateTime(_cur_date, _cur_time)`
- L50: `WtHelper::setTime(_cur_date, _cur_time, _cur_secs)`
- L95: `WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs)`
- L109: `WtHelper::setTDate(curTDate)`

### `src/WtUftCore/WtUftEngine.h` (159 行)
- 类型: WTSSessionInfo:27, WTSCommodityInfo:28, WTSContractInfo:29, IBaseDataMgr:31, IHotMgr:32, WTSVariant:34, WTSTickData:36, WTSBarStruct:37, WTSTickSlice:38, WTSKlineSlice:39, WTSPortFundInfo:40, WtUftDtMgr:42, TraderAdapterMgr:43, EventNotifier:45, WTSVariant:50, WtUftRtTicker:51, EventNotifier:52, WtUftEngine:56
- L60: `virtual ~WtUftEngine()`
- L63: `inline voidset_adapter_mgr(TraderAdapterMgr* mgr) _adapter_mgr = mgr; }`
- L65: `voidset_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs = 0, uint32_t rawTime = 0)`
- L67: `voidset_trading_date(uint32_t curTDate)`
- L69: `inline uint32_tget_date() return _cur_date; }`
- L70: `inline uint32_tget_min_time() return _cur_time; }`
- L71: `inline uint32_tget_raw_time() return _cur_raw_time; }`
- L72: `inline uint32_tget_secs() return _cur_secs; }`
- L73: `inline uint32_tget_trading_date() return _cur_tdate; }`
- L75: `inline IBaseDataMgr*get_basedata_mgr() return _base_data_mgr; }`
- L76: `WTSSessionInfo*get_session_info(const char* sid, bool isCode = false)`
- L77: `WTSCommodityInfo*get_commodity_info(const char* stdCode)`
- L78: `WTSContractInfo*get_contract_info(const char* stdCode)`
- L80: `WTSTickData*get_last_tick(uint32_t sid, const char* stdCode)`
- L81: `WTSTickSlice*get_tick_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L82: `WTSKlineSlice*get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times = 1, uint64_t etime = 0)`
- L84: `voidsub_tick(uint32_t sid, const char* code)`
- L86: `doubleget_cur_price(const char* stdCode)`
- L88: `voidnotify_params_update(const char* name)`
- L91: `voidinit(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtUftDtMgr* dataMgr, EventNotifier* notifier)`
- L93: `voidrun()`
- L95: `voidon_tick(const char* stdCode, WTSTickData* curTick)`
- L97: `voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)`
- L99: `voidon_init() }`
- L101: `voidon_session_begin()`
- L103: `voidon_session_end()`
- L105: `virtual voidhandle_push_quote(WTSTickData* newTick) override`
- L106: `virtual voidhandle_push_order_detail(WTSOrdDtlData* curOrdDtl) override`
- L107: `virtual voidhandle_push_order_queue(WTSOrdQueData* curOrdQue) override`
- L108: `virtual voidhandle_push_transaction(WTSTransData* curTrans) override`
- L111: `WTSOrdQueSlice*get_order_queue_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L112: `WTSOrdDtlSlice*get_order_detail_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L113: `WTSTransSlice*get_transaction_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L116: `voidon_minute_end(uint32_t curDate, uint32_t curTime)`
- L118: `voidaddContext(UftContextPtr ctx)`
- L120: `UftContextPtrgetContext(uint32_t id)`
- L122: `voidsub_order_queue(uint32_t sid, const char* stdCode)`
- L123: `voidsub_order_detail(uint32_t sid, const char* stdCode)`
- L124: `voidsub_transaction(uint32_t sid, const char* stdCode)`

### `src/WtUftCore/WtUftTicker.cpp` (264 行)
- L60: `TimeUtils::getDateTime(_date, _time)`
- L125: `StdUniqueLocklock(_mtx)`
- L198: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L205: `StdUniqueLocklock(_mtx)`
- L246: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtUftCore/WtUftTicker.h` (84 行)
- 类型: WTSSessionInfo:20, WTSTickData:21, WtUftEngine:23, WtUftRtTicker:30
- L49: `voidinit(const char* sessionID)`
- L55: `voidon_tick(WTSTickData* curTick)`
- L60: `voidrun()`
- L65: `voidstop()`

