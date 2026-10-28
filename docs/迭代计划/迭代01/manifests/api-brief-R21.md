# API 简报 R21

测试目标: adstorage。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

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

