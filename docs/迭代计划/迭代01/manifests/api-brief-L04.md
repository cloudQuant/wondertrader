# API 简报 L04

覆盖模块: WtDataStorage, WtDataStorageAD。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WtDataStorage`

### `src/WtDataStorage/DataDefine.h` (339 行)
- L83: `inline boolis_old_version() const`
- L91: `inline boolis_compressed() const`
- L112: `inline boolis_old_version() const`
- L120: `inline boolis_compressed() const`

### `src/WtDataStorage/WtBtDtReader.cpp` (277 行)
- L39: `fmt::format_to(buffer, format, args...)`
- L51: `extern boolproc_block_data(std::string& content, bool isBar, bool bKeepHead = true)`
- L88: `extern boolproc_block_data(std::string& content, bool isBar, bool bKeepHead)`
- L151: `StdFile::read_file_content(filename.c_str(), buffer)`
- L181: `StdFile::read_file_content(filename.c_str(), buffer)`
- L211: `StdFile::read_file_content(filename.c_str(), buffer)`
- L241: `StdFile::read_file_content(filename.c_str(), buffer)`
- L271: `StdFile::read_file_content(filename.c_str(), buffer)`

### `src/WtDataStorage/WtBtDtReader.h` (126 行)
- 类型: WTSVariant:25, WTSTickSlice:26, WTSKlineSlice:27, WTSOrdDtlSlice:28, WTSOrdQueSlice:29, WTSTransSlice:30, WTSArray:31, IBaseDataMgr:33, IHotMgr:34, WtBtDtReader:43
- L53: `virtual ~WtBtDtReader()`
- L63: `virtual voidinit(WTSVariant* cfg, IBtDtReaderSink* sink) override`
- L75: `virtual boolread_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer) override`
- L86: `virtual boolread_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override`
- L98: `virtual boolread_raw_order_details(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override`
- L109: `virtual boolread_raw_order_queues(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override`
- L120: `virtual boolread_raw_transactions(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override`

### `src/WtDataStorage/WtDataReader.cpp` (2391 行)
- L153: `IDataReader::init(cfg, sink, loader)`
- L243: `std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right)`
- L311: `std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right)`
- L415: `StdFile::read_file_content(filename.c_str(), tBlkPair._buffer)`
- L557: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L714: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L871: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L1035: `StdFile::read_file_content(filename.c_str(), content)`
- L1166: `StdFile::read_file_content(filename.c_str(), content)`
- L1345: `StdFile::read_file_content(filename.c_str(), content)`
- L1428: `StdFile::read_file_content(filename.c_str(), content)`
- L1668: `StdFile::read_file_content(filename.c_str(), content)`
- L2381: `std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right)`

### `src/WtDataStorage/WtDataReader.h` (260 行)
- 类型: WtDataReader:16
- L20: `virtual ~WtDataReader()`
- L183: `RTKlineBlockPair*getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period)`
- L184: `TickBlockPair*getRTTickBlock(const char* exchg, const char* code)`
- L185: `OrdQueBlockPair*getRTOrdQueBlock(const char* exchg, const char* code)`
- L186: `OrdDtlBlockPair*getRTOrdDtlBlock(const char* exchg, const char* code)`
- L187: `TransBlockPair*getRTTransBlock(const char* exchg, const char* code)`
- L189: `boolcacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)`
- L190: `boolcacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)`
- L195: `boolcacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)`
- L196: `boolcacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)`
- L199: `boolloadStkAdjFactorsFromFile(const char* adjfile)`
- L200: `boolloadStkAdjFactorsFromLoader()`
- L203: `virtual voidinit(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override`
- L205: `virtual voidonMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override`
- L207: `virtual WTSTickSlice*readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L208: `virtual WTSOrdDtlSlice*readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L209: `virtual WTSOrdQueSlice*readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L210: `virtual WTSTransSlice*readTransSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L211: `virtual WTSKlineSlice*readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override`
- L213: `virtual doublegetAdjFactorByDate(const char* stdCode, uint32_t date = 0) override`
- L215: `virtual uint32_tgetAdjustingFlag() override { return _adjust_flag; }`
- L256: `const AdjFactorList&getAdjFactors(const char* code, const char* exchg, const char* pid)`

### `src/WtDataStorage/WtDataWriter.cpp` (2701 行)
- L39: `extern boolproc_block_data(std::string& content, bool isBar, bool bKeepHead = true)`
- L119: `IDataWriter::init(params, sink)`
- L126: `fs::create_directories(_base_dir.c_str())`
- L444: `SpinLocklock(pBlockPair->_mutex)`
- L498: `SpinLocklock(pBlockPair->_mutex)`
- L552: `SpinLocklock(pBlockPair->_mutex)`
- L583: `StdUniqueLocklck(_task_mtx)`
- L594: `StdUniqueLocklck(_task_mtx)`
- L601: `StdUniqueLocklck(_task_mtx)`
- L610: `case 0:procTick((WTSTickData*)curTask._obj, curTask._flag) break`
- L611: `case 1:procQueue((WTSOrdQueData*)curTask._obj) break`
- L612: `case 2:procOrder((WTSOrdDtlData*)curTask._obj) break`
- L613: `case 3:procTrans((WTSTransData*)curTask._obj) break`
- L630: `SpinLocklock(pBlockPair->_mutex)`
- L686: `fs::create_directories(path.c_str())`
- L780: `fs::create_directories(path.c_str())`
- L875: `fs::create_directories(path.c_str())`
- L970: `fs::create_directories(path.c_str())`
- L1091: `SpinLocklock(pBlockPair->_mutex)`
- L1198: `SpinLocklock(pBlockPair->_mutex)`
- L1305: `SpinLocklock(block->_mutex)`
- L1357: `fs::create_directories(path)`
- L1428: `SpinLocklock(_lck_tick_cache)`
- L1446: `SpinLocklock(_lck_tick_cache)`
- L1571: `StdUniqueLocklock(_proc_mtx)`
- L1621: `std::this_thread::sleep_for(std::chrono::seconds(10))`
- L1753: `SpinLocklock(kBlkPair->_mutex)`
- L1787: `SpinLocklock(kBlkPair->_mutex)`
- L1914: `fs::create_directories(ss.str().c_str())`
- L1952: `StdFile::read_file_content(filename.c_str(), content)`
- L2065: `SpinLocklock(kBlkPair->_mutex)`
- L2069: `fs::create_directories(ss.str().c_str())`
- L2071: `fs::create_directories(ss.str().c_str())`
- L2100: `StdFile::read_file_content(filename.c_str(), content)`
- L2144: `SpinLocklock(kBlkPair->_mutex)`
- L2148: `fs::create_directories(ss.str().c_str())`
- L2150: `fs::create_directories(ss.str().c_str())`
- L2179: `StdFile::read_file_content(filename.c_str(), content)`
- L2222: `StdUniqueLocklock(_proc_mtx)`
- L2230: `StdUniqueLocklock(_proc_mtx)`
- L2243: `SpinLocklock(_lck_tick_cache)`
- L2284: `fs::remove(path.c_str())`
- L2345: `fs::create_directories(ss.str().c_str())`
- L2369: `fs::remove_all(fs::path(path))`
- L2371: `fs::remove_all(fs::path(path))`
- L2373: `fs::remove_all(fs::path(path))`
- L2375: `fs::remove_all(fs::path(path))`
- L2377: `fs::remove_all(fs::path(path))`
- L2379: `fs::remove_all(fs::path(path))`
- L2385: `std::this_thread::sleep_for(std::chrono::seconds(300))`
- L2430: `SpinLocklock(tBlkPair->_mutex)`
- L2449: `fs::create_directories(ss.str().c_str())`
- L2498: `SpinLocklock(tBlkPair->_mutex)`
- L2516: `fs::create_directories(ss.str().c_str())`
- L2564: `SpinLocklock(tBlkPair->_mutex)`
- L2582: `fs::create_directories(ss.str().c_str())`
- L2630: `SpinLocklock(tBlkPair->_mutex)`
- L2648: `fs::create_directories(ss.str().c_str())`

### `src/WtDataStorage/WtDataWriter.h` (728 行)
- 类型: WTSObject:29, WTSContractInfo:30, WtDataWriter:42
- L67: `void*resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount)`
- L74: `voidproc_loop()`
- L81: `voidcheck_loop()`
- L90: `uint32_tdump_bars_to_file(WTSContractInfo* ct)`
- L99: `uint32_tdump_bars_via_dumper(WTSContractInfo* ct)`
- L110: `booldump_day_data(WTSContractInfo* ct, WTSBarStruct* newBar)`
- L122: `boolproc_block_data(const char* tag, std::string& content, bool isBar, bool bKeepHead = true)`
- L131: `voidprocTick(WTSTickData* curTick, uint32_t procFlag)`
- L139: `voidprocQueue(WTSOrdQueData* curOrdQue)`
- L147: `voidprocOrder(WTSOrdDtlData* curOrdDetail)`
- L155: `voidprocTrans(WTSTransData* curTrans)`
- L166: `virtual boolinit(WTSVariant* params, IDataWriterSink* sink) override`
- L172: `virtual voidrelease() override`
- L182: `virtual boolwriteTick(WTSTickData* curTick, uint32_t procFlag) override`
- L191: `virtual boolwriteOrderQueue(WTSOrdQueData* curOrdQue) override`
- L200: `virtual boolwriteOrderDetail(WTSOrdDtlData* curOrdDetail) override`
- L209: `virtual boolwriteTransaction(WTSTransData* curTrans) override`
- L217: `virtual voidtransHisData(const char* sid) override`
- L226: `virtual boolisSessionProceeded(const char* sid) override`
- L236: `virtual WTSTickData*getCurTick(const char* code, const char* exchg = "") override`
- L624: `voidloadCache()`
- L635: `boolupdateCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag)`
- L644: `voidpipeToTicks(WTSContractInfo* ct, WTSTickData* curTick)`
- L653: `voidpipeToKlines(WTSContractInfo* ct, WTSTickData* curTick)`
- L664: `KBlockPair*getKlineBlock(WTSContractInfo* ct, WTSKlinePeriod period, bool bAutoCreate = true)`
- L675: `TickBlockPair*getTickBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true)`
- L686: `TransBlockPair*getTransBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true)`
- L697: `OrdDtlBlockPair*getOrdDtlBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true)`
- L708: `OrdQueBlockPair*getOrdQueBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true)`
- L718: `voidreleaseBlock(T* block)`
- L726: `voidpushTask(const TaskInfo& task)`

### `src/WtDataStorage/WtRdmDtReader.cpp` (2654 行)
- L29: `fmtutil::format_to(buffer, format, args...)`
- L52: `extern boolproc_block_data(std::string& content, bool isBar, bool bKeepHead = true)`
- L90: `std::this_thread::sleep_for(std::chrono::seconds(5))`
- L99: `StdUniqueLocklock(*tPair._mtx)`
- L111: `StdUniqueLocklock(*tPair._mtx)`
- L123: `StdUniqueLocklock(*tPair._mtx)`
- L135: `StdUniqueLocklock(*tPair._mtx)`
- L147: `StdUniqueLocklock(*tPair._mtx)`
- L159: `StdUniqueLocklock(*tPair._mtx)`
- L228: `std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right)`
- L300: `StdFile::read_file_content(filename.c_str(), tBlkPair._buffer)`
- L353: `StdUniqueLocklock(*tPair->_mtx)`
- L450: `StdFile::read_file_content(filename.c_str(), tBlkPair._buffer)`
- L550: `StdUniqueLocklock(*tPair->_mtx)`
- L711: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L886: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L1060: `StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer)`
- L1179: `StdFile::read_file_content(filename.c_str(), content)`
- L1274: `StdFile::read_file_content(filename.c_str(), content)`
- L1380: `StdFile::read_file_content(filename.c_str(), content)`
- L1472: `StdFile::read_file_content(filename.c_str(), content)`
- L1579: `StdFile::read_file_content(filename.c_str(), content)`
- L1871: `StdUniqueLocklock(*kPair->_mtx)`
- L2294: `StdUniqueLocklock(*(kPair->_mtx))`
- L2435: `StdUniqueLocklock(*tPair->_mtx)`
- L2537: `StdFile::read_file_content(filename.c_str(), tBlkPair._buffer)`

### `src/WtDataStorage/WtRdmDtReader.h` (544 行)
- 类型: WTSVariant:22, WTSTickSlice:23, WTSKlineSlice:24, WTSOrdDtlSlice:25, WTSOrdQueSlice:26, WTSTransSlice:27, WTSArray:28, IBaseDataMgr:30, IHotMgr:31, WtRdmDtReader:35
- L42: `virtual ~WtRdmDtReader()`
- L281: `RTKlineBlockPair*getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period)`
- L290: `TickBlockPair*getRTTickBlock(const char* exchg, const char* code)`
- L299: `OrdQueBlockPair*getRTOrdQueBlock(const char* exchg, const char* code)`
- L308: `OrdDtlBlockPair*getRTOrdDtlBlock(const char* exchg, const char* code)`
- L317: `TransBlockPair*getRTTransBlock(const char* exchg, const char* code)`
- L329: `boolcacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)`
- L342: `uint32_treadBarsFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, std::vector<WTSBarStruct>& ayBars, bool isDay = false)`
- L354: `WTSBarStruct*indexBarFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, uint32_t& count, bool isDay = false)`
- L366: `WTSBarStruct*indexBarFromCacheByCount(const std::string& key, uint64_t etime, uint32_t& count, bool isDay = false)`
- L375: `boolloadStkAdjFactorsFromFile(const char* adjfile)`
- L388: `virtual voidinit(WTSVariant* cfg, IRdmDtReaderSink* sink) override`
- L399: `virtual WTSOrdDtlSlice*readOrdDtlSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override`
- L409: `virtual WTSOrdQueSlice*readOrdQueSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override`
- L419: `virtual WTSTransSlice*readTransSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override`
- L430: `virtual WTSTickSlice*readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override`
- L441: `virtual WTSKlineSlice*readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime = 0) override`
- L452: `virtual WTSTickSlice*readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L463: `virtual WTSKlineSlice*readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override`
- L473: `virtual WTSTickSlice*readTickSliceByDate(const char* stdCode, uint32_t uDate = 0) override`
- L483: `virtual doublegetAdjFactorByDate(const char* stdCode, uint32_t date = 0) override`
- L490: `virtual voidclearCache() override`

## `WtDataStorageAD`

### `src/WtDataStorageAD/WtBtDtReaderAD.cpp` (193 行)
- L21: `fmt::format_to(buffer, format, args...)`
- L72: `WtLMDBQueryquery(*db)`
- L73: `LMDBBarKeyrKey(exchg, code, 0xffffffff)`
- L74: `LMDBBarKeylKey(exchg, code, 0)`
- L102: `WtLMDBQueryquery(*db)`
- L103: `LMDBHftKeyrKey(exchg, code, uDate, 240000000)`
- L104: `LMDBHftKeylKey(exchg, code, uDate, 0)`
- L150: `WtLMDBPtrdbPtr(new WtLMDB(true))`
- L176: `WtLMDBPtrdbPtr(new WtLMDB(true))`

### `src/WtDataStorageAD/WtBtDtReaderAD.h` (139 行)
- 类型: WtBtDtReaderAD:27
- L38: `virtual ~WtBtDtReaderAD()`
- L49: `virtual voidinit(WTSVariant* cfg, IBtDtReaderSink* sink) override`
- L62: `virtual boolread_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer) override`
- L75: `virtual boolread_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer) override`
- L127: `WtLMDBPtrget_k_db(const char* exchg, WTSKlinePeriod period)`
- L136: `WtLMDBPtrget_t_db(const char* exchg, const char* code)`

### `src/WtDataStorageAD/WtDataReaderAD.cpp` (619 行)
- L27: `fmt::format_to(buffer, format, args...)`
- L62: `IDataReader::init(cfg, sink, loader)`
- L151: `WtLMDBQueryquery(*db)`
- L152: `LMDBHftKeylKey(cInfo._exchg, cInfo._code, (uint32_t)(last_access_time / 1000000000), (uint32_t)(last_access_time % 1000000000))`
- L153: `LMDBHftKeyrKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000))`
- L155: `std::string((const char*)&rKey, sizeof(rKey)), [&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals)`
- L168: `WtLMDBQueryquery(*db)`
- L169: `LMDBHftKeyrKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000))`
- L170: `LMDBHftKeylKey(cInfo._exchg, cInfo._code, 0, 0)`
- L216: `WtLMDBQueryquery(*db)`
- L217: `LMDBBarKeylKey(exchg, code, 0)`
- L218: `LMDBBarKeyrKey(exchg, code, 0xffffffff)`
- L220: `std::string((const char*)&lKey, sizeof(lKey)), [&buffer](const ValueArray& /*ayKeys*/, const ValueArray& ayVals)`
- L246: `WtLMDBQueryquery(*db)`
- L247: `LMDBBarKeyrKey(cInfo._exchg, cInfo._code, 0xffffffff)`
- L248: `LMDBBarKeylKey(cInfo._exchg, cInfo._code, 0)`
- L273: `WtLMDBQueryquery(*db)`
- L274: `LMDBBarKeylKey(exchg, code, lastBarTime)`
- L275: `LMDBBarKeyrKey(exchg, code, 0xFFFFFFFF)`
- L580: `WtLMDBPtrdbPtr(new WtLMDB(true))`
- L582: `fs::create_directories(path)`
- L604: `WtLMDBPtrdbPtr(new WtLMDB(true))`
- L606: `fs::create_directories(path)`

### `src/WtDataStorageAD/WtDataReaderAD.h` (363 行)
- 类型: WtDataReaderAD:38
- L49: `virtual ~WtDataReaderAD()`
- L60: `virtual voidinit(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override`
- L69: `virtual voidonMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override`
- L79: `virtual WTSTickSlice*readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L90: `virtual WTSKlineSlice*readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override`
- L154: `inline boolempty() const { return _cache_block == NULL; }`
- L281: `boolcacheBarsFromStorage(const std::string& key, const char* stdCode, WTSKlinePeriod period, uint32_t count)`
- L292: `voidupdate_cache_from_lmdb(BarsList& barsList, const char* exchg, const char* code, WTSKlinePeriod period, uint32_t& lastBarTime)`
- L302: `std::stringread_bars_to_buffer(const char* exchg, const char* code, WTSKlinePeriod period)`
- L312: `WTSBarStruct*get_rt_cache_bar(const char* exchg, const char* code, WTSKlinePeriod period)`
- L351: `WtLMDBPtrget_k_db(const char* exchg, WTSKlinePeriod period)`
- L360: `WtLMDBPtrget_t_db(const char* exchg, const char* code)`

### `src/WtDataStorageAD/WtDataWriterAD.cpp` (1073 行)
- L29: `fmt::format_to(buffer, format, args...)`
- L75: `IDataWriter::init(params, sink)`
- L81: `fs::create_directories(_base_dir.c_str())`
- L377: `StdUniqueLocklck(_task_mtx)`
- L394: `StdUniqueLocklck(_task_mtx)`
- L401: `StdUniqueLocklck(_task_mtx)`
- L427: `LMDBHftKeykey(ct->getExchg(), ct->getCode(), curTick->tradingdate(), offTime)`
- L428: `WtLMDBQueryquery(*db)`
- L456: `LMDBBarKeykey(ct->getExchg(), ct->getCode(), bar.date)`
- L457: `WtLMDBQueryquery(*db)`
- L489: `LMDBBarKeykey(ct->getExchg(), ct->getCode(), (uint32_t)bar.time)`
- L490: `WtLMDBQueryquery(*db)`
- L522: `LMDBBarKeykey(ct->getExchg(), ct->getCode(), (uint32_t)bar.time)`
- L523: `WtLMDBQueryquery(*db)`
- L572: `StdUniqueLocklock(_d1_cache._mtx)`
- L661: `StdUniqueLocklock(_m1_cache._mtx)`
- L764: `StdUniqueLocklock(_m5_cache._mtx)`
- L852: `StdUniqueLocklock(_mtx_tick_cache)`
- L870: `StdUniqueLocklock(_mtx_tick_cache)`
- L1042: `WtLMDBPtrdbPtr(new WtLMDB(false))`
- L1044: `boost::filesystem::create_directories(path)`
- L1062: `WtLMDBPtrdbPtr(new WtLMDB(false))`
- L1064: `boost::filesystem::create_directories(path)`

### `src/WtDataStorageAD/WtDataWriterAD.h` (428 行)
- 类型: WTSContractInfo:24, WtDataWriterAD:35
- L61: `void*resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount)`
- L71: `virtual boolinit(WTSVariant* params, IDataWriterSink* sink) override`
- L77: `virtual voidrelease() override`
- L86: `virtual boolwriteTick(WTSTickData* curTick, uint32_t procFlag) override`
- L95: `virtual WTSTickData*getCurTick(const char* code, const char* exchg = "") override`
- L190: `inline boolempty() const { return _cache_block == NULL; }`
- L353: `WtLMDBPtrget_k_db(const char* exchg, WTSKlinePeriod period)`
- L362: `WtLMDBPtrget_t_db(const char* exchg, const char* code)`
- L369: `voidloadCache()`
- L379: `boolupdateTickCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag)`
- L387: `voidupdateBarCache(WTSContractInfo* ct, WTSTickData* curTick)`
- L395: `voidpipeToTicks(WTSContractInfo* ct, WTSTickData* curTick)`
- L403: `voidpipeToDayBars(WTSContractInfo* ct, const WTSBarStruct& bar)`
- L411: `voidpipeToM1Bars(WTSContractInfo* ct, const WTSBarStruct& bar)`
- L419: `voidpipeToM5Bars(WTSContractInfo* ct, const WTSBarStruct& bar)`
- L426: `voidpushTask(TaskInfo task)`

### `src/WtDataStorageAD/WtRdmDtReaderAD.cpp` (458 行)
- L28: `fmt::format_to(buffer, format, args...)`
- L62: `IRdmDtReader::init(cfg, sink)`
- L121: `WtLMDBQueryquery(*db)`
- L122: `LMDBHftKeylKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs)`
- L123: `LMDBHftKeyrKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs)`
- L125: `std::string((const char*)&rKey, sizeof(rKey)), [&tickList](const ValueArray& /*ayKeys*/, const ValueArray& ayVals)`
- L151: `WtLMDBQueryquery(*db)`
- L152: `LMDBHftKeyrKey(cInfo._exchg, cInfo._code, (uint32_t)(tickList._first_tick_time / 1000000000), (uint32_t)(tickList._first_tick_time % 1000000000))`
- L153: `LMDBHftKeylKey(cInfo._exchg, cInfo._code, beginTDate, lTime * 100000 + lSecs)`
- L183: `WtLMDBQueryquery(*db)`
- L184: `LMDBHftKeylKey(cInfo._exchg, cInfo._code, (uint32_t)(tickList._last_tick_time / 1000000000), (uint32_t)(tickList._last_tick_time % 1000000000))`
- L185: `LMDBHftKeyrKey(cInfo._exchg, cInfo._code, endTDate, rTime * 100000 + rSecs)`
- L299: `WtLMDBQueryquery(*db)`
- L300: `LMDBBarKeyrKey(cInfo._exchg, cInfo._code, 0xffffffff)`
- L301: `LMDBBarKeylKey(cInfo._exchg, cInfo._code, 0)`
- L321: `WtLMDBQueryquery(*db)`
- L322: `LMDBBarKeyrKey(cInfo._exchg, cInfo._code, 0xffffffff)`
- L323: `LMDBBarKeylKey(cInfo._exchg, cInfo._code, (uint32_t)barsList._last_bar_time)`
- L419: `WtLMDBPtrdbPtr(new WtLMDB(true))`
- L421: `fs::create_directories(path)`
- L443: `WtLMDBPtrdbPtr(new WtLMDB(true))`
- L445: `fs::create_directories(path)`

### `src/WtDataStorageAD/WtRdmDtReaderAD.h` (227 行)
- 类型: WtRdmDtReaderAD:33
- L46: `virtual ~WtRdmDtReaderAD()`
- L58: `virtual voidinit(WTSVariant* cfg, IRdmDtReaderSink* sink) override`
- L68: `virtual WTSOrdDtlSlice*readOrdDtlSliceByRange(const char* /*stdCode*/, uint64_t /*stime*/, uint64_t /*etime*/ = 0) override { return NULL; }`
- L78: `virtual WTSOrdQueSlice*readOrdQueSliceByRange(const char* /*stdCode*/, uint64_t /*stime*/, uint64_t /*etime*/ = 0) override { return NULL; }`
- L88: `virtual WTSTransSlice*readTransSliceByRange(const char* /*stdCode*/, uint64_t /*stime*/, uint64_t /*etime*/ = 0) override { return NULL; }`
- L98: `virtual WTSTickSlice*readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime = 0) override`
- L109: `virtual WTSKlineSlice*readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime = 0) override`
- L119: `virtual WTSTickSlice*readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L130: `virtual WTSKlineSlice*readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override`
- L139: `virtual WTSTickSlice*readTickSliceByDate(const char* stdCode, uint32_t uDate /* = 0 */) override`
- L215: `WtLMDBPtrget_k_db(const char* exchg, WTSKlinePeriod period)`
- L224: `WtLMDBPtrget_t_db(const char* exchg, const char* code)`

