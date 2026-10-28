# API 简报 L05

覆盖模块: WtDtCore, WtDtHelper, WtDtPorter, WtDtServo。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WtDtCore`

### `src/WtDtCore/DataManager.cpp` (324 行)
- L84: `WTSLogger::error("Initializing of data writer failed: function createWriter not found...")`
- L90: `WTSLogger::error("Initializing of data writer failed: function deleteWriter not found...")`
- L321: `WTSLogger::log_raw(ll, message)`

### `src/WtDtCore/DataManager.h` (250 行)
- 类型: WTSTickData:19, WTSOrdQueData:20, WTSOrdDtlData:21, WTSTransData:22, WTSVariant:23, IDataCaster:24, WTSBaseDataMgr:29, StateMonitor:30, UDPCaster:31, DataManager:39
- L63: `boolinit(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor)`
- L71: `voidadd_ext_dumper(const char* id, IHisDataDumper* dumper)`
- L90: `voidrelease()`
- L99: `boolwriteTick(WTSTickData* curTick, uint32_t procFlag)`
- L107: `boolwriteOrderQueue(WTSOrdQueData* curOrdQue)`
- L115: `boolwriteOrderDetail(WTSOrdDtlData* curOrdDetail)`
- L123: `boolwriteTransaction(WTSTransData* curTrans)`
- L130: `voidtransHisData(const char* sid)`
- L138: `boolisSessionProceeded(const char* sid)`
- L147: `WTSTickData*getCurTick(const char* code, const char* exchg = "")`
- L157: `virtual IBaseDataMgr*getBDMgr() override`
- L165: `virtual boolcanSessionReceive(const char* sid) override`
- L172: `virtual voidbroadcastTick(WTSTickData* curTick) override`
- L179: `virtual voidbroadcastOrdQue(WTSOrdQueData* curOrdQue) override`
- L186: `virtual voidbroadcastOrdDtl(WTSOrdDtlData* curOrdDtl) override`
- L193: `virtual voidbroadcastTrans(WTSTransData* curTrans) override`
- L201: `virtual CodeSet*getSessionComms(const char* sid) override`
- L209: `virtual uint32_tgetTradingDate(const char* pid) override`
- L217: `virtual voidoutputLog(WTSLogLevel ll, const char* message) override`

### `src/WtDtCore/IDataCaster.h` (55 行)
- 类型: WTSTickData:15, WTSVariant:16, WTSOrdDtlData:17, WTSOrdQueData:18, WTSTransData:19, IDataCaster:26
- L34: `virtual voidbroadcast(WTSTickData* curTick) = 0`
- L40: `virtual voidbroadcast(WTSOrdQueData* /*curOrdQue*/) }`
- L46: `virtual voidbroadcast(WTSOrdDtlData* /*curOrdDtl*/) }`
- L52: `virtual voidbroadcast(WTSTransData* /*curTrans*/) }`

### `src/WtDtCore/IndexWorkerMgr.cpp` (226 行)
- L44: `fs::pathmyPath(path)`
- L69: `DLLHelper::free_library(hInst)`
- L89: `DLLHelper::free_library(hInst)`
- L124: `IndexWorkerPtrret(new IdxWorkerWrapper(worker, fInfo._fact))`
- L225: `WTSLogger::log_raw(ll, message)`

### `src/WtDtCore/IndexWorkerMgr.h` (95 行)
- 类型: DataManager:21, IdxWorkerWrapper:23, IndexWorkerMgr:45
- L35: `IIndexWorker*self() return _worker; }`
- L52: `boolloadFactories(const char* path)`
- L55: `IndexWorkerPtrcreateWorker(const char* name, const char* id)`
- L58: `boolinit(WTSVariant* config, IHotMgr* hotMgr, IBaseDataMgr* bdMgr, DataManager* dataMgr)`
- L59: `voidhandle_quote(WTSTickData* newTick)`
- L62: `virtual IHotMgr*get_hot_mgr() override { return _hot_mgr; }`
- L63: `virtual IBaseDataMgr*get_bd_mgr() override { return _bd_mgr; }`
- L65: `virtual WTSTickData*sub_ticks(const char* fullCode) override`
- L66: `virtual WTSTickData*get_tick(const char* code, const char* exchg) override`
- L67: `virtual voidpush_tick(WTSTickData* newTick) override`
- L68: `virtual voidoutput_log(WTSLogLevel ll, const char* message) override`

### `src/WtDtCore/ParserAdapter.cpp` (373 行)
- L316: `WTSLogger::log_raw_by_cat("parser", ll, message)`

### `src/WtDtCore/ParserAdapter.h` (94 行)
- 类型: WTSVariant:18, wxMainFrame:22, WTSBaseDataMgr:23, DataManager:24, IndexWorkerMgr:25, ParserAdapter:27, ParserAdapterMgr:77
- L31: `virtual ~ParserAdapter()`
- L34: `boolinit(const char* id, WTSVariant* cfg)`
- L36: `boolinitExt(const char* id, IParserApi* api)`
- L38: `voidrelease()`
- L40: `boolrun()`
- L42: `const char*id() }`
- L45: `virtual voidhandleSymbolList(const WTSArray* aySymbols) override`
- L47: `virtual voidhandleQuote(WTSTickData *quote, uint32_t procFlag) override`
- L49: `virtual voidhandleOrderQueue(WTSOrdQueData* ordQueData) override`
- L51: `virtual voidhandleTransaction(WTSTransData* transData) override`
- L53: `virtual voidhandleOrderDetail(WTSOrdDtlData* ordDetailData) override`
- L55: `virtual voidhandleParserLog(WTSLogLevel ll, const char* message) override`
- L57: `virtual IBaseDataMgr*getBaseDataMgr() override`
- L80: `voidrelease()`
- L82: `voidrun()`
- L84: `ParserAdapterPtrgetAdapter(const char* id)`
- L86: `booladdAdapter(const char* id, ParserAdapterPtr& adapter)`
- L88: `uint32_tsize() }`

### `src/WtDtCore/ShmCaster.h` (145 行)
- 类型: WTSVariant:18, ShmCaster:28, _DataQueue:66
- L106: `boolinit(WTSVariant* cfg)`
- L113: `virtual voidbroadcast(WTSTickData* curTick) override`
- L119: `virtual voidbroadcast(WTSOrdQueData* curOrdQue) override`
- L125: `virtual voidbroadcast(WTSOrdDtlData* curOrdDtl) override`
- L131: `virtual voidbroadcast(WTSTransData* curTrans) override`

### `src/WtDtCore/StatHelper.hpp` (126 行)
- 类型: StatHelper:22
- L91: `BoostWriteLocklock(_mutexes[sType]) // 获取写锁，确保线程安全`
- L118: `BoostReadLocklock(_mutexes[sType]) // 获取读锁，确保线程安全`

### `src/WtDtCore/StateMonitor.cpp` (543 行)
- L52: `WTSLogger::error("Loading state config failed")`
- L68: `StatePtrstateInfo(new StateInfo)`
- L168: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`

### `src/WtDtCore/StateMonitor.h` (232 行)
- 类型: WTSSessionInfo:20, WTSBaseDataMgr:114, DataManager:119, StateMonitor:127
- L150: `boolinitialize(const char* filename, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)`
- L156: `voidrun()`
- L162: `voidstop()`

### `src/WtDtCore/UDPCaster.cpp` (417 行)
- 类型: UDPDataPacket:38
- L106: `boost::asio::socket_base::broadcastoption(true)`
- L227: `UDPReceiverPtritem(new UDPReceiver(EndPoint(addr, port), type))`
- L249: `UDPReceiverPtritem(new UDPReceiver(EndPoint(addr, port), type))`
- L250: `UDPSocketPtrsock(new UDPSocket(m_ioservice, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), sendport)))`
- L251: `boost::asio::ip::multicast::join_groupoption(item->_ep.address())`
- L294: `StdUniqueLocklock(m_mtxCast)`
- L306: `StdUniqueLocklock(m_mtxCast)`
- L313: `StdUniqueLocklock(m_mtxCast)`

### `src/WtDtCore/UDPCaster.h` (137 行)
- 类型: WTSVariant:19, WTSBaseDataMgr:24, DataManager:25, UDPCaster:27
- L51: `voidhandle_send_broad(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)`
- L52: `voidhandle_send_multi(const EndPoint& ep, const boost::system::error_code& error, std::size_t bytes_transferred)`
- L54: `voiddo_receive()`
- L56: `voiddo_broadcast(WTSObject* data, uint32_t dataType)`
- L59: `boolinit(WTSVariant* cfg, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)`
- L60: `voidstart(int bport)`
- L61: `voidstop()`
- L63: `booladdBRecver(const char* remote, int port, int type = 0)`
- L64: `booladdMRecver(const char* remote, int port, int sendport, int type = 0)`
- L67: `virtual voidbroadcast(WTSTickData* curTick) override`
- L68: `virtual voidbroadcast(WTSOrdQueData* curOrdQue) override`
- L69: `virtual voidbroadcast(WTSOrdDtlData* curOrdDtl) override`
- L70: `virtual voidbroadcast(WTSTransData* curTrans) override`

### `src/WtDtCore/WtHelper.h` (48 行)
- 类型: WtHelper:21
- L29: `static const char*get_cwd()`
- L36: `static const char*get_module_dir() }`
- L43: `static voidset_module_dir(const char* mod_dir) _bin_dir = mod_dir; }`

## `WtDtHelper`

### `src/WtDtHelper/WtDtHelper.cpp` (1247 行)
- L176: `fs::create_directories(csvFolder)`
- L178: `fs::pathmyPath(srcFolder)`
- L196: `StdFile::read_file_content(path.c_str(), buffer)`
- L260: `StdFile::write_file_content(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size())`
- L281: `fs::create_directories(csvFolder)`
- L283: `fs::pathmyPath(srcFolder)`
- L301: `StdFile::read_file_content(path.c_str(), buffer)`
- L366: `StdFile::write_file_content(filename.c_str(), ss.str().c_str(), (uint32_t)ss.str().size())`
- L382: `fs::create_directories(binFolder)`
- L392: `fs::pathmyPath(csvFolder)`
- L407: `CsvReaderreader(",")`
- L603: `StdFile::read_file_content(path.c_str(), content)`
- L638: `StdFile::read_file_content(path.c_str(), content)`
- L673: `StdFile::read_file_content(path.c_str(), content)`
- L708: `StdFile::read_file_content(path.c_str(), content)`
- L742: `StdFile::read_file_content(path.c_str(), content)`
- L774: `StdFile::read_file_content(path.c_str(), buffer)`
- L807: `StdFile::read_file_content(path.c_str(), buffer)`
- L889: `std::swap(fromTime, endTime)`
- L931: `StdFile::read_file_content(path.c_str(), buffer)`

### `src/WtDtHelper/WtDtHelper.h` (299 行)
- 类型: WTSBarStruct:20, WTSTickStruct:21, WTSOrdDtlStruct:22, WTSOrdQueStruct:23, WTSTransStruct:24
- L34: `typedefvoid(PORTER_FLAG *FuncLogCallback)(WtString message)`
- L43: `typedefvoid(PORTER_FLAG *FuncGetBarsCallback)(WTSBarStruct* bar, WtUInt32 count, bool isLast)`
- L52: `typedefvoid(PORTER_FLAG *FuncGetTicksCallback)(WTSTickStruct* tick, WtUInt32 count, bool isLast)`
- L61: `typedefvoid(PORTER_FLAG *FuncGetOrdDtlCallback)(WTSOrdDtlStruct* item, WtUInt32 count, bool isLast)`
- L70: `typedefvoid(PORTER_FLAG *FuncGetOrdQueCallback)(WTSOrdQueStruct* item, WtUInt32 count, bool isLast)`
- L79: `typedefvoid(PORTER_FLAG *FuncGetTransCallback)(WTSTransStruct* item, WtUInt32 count, bool isLast)`
- L86: `typedefvoid(PORTER_FLAG *FuncCountDataCallback)(WtUInt32 dataCnt)`
- L105: `EXPORT_FLAG voiddump_bars(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL)`
- L116: `EXPORT_FLAG voiddump_ticks(WtString binFolder, WtString csvFolder, WtString strFilter = "", FuncLogCallback cbLogger = NULL)`
- L127: `EXPORT_FLAG voidtrans_csv_bars(WtString csvFolder, WtString binFolder, WtString period, FuncLogCallback cbLogger = NULL)`
- L139: `EXPORT_FLAG WtUInt32read_dsb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L151: `EXPORT_FLAG WtUInt32read_dsb_order_details(WtString dataFile, FuncGetOrdDtlCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L163: `EXPORT_FLAG WtUInt32read_dsb_order_queues(WtString dataFile, FuncGetOrdQueCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L175: `EXPORT_FLAG WtUInt32read_dsb_transactions(WtString dataFile, FuncGetTransCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L187: `EXPORT_FLAG WtUInt32read_dsb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L200: `EXPORT_FLAG WtUInt32read_dmb_ticks(WtString tickFile, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L213: `EXPORT_FLAG WtUInt32read_dmb_bars(WtString barFile, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt, FuncLogCallback cbLogger = NULL)`
- L229: `EXPORT_FLAG boolstore_bars(WtString barFile, WTSBarStruct* firstBar, int count, WtString period, FuncLogCallback cbLogger = NULL)`
- L241: `EXPORT_FLAG boolstore_ticks(WtString tickFile, WTSTickStruct* firstTick, int count, FuncLogCallback cbLogger = NULL)`
- L253: `EXPORT_FLAG boolstore_order_details(WtString tickFile, WTSOrdDtlStruct* firstItem, int count, FuncLogCallback cbLogger = NULL)`
- L265: `EXPORT_FLAG boolstore_order_queues(WtString tickFile, WTSOrdQueStruct* firstItem, int count, FuncLogCallback cbLogger = NULL)`
- L277: `EXPORT_FLAG boolstore_transactions(WtString tickFile, WTSTransStruct* firstItem, int count, FuncLogCallback cbLogger = NULL)`

## `WtDtPorter`

### `src/WtDtPorter/ExpDumper.cpp` (95 行)
- L20: `extern WtDtRunner&getRunner()`

### `src/WtDtPorter/ExpDumper.h` (98 行)
- 类型: ExpDumper:23
- L37: `virtual ~ExpDumper() }`
- L49: `virtual booldumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) override`
- L60: `virtual booldumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) override`
- L71: `virtual booldumpHisOrdQue(const char* stdCode, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count) override`
- L82: `virtual booldumpHisOrdDtl(const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count) override`
- L93: `virtual booldumpHisTrans(const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count) override`

### `src/WtDtPorter/ExpParser.cpp` (107 行)
- L21: `extern WtDtRunner&getRunner()`

### `src/WtDtPorter/ExpParser.h` (101 行)
- 类型: ExpParser:22
- L36: `virtual ~ExpParser() }`
- L45: `virtual boolinit(WTSVariant* config) override`
- L51: `virtual voidrelease() override`
- L58: `virtual boolconnect() override`
- L65: `virtual booldisconnect() override`
- L73: `virtual boolisConnected() override { return true; }`
- L80: `virtual voidsubscribe(const CodeSet& setCodes) override`
- L87: `virtual voidunsubscribe(const CodeSet& setCodes) override`
- L94: `virtual voidregisterSpi(IParserSpi* listener) override`

### `src/WtDtPorter/PorterDefs.h` (169 行)
- 类型: WTSTickStruct:25, WTSBarStruct:31, WTSOrdDtlStruct:37, WTSOrdQueStruct:43, WTSTransStruct:49
- L93: `typedefvoid(PORTER_FLAG *FuncParserEvtCallback)(WtUInt32 evtId, const char* id)`
- L102: `typedefvoid(PORTER_FLAG *FuncParserSubCallback)(const char* id, const char* fullCode, bool isForSub)`
- L121: `typedefbool(PORTER_FLAG *FuncDumpBars)(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, WtUInt32 count)`
- L133: `typedefbool(PORTER_FLAG *FuncDumpTicks)(const char* id, const char* stdCode, WtUInt32 uDate, WTSTickStruct* ticks, WtUInt32 count)`
- L145: `typedefbool(PORTER_FLAG *FuncDumpOrdQue)(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdQueStruct* items, WtUInt32 count)`
- L157: `typedefbool(PORTER_FLAG *FuncDumpOrdDtl)(const char* id, const char* stdCode, WtUInt32 uDate, WTSOrdDtlStruct* items, WtUInt32 count)`
- L169: `typedefbool(PORTER_FLAG *FuncDumpTrans)(const char* id, const char* stdCode, WtUInt32 uDate, WTSTransStruct* items, WtUInt32 count)`

### `src/WtDtPorter/WtDtPorter.cpp` (131 行)
- L40: `fs::pathp(MODULE_NAME)`
- L57: `CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd())`
- L87: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L91: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtDtPorter/WtDtPorter.h` (111 行)
- L29: `EXPORT_FLAG voidinitialize(WtString cfgFile, WtString logCfg, bool bCfgFile, bool bLogCfgFile)`
- L36: `EXPORT_FLAG voidstart(bool bAsync = false)`
- L43: `EXPORT_FLAG WtStringget_version()`
- L52: `EXPORT_FLAG voidwrite_log(unsigned int level, const char* message, const char* catName)`
- L62: `EXPORT_FLAG boolcreate_ext_parser(const char* id)`
- L70: `EXPORT_FLAG voidregister_parser_callbacks(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)`
- L79: `EXPORT_FLAG voidparser_push_quote(const char* id, WTSTickStruct* curTick, WtUInt32 uProcFlag)`
- L89: `EXPORT_FLAG boolcreate_ext_dumper(const char* id)`
- L97: `EXPORT_FLAG voidregister_extended_dumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper)`
- L106: `EXPORT_FLAG voidregister_extended_hftdata_dumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper)`

### `src/WtDtPorter/WtDtRunner.cpp` (438 行)
- L52: `WTSLogger::error(message)`
- L60: `boost::asio::post(_async_io, [this, bAlldayMode]()`
- L63: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L71: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L82: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L90: `WTSLogger::init(logCfg, bLogCfgFile)`
- L91: `WtHelper::set_module_dir(modDir)`
- L110: `WTSLogger::info("Trading sessions loaded")`
- L148: `WTSLogger::info("Holidays loaded")`
- L155: `WTSLogger::log_raw(LL_INFO, "Hot rules loaded")`
- L161: `WTSLogger::log_raw(LL_INFO, "Second rules loaded")`
- L196: `WTSLogger::info("datakit will run in allday mode")`
- L252: `WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers")`
- L281: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory))`
- L295: `WTSLogger::info("Callbacks of Extented Parser registration done")`
- L351: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory))`
- L363: `ExpDumperPtrdumper(new ExpDumper(id))`
- L389: `WTSLogger::error("Extended tick dumper not enabled")`
- L400: `WTSLogger::error("Extended bar dumper not enabled")`
- L411: `WTSLogger::error("Extended order detail dumper not enabled")`
- L422: `WTSLogger::error("Extended order queue dumper not enabled")`
- L433: `WTSLogger::error("Extended transaction dumper not enabled")`

### `src/WtDtPorter/WtDtRunner.h` (108 行)
- 类型: WTSVariant:27, WtDtRunner:30
- L37: `voidinitialize(const char* cfgFile, const char* logCfg, const char* modDir = "", bool bCfgFile = true, bool bLogCfgFile = true)`
- L38: `voidstart(bool bAsync = false, bool bAlldayMode = false)`
- L40: `boolcreateExtParser(const char* id)`
- L46: `voidregisterParserPorter(FuncParserEvtCallback cbEvt, FuncParserSubCallback cbSub)`
- L48: `voidparser_init(const char* id)`
- L49: `voidparser_connect(const char* id)`
- L50: `voidparser_release(const char* id)`
- L51: `voidparser_disconnect(const char* id)`
- L52: `voidparser_subscribe(const char* id, const char* code)`
- L53: `voidparser_unsubscribe(const char* id, const char* code)`
- L55: `voidon_ext_parser_quote(const char* id, WTSTickStruct* curTick, uint32_t uProcFlag)`
- L60: `boolcreateExtDumper(const char* id)`
- L62: `voidregisterExtDumper(FuncDumpBars barDumper, FuncDumpTicks tickDumper)`
- L64: `voidregisterExtHftDataDumper(FuncDumpOrdQue ordQueDumper, FuncDumpOrdDtl ordDtlDumper, FuncDumpTrans transDumper)`
- L66: `booldumpHisBars(const char* id, const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count)`
- L68: `booldumpHisTicks(const char* id, const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count)`
- L70: `booldumpHisOrdQue(const char* id, const char* stdCode, uint32_t uDate, WTSOrdQueStruct* item, uint32_t count)`
- L72: `booldumpHisOrdDtl(const char* id, const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count)`
- L74: `booldumpHisTrans(const char* id, const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count)`
- L77: `voidinitDataMgr(WTSVariant* config, bool bAlldayMode = false)`
- L78: `voidinitParsers(WTSVariant* cfg)`

## `WtDtServo`

### `src/WtDtServo/ParserAdapter.cpp` (347 行)
- L290: `WTSLogger::log_raw_by_cat("parser", ll, message)`

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

### `src/WtDtServo/PorterDefs.h` (73 行)
- 类型: WTSBarStruct:20, WTSTickStruct:21
- L35: `typedefvoid(PORTER_FLAG *FuncGetBarsCallback)(WTSBarStruct* bar, WtUInt32 count, bool isLast)`
- L45: `typedefvoid(PORTER_FLAG *FuncGetTicksCallback)(WTSTickStruct* tick, WtUInt32 count, bool isLast)`
- L53: `typedefvoid(PORTER_FLAG *FuncCountDataCallback)(WtUInt32 dataCnt)`
- L62: `typedefvoid(PORTER_FLAG *FuncOnTickCallback)(const char* stdCode, WTSTickStruct* tick)`
- L72: `typedefvoid(PORTER_FLAG *FuncOnBarCallback)(const char* stdCode, const char* period, WTSBarStruct* bar)`

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

### `src/WtDtServo/WtDtServo.cpp` (241 行)
- L41: `fs::pathp(MODULE_NAME)`

### `src/WtDtServo/WtDtServo.h` (175 行)
- L34: `EXPORT_FLAG voidinitialize(WtString cfgFile, bool isFile, WtString logCfg, FuncOnTickCallback cbTick, FuncOnBarCallback cbBar)`
- L42: `EXPORT_FLAG WtStringget_version()`
- L57: `EXPORT_FLAG WtUInt32get_bars_by_range(const char* stdCode, const char* period, WtUInt64 beginTime, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)`
- L71: `EXPORT_FLAG WtUInt32get_ticks_by_range(const char* stdCode, WtUInt64 beginTime, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)`
- L87: `EXPORT_FLAG WtUInt32get_bars_by_count(const char* stdCode, const char* period, WtUInt32 count, WtUInt64 endTime, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)`
- L102: `EXPORT_FLAG WtUInt32get_ticks_by_count(const char* stdCode, WtUInt32 count, WtUInt64 endTime, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)`
- L115: `EXPORT_FLAG WtUInt32get_ticks_by_date(const char* stdCode, WtUInt32 uDate, FuncGetTicksCallback cb, FuncCountDataCallback cbCnt)`
- L129: `EXPORT_FLAG WtUInt32get_sbars_by_date(const char* stdCode, WtUInt32 secs, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)`
- L143: `EXPORT_FLAG WtUInt32get_bars_by_date(const char* stdCode, const char* period, WtUInt32 uDate, FuncGetBarsCallback cb, FuncCountDataCallback cbCnt)`
- L153: `EXPORT_FLAG voidsubscribe_tick(const char* stdCode, bool bReplace)`
- L163: `EXPORT_FLAG voidsubscribe_bar(const char* stdCode, const char* period)`
- L171: `EXPORT_FLAG voidclear_cache()`

### `src/WtDtServo/WtHelper.h` (70 行)
- 类型: WtHelper:24
- L36: `static const char*get_cwd()`
- L47: `static const char*get_module_dir() }`
- L58: `static voidset_module_dir(const char* mod_dir) _bin_dir = mod_dir; }`

