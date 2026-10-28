# API 简报 L02

覆盖模块: WTSTools, WtMsgQue, WtShareHelper。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WTSTools`

### `src/WTSTools/CsvHelper.cpp` (179 行)
- L32: `StrUtil::replace(row, "<", "")`
- L33: `StrUtil::replace(row, ">", "")`
- L34: `StrUtil::replace(row, "\"", "")`
- L35: `StrUtil::replace(row, "'", "")`
- L38: `StrUtil::toLowerCase(row)`
- L43: `StrUtil::trim(fields[i], " ")`
- L44: `StrUtil::trim(fields[i], "\n")`
- L45: `StrUtil::trim(fields[i], "\t")`
- L46: `StrUtil::trim(fields[i], "\r")`
- L73: `StrUtil::split(_buffer, _current_cells, _item_splitter.c_str())`

### `src/WTSTools/CsvHelper.h` (184 行)
- 类型: CsvReader:22
- L39: `boolload_from_file(const char* filename)`
- L46: `inline uint32_tcol_count() }`
- L53: `int32_tget_int32(int32_t col)`
- L60: `uint32_tget_uint32(int32_t col)`
- L67: `int64_tget_int64(int32_t col)`
- L74: `uint64_tget_uint64(int32_t col)`
- L81: `doubleget_double(int32_t col)`
- L88: `const char*get_string(int32_t col)`
- L95: `int32_tget_int32(const char* field)`
- L102: `uint32_tget_uint32(const char* field)`
- L109: `int64_tget_int64(const char* field)`
- L116: `uint64_tget_uint64(const char* field)`
- L123: `doubleget_double(const char* field)`
- L130: `const char*get_string(const char* field)`
- L138: `boolnext_row()`
- L168: `boolcheck_cell(int32_t col)`
- L175: `int32_tget_col_by_filed(const char* field)`

### `src/WTSTools/WTSBaseDataMgr.cpp` (888 行)
- L554: `std::sort(_code_list->begin(), _code_list->end(), [](WTSObject* left, WTSObject* right)`
- L677: `TimeUtils::getDateTime(uDate, uTime)`

### `src/WTSTools/WTSBaseDataMgr.h` (84 行)
- 类型: WTSBaseDataMgr:23
- L30: `virtual WTSCommodityInfo*getCommodity(const char* stdPID) override`
- L31: `virtual WTSCommodityInfo*getCommodity(const char* exchg, const char* pid) override`
- L33: `virtual WTSContractInfo*getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override`
- L34: `virtual WTSArray*getContracts(const char* exchg = "", uint32_t uDate = 0) override`
- L36: `virtual WTSSessionInfo*getSession(const char* sid) override`
- L37: `virtual WTSSessionInfo*getSessionByCode(const char* code, const char* exchg = "") override`
- L38: `virtual WTSArray*getAllSessions() override`
- L39: `virtual boolisHoliday(const char* stdPID, uint32_t uDate, bool isTpl = false) override`
- L41: `virtual uint32_tcalcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override`
- L42: `virtual uint64_tgetBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override`
- L44: `virtual uint32_tgetContractSize(const char* exchg = "", uint32_t uDate = 0) override`
- L46: `virtual uint32_tgetGlobalSize() }`
- L48: `virtual WTSContractInfo*getContractByIndex(uint32_t idx) override`
- L51: `boolloadSessions(const char* filename)`
- L52: `boolloadCommodities(const char* filename)`
- L53: `boolloadHolidays(const char* filename)`
- L55: `boolloadContracts(const char* filename)`
- L58: `uint32_tgetTradingDate(const char* stdPID, uint32_t uOffDate = 0, uint32_t uOffMinute = 0, bool isTpl = false)`
- L59: `uint32_tgetNextTDate(const char* stdPID, uint32_t uDate, int days = 1, bool isTpl = false)`
- L60: `uint32_tgetPrevTDate(const char* stdPID, uint32_t uDate, int days = 1, bool isTpl = false)`
- L61: `boolisTradingDate(const char* stdPID, uint32_t uDate, bool isTpl = false)`
- L62: `voidsetTradingDate(const char* stdPID, uint32_t uDate, bool isTpl = false)`
- L64: `CodeSet*getSessionComms(const char* sid)`
- L67: `const char*getTplIDByPID(const char* stdPID)`

### `src/WTSTools/WTSDataFactory.h` (86 行)
- 类型: WTSDataFactory:15
- L24: `virtual WTSBarStruct*updateKlineData(WTSKlineData* klineData, WTSTickData* tick, WTSSessionInfo* sInfo, bool bAlignSec = false)`
- L32: `virtual WTSBarStruct*updateKlineData(WTSKlineData* klineData, WTSBarStruct* newBasicBar, WTSSessionInfo* sInfo, bool bAlignSec = false)`
- L43: `virtual WTSKlineData*extractKlineData(WTSKlineSlice* baseKline, WTSKlinePeriod period, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false)`
- L53: `virtual WTSKlineData*extractKlineData(WTSTickSlice* ayTicks, uint32_t seconds, WTSSessionInfo* sInfo, bool bUnixTime = false, bool bAlignSec = false)`
- L60: `virtual boolmergeKlineData(WTSKlineData* klineData, WTSKlineData* newKline)`
- L63: `WTSBarStruct*updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec = false)`
- L64: `WTSBarStruct*updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec = false)`
- L66: `WTSBarStruct*updateDayData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick)`
- L67: `WTSBarStruct*updateSecData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick)`
- L69: `WTSBarStruct*updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec = false)`
- L70: `WTSBarStruct*updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec = false)`
- L72: `WTSBarStruct*updateHourData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar)`
- L73: `WTSBarStruct*updateHalfData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar)`
- L75: `WTSKlineData*extractMin1Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false)`
- L76: `WTSKlineData*extractMin5Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false)`
- L78: `WTSKlineData*extractHourData(WTSKlineSlice* baseKline, WTSSessionInfo* sInfo, bool bIncludeOpen = true)`
- L79: `WTSKlineData*extractHalfData(WTSKlineSlice* baseKline, WTSSessionInfo* sInfo, bool bIncludeOpen = true)`
- L81: `WTSKlineData*extractDayData(WTSKlineSlice* baseKline, uint32_t times, bool bIncludeOpen = true)`
- L84: `static uint32_tgetPrevMinute(uint32_t curMinute, int period = 1)`

### `src/WTSTools/WTSHotMgr.cpp` (484 行)
- L378: `std::stringfullPid(fullCode, idx)`

### `src/WTSTools/WTSHotMgr.h` (96 行)
- 类型: WTSSwitchItem:17, WTSHotMgr:32
- L39: `boolloadHots(const char* filename)`
- L40: `boolloadSeconds(const char* filename)`
- L41: `voidrelease()`
- L43: `boolloadCustomRules(const char* tag, const char* filename)`
- L45: `inline boolisInitialized() const {return m_bInitialized;}`
- L47: `const std::vector<std::string>&getHotTags() const`
- L50: `virtual const char*getRuleTag(const char* stdCode) override`
- L52: `virtual doublegetRuleFactor(const char* ruleTag, const char* fullPid, uint32_t uDate = 0) override`
- L56: `virtual const char*getRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override`
- L58: `virtual const char*getPrevRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override`
- L60: `virtual boolisHot(const char* exchg, const char* rawCode, uint32_t dt = 0) override`
- L62: `virtual boolsplitHotSecions(const char* exchg, const char* pid, uint32_t sDt, uint32_t eDt, HotSections& sections) override`
- L66: `virtual const char*getSecondRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override`
- L68: `virtual const char*getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override`
- L70: `virtual boolisSecond(const char* exchg, const char* rawCode, uint32_t dt = 0) override`
- L72: `virtual boolsplitSecondSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) override`
- L76: `virtual const char*getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt) override`
- L78: `virtual const char*getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt) override`
- L80: `virtual boolisCustomHot(const char* tag, const char* fullCode, uint32_t dt) override`
- L82: `virtual boolsplitCustomSections(const char* tag, const char* fullPid, uint32_t sDt, uint32_t eDt, HotSections& sections) override`

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

## `WtMsgQue`

### `src/WtMsgQue/MQClient.cpp` (199 行)
- L160: `std::this_thread::sleep_for(std::chrono::milliseconds(1))`

### `src/WtMsgQue/MQClient.h` (162 行)
- 类型: MQManager:19, MQClient:27
- L49: `voidextract_buffer()`
- L76: `inline uint32_tid() const { return _id; }`
- L86: `boolinit(const char* url, FuncMQCallback cb)`
- L94: `voidstart()`

### `src/WtMsgQue/MQManager.cpp` (106 行)
- L8: `MQServerPtrserver(new MQServer(this))`
- L60: `MQClientPtrclient(new MQClient(this))`

### `src/WtMsgQue/MQManager.h` (159 行)
- 类型: MQManager:47
- L62: `inline voidregiter_callbacks(FuncLogCallback cbLog) _cb_log = cbLog; }`
- L71: `WtUInt32create_server(const char* url, bool confirm)`
- L78: `voiddestroy_server(WtUInt32 id)`
- L88: `voidpublish_message(WtUInt32 id, const char* topic, const void* data, WtUInt32 dataLen)`
- L97: `WtUInt32create_client(const char* url, FuncMQCallback cb)`
- L104: `voiddestroy_client(WtUInt32 id)`
- L112: `voidsub_topic(WtUInt32 id, const char* topic)`
- L119: `voidstart_client(WtUInt32 id)`
- L127: `voidlog_server(WtUInt32 id, const char* message)`
- L135: `voidlog_client(WtUInt32 id, const char* message)`

### `src/WtMsgQue/MQServer.cpp` (249 行)
- L121: `SpinLocklock(m_mtxCast)`
- L140: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L148: `SpinLocklock(m_mtxCast)`
- L162: `SpinLocklock(m_mtxCast)`

### `src/WtMsgQue/MQServer.h` (72 行)
- 类型: MQManager:19, MQServer:21
- L28: `inline uint32_tid() const { return _id; }`
- L30: `boolinit(const char* url, bool confirm = false)`
- L32: `voidpublish(const char* topic, const void* data, uint32_t dataLen)`

### `src/WtMsgQue/PorterDefs.h` (31 行)
- L22: `typedefvoid(PORTER_FLAG *FuncMQCallback)(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen)`
- L31: `typedefvoid(PORTER_FLAG *FuncLogCallback)(WtUInt32 id, const char* message, bool bServer)`

### `src/WtMsgQue/WtMsgQue.h` (81 行)
- L23: `EXPORT_FLAG voidregiter_callbacks(FuncLogCallback cbLog)`
- L32: `EXPORT_FLAG WtUInt32create_server(const char* url, bool confirm)`
- L38: `EXPORT_FLAG voiddestroy_server(WtUInt32 id)`
- L47: `EXPORT_FLAG voidpublish_message(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen)`
- L56: `EXPORT_FLAG WtUInt32create_client(const char* url, FuncMQCallback cb)`
- L63: `EXPORT_FLAG voiddestroy_client(WtUInt32 id)`
- L71: `EXPORT_FLAG voidsubscribe_topic(WtUInt32 id, const char* topic)`
- L78: `EXPORT_FLAG voidstart_client(WtUInt32 id)`

## `WtShareHelper`

### `src/WtShareHelper/ShareBlocks.h` (214 行)
- 类型: _CmdBlock:91, ShareBlocks:107
- L119: `typedefvoid(*FuncLogger)(uint32_t, const char*)`
- L135: `boolinit_master(const char* name, const char* path = "")`
- L136: `boolinit_slave(const char* name, const char* path = "")`
- L138: `boolupdate_slave(const char* name, bool bForce)`
- L139: `boolrelease_slave(const char* name)`
- L141: `std::vector<std::string>get_sections(const char* domain)`
- L142: `std::vector<KeyInfo*>get_keys(const char* domain, const char* section)`
- L144: `uint64_tget_section_updatetime(const char* domain, const char* section)`
- L145: `boolcommit_section(const char* domain, const char* section)`
- L147: `booldelete_section(const char* domain, const char*section)`
- L149: `const char*allocate_string(const char* domain, const char* section, const char* key, const char* initVal = "", bool bForceWrite = false)`
- L150: `int32_t*allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal = 0, bool bForceWrite = false)`
- L151: `int64_t*allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal = 0, bool bForceWrite = false)`
- L152: `uint32_t*allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal = 0, bool bForceWrite = false)`
- L153: `uint64_t*allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal = 0, bool bForceWrite = false)`
- L154: `double*allocate_double(const char* domain, const char* section, const char* key, double initVal = 0, bool bForceWrite = false)`
- L156: `boolset_string(const char* domain, const char* section, const char* key, const char* val)`
- L157: `boolset_int32(const char* domain, const char* section, const char* key, int32_t val)`
- L158: `boolset_int64(const char* domain, const char* section, const char* key, int64_t val)`
- L159: `boolset_uint32(const char* domain, const char* section, const char* key, uint32_t val)`
- L160: `boolset_uint64(const char* domain, const char* section, const char* key, uint64_t val)`
- L161: `boolset_double(const char* domain, const char* section, const char* key, double val)`
- L163: `const char*get_string(const char* domain, const char* section, const char* key, const char* defVal = "")`
- L164: `int32_tget_int32(const char* domain, const char* section, const char* key, int32_t defVal = 0)`
- L165: `int64_tget_int64(const char* domain, const char* section, const char* key, int64_t defVal = 0)`
- L166: `uint32_tget_uint32(const char* domain, const char* section, const char* key, uint32_t defVal = 0)`
- L167: `uint64_tget_uint64(const char* domain, const char* section, const char* key, uint64_t defVal = 0)`
- L168: `doubleget_double(const char* domain, const char* section, const char* key, double defVal = 0)`
- L171: `boolinit_cmder(const char* name, bool isCmder = false, const char* path = "")`
- L172: `booladd_cmd(const char* name, const char* cmd)`
- L173: `const char*get_cmd(const char* name, uint32_t& lastIdx)`
- L176: `void*make_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)`
- L177: `void*check_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)`

### `src/WtShareHelper/WtShareHelper.h` (80 行)
- L6: `typedefvoid(PORTER_FLAG *FuncGetSections)(const char*)`
- L8: `typedefvoid(PORTER_FLAG *FuncGetKeys)(const char*, uint64_t)`
- L10: `typedefvoid(PORTER_FLAG *FuncLogger)(uint32_t, const char*)`
- L17: `EXPORT_FLAG boolinit_master(const char* id, const char* path = "")`
- L19: `EXPORT_FLAG boolinit_slave(const char* id, const char* path = "")`
- L21: `EXPORT_FLAG boolupdate_slave(const char* id, bool bForce = false)`
- L23: `EXPORT_FLAG boolrelease_slave(const char* name)`
- L25: `EXPORT_FLAG voidregister_logger(FuncLogger logger)`
- L27: `EXPORT_FLAG uint32_tget_sections(const char* domain, FuncGetSections cb)`
- L29: `EXPORT_FLAG uint32_tget_keys(const char* domain, const char* section, FuncGetKeys cb)`
- L31: `EXPORT_FLAG uint64_tget_section_updatetime(const char* domain, const char* section)`
- L33: `EXPORT_FLAG boolcommit_section(const char* domain, const char* section)`
- L35: `EXPORT_FLAG booldelete_section(const char* domain, const char*section)`
- L37: `EXPORT_FLAG const char*allocate_string(const char* domain, const char* section, const char* key, const char* initVal, bool bForceWrite = false)`
- L39: `EXPORT_FLAG int32_t*allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal, bool bForceWrite = false)`
- L41: `EXPORT_FLAG int64_t*allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal, bool bForceWrite = false)`
- L43: `EXPORT_FLAG uint32_t*allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal, bool bForceWrite = false)`
- L45: `EXPORT_FLAG uint64_t*allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal, bool bForceWrite = false)`
- L47: `EXPORT_FLAG double*allocate_double(const char* domain, const char* section, const char* key, double initVal, bool bForceWrite = false)`
- L49: `EXPORT_FLAG boolset_string(const char* domain, const char* section, const char* key, const char* val)`
- L51: `EXPORT_FLAG boolset_int32(const char* domain, const char* section, const char* key, int32_t val)`
- L53: `EXPORT_FLAG boolset_int64(const char* domain, const char* section, const char* key, int64_t val)`
- L55: `EXPORT_FLAG boolset_uint32(const char* domain, const char* section, const char* key, uint32_t val)`
- L57: `EXPORT_FLAG boolset_uint64(const char* domain, const char* section, const char* key, uint64_t val)`
- L59: `EXPORT_FLAG boolset_double(const char* domain, const char* section, const char* key, double val)`
- L61: `EXPORT_FLAG const char*get_string(const char* domain, const char* section, const char* key, const char* defVal = "")`
- L63: `EXPORT_FLAG int32_tget_int32(const char* domain, const char* section, const char* key, int32_t defVal = 0)`
- L65: `EXPORT_FLAG int64_tget_int64(const char* domain, const char* section, const char* key, int64_t defVal = 0)`
- L67: `EXPORT_FLAG uint32_tget_uint32(const char* domain, const char* section, const char* key, uint32_t defVal = 0)`
- L69: `EXPORT_FLAG uint64_tget_uint64(const char* domain, const char* section, const char* key, uint64_t defVal = 0)`
- L71: `EXPORT_FLAG doubleget_double(const char* domain, const char* section, const char* key, double defVal = 0)`
- L74: `EXPORT_FLAG boolinit_cmder(const char* name, bool isCmder = false, const char* path = "")`
- L75: `EXPORT_FLAG booladd_cmd(const char* name, const char* cmd)`
- L76: `EXPORT_FLAG const char*get_cmd(const char* name, uint32_t& lastIdx)`

