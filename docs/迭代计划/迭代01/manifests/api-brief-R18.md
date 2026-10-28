# API 简报 R18

测试目标: dtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtDtCore/ParserAdapter.cpp` (373 行)
- L316: `WTSLogger::log_raw_by_cat("parser", ll, message)`

### `src/WtDtCore/IndexWorkerMgr.cpp` (226 行)
- L44: `fs::pathmyPath(path)`
- L69: `DLLHelper::free_library(hInst)`
- L89: `DLLHelper::free_library(hInst)`
- L124: `IndexWorkerPtrret(new IdxWorkerWrapper(worker, fInfo._fact))`
- L225: `WTSLogger::log_raw(ll, message)`

### `src/WtDtCore/StateMonitor.h` (232 行)
- 类型: WTSSessionInfo:20, WTSBaseDataMgr:114, DataManager:119, StateMonitor:127
- L150: `boolinitialize(const char* filename, WTSBaseDataMgr* bdMgr, DataManager* dtMgr)`
- L156: `voidrun()`
- L162: `voidstop()`

### `src/WtDtCore/DataManager.cpp` (324 行)
- L84: `WTSLogger::error("Initializing of data writer failed: function createWriter not found...")`
- L90: `WTSLogger::error("Initializing of data writer failed: function deleteWriter not found...")`
- L321: `WTSLogger::log_raw(ll, message)`

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

### `src/WtDtCore/ShmCaster.h` (145 行)
- 类型: WTSVariant:18, ShmCaster:28, _DataQueue:66
- L106: `boolinit(WTSVariant* cfg)`
- L113: `virtual voidbroadcast(WTSTickData* curTick) override`
- L119: `virtual voidbroadcast(WTSOrdQueData* curOrdQue) override`
- L125: `virtual voidbroadcast(WTSOrdDtlData* curOrdDtl) override`
- L131: `virtual voidbroadcast(WTSTransData* curTrans) override`

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

### `src/WtDtCore/WtHelper.h` (48 行)
- 类型: WtHelper:21
- L29: `static const char*get_cwd()`
- L36: `static const char*get_module_dir() }`
- L43: `static voidset_module_dir(const char* mod_dir) _bin_dir = mod_dir; }`

