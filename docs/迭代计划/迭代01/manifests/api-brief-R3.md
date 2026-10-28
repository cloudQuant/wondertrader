# API 简报 R3

测试目标: wtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtCore/CtaStraBaseCtx.cpp` (2063 行)
- L82: `fs::create_directories(folder.c_str())`
- L221: `rj::Documentroot(rj::kObjectType)`
- L238: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L259: `StdFile::read_file_content(filename.c_str(), content)`
- L289: `StdFile::read_file_content(filename.c_str(), content)`
- L497: `rj::Documentroot(rj::kObjectType)`
- L500: `rj::ValuejPos(rj::kArrayType)`
- L509: `rj::ValuepItem(rj::kObjectType)`
- L519: `rj::Valuedetails(rj::kArrayType)`
- L523: `rj::ValuedItem(rj::kObjectType)`
- L550: `rj::ValuejFund(rj::kObjectType)`
- L562: `rj::ValuejSigs(rj::kObjectType)`
- L570: `rj::ValuejItem(rj::kObjectType)`
- L584: `rj::ValuejCond(rj::kObjectType)`
- L585: `rj::ValuejItems(rj::kObjectType)`
- L594: `rj::ValuecArray(rj::kArrayType)`
- L597: `rj::ValuecItem(rj::kObjectType)`
- L619: `rj::ValuejUtils(rj::kObjectType)`
- L637: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L681: `rj::Documentroot(rj::kObjectType)`
- L684: `rj::ValueklineItem(rj::kObjectType)`
- L701: `rj::ValuejIndice(rj::kArrayType)`
- L705: `rj::ValuejIndex(rj::kObjectType)`
- L709: `rj::ValuejLines(rj::kArrayType)`
- L713: `rj::ValuejLine(rj::kObjectType)`
- L722: `rj::ValuejBaseLines(rj::kObjectType)`
- L741: `fs::create_directories(folder.c_str())`
- L747: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L749: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L954: `std::stringstdCode(key, idx)`
- L1048: `SpinLocklock(_mutex)`
- L1432: `SpinLocklock(_mutex)`
- L1728: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L1733: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L1738: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L1743: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L1976: `WTSLogger::error("Marks can be added only during schedule")`
- L2032: `WTSLogger::error("Marks can be added only during schedule")`

### `src/WtCore/TraderAdapter.cpp` (2423 行)
- L245: `fs::create_directories(folder.c_str())`
- L305: `rj::Documentroot(rj::kObjectType)`
- L309: `rj::ValuejPos(rj::kArrayType)`
- L329: `rj::ValuepItem(rj::kObjectType)`
- L332: `rj::ValuelongItem(rj::kObjectType)`
- L339: `rj::ValueshortItem(rj::kObjectType)`
- L353: `rj::ValuejFunds(rj::kObjectType)`
- L360: `rj::ValuefItem(rj::kObjectType)`
- L384: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L474: `SpinLocklock(_mtx_orders)`
- L1278: `SpinLocklock(_mtx_orders)`
- L1454: `WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, err->getMessage())`
- L1706: `SpinLocklock(_mtx_orders)`
- L2119: `SpinLocklock(_mtx_orders)`
- L2353: `WTSLogger::log_dyn_raw("trader", _id.c_str(), ll, message)`

### `src/WtCore/WtEngine.cpp` (1237 行)
- L50: `TimeUtils::getDateTime(_cur_date, _cur_time)`
- L56: `WtHelper::setTime(_cur_date, _cur_time, _cur_secs)`
- L70: `WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs)`
- L77: `WtHelper::setTDate(curTDate)`
- L157: `SpinLocklock(pInfo->_mtx)`
- L200: `SpinLocklock(_pos_mtx)`
- L244: `WTSLogger::log_raw_by_cat("risk", LL_INFO, szBuf)`
- L292: `WTSLogger::info("Running mode: Production")`
- L314: `WTSLogger::log_raw(LL_WARN, "RiskMon is not configured, portfilio fund will be updated every 5s")`
- L326: `BoostFilePtrfund_log(new BoostFile())`
- L369: `rj::Documentroot(rj::kObjectType)`
- L375: `rj::ValuejFund(rj::kObjectType)`
- L391: `rj::ValuejMaxMD(rj::kObjectType)`
- L395: `rj::ValuejMinMD(rj::kObjectType)`
- L408: `rj::ValuejPos(rj::kArrayType)`
- L415: `rj::ValuepItem(rj::kObjectType)`
- L421: `rj::Valuedetails(rj::kArrayType)`
- L427: `rj::ValuedItem(rj::kObjectType)`
- L449: `rj::ValuejRisk(rj::kObjectType)`
- L465: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L486: `StdFile::read_file_content(filename.c_str(), content)`
- L959: `SpinLocklock(_pos_mtx)`
- L969: `SpinLocklock(pInfo->_mtx)`
- L1089: `StdUniqueLocklock(_mtx_task)`
- L1110: `StdUniqueLocklock(_mtx_task)`
- L1168: `DLLHelper::free_library(hInst)`

### `src/WtCore/WtCtaEngine.cpp` (720 行)
- L58: `WtEngine::release()`
- L69: `rj::Documentroot(rj::kObjectType)`
- L72: `rj::ValuejStraList(rj::kArrayType)`
- L81: `rj::ValuejChnlList(rj::kArrayType)`
- L90: `rj::ValuejExecList(rj::kArrayType)`
- L104: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L106: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L118: `WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier)`
- L236: `WtEngine::on_session_end()`
- L452: `SpinLocklock(_pos_mtx)`
- L492: `WtEngine::on_tick(stdCode, curTick)`

### `src/WtCore/HftStraBaseCtx.cpp` (987 行)
- L73: `fs::create_directories(folder.c_str())`
- L518: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L523: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L528: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L533: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L729: `rj::Documentroot(rj::kObjectType)`
- L746: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L767: `StdFile::read_file_content(filename.c_str(), content)`

