# API 简报 R1

测试目标: wtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

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

### `src/WtCore/WtCtaTicker.cpp` (299 行)
- L37: `TimeUtils::getDateTime(_date, _time)`
- L122: `StdUniqueLocklock(_mtx)`
- L196: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L202: `StdUniqueLocklock(_mtx)`
- L250: `StdUniqueLocklock(_mtx)`
- L271: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtCore/WtHftEngine.cpp` (375 行)
- L56: `WtEngine::release()`
- L61: `WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier)`
- L81: `rj::Documentroot(rj::kObjectType)`
- L84: `rj::ValuejStraList(rj::kArrayType)`
- L93: `rj::ValuejChnlList(rj::kArrayType)`
- L108: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L110: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L223: `WtEngine::on_tick(stdCode, curTick)`
- L308: `WtEngine::on_session_begin()`
- L324: `WtEngine::on_session_end()`

### `src/WtCore/WtHftTicker.cpp` (259 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L114: `StdUniqueLocklock(_mtx)`
- L177: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L183: `StdUniqueLocklock(_mtx)`
- L228: `StdUniqueLocklock(_mtx)`
- L246: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtCore/WtSelTicker.cpp` (246 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L117: `StdUniqueLocklock(_mtx)`
- L180: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L186: `StdUniqueLocklock(_mtx)`
- L226: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`

