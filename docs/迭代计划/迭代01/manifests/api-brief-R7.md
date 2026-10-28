# API 简报 R7

测试目标: exefact。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtExeFact/WtStockVWapExeUnit.cpp` (836 行)
- L62: `inline doubleget_real_target(double _target)`
- L165: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L238: `stringstreams(line)`
- L485: `CalcFlagflag(&_in_calc)`
- L490: `StdUniqueLocklock(_mtx_calc)`
- L822: `void WtStockVWapExeUnit::clear_all_position(const char* stdCode)`

### `src/WtExeFact/WtStockMinImpactExeUnit.cpp` (721 行)
- L85: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L172: `StdUniqueLocklock(_mtx_calc)`
- L397: `StdUniqueLocklock(_mtx_calc)`
- L518: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtVWapExeUnit.cpp` (513 行)
- L46: `inline doubleget_real_target(double _target)`
- L101: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L138: `stringstreams(line)`
- L301: `CalcFlagflag(&_in_calc)`
- L305: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtTWapExeUnit.cpp` (799 行)
- L74: `inline doubleget_real_target(double _target)`
- L133: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L547: `CalcFlagflag(&_in_calc)`
- L560: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExecMon/WtExecRunner.cpp` (453 行)
- L28: `fs::pathp(MODULE_NAME)`
- L43: `CMiniDumper::Enable(getModuleName(), true, WtHelper::getCWD().c_str())`
- L49: `WTSLogger::init(path.c_str(), true)`
- L53: `WTSLogger::init(logCfg, false)`
- L57: `WtHelper::setInstDir(getBinDir())`
- L66: `WTSLogger::log_raw(LL_ERROR, "Loading config file failed")`
- L75: `WTSLogger::info("Trading sessions loaded")`
- L113: `WTSLogger::info("Holidays loaded")`
- L133: `WTSLogger::error("Loading parsers failed")`
- L151: `WTSLogger::error("Loading traders failed")`
- L168: `WTSLogger::error("Loading executers failed")`
- L191: `WTSLogger::error(message)`
- L220: `ParserAdapterPtradapter(new ParserAdapter)`
- L342: `TraderAdapterPtradapter(new TraderAdapter)`
- L362: `WTSLogger::info("Data Manager initialized")`
- L393: `WtHelper::setTime(uDate, curMin, curSec)`
- L394: `WtHelper::setTDate(quote->tradingdate())`
- L403: `WTSLogger::stop()`
- L425: `WTSLogger::info("Action policies initialized")`

