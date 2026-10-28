# API 简报 R6

测试目标: porter, m_WtRunner。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtPorter/WtRtRunner.cpp` (1272 行)
- L47: `fs::pathp(MODULE_NAME)`
- L110: `CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str())`
- L116: `WTSLogger::init(path.c_str(), true, this)`
- L120: `WTSLogger::init(logCfg, false, this)`
- L124: `WtHelper::setInstDir(getBinDir())`
- L125: `WtHelper::setGenerateDir(StrUtil::standardisePath(genDir).c_str())`
- L143: `WTSLogger::info("Callbacks of Extented Parser registration done")`
- L151: `WTSLogger::info("Callbacks of Extented Executer registration done")`
- L164: `WTSLogger::info("Callbacks of CTA engine registration done")`
- L176: `WTSLogger::info("Callbacks of SEL engine registration done")`
- L200: `WTSLogger::info("Callbacks of HFT engine registration done")`
- L205: `StdUniqueLocklock(_feed_mtx)`
- L222: `WTSLogger::error("Unsupported period of extended data loader")`
- L230: `StdUniqueLocklock(_feed_mtx)`
- L247: `WTSLogger::error("Unsupported period of extended data loader")`
- L255: `StdUniqueLocklock(_feed_mtx)`
- L267: `StdUniqueLocklock(_feed_mtx)`
- L287: `WTSLogger::error("Cannot feed bars because of no extented bar loader registered.")`
- L297: `ParserAdapterPtradapter(new ParserAdapter)`
- L301: `WTSLogger::info("Extended parser created")`
- L310: `WTSLogger::info("Extended Executer created")`
- L498: `WTSLogger::info("Trading sessions loaded")`
- L536: `WTSLogger::log_raw(LL_INFO, "Holidays loaded")`
- L542: `WTSLogger::log_raw(LL_INFO, "Hot rules loaded")`
- L548: `WTSLogger::log_raw(LL_INFO, "Second rules loaded")`
- L607: `WTSLogger::error("Loading parsers failed")`
- L640: `WTSLogger::error("Loading traders failed")`
- L678: `WTSLogger::error("Loading executers failed")`
- L862: `WTSLogger::info("Trading environment initialized, engine name: HFT")`
- L868: `WTSLogger::info("Trading environment initialized, engine name: SEL")`
- L874: `WTSLogger::info("Trading environment initialized, engine name: CTA")`
- L894: `WTSLogger::log_raw(LL_INFO, "Data manager initialized")`
- L921: `ParserAdapterPtradapter(new ParserAdapter)`
- L1079: `TraderAdapterPtradapter(new TraderAdapter(&_notifier))`
- L1104: `WTSLogger::error(message)`
- L1114: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L1121: `WTSLogger::error(message)`
- L1154: `WTSLogger::stop()`
- L1164: `WTSLogger::info("Action policies initialized")`

### `src/WtRunner/WtRunner.cpp` (638 行)
- L57: `WTSLogger::init(filename.c_str())`
- L59: `WtHelper::setInstDir(getBinDir())`
- L176: `WTSLogger::error("Loading parsers failed")`
- L209: `WTSLogger::error("Loading traders failed")`
- L246: `WTSLogger::error("Loading executers failed")`
- L389: `WTSLogger::info("Trading enviroment initialzied with engine: HFT")`
- L395: `WTSLogger::info("Trading enviroment initialzied with engine: SEL")`
- L401: `WTSLogger::info("Trading enviroment initialzied with engine: CTA")`
- L423: `WTSLogger::info("Data manager initialized")`
- L450: `ParserAdapterPtradapter(new ParserAdapter)`
- L565: `TraderAdapterPtradapter(new TraderAdapter(&_notifier))`
- L592: `WTSLogger::error(message)`
- L602: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L609: `WTSLogger::error(message)`

