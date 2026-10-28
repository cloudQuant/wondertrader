# API 简报 R22

测试目标: (主目标 CoverageTests)。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtDtPorter/WtDtRunner.cpp` (438 行)
- L52: `WTSLogger::error(message)`
- L60: `boost::asio::post(_async_io, [this, bAlldayMode]()`
- L63: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L71: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L82: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L90: `WTSLogger::init(logCfg, bLogCfgFile)`
- L91: `WtHelper::set_module_dir(modDir)`
- L110: `WTSLogger::info("Trading sessions loaded")`
- L148: `WTSLogger::info("Holidays loaded")`
- L155: `WTSLogger::log_raw(LL_INFO, "Hot rules loaded")`
- L161: `WTSLogger::log_raw(LL_INFO, "Second rules loaded")`
- L196: `WTSLogger::info("datakit will run in allday mode")`
- L252: `WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers")`
- L281: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory))`
- L295: `WTSLogger::info("Callbacks of Extented Parser registration done")`
- L351: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, &_data_mgr, &_idx_factory))`
- L363: `ExpDumperPtrdumper(new ExpDumper(id))`
- L389: `WTSLogger::error("Extended tick dumper not enabled")`
- L400: `WTSLogger::error("Extended bar dumper not enabled")`
- L411: `WTSLogger::error("Extended order detail dumper not enabled")`
- L422: `WTSLogger::error("Extended order queue dumper not enabled")`
- L433: `WTSLogger::error("Extended transaction dumper not enabled")`

### `src/WtDtPorter/WtDtPorter.cpp` (131 行)
- L40: `fs::pathp(MODULE_NAME)`
- L57: `CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd())`
- L87: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L91: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtDtPorter/ExpParser.h` (101 行)
- 类型: ExpParser:22
- L36: `virtual ~ExpParser() }`
- L45: `virtual boolinit(WTSVariant* config) override`
- L51: `virtual voidrelease() override`
- L58: `virtual boolconnect() override`
- L65: `virtual booldisconnect() override`
- L73: `virtual boolisConnected() override { return true; }`
- L80: `virtual voidsubscribe(const CodeSet& setCodes) override`
- L87: `virtual voidunsubscribe(const CodeSet& setCodes) override`
- L94: `virtual voidregisterSpi(IParserSpi* listener) override`

### `src/WtDtPorter/ExpDumper.h` (98 行)
- 类型: ExpDumper:23
- L37: `virtual ~ExpDumper() }`
- L49: `virtual booldumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) override`
- L60: `virtual booldumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) override`
- L71: `virtual booldumpHisOrdQue(const char* stdCode, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count) override`
- L82: `virtual booldumpHisOrdDtl(const char* stdCode, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count) override`
- L93: `virtual booldumpHisTrans(const char* stdCode, uint32_t uDate, WTSTransStruct* items, uint32_t count) override`

### `src/WtDtPorter/ExpParser.cpp` (107 行)
- L21: `extern WtDtRunner&getRunner()`

