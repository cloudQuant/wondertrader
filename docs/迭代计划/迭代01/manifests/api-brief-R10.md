# API 简报 R10

测试目标: wtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtCore/HftStraBaseCtx.cpp` (987 行)
- L73: `fs::create_directories(folder.c_str())`
- L518: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L523: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L528: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L533: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L729: `rj::Documentroot(rj::kObjectType)`
- L746: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L767: `StdFile::read_file_content(filename.c_str(), content)`

### `src/WtCore/EventNotifier.cpp` (400 行)
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
- L245: `rj::Documentroot(rj::kObjectType)`
- L260: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L281: `rj::Documentroot(rj::kObjectType)`
- L300: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L315: `boost::asio::post(_asyncio, [this, time, sid, iname, lname, val]()`
- L318: `rj::Documentroot(rj::kObjectType)`
- L328: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L346: `boost::asio::post(_asyncio, [this, time, sid, sIcon, sTag, price]()`
- L349: `rj::Documentroot(rj::kObjectType)`
- L359: `rj::Writer<rj::StringBuffer>writer(sb)`
- L377: `boost::asio::post(_asyncio, [this, sid, code, tag, isLong, isOpen, curTime, price]()`
- L380: `rj::Documentroot(rj::kObjectType)`
- L392: `rj::Writer<rj::StringBuffer>writer(sb)`

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

### `src/WtCore/WtSelEngine.cpp` (509 行)
- L56: `WtEngine::release()`
- L107: `WtEngine::on_tick(stdCode, curTick)`
- L330: `StdThreadPtrthrd(new StdThread([ctx, curDate, curTime, nextTime]()`
- L347: `rj::Documentroot(rj::kObjectType)`
- L350: `rj::ValuejStraList(rj::kArrayType)`
- L359: `rj::ValuejChnlList(rj::kArrayType)`
- L374: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L376: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L384: `WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier)`
- L402: `TaskInfoPtrtInfo(new TaskInfo)`
- L459: `SpinLocklock(_pos_mtx)`

### `src/WtCore/WtDiffExecuter.cpp` (902 行)
- L125: `StdFile::read_file_content(filename.c_str(), content)`
- L185: `rj::Documentroot(rj::kObjectType)`
- L189: `rj::ValuejTarget(rj::kArrayType)`
- L193: `rj::ValuejItem(rj::kObjectType)`
- L204: `rj::ValuejDiff(rj::kArrayType)`
- L208: `rj::ValuejItem(rj::kObjectType)`
- L226: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L444: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`

