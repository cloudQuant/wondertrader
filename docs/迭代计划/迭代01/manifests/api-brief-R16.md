# API 简报 R16

测试目标: (主目标 CoverageTests)。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/Share/TimeUtils.hpp` (481 行)
- 类型: KUSER_SHARED_DATA:44, tm:218, tm:267, Time32:371, tm:433, Ticker:437

### `src/Share/StrUtil.hpp` (340 行)
- L55: `std::stringpattern(chars)`
- L310: `std::replace(path.begin(), path.end(), '\\', '/')`

### `src/Share/IniHelper.hpp` (420 行)
- 类型: IniHelper:34
- L78: `boost::property_tree::ini_parser::read_ini(szFile, _root)`
- L98: `boost::property_tree::ini_parser::write_ini(filename, _root)`
- L100: `boost::property_tree::ini_parser::write_ini(_fname.c_str(), _root)`
- L108: `inline boolisLoaded() const{ return _loaded; }`

### `src/WTSUtils/WtLMDB.hpp` (400 行)
- 类型: WtLMDB:25, WtLMDBQuery:104
- L45: `inline MDB_env*env() const{ return _env; }`
- L46: `inline MDB_dbidbi() const { return _dbi; }`
- L86: `inline voidupdate_errno(int error) _errno = error; }`
- L88: `inline boolhas_error() const { return _errno != MDB_SUCCESS; }`
- L90: `inline boolis_readonly() const { return _readonly; }`
- L319: `std::reverse(ayKeys.begin(), ayKeys.end())`
- L320: `std::reverse(ayVals.begin(), ayVals.end())`

### `src/Share/WtKVCache.hpp` (277 行)
- 类型: WtKVCache:22

### `src/Share/BoostFile.hpp` (209 行)
- 类型: BoostFile:32
- L80: `bip::ipcdetail::close_file(_handle)`

### `src/Share/StdUtils.hpp` (147 行)
- 类型: StdLocker:35, StdFile:54
- L62: `throw std::runtime_error("File does not exist")`
- L71: `throw std::runtime_error("Offset exceeds file size")`
- L87: `std::ifstreamfile(filename, std::ios::binary)`
- L90: `throw std::runtime_error("Failed to open file")`
- L107: `throw std::runtime_error("Failed to read expected number of bytes")`
- L117: `std::ofstreamfile(filename, std::ios::binary)`
- L120: `throw std::runtime_error("Failed to open file for writing")`
- L128: `std::ofstreamfile(filename, std::ios::binary)`
- L131: `throw std::runtime_error("Failed to open file for writing")`

### `src/WTSTools/WTSLogger.h` (450 行)
- 类型: ILogHandler:32, WTSVariant:33, WTSLogger:49
- L57: `static voiddebug_imp(SpdLoggerPtr logger, const char* message)`
- L64: `static voidinfo_imp(SpdLoggerPtr logger, const char* message)`
- L71: `static voidwarn_imp(SpdLoggerPtr logger, const char* message)`
- L78: `static voiderror_imp(SpdLoggerPtr logger, const char* message)`
- L85: `static voidfatal_imp(SpdLoggerPtr logger, const char* message)`
- L92: `static voidinitLogger(const char* catName, WTSVariant* cfgLogger)`
- L100: `static SpdLoggerPtrgetLogger(const char* logger, const char* pattern = "")`
- L106: `static voidprint_message(const char* buffer)`
- L117: `static voidlog_raw(WTSLogLevel ll, const char* message)`
- L128: `static voidlog_raw_by_cat(const char* catName, WTSLogLevel ll, const char* message)`
- L140: `static voidlog_dyn_raw(const char* patttern, const char* catName, WTSLogLevel ll, const char* message)`
- L166: `fmtutil::format_to(m_buffer, format, args...)`
- L192: `fmtutil::format_to(m_buffer, format, args...)`
- L218: `fmtutil::format_to(m_buffer, format, args...)`
- L244: `fmtutil::format_to(m_buffer, format, args...)`
- L270: `fmtutil::format_to(m_buffer, format, args...)`
- L297: `fmtutil::format_to(m_buffer, format, args...)`
- L319: `fmtutil::format_to(m_buffer, format, args...)`
- L367: `fmtutil::format_to(m_buffer, format, args...)`
- L395: `fmtutil::format_to(s, format, args...)`
- L409: `static voidinit(const char* propFile = "logcfg.json", bool isFile = true, ILogHandler* handler = NULL)`
- L418: `static voidregisterHandler(ILogHandler* handler = NULL)`
- L425: `static voidstop()`
- L432: `static voidfreeAllDynLoggers()`

### `src/Share/ModuleHelper.hpp` (97 行)
- L48: `voidinst_hlp() }`

### `src/Share/DLLHelper.hpp` (90 行)
- 类型: DLLHelper:23
- L79: `std::stringret(name, idx)`

