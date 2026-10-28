# API 简报 R4

测试目标: adstorage。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

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

