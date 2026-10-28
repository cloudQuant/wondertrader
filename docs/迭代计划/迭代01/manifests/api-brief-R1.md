# API 简报 R1

测试目标: (主目标 CoverageTests), t_TraderFemas, t_TraderXTP, t_ParserXTP, t_CTPOptLoader, t_ParserXeleSkt。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

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

### `src/Includes/WTSDataDef.hpp` (1648 行)
- 类型: WTSContractInfo:32, WTSValueArray:39, WTSKlineSlice:204, WTSKlineData:444, WTSTickData:858, WTSTickRef:1039, WTSOrdQueData:1189, WTSOrdDtlData:1225, WTSTransData:1262, WTSHisTickData:1302, WTSTickSlice:1390, WTSOrdDtlSlice:1498, WTSOrdQueSlice:1550, WTSTransSlice:1602
- L59: `inline uint32_tsize() }`
- L60: `inline boolempty() }`
- L368: `inline int32_tsize() const{ return _count; }`
- L369: `inline boolempty() const{ return _count == 0; }`
- L374: `inline const char*code() const{ return _code; }`
- L375: `inline voidsetCode(const char* code) }`
- L492: `inline voidsetClosed(bool bClosed) m_bClosed = bClosed; }`
- L493: `inline boolisClosed() const{ return m_bClosed; }`
- L500: `inline voidsetPeriod(WTSKlinePeriod period, uint32_t times = 1) m_kpPeriod = period; m_uTimes = times; }`
- L502: `inline voidsetUnixTime(bool bEnabled = true) m_bUnixTime = bEnabled; }`
- L504: `inline WTSKlinePeriodperiod() const{ return m_kpPeriod; }`
- L505: `inline uint32_ttimes() const{ return m_uTimes; }`
- L506: `inline boolisUnixTime() const{ return m_bUnixTime; }`
- L563: `inline uint32_tsize() }`
- L564: `inline boolIsEmpty() }`
- L569: `inline const char*code() const{ return m_strCode; }`
- L570: `inline voidsetCode(const char* code) }`
- L802: `inline WTSBarList&getDataRef() return m_vecBarData; }`
- L824: `WTSObject::release()`
- L900: `constexpr inline const char*code() const noexcept { return m_tickStruct.code; }`
- L905: `constexpr inline const char*exchg() const noexcept { return m_tickStruct.exchg; }`
- L910: `constexpr inline doubleprice() const noexcept { return m_tickStruct.price; }`
- L912: `constexpr inline doubleopen() const noexcept { return m_tickStruct.open; }`
- L917: `constexpr inline doublehigh() const noexcept { return m_tickStruct.high; }`
- L922: `constexpr inline doublelow() const noexcept { return m_tickStruct.low; }`
- L925: `constexpr inline doublepreclose() const noexcept { return m_tickStruct.pre_close; }`
- L926: `constexpr inline doublepresettle() const noexcept { return m_tickStruct.pre_settle; }`
- L927: `constexpr inline doublepreinterest() const noexcept { return m_tickStruct.pre_interest; }`
- L929: `constexpr inline doubleupperlimit() const noexcept { return m_tickStruct.upper_limit; }`
- L930: `constexpr inline doublelowerlimit() const noexcept { return m_tickStruct.lower_limit; }`
- L932: `constexpr inline doubletotalvolume() const noexcept { return m_tickStruct.total_volume; }`
- L935: `constexpr inline doublevolume() const noexcept { return m_tickStruct.volume; }`
- L938: `constexpr inline doublesettlepx() const noexcept { return m_tickStruct.settle_price; }`
- L941: `constexpr inline doubleopeninterest() const noexcept { return m_tickStruct.open_interest; }`
- L943: `constexpr inline doubleadditional() const noexcept { return m_tickStruct.diff_interest; }`
- L946: `constexpr inline doubletotalturnover() const noexcept { return m_tickStruct.total_turnover; }`
- L949: `constexpr inline doubleturnover() const noexcept { return m_tickStruct.turn_over; }`
- L952: `constexpr inline uint32_ttradingdate() const noexcept { return m_tickStruct.trading_date; }`
- L955: `constexpr inline uint32_tactiondate() const noexcept { return m_tickStruct.action_date; }`
- L958: `constexpr inline uint32_tactiontime() const noexcept { return m_tickStruct.action_time; }`
- L1012: `constexpr inline WTSTickStruct&getTickStruct() noexcept{ return m_tickStruct; }`
- L1014: `constexpr inline voidsetContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }`
- L1015: `constexpr inline WTSContractInfo*getContractInfo() const noexcept { return m_pContract; }`
- L1017: `inline voidsetLimitFlag(uint32_t flag) m_uLimitFlag = flag; }`
- L1018: `constexpr inline uint32_tgetLimitFlag() const { return m_uLimitFlag; }`
- L1020: `constexpr inline boolisNewHigh() const { return m_uLimitFlag & 1; }`
- L1021: `constexpr inline boolisNewLow() const { return m_uLimitFlag & 2; }`
- L1062: `constexpr inline const char*code() const noexcept { return m_tickStruct->code; }`
- L1067: `constexpr inline const char*exchg() const noexcept { return m_tickStruct->exchg; }`
- L1072: `constexpr inline doubleprice() const noexcept { return m_tickStruct->price; }`
- L1074: `constexpr inline doubleopen() const noexcept { return m_tickStruct->open; }`
- L1079: `constexpr inline doublehigh() const noexcept { return m_tickStruct->high; }`
- L1084: `constexpr inline doublelow() const noexcept { return m_tickStruct->low; }`
- L1087: `constexpr inline doublepreclose() const noexcept { return m_tickStruct->pre_close; }`
- L1088: `constexpr inline doublepresettle() const noexcept { return m_tickStruct->pre_settle; }`
- L1089: `constexpr inline doublepreinterest() const noexcept { return m_tickStruct->pre_interest; }`
- L1091: `constexpr inline doubleupperlimit() const noexcept { return m_tickStruct->upper_limit; }`
- L1092: `constexpr inline doublelowerlimit() const noexcept { return m_tickStruct->lower_limit; }`
- L1094: `constexpr inline doubletotalvolume() const noexcept { return m_tickStruct->total_volume; }`
- L1097: `constexpr inline doublevolume() const noexcept { return m_tickStruct->volume; }`
- L1100: `constexpr inline doublesettlepx() const noexcept { return m_tickStruct->settle_price; }`
- L1103: `constexpr inline doubleopeninterest() const noexcept { return m_tickStruct->open_interest; }`
- L1105: `constexpr inline doubleadditional() const noexcept { return m_tickStruct->diff_interest; }`
- L1108: `constexpr inline doubletotalturnover() const noexcept { return m_tickStruct->total_turnover; }`
- L1111: `constexpr inline doubleturnover() const noexcept { return m_tickStruct->turn_over; }`
- L1114: `constexpr inline uint32_ttradingdate() const noexcept { return m_tickStruct->trading_date; }`
- L1117: `constexpr inline uint32_tactiondate() const noexcept { return m_tickStruct->action_date; }`
- L1120: `constexpr inline uint32_tactiontime() const noexcept { return m_tickStruct->action_time; }`
- L1171: `constexpr inline boolvalid() const noexcept { return m_tickStruct != nullptr; }`
- L1173: `constexpr inline operatorbool() const noexcept { return m_tickStruct != nullptr; }`
- （另有 43 个声明，按需用 Read 定位）

### `src/WTSTools/WTSBaseDataMgr.cpp` (888 行)
- L554: `std::sort(_code_list->begin(), _code_list->end(), [](WTSObject* left, WTSObject* right)`
- L677: `TimeUtils::getDateTime(uDate, uTime)`

### `src/WTSTools/WTSLogger.cpp` (471 行)
- L115: `fs::create_directories(s.substr(0, pos).c_str())`
- L126: `fmt::print(" ")`
- L132: `fmt::print(buffer)`
- L133: `fmt::print("\r\n")`
- L150: `StrUtil::replace(filename, "%s", catName)`
- L159: `StrUtil::replace(filename, "%s", catName)`
- L183: `spdlog::register_logger(logger)`
- L189: `spdlog::init_thread_pool(8192, 2)`
- L195: `spdlog::register_logger(logger)`
- L240: `throw std::runtime_error("root logger can not be null, please check the config file")`
- L242: `spdlog::set_default_logger(m_rootLogger)`
- L243: `spdlog::flush_every(std::chrono::seconds(2))`
- L260: `spdlog::shutdown()`
- L368: `fmt::print(message)`
- L369: `fmt::print("\n")`
- L410: `fmt::print(m_buffer)`
- L411: `fmt::print("\n")`
- L469: `spdlog::drop(logger)`

### `src/TraderFemas/TraderFemas.cpp` (1180 行)
- L174: `fs::create_directories(path.c_str())`
- L206: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L216: `boost::asio::post(m_asyncIO, [this]()`
- L431: `StdUniqueLocklock(m_mtxQuery)`
- L453: `StdUniqueLocklock(m_mtxQuery)`
- L473: `StdUniqueLocklock(m_mtxQuery)`
- L494: `StdUniqueLocklock(m_mtxQuery)`
- L575: `StrUtil::trim(m_strSessionID, "0", false, true)`
- L997: `StrUtil::replace(strTime, ":", "")`
- L1074: `StrUtil::replace(strTime, ":", "")`
- L1154: `boost::asio::post(*m_strandIO, [this]()`
- L1161: `std::this_thread::sleep_for(std::chrono::milliseconds(50))`
- L1162: `boost::asio::post(*m_strandIO, [this]()`
- L1174: `StdUniqueLocklock(m_mtxQuery)`

### `src/TraderXTP/TraderXTP.cpp` (952 行)
- L162: `static std::atomic<uint32_t>auto_refid(0)`
- L350: `boost::asio::post(_asyncio, [this]()`
- L352: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L662: `fs::create_directories(ss.str().c_str())`
- L670: `StdThreadPtrthrd(new StdThread([this]()`
- L671: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L699: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L782: `fs::create_directories(path.c_str())`
- L795: `fs::create_directories(path.c_str())`

### `src/ParserXTP/ParserXTP.cpp` (696 行)
- L34: `fmt::format_to(buffer, format, args...)`
- L117: `fs::create_directories(path.c_str())`
- L153: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L191: `boost::asio::post(_asyncio, [this]()`
- L193: `std::this_thread::sleep_for(std::chrono::seconds(2))`

### `src/CTPOptLoader/TraderSpi.cpp` (623 行)
- L211: `throw std::runtime_error("non implemented category")`
- L492: `rj::DocumentjComms(rj::kObjectType)`
- L504: `rj::ValuejComm(rj::kObjectType)`
- L524: `rj::DocumentjContracts(rj::kObjectType)`
- L536: `rj::ValuejcInfo(rj::kObjectType)`
- L563: `rj::ValueoptInfo(rj::kObjectType)`
- L582: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L594: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`

### `src/ParserXeleSkt/ParserXeleSkt.cpp` (541 行)
- 类型: tm:391, tm:451
- L32: `fmt::format_to(buffer, format, args...)`
- L159: `ip::tcp::sockets(io_context)`
- L289: `boost::asio::post(_strand, boost::bind(&ParserXeleSkt::doOnDisconnected, this))`
- L343: `std::this_thread::sleep_for(std::chrono::seconds(2))`

