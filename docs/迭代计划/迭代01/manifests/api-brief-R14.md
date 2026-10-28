# API 简报 R14

测试目标: m_WtUftRunner。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtUftRunner/WtUftRunner.cpp` (387 行)
- L41: `WTSLogger::error(message)`
- L55: `WTSLogger::init(filename.c_str())`
- L57: `WtHelper::setInstDir(getBinDir())`
- L118: `ShareManager::self().set_engine(&_uft_engine)`
- L120: `ShareManager::self().initialize(cfg->getCString("module"))`
- L121: `ShareManager::self().init_domain(cfg->getCString("name"))`
- L126: `WTSLogger::error("ActionPolicyMgr init failed, please check config")`
- L143: `WTSLogger::error("Loading parsers failed")`
- L176: `WTSLogger::error("Loading traders failed")`
- L259: `WTSLogger::info("Trading enviroment initialzied with engine: UFT")`
- L275: `WTSLogger::info("Data manager initialized")`
- L302: `ParserAdapterPtradapter(new ParserAdapter)`
- L327: `TraderAdapterPtradapter(new TraderAdapter())`
- L360: `ShareManager::self().start_watching(2)`
- L364: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L370: `WTSLogger::error(message)`

### `src/WtLatencyHFT/HftLatencyTool.cpp` (507 行)
- 类型: TestParser:89, TestTrader:223, TestStrategy:272
- L287: `virtual const char*getName() override { return "TestStrategy"; }`
- L294: `virtual const char*getFactName() override { return "TestStrategyFact"; }`
- L350: `WTSLogger::init("logcfg.yaml")`
- L356: `WTSLogger::log_raw(LL_ERROR, "Loading config file config.yaml failed")`
- L401: `WTSLogger::log_raw(LL_INFO, "Hot rules loades")`
- L448: `WTSLogger::warn("Trading enviroment initialzied with engine: HFT")`
- L465: `ParserAdapterPtradapter(new ParserAdapter)`
- L472: `TraderAdapterPtradapter(new TraderAdapter())`

### `src/WtLatencyUFT/UftLatencyTool.cpp` (328 行)
- 类型: TestParser:38, TestTrader:134, TestStrategy:159
- L167: `virtual const char*getName() override { return "TestStrategy"; }`
- L172: `virtual const char*getFactName() override { return "TestStrategyFact"; }`
- L200: `WTSLogger::init("logcfg.yaml")`
- L278: `WTSLogger::warn("Trading enviroment initialzied with engine: UFT")`
- L290: `ParserAdapterPtradapter(new ParserAdapter)`
- L297: `TraderAdapterPtradapter(new TraderAdapter())`

### `src/QuoteFactory/main.cpp` (331 行)
- L88: `ParserAdapterPtradapter(new ParserAdapter(&g_baseDataMgr, &g_dataMgr, &g_idxFactory))`
- L98: `WtHelper::set_module_dir(getBinDir())`
- L112: `WTSLogger::info("Trading sessions loaded")`
- L150: `WTSLogger::info("Holidays loaded")`
- L155: `WTSLogger::log_raw(LL_INFO, "Hot rules loaded")`
- L161: `WTSLogger::log_raw(LL_INFO, "Second rules loaded")`
- L197: `WTSLogger::info("QuoteFactory will run in allday mode")`
- L259: `std::this_thread::sleep_for(std::chrono::milliseconds(5))`
- L266: `cppcli::Optionopt(argc, argv)`
- L283: `WTSLogger::init(filename.c_str())`
- L296: `CMiniDumper::Enable("QuoteFactory.exe", true)`
- L315: `WTSLogger::error(message)`
- L326: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`

