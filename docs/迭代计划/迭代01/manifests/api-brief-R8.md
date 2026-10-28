# API 简报 R8

测试目标: dtservo。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtDtServo/WtDtRunner.cpp` (648 行)
- L40: `extern const char*getModuleName()`
- L48: `WTSLogger::error("WtDtServo has already been initialized")`
- L55: `WTSLogger::init(logCfg)`
- L56: `WtHelper::set_module_dir(modDir)`
- L61: `WTSLogger::error("Loading config failed")`
- L62: `WTSLogger::log_raw(LL_INFO, cfgFile)`
- L69: `CMiniDumper::Enable(getModuleName(), true, WtHelper::get_cwd())`
- L77: `WTSLogger::info("Trading sessions loaded")`
- L115: `WTSLogger::info("Holidays loaded")`
- L121: `WTSLogger::info("Hot rules loaded")`
- L127: `WTSLogger::info("Second rules loaded")`
- L183: `WTSLogger::log_raw(LL_WARN, "No parsers config, skipped loading parsers")`
- L199: `WTSLogger::info("Data manager initialized")`
- L206: `WTSLogger::error("WtDtServo not initialized")`
- L246: `WTSLogger::error("WtDtServo not initialized")`
- L272: `WTSLogger::log_raw(LL_ERROR, "get_bars_by_date only supports minute period")`
- L288: `WTSLogger::error("WtDtServo not initialized")`
- L304: `WTSLogger::error("WtDtServo not initialized")`
- L315: `WTSLogger::error("WtDtServo not initialized")`
- L355: `WTSLogger::error("WtDtServo not initialized")`
- L371: `WTSLogger::error("WtDtServo not initialized")`
- L397: `ParserAdapterPtradapter(new ParserAdapter(&_bd_mgr, this))`
- L465: `StdUniqueLocklock(_mtx_subs)`
- L511: `StdUniqueLocklock(_mtx_innersubs)`
- L561: `StdUniqueLocklock(_mtx_innersubs)`
- L581: `StdUniqueLocklock(_mtx_subs)`

### `src/WtDtServo/WtDataManager.cpp` (617 行)
- L65: `WTSLogger::error("Initializing of random data reader failed: function createRdmDtReader not found...")`
- L71: `WTSLogger::error("Initializing of random data reader failed: function deleteRdmDtReader not found...")`
- L109: `WTSLogger::log_raw(ll, message)`
- L530: `StdUniqueLocklock(_mtx_rtbars)`
- L548: `StdUniqueLocklock(_mtx_rtbars)`
- L562: `StdUniqueLocklock(_mtx_rtbars)`
- L572: `StdUniqueLocklock(_mtx_rtbars)`
- L611: `WTSLogger::warn("DataReader not initialized, clearing canceled")`
- L616: `WTSLogger::warn("All cache cleared")`

### `src/WtDtServo/ParserAdapter.cpp` (347 行)
- L290: `WTSLogger::log_raw_by_cat("parser", ll, message)`

