# API 简报 R2

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

### `src/WtCore/WtLocalExecuter.cpp` (995 行)
- L186: `SpinLocklock(_mtx_units)`
- L409: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`
- L803: `SpinLocklock(_mtx_units)`
- L836: `SpinLocklock(_mtx_units)`
- L879: `SpinLocklock(_mtx_units)`

### `src/WtCore/HftStraBaseCtx.cpp` (987 行)
- L73: `fs::create_directories(folder.c_str())`
- L518: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L523: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L528: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L533: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L729: `rj::Documentroot(rj::kObjectType)`
- L746: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L767: `StdFile::read_file_content(filename.c_str(), content)`

### `src/WtCore/WtDtMgr.cpp` (542 行)
- L72: `DLLHelper::free_library(hInst)`
- L80: `DLLHelper::free_library(hInst)`
- L109: `WTSLogger::debug("All bars updated, on_bar will be triggered")`
- L146: `WTSLogger::log_raw(ll, message)`
- L295: `std::stringpureStdCode(stdCode, len - 1)`

### `src/WtCore/WtArbiExecuter.cpp` (891 行)
- L173: `SpinLocklock(_mtx_units)`
- L357: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`
- L710: `SpinLocklock(_mtx_units)`
- L741: `SpinLocklock(_mtx_units)`
- L782: `SpinLocklock(_mtx_units)`

### `src/WtCore/WtDiffExecuter.cpp` (902 行)
- L125: `StdFile::read_file_content(filename.c_str(), content)`
- L185: `rj::Documentroot(rj::kObjectType)`
- L189: `rj::ValuejTarget(rj::kArrayType)`
- L193: `rj::ValuejItem(rj::kObjectType)`
- L204: `rj::ValuejDiff(rj::kArrayType)`
- L208: `rj::ValuejItem(rj::kObjectType)`
- L226: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L444: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`

### `src/WtCore/SelStraBaseCtx.cpp` (1275 行)
- L64: `fs::create_directories(folder.c_str())`
- L173: `rj::Documentroot(rj::kObjectType)`
- L190: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L211: `StdFile::read_file_content(filename.c_str(), content)`
- L241: `StdFile::read_file_content(filename.c_str(), content)`
- L381: `rj::Documentroot(rj::kObjectType)`
- L384: `rj::ValuejPos(rj::kArrayType)`
- L393: `rj::ValuepItem(rj::kObjectType)`
- L403: `rj::Valuedetails(rj::kArrayType)`
- L407: `rj::ValuedItem(rj::kObjectType)`
- L433: `rj::ValuejFund(rj::kObjectType)`
- L445: `rj::ValuejSigs(rj::kObjectType)`
- L453: `rj::ValuejItem(rj::kObjectType)`
- L475: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L1048: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L1053: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L1058: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L1063: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`

### `src/WtCore/ParserAdapter.cpp` (425 行)
- L372: `WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message)`

