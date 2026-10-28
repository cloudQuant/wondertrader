# API 简报 L08

覆盖模块: WtCtaStraFact, WtCtaTwoMaFact, WtExeFact, WtExecMon, WtHftStraFact, WtRiskMonFact, WtSelStraFact, WtUftStraFact。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WtCtaStraFact`

### `src/WtCtaStraFact/WtCtaStraFact.h` (77 行)
- 类型: WtStraFact:22
- L33: `virtual ~WtStraFact()`
- L43: `virtual const char*getName() override`
- L55: `virtual CtaStrategy*createStrategy(const char* name, const char* id) override`
- L64: `virtual voidenumStrategy(FuncEnumStrategyCallback cb) override`
- L75: `virtual booldeleteStrategy(CtaStrategy* stra) override`

### `src/WtCtaStraFact/WtStraDualThrust.h` (162 行)
- 类型: WtStraDualThrust:21
- L34: `virtual ~WtStraDualThrust()`
- L42: `virtual const char*getFactName() override`
- L49: `virtual const char*getName() override`
- L59: `virtual boolinit(WTSVariant* cfg) override`
- L70: `virtual voidon_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime) override`
- L79: `virtual voidon_init(ICtaStraCtx* ctx) override`
- L90: `virtual voidon_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override`
- L100: `virtual voidon_session_begin(ICtaStraCtx* ctx, uint32_t uTDate) override`

## `WtCtaTwoMaFact`

### `src/WtCtaTwoMaFact/WtCtaTwoMaFact.h` (77 行)
- 类型: WtStraFact:22
- L33: `virtual ~WtStraFact()`
- L43: `virtual const char*getName() override`
- L55: `virtual CtaStrategy*createStrategy(const char* name, const char* id) override`
- L64: `virtual voidenumStrategy(FuncEnumStrategyCallback cb) override`
- L75: `virtual booldeleteStrategy(CtaStrategy* stra) override`

### `src/WtCtaTwoMaFact/WtStraTwoMa.h` (148 行)
- 类型: WtStraTwoMa:20
- L33: `virtual ~WtStraTwoMa()`
- L41: `virtual const char*getFactName() override`
- L48: `virtual const char*getName() override`
- L58: `virtual boolinit(WTSVariant* cfg) override`
- L69: `virtual voidon_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime) override`
- L78: `virtual voidon_init(ICtaStraCtx* ctx) override`
- L89: `virtual voidon_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override`
- L99: `virtual voidon_session_begin(ICtaStraCtx* ctx, uint32_t uTDate) override`

## `WtExeFact`

### `src/WtExeFact/WtDiffMinImpactExeUnit.cpp` (522 行)
- L85: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L311: `CalcFlagflag(&_in_calc)`
- L329: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtDiffMinImpactExeUnit.h` (218 行)
- 类型: WtDiffMinImpactExeUnit:22
- L35: `virtual ~WtDiffMinImpactExeUnit()`
- L43: `voiddo_calc()`
- L51: `virtual const char*getFactName() override`
- L58: `virtual const char*getName() override`
- L67: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L79: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L86: `virtual voidon_tick(WTSTickData* newTick) override`
- L97: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L107: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L115: `virtual voidset_position(const char* stdCode, double newVol) override`
- L122: `virtual voidclear_all_position(const char* stdCode) override`
- L128: `virtual voidon_channel_ready() override`
- L134: `virtual voidon_channel_lost() override`
- L215: `operatorbool() const { return _result; }`

### `src/WtExeFact/WtExeFact.h` (73 行)
- 类型: WtExeFact:19
- L30: `virtual ~WtExeFact()`
- L37: `virtual const char*getName() override`
- L42: `virtual voidenumExeUnit(FuncEnumUnitCallback cb) override`
- L49: `virtual ExecuteUnit*createExeUnit(const char* name) override`
- L56: `virtual ExecuteUnit*createDiffExeUnit(const char* name) override`
- L63: `virtual ExecuteUnit*createArbiExeUnit(const char* name) override`
- L70: `virtual booldeleteExeUnit(ExecuteUnit* unit) override`

### `src/WtExeFact/WtMinImpactExeUnit.cpp` (600 行)
- L122: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L343: `CalcFlagflag(&_in_calc)`
- L355: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtMinImpactExeUnit.h` (222 行)
- 类型: WtMinImpactExeUnit:20
- L33: `virtual ~WtMinImpactExeUnit()`
- L41: `voiddo_calc()`
- L48: `virtual const char*getFactName() override`
- L54: `virtual const char*getName() override`
- L63: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L75: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L82: `virtual voidon_tick(WTSTickData* newTick) override`
- L93: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L103: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L111: `virtual voidset_position(const char* stdCode, double newVol) override`
- L118: `virtual voidclear_all_position(const char* stdCode) override`
- L124: `virtual voidon_channel_ready() override`
- L130: `virtual voidon_channel_lost() override`
- L219: `operatorbool() const { return _result; }`

### `src/WtExeFact/WtOrdMon.cpp` (92 行)
- L19: `StdLocker<StdRecurMutex>lock(_mtx_ords)`
- L36: `StdLocker<StdRecurMutex>lock(_mtx_ords)`
- L57: `StdLocker<StdRecurMutex>lock(_mtx_ords)`
- L82: `StdLocker<StdRecurMutex>lock(_mtx_ords)`

### `src/WtExeFact/WtOrdMon.h` (107 行)
- 类型: WtOrdMon:34
- L45: `voidpush_order(const uint32_t* ids, uint32_t cnt, uint64_t curTime, bool bCanCancel = true)`
- L52: `voiderase_order(uint32_t localid)`
- L79: `voidcheck_orders(uint32_t expiresecs, uint64_t curTime, EnumOrderCallback callback)`
- L95: `voidenumOrder(EnumAllOrderCallback cb)`

### `src/WtExeFact/WtStockMinImpactExeUnit.cpp` (721 行)
- L85: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L172: `StdUniqueLocklock(_mtx_calc)`
- L397: `StdUniqueLocklock(_mtx_calc)`
- L518: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtStockMinImpactExeUnit.h` (350 行)
- 类型: WtStockMinImpactExeUnit:49
- L91: `virtual ~WtStockMinImpactExeUnit()`
- L98: `voiddo_calc()`
- L105: `boolis_clear()`
- L112: `virtual const char*getFactName() override`
- L119: `virtual const char*getName() override`
- L128: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L140: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L147: `virtual voidon_tick(WTSTickData* newTick) override`
- L158: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L168: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L176: `virtual voidset_position(const char* stdCode, double newVol) override`
- L183: `virtual voidclear_all_position(const char* stdCode) override`
- L189: `virtual voidon_channel_ready() override`
- L195: `virtual voidon_channel_lost() override`
- L212: `virtual voidon_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override`
- L219: `voidcheck_unmanager_order()`

### `src/WtExeFact/WtStockVWapExeUnit.cpp` (836 行)
- L62: `inline doubleget_real_target(double _target)`
- L165: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L238: `stringstreams(line)`
- L485: `CalcFlagflag(&_in_calc)`
- L490: `StdUniqueLocklock(_mtx_calc)`
- L822: `void WtStockVWapExeUnit::clear_all_position(const char* stdCode)`

### `src/WtExeFact/WtStockVWapExeUnit.h` (389 行)
- 类型: WtStockVWapExeUnit:26
- L67: `virtual ~WtStockVWapExeUnit()`
- L75: `voiddo_calc()`
- L82: `voidfire_at_once(double qty)`
- L92: `virtual const char*getFactName() override`
- L101: `virtual const char*getName() override`
- L115: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L136: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L147: `virtual voidon_tick(WTSTickData* newTick) override`
- L165: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L178: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L191: `virtual voidset_position(const char* stdCode, double newVol) override`
- L200: `virtual voidon_channel_ready() override`
- L209: `virtual voidon_channel_lost() override`
- L216: `virtual voidclear_all_position(const char* stdCode) override`
- L338: `operatorbool() const { return _result; }`

### `src/WtExeFact/WtTWapExeUnit.cpp` (799 行)
- L74: `inline doubleget_real_target(double _target)`
- L133: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L547: `CalcFlagflag(&_in_calc)`
- L560: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtTWapExeUnit.h` (272 行)
- 类型: WtTWapExeUnit:21
- L34: `virtual ~WtTWapExeUnit()`
- L42: `voiddo_calc()`
- L49: `voidfire_at_once(double qty)`
- L57: `virtual const char*getFactName() override`
- L64: `virtual const char*getName() override`
- L74: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L87: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L95: `virtual voidon_tick(WTSTickData* newTick) override`
- L106: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L117: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L126: `virtual voidset_position(const char* stdCode, double newVol) override`
- L133: `virtual voidon_channel_ready() override`
- L140: `virtual voidon_channel_lost() override`
- L269: `operatorbool() const { return _result; }`

### `src/WtExeFact/WtVWapExeUnit.cpp` (513 行)
- L46: `inline doubleget_real_target(double _target)`
- L101: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L138: `stringstreams(line)`
- L301: `CalcFlagflag(&_in_calc)`
- L305: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExeFact/WtVWapExeUnit.h` (262 行)
- 类型: WtVWapExeUnit:22
- L35: `virtual ~WtVWapExeUnit()`
- L43: `voiddo_calc()`
- L50: `voidfire_at_once(double qty)`
- L58: `virtual const char*getFactName() override`
- L65: `virtual const char*getName() override`
- L76: `virtual voidinit(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg) override`
- L90: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled) override`
- L99: `virtual voidon_tick(WTSTickData* newTick) override`
- L112: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L123: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L133: `virtual voidset_position(const char* stdCode, double newVol) override`
- L140: `virtual voidon_channel_ready() override`
- L147: `virtual voidon_channel_lost() override`
- L260: `operatorbool() const { return _result; }`

## `WtExecMon`

### `src/WtExecMon/WtExecPorter.cpp` (161 行)
- L132: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L137: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtExecMon/WtExecPorter.h` (85 行)
- L31: `EXPORT_FLAG voidinit_exec(WtString logCfg, bool isFile = true)`
- L39: `EXPORT_FLAG voidconfig_exec(WtString cfgfile, bool isFile = true)`
- L45: `EXPORT_FLAG voidrun_exec()`
- L54: `EXPORT_FLAG voidwrite_log(unsigned int level, WtString message, WtString catName)`
- L61: `EXPORT_FLAG WtStringget_version()`
- L67: `EXPORT_FLAG voidrelease_exec()`
- L75: `EXPORT_FLAG voidset_position(WtString stdCode, double targetPos)`
- L81: `EXPORT_FLAG voidcommit_positions()`

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

### `src/WtExecMon/WtExecRunner.h` (217 行)
- 类型: WTSVariant:23, WtExecRunner:33
- L49: `boolinit(const char* logCfg = "logcfgexec.json", bool isFile = true)`
- L58: `boolconfig(const char* cfgFile, bool isFile = true)`
- L64: `voidrun()`
- L70: `voidrelease()`
- L78: `voidsetPosition(const char* stdCode, double targetPos)`
- L84: `voidcommitPositions()`
- L92: `booladdExeFactories(const char* folder)`
- L98: `IBaseDataMgr*get_bd_mgr() return &_bd_mgr; }`
- L104: `IHotMgr*get_hot_mgr() return &_hot_mgr; }`
- L113: `WTSSessionInfo*get_session_info(const char* sid, bool isCode = true)`
- L121: `virtual voidhandle_push_quote(WTSTickData* curTick) override`
- L130: `virtual uint64_tget_real_time() override`
- L138: `virtual WTSCommodityInfo*get_comm_info(const char* stdCode) override`
- L146: `virtual WTSSessionInfo*get_sess_info(const char* stdCode) override`
- L153: `virtual IHotMgr*get_hot_mon() override { return &_hot_mgr; }`
- L160: `virtual uint32_tget_trading_day() override`
- L169: `boolinitTraders(WTSVariant* cfgTrader)`
- L177: `boolinitParsers(WTSVariant* cfgParser)`
- L185: `boolinitExecuters(WTSVariant* cfgExecuter)`
- L192: `boolinitDataMgr()`
- L199: `boolinitActionPolicy()`

### `src/WtExecMon/WtSimpDataMgr.cpp` (233 行)
- L65: `DLLHelper::free_library(hInst)`
- L73: `DLLHelper::free_library(hInst)`
- L122: `WTSLogger::log_raw(ll, message)`

### `src/WtExecMon/WtSimpDataMgr.h` (187 行)
- 类型: WtExecRunner:15, WTSVariant:18, WTSHisTickData:19, WTSKlineData:20, WTSTickData:21, WTSKlineSlice:22, WTSTickSlice:23, IBaseDataMgr:24, IBaseDataMgr:25, WTSSessionInfo:26, WtSimpDataMgr:32
- L51: `boolinitStore(WTSVariant* cfg)`
- L60: `boolinit(WTSVariant* cfg, WtExecRunner* runner)`
- L67: `voidhandle_push_quote(const char* stdCode, WTSTickData* newTick)`
- L78: `WTSTickSlice*get_tick_slice(const char* code, uint32_t count, uint64_t etime = 0) override`
- L89: `WTSKlineSlice*get_kline_slice(const char* code, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override`
- L96: `WTSTickData*grab_last_tick(const char* code) override`
- L106: `virtual voidon_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar) override`
- L112: `virtual voidon_all_bar_updated(uint32_t updateTime) override`
- L118: `virtual IBaseDataMgr*get_basedata_mgr() override`
- L124: `virtual IHotMgr*get_hot_mgr() override`
- L130: `virtual uint32_tget_date() override`
- L136: `virtual uint32_tget_min_time() override`
- L142: `virtual uint32_tget_secs() override`
- L149: `virtual voidreader_log(WTSLogLevel ll, const char* message) override`
- L155: `inline IDataReader*reader() return _reader; }`
- L161: `inline uint32_tget_raw_time() const { return _cur_raw_time; }`
- L167: `inline uint32_tget_trading_day() const { return _cur_tdate; }`

## `WtHftStraFact`

### `src/WtHftStraFact/WtHftStraDemo.h` (188 行)
- 类型: WtHftStraDemo:22
- L43: `voidcheck_orders()`
- L50: `voiddo_calc(IHftStraCtx* ctx)`
- L58: `virtual const char*getName() override`
- L65: `virtual const char*getFactName() override`
- L73: `virtual boolinit(WTSVariant* cfg) override`
- L80: `virtual voidon_init(IHftStraCtx* ctx) override`
- L89: `virtual voidon_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick) override`
- L100: `virtual voidon_bar(IHftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L113: `virtual voidon_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double qty, double price, const char* userTag) override`
- L126: `virtual voidon_position(IHftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail) override`
- L141: `virtual voidon_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled, const char* userTag) override`
- L148: `virtual voidon_channel_ready(IHftStraCtx* ctx) override`
- L155: `virtual voidon_channel_lost(IHftStraCtx* ctx) override`
- L165: `virtual voidon_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag) override`

### `src/WtHftStraFact/WtHftStraFact.h` (65 行)
- 类型: WtHftStraFact:18
- L31: `virtual ~WtHftStraFact()`
- L39: `virtual const char*getName() override`
- L46: `virtual voidenumStrategy(FuncEnumHftStrategyCallback cb) override`
- L55: `virtual HftStrategy*createStrategy(const char* name, const char* id) override`
- L63: `virtual booldeleteStrategy(HftStrategy* stra) override`

## `WtRiskMonFact`

### `src/WtRiskMonFact/WtRiskMonFact.h` (69 行)
- 类型: WtRiskMonFact:22
- L35: `virtual ~WtRiskMonFact()`
- L43: `virtual const char*getName() override`
- L50: `virtual voidenumRiskMonitors(FuncEnumRiskMonCallback cb) override`
- L58: `virtual WtRiskMonitor*createRiskMonotor(const char* name) override`
- L66: `virtual booldeleteRiskMonotor(WtRiskMonitor* unit) override`

### `src/WtRiskMonFact/WtSimpRiskMon.cpp` (192 行)
- L61: `WtRiskMonitor::init(ctx, cfg)`
- L171: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`

### `src/WtRiskMonFact/WtSimpRiskMon.h` (108 行)
- 类型: WtSimpleRiskMon:24
- L38: `virtual const char*getName() override`
- L44: `virtual const char*getFactName() override`
- L52: `virtual voidinit(WtPortContext* ctx, WTSVariant* cfg) override`
- L58: `virtual voidrun() override`
- L64: `virtual voidstop() override`

## `WtSelStraFact`

### `src/WtSelStraFact/WtSelStraFact.h` (60 行)
- 类型: WtSelStraFact:19
- L30: `virtual ~WtSelStraFact()`
- L37: `virtual const char*getName() override`
- L45: `virtual SelStrategy*createStrategy(const char* name, const char* id) override`
- L51: `virtual voidenumStrategy(FuncEnumSelStrategyCallback cb) override`
- L58: `virtual booldeleteStrategy(SelStrategy* stra) override`

### `src/WtSelStraFact/WtStraDtSel.h` (106 行)
- 类型: WtStraDtSel:21
- L40: `virtual const char*getName() override`
- L46: `virtual const char*getFactName() override`
- L53: `virtual boolinit(WTSVariant* cfg) override`
- L59: `virtual voidon_init(ISelStraCtx* ctx) override`
- L67: `virtual voidon_schedule(ISelStraCtx* ctx, uint32_t uDate, uint32_t uTime) override`
- L75: `virtual voidon_tick(ISelStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override`
- L84: `virtual voidon_bar(ISelStraCtx* ctx, const char* stdCode, const char* period, WTSBarStruct* newBar) override`

## `WtUftStraFact`

### `src/WtUftStraFact/WtUftStraDemo.h` (203 行)
- 类型: WtUftStraDemo:25
- L46: `voidcheck_orders()`
- L54: `virtual const char*getName() override`
- L61: `virtual const char*getFactName() override`
- L69: `virtual boolinit(WTSVariant* cfg) override`
- L76: `virtual voidon_init(IUftStraCtx* ctx) override`
- L85: `virtual voidon_tick(IUftStraCtx* ctx, const char* code, WTSTickData* newTick) override`
- L96: `virtual voidon_bar(IUftStraCtx* ctx, const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L109: `virtual voidon_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double qty, double price) override`
- L122: `virtual voidon_position(IUftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail) override`
- L137: `virtual voidon_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled) override`
- L144: `virtual voidon_channel_ready(IUftStraCtx* ctx) override`
- L151: `virtual voidon_channel_lost(IUftStraCtx* ctx) override`
- L157: `virtual voidon_params_updated() override`
- L166: `virtual voidon_entrust(uint32_t localid, bool bSuccess, const char* message) override`

### `src/WtUftStraFact/WtUftStraFact.h` (66 行)
- 类型: WtUftStraFact:19
- L32: `virtual ~WtUftStraFact()`
- L40: `virtual const char*getName() override`
- L47: `virtual voidenumStrategy(FuncEnumUftStrategyCallback cb) override`
- L56: `virtual UftStrategy*createStrategy(const char* name, const char* id) override`
- L64: `virtual booldeleteStrategy(UftStrategy* stra) override`

