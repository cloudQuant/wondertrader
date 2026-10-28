# API 简报 L07

覆盖模块: WtCore。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `WtCore`

### `src/WtCore/ActionPolicyMgr.h` (115 行)
- 类型: WTSVariant:22, ActionPolicyMgr:71
- L93: `boolinit(const char* filename)`
- L101: `const ActionRuleGroup&getActionRules(const char* pid)`

### `src/WtCore/CtaStraBaseCtx.cpp` (2063 行)
- L82: `fs::create_directories(folder.c_str())`
- L221: `rj::Documentroot(rj::kObjectType)`
- L238: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L259: `StdFile::read_file_content(filename.c_str(), content)`
- L289: `StdFile::read_file_content(filename.c_str(), content)`
- L497: `rj::Documentroot(rj::kObjectType)`
- L500: `rj::ValuejPos(rj::kArrayType)`
- L509: `rj::ValuepItem(rj::kObjectType)`
- L519: `rj::Valuedetails(rj::kArrayType)`
- L523: `rj::ValuedItem(rj::kObjectType)`
- L550: `rj::ValuejFund(rj::kObjectType)`
- L562: `rj::ValuejSigs(rj::kObjectType)`
- L570: `rj::ValuejItem(rj::kObjectType)`
- L584: `rj::ValuejCond(rj::kObjectType)`
- L585: `rj::ValuejItems(rj::kObjectType)`
- L594: `rj::ValuecArray(rj::kArrayType)`
- L597: `rj::ValuecItem(rj::kObjectType)`
- L619: `rj::ValuejUtils(rj::kObjectType)`
- L637: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L681: `rj::Documentroot(rj::kObjectType)`
- L684: `rj::ValueklineItem(rj::kObjectType)`
- L701: `rj::ValuejIndice(rj::kArrayType)`
- L705: `rj::ValuejIndex(rj::kObjectType)`
- L709: `rj::ValuejLines(rj::kArrayType)`
- L713: `rj::ValuejLine(rj::kObjectType)`
- L722: `rj::ValuejBaseLines(rj::kObjectType)`
- L741: `fs::create_directories(folder.c_str())`
- L747: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L749: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L954: `std::stringstdCode(key, idx)`
- L1048: `SpinLocklock(_mutex)`
- L1432: `SpinLocklock(_mutex)`
- L1728: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L1733: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L1738: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L1743: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L1976: `WTSLogger::error("Marks can be added only during schedule")`
- L2032: `WTSLogger::error("Marks can be added only during schedule")`

### `src/WtCore/CtaStraBaseCtx.h` (1179 行)
- 类型: CtaStrategy:23, WtCtaEngine:27, CtaStraBaseCtx:79
- L93: `virtual ~CtaStraBaseCtx()`
- L100: `voidinit_outputs()`
- L110: `inline voidlog_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "")`
- L124: `inline voidlog_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0, uint32_t barNo = 0)`
- L149: `voidsave_data(uint32_t flag = 0xFFFFFFFF)`
- L155: `voidload_data(uint32_t flag = 0xFFFFFFFF)`
- L161: `voidload_userdata()`
- L167: `voidsave_userdata()`
- L174: `voidupdate_dyn_profit(const char* stdCode, double price)`
- L183: `voiddo_set_position(const char* stdCode, double qty, const char* userTag = "", bool bFireAtOnce = false)`
- L192: `voidappend_signal(const char* stdCode, double qty, const char* userTag = "", uint32_t sigType = 0)`
- L199: `inline CondList&get_cond_entrusts(const char* stdCode)`
- L242: `voiddump_chart_info()`
- L249: `virtual uint32_tid() override { return _context_id; }`
- L256: `virtual voidon_init() override`
- L262: `virtual voidon_session_begin(uint32_t uTDate) override`
- L268: `virtual voidon_session_end(uint32_t uTDate) override`
- L276: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override`
- L285: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L293: `virtual boolon_schedule(uint32_t curDate, uint32_t curTime) override`
- L300: `virtual voidenum_position(FuncEnumCtaPosCallBack cb, bool bForExecute = false) override`
- L313: `virtual voidstra_enter_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override`
- L323: `virtual voidstra_enter_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override`
- L333: `virtual voidstra_exit_long(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override`
- L343: `virtual voidstra_exit_short(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override`
- L352: `virtual doublestra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override`
- L362: `virtual voidstra_set_position(const char* stdCode, double qty, const char* userTag = "", double limitprice = 0.0, double stopprice = 0.0) override`
- L369: `virtual doublestra_get_price(const char* stdCode) override`
- L377: `virtual doublestra_get_day_price(const char* stdCode, int flag = 0) override`
- L383: `virtual uint32_tstra_get_tdate() override`
- L389: `virtual uint32_tstra_get_date() override`
- L395: `virtual uint32_tstra_get_time() override`
- L402: `virtual doublestra_get_fund_data(int flag /* = 0 */) override`
- L409: `virtual uint64_tstra_get_first_entertime(const char* stdCode) override`
- L416: `virtual uint64_tstra_get_last_entertime(const char* stdCode) override`
- L423: `virtual uint64_tstra_get_last_exittime(const char* stdCode) override`
- L430: `virtual doublestra_get_last_enterprice(const char* stdCode) override`
- L437: `virtual doublestra_get_position_avgpx(const char* stdCode) override`
- L444: `virtual doublestra_get_position_profit(const char* stdCode) override`
- L452: `virtual uint64_tstra_get_detail_entertime(const char* stdCode, const char* userTag) override`
- L460: `virtual doublestra_get_detail_cost(const char* stdCode, const char* userTag) override`
- L469: `virtual doublestra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override`
- L476: `virtual WTSCommodityInfo*stra_get_comminfo(const char* stdCode) override`
- L486: `virtual WTSKlineSlice*stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain = false) override`
- L494: `virtual WTSTickSlice*stra_get_ticks(const char* stdCode, uint32_t count) override`
- L501: `virtual WTSTickData*stra_get_last_tick(const char* stdCode) override`
- L509: `virtual std::stringstra_get_rawcode(const char* stdCode) override`
- L515: `virtual voidstra_sub_ticks(const char* stdCode) override`
- L522: `virtual voidstra_sub_bar_events(const char* stdCode, const char* period) override`
- L528: `virtual voidstra_log_info(const char* message) override`
- L534: `virtual voidstra_log_debug(const char* message) override`
- L540: `virtual voidstra_log_warn(const char* message) override`
- L546: `virtual voidstra_log_error(const char* message) override`
- L553: `virtual voidstra_save_user_data(const char* key, const char* val) override`
- L561: `virtual const char*stra_load_user_data(const char* key, const char* defVal = "") override`
- L568: `virtual const char*stra_get_last_entertag(const char* stdCode) override`
- L577: `virtual voidset_chart_kline(const char* stdCode, const char* period) override`
- L586: `virtual voidadd_chart_mark(double price, const char* icon, const char* tag) override`
- L594: `virtual voidregister_index(const char* idxName, uint32_t indexType) override`
- L604: `virtual boolregister_index_line(const char* idxName, const char* lineName, uint32_t lineType) override`
- L614: `virtual booladd_index_baseline(const char* idxName, const char* lineName, double val) override`
- L624: `virtual boolset_index_value(const char* idxName, const char* lineName, double val) override`

### `src/WtCore/CtaStraContext.cpp` (147 行)
- L66: `CtaStraBaseCtx::on_init()`
- L83: `CtaStraBaseCtx::on_session_begin(uTDate)`
- L101: `CtaStraBaseCtx::on_session_end(uTDate)`

### `src/WtCore/CtaStraContext.h` (124 行)
- 类型: WtCtaEngine:19, CtaStrategy:24, CtaStraContext:32
- L46: `virtual ~CtaStraContext()`
- L53: `voidset_strategy(CtaStrategy* stra) _strategy = stra; }`
- L59: `CtaStrategy*get_stragety() return _strategy; }`
- L71: `virtual voidon_init() override`
- L77: `virtual voidon_session_begin(uint32_t uTDate) override`
- L83: `virtual voidon_session_end(uint32_t uTDate) override`
- L90: `virtual voidon_tick_updated(const char* stdCode, WTSTickData* newTick) override`
- L98: `virtual voidon_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override`
- L105: `virtual voidon_calculate(uint32_t curDate, uint32_t curTime) override`
- L114: `virtual voidon_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override`

### `src/WtCore/CtaStrategyMgr.cpp` (134 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L102: `CtaStrategyPtrret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L121: `CtaStrategyPtrret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

### `src/WtCore/CtaStrategyMgr.h` (144 行)
- 类型: CtaStraWrapper:25, CtaStrategyMgr:69
- L51: `CtaStrategy*self() return _stra; }`
- L88: `boolloadFactories(const char* path)`
- L97: `CtaStrategyPtrcreateStrategy(const char* name, const char* id)`
- L106: `CtaStrategyPtrcreateStrategy(const char* factname, const char* unitname, const char* id)`
- L113: `CtaStrategyPtrgetStrategy(const char* id)`

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

### `src/WtCore/EventNotifier.h` (70 行)
- 类型: WTSTradeInfo:27, WTSOrderInfo:28, WTSVariant:29, EventNotifier:31
- L18: `typedef unsignedlong(*FuncCreateMQServer)(const char*)`
- L19: `typedefvoid(*FuncDestroyMQServer)(unsigned long)`
- L20: `typedefvoid(*FundPublishMessage)(unsigned long, const char*, const char*, unsigned long)`
- L22: `typedefvoid(*FuncLogCallback)(unsigned long, const char*, bool)`
- L23: `typedefvoid(*FuncRegCallbacks)(FuncLogCallback)`
- L39: `voidtradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output)`
- L40: `voidorderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output)`
- L43: `boolinit(WTSVariant* cfg)`
- L45: `voidnotify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)`
- L46: `voidnotify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)`
- L47: `voidnotify(const char* trader, const char* message)`
- L49: `voidnotify_log(const char* tag, const char* message)`
- L51: `voidnotify_event(const char* message)`
- L53: `voidnotify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag)`
- L54: `voidnotify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val)`
- L55: `voidnotify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag)`

### `src/WtCore/HftStraBaseCtx.cpp` (987 行)
- L73: `fs::create_directories(folder.c_str())`
- L518: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L523: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L528: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L533: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`
- L729: `rj::Documentroot(rj::kObjectType)`
- L746: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L767: `StdFile::read_file_content(filename.c_str(), content)`

### `src/WtCore/HftStraBaseCtx.h` (802 行)
- 类型: WtHftEngine:26, TraderAdapter:27, HftStraBaseCtx:39
- L56: `virtual ~HftStraBaseCtx()`
- L63: `voidsetTrader(TraderAdapter* trader)`
- L73: `virtual uint32_tid() override`
- L79: `virtual voidon_init() override`
- L87: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick) override`
- L95: `virtual voidon_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override`
- L103: `virtual voidon_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override`
- L111: `virtual voidon_transaction(const char* stdCode, WTSTransData* newTrans) override`
- L121: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L128: `virtual voidon_session_begin(uint32_t uTDate) override`
- L135: `virtual voidon_session_end(uint32_t uTDate) override`
- L143: `virtual boolstra_cancel(uint32_t localid) override`
- L153: `virtual OrderIDsstra_cancel(const char* stdCode, bool isBuy, double qty) override`
- L166: `virtual OrderIDsstra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override`
- L179: `virtual OrderIDsstra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag = 0, bool bForceClose = false) override`
- L191: `virtual uint32_tstra_enter_long(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) override`
- L203: `virtual uint32_tstra_enter_short(const char* stdCode, double price, double qty, const char* userTag, int flag = 0) override`
- L216: `virtual uint32_tstra_exit_long(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) override`
- L229: `virtual uint32_tstra_exit_short(const char* stdCode, double price, double qty, const char* userTag, bool isToday = false, int flag = 0) override`
- L237: `virtual WTSCommodityInfo*stra_get_comminfo(const char* stdCode) override`
- L247: `virtual WTSKlineSlice*stra_get_bars(const char* stdCode, const char* period, uint32_t count) override`
- L256: `virtual WTSTickSlice*stra_get_ticks(const char* stdCode, uint32_t count) override`
- L265: `virtual WTSOrdDtlSlice*stra_get_order_detail(const char* stdCode, uint32_t count) override`
- L274: `virtual WTSOrdQueSlice*stra_get_order_queue(const char* stdCode, uint32_t count) override`
- L283: `virtual WTSTransSlice*stra_get_transaction(const char* stdCode, uint32_t count) override`
- L291: `virtual WTSTickData*stra_get_last_tick(const char* stdCode) override`
- L299: `virtual std::stringstra_get_rawcode(const char* stdCode) override`
- L306: `virtual voidstra_log_info(const char* message) override`
- L313: `virtual voidstra_log_debug(const char* message) override`
- L320: `virtual voidstra_log_warn(const char* message) override`
- L327: `virtual voidstra_log_error(const char* message) override`
- L337: `virtual doublestra_get_position(const char* stdCode, bool bOnlyValid = false, int flag = 3) override`
- L345: `virtual doublestra_get_position_avgpx(const char* stdCode) override`
- L353: `virtual doublestra_get_position_profit(const char* stdCode) override`
- L361: `virtual doublestra_get_price(const char* stdCode) override`
- L369: `virtual doublestra_get_undone(const char* stdCode) override`
- L376: `virtual uint32_tstra_get_date() override`
- L383: `virtual uint32_tstra_get_time() override`
- L390: `virtual uint32_tstra_get_secs() override`
- L397: `virtual voidstra_sub_ticks(const char* stdCode) override`
- L404: `virtual voidstra_sub_order_details(const char* stdCode) override`
- L411: `virtual voidstra_sub_order_queues(const char* stdCode) override`
- L418: `virtual voidstra_sub_transactions(const char* stdCode) override`
- L426: `virtual voidstra_save_user_data(const char* key, const char* val) override`
- L435: `virtual const char*stra_load_user_data(const char* key, const char* defVal = "") override`
- L447: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L460: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled) override`
- L466: `virtual voidon_channel_ready() override`
- L472: `virtual voidon_channel_lost() override`
- L482: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L495: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`
- L547: `const char*get_inner_code(const char* stdCode)`
- L553: `voidload_userdata()`
- L559: `voidsave_userdata()`
- L565: `voidinit_outputs()`
- L575: `voiddo_set_position(const char* stdCode, double qty, double price = 0.0, const char* userTag = "")`
- L583: `voidupdate_dyn_profit(const char* stdCode, WTSTickData* newTick)`
- L597: `inline voidlog_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee, const char* userTag)`

### `src/WtCore/HftStraContext.cpp` (280 行)
- L47: `HftStraBaseCtx::on_init()`
- L62: `HftStraBaseCtx::on_session_begin(uTDate)`
- L81: `HftStraBaseCtx::on_session_end(uTDate)`
- L105: `HftStraBaseCtx::on_tick(stdCode, newTick)`
- L121: `HftStraBaseCtx::on_order_queue(stdCode, newOrdQue)`
- L137: `HftStraBaseCtx::on_order_detail(stdCode, newOrdDtl)`
- L153: `HftStraBaseCtx::on_transaction(stdCode, newTrans)`
- L171: `HftStraBaseCtx::on_bar(code, period, times, newBar)`
- L192: `HftStraBaseCtx::on_trade(localid, innerCode, isBuy, vol, price)`
- L215: `HftStraBaseCtx::on_order(localid, innerCode, isBuy, totalQty, leftQty, price, isCanceled)`
- L247: `HftStraBaseCtx::on_channel_ready()`
- L261: `HftStraBaseCtx::on_channel_lost()`
- L279: `HftStraBaseCtx::on_entrust(localid, get_inner_code(stdCode), bSuccess, message)`

### `src/WtCore/HftStraContext.h` (169 行)
- 类型: HftStrategy:18, HftStraContext:25
- L40: `virtual ~HftStraContext()`
- L46: `voidset_strategy(HftStrategy* stra) _strategy = stra; }`
- L52: `HftStrategy*get_stragety() return _strategy; }`
- L59: `virtual voidon_init() override`
- L65: `virtual voidon_session_begin(uint32_t uTDate) override`
- L71: `virtual voidon_session_end(uint32_t uTDate) override`
- L78: `virtual voidon_tick(const char* code, WTSTickData* newTick) override`
- L85: `virtual voidon_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue) override`
- L92: `virtual voidon_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl) override`
- L99: `virtual voidon_transaction(const char* stdCode, WTSTransData* newTrans) override`
- L108: `virtual voidon_bar(const char* code, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L118: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L130: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override`
- L136: `virtual voidon_channel_ready() override`
- L142: `virtual voidon_channel_lost() override`
- L151: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L163: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`

### `src/WtCore/HftStrategyMgr.cpp` (132 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L101: `HftStrategyPtrret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L120: `HftStrategyPtrret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

### `src/WtCore/HftStrategyMgr.h` (164 行)
- 类型: HftStraWrapper:24, HftStrategyMgr:67
- L50: `HftStrategy*self() return _stra; }`
- L87: `boolloadFactories(const char* path)`
- L96: `HftStrategyPtrcreateStrategy(const char* name, const char* id)`
- L106: `HftStrategyPtrcreateStrategy(const char* factname, const char* unitname, const char* id)`
- L114: `HftStrategyPtrgetStrategy(const char* id)`

### `src/WtCore/IExecCommand.h` (116 行)
- 类型: WTSCommodityInfo:12, WTSSessionInfo:13, IHotMgr:14, WTSTickData:15, IExecuterStub:21, IExecCommand:61
- L28: `virtual uint64_tget_real_time() = 0`
- L35: `virtual WTSCommodityInfo*get_comm_info(const char* stdCode) = 0`
- L42: `virtual WTSSessionInfo*get_sess_info(const char* stdCode) = 0`
- L48: `virtual IHotMgr*get_hot_mon() = 0`
- L54: `virtual uint32_tget_trading_day() = 0`
- L75: `virtual voidset_position(const wt_hashmap<std::string, double>& /*targets*/) }`
- L83: `virtual voidon_position_changed(const char* /*stdCode*/, double /*diffPos*/) }`
- L91: `virtual voidon_tick(const char* /*stdCode*/, WTSTickData* /*newTick*/) }`
- L98: `inline voidsetStub(IExecuterStub* stub) _stub = stub; }`
- L104: `inline const char*name() }`
- L110: `inline voidsetName(const char* name) _name = name; }`

### `src/WtCore/ITrdNotifySink.h` (101 行)
- 类型: ITrdNotifySink:21
- L33: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) = 0`
- L46: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) = 0`
- L59: `virtual voidon_position(const char* /*stdCode*/, bool /*isLong*/, double /*prevol*/, double /*preavail*/, double /*newvol*/, double /*newavail*/, uint32_t /*tradingday*/) }`
- L65: `virtual voidon_channel_ready() = 0`
- L71: `virtual voidon_channel_lost() = 0`
- L81: `virtual voidon_entrust(uint32_t /*localid*/, const char* /*stdCode*/, bool /*bSuccess*/, const char* /*message*/) }`
- L98: `virtual voidon_account(const char* /*currency*/, double /*prebalance*/, double /*balance*/, double /*dynbalance*/, double /*avaliable*/, double /*closeprofit*/, double /*dynprofit*/, double /*margin*/, double /*fee*/, double /*deposit*/, double /*withdraw*/) }`

### `src/WtCore/ParserAdapter.cpp` (425 行)
- L372: `WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message)`

### `src/WtCore/ParserAdapter.h` (126 行)
- 类型: WTSVariant:19, IHotMgr:20, IParserStub:22, ParserAdapter:32, ParserAdapterMgr:110
- L25: `virtual voidhandle_push_quote(WTSTickData* /*curTick*/) }`
- L27: `virtual voidhandle_push_order_detail(WTSOrdDtlData* /*curOrdDtl*/) }`
- L28: `virtual voidhandle_push_order_queue(WTSOrdQueData* /*curOrdQue*/) }`
- L29: `virtual voidhandle_push_transaction(WTSTransData* /*curTrans*/) }`
- L37: `virtual ~ParserAdapter()`
- L40: `boolinit(const char* id, WTSVariant* cfg, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL)`
- L42: `boolinitExt(const char* id, IParserApi* api, IParserStub* stub, IBaseDataMgr* bgMgr, IHotMgr* hotMgr = NULL)`
- L44: `voidrelease()`
- L46: `boolrun()`
- L48: `const char*id() }`
- L51: `virtual voidhandleSymbolList(const WTSArray* /*aySymbols*/) override {}`
- L58: `virtual voidhandleQuote(WTSTickData *quote, uint32_t procFlag) override`
- L64: `virtual voidhandleOrderQueue(WTSOrdQueData* ordQueData) override`
- L70: `virtual voidhandleOrderDetail(WTSOrdDtlData* ordDetailData) override`
- L76: `virtual voidhandleTransaction(WTSTransData* transData) override`
- L78: `virtual voidhandleParserLog(WTSLogLevel ll, const char* message) override`
- L80: `virtual IBaseDataMgr*getBaseDataMgr() override { return _bd_mgr; }`
- L113: `voidrelease()`
- L115: `voidrun()`
- L117: `ParserAdapterPtrgetAdapter(const char* id)`
- L119: `booladdAdapter(const char* id, ParserAdapterPtr& adapter)`

### `src/WtCore/SelStraBaseCtx.cpp` (1275 行)
- L64: `fs::create_directories(folder.c_str())`
- L173: `rj::Documentroot(rj::kObjectType)`
- L190: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L211: `StdFile::read_file_content(filename.c_str(), content)`
- L241: `StdFile::read_file_content(filename.c_str(), content)`
- L381: `rj::Documentroot(rj::kObjectType)`
- L384: `rj::ValuejPos(rj::kArrayType)`
- L393: `rj::ValuepItem(rj::kObjectType)`
- L403: `rj::Valuedetails(rj::kArrayType)`
- L407: `rj::ValuedItem(rj::kObjectType)`
- L433: `rj::ValuejFund(rj::kObjectType)`
- L445: `rj::ValuejSigs(rj::kObjectType)`
- L453: `rj::ValuejItem(rj::kObjectType)`
- L475: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L1048: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message)`
- L1053: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message)`
- L1058: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message)`
- L1063: `WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message)`

### `src/WtCore/SelStraBaseCtx.h` (929 行)
- 类型: WtSelEngine:23, SelStraBaseCtx:32
- L48: `virtual ~SelStraBaseCtx()`
- L55: `voidinit_outputs()`
- L66: `inline voidlog_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag = "")`
- L80: `inline voidlog_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag = "", double fee = 0.0)`
- L105: `voidsave_data(uint32_t flag = 0xFFFFFFFF)`
- L112: `voidload_data(uint32_t flag = 0xFFFFFFFF)`
- L118: `voidload_userdata()`
- L124: `voidsave_userdata()`
- L132: `voidupdate_dyn_profit(const char* stdCode, double price)`
- L142: `voiddo_set_position(const char* stdCode, double qty, const char* userTag = "", bool bTriggered = false)`
- L151: `voidappend_signal(const char* stdCode, double qty, const char* userTag = "")`
- L202: `virtual uint32_tid() override { return _context_id; }`
- L209: `virtual voidon_init() override`
- L216: `virtual voidon_session_begin(uint32_t uTDate) override`
- L223: `virtual voidon_session_end(uint32_t uTDate) override`
- L232: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override`
- L242: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L252: `virtual boolon_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime) override`
- L259: `virtual voidenum_position(FuncEnumSelPositionCallBack cb) override`
- L271: `virtual doublestra_get_position(const char* stdCode, bool bOnlyValid = false, const char* userTag = "") override`
- L280: `virtual voidstra_set_position(const char* stdCode, double qty, const char* userTag = "") override`
- L288: `virtual doublestra_get_price(const char* stdCode) override`
- L297: `virtual doublestra_get_day_price(const char* stdCode, int flag = 0) override`
- L304: `virtual uint32_tstra_get_tdate() override`
- L311: `virtual uint32_tstra_get_date() override`
- L318: `virtual uint32_tstra_get_time() override`
- L326: `virtual doublestra_get_fund_data(int flag /* = 0 */) override`
- L334: `virtual uint64_tstra_get_first_entertime(const char* stdCode) override`
- L342: `virtual uint64_tstra_get_last_entertime(const char* stdCode) override`
- L350: `virtual doublestra_get_last_enterprice(const char* stdCode) override`
- L358: `virtual const char*stra_get_last_entertag(const char* stdCode) override`
- L366: `virtual uint64_tstra_get_last_exittime(const char* stdCode) override`
- L374: `virtual doublestra_get_position_avgpx(const char* stdCode) override`
- L382: `virtual doublestra_get_position_profit(const char* stdCode) override`
- L391: `virtual uint64_tstra_get_detail_entertime(const char* stdCode, const char* userTag) override`
- L400: `virtual doublestra_get_detail_cost(const char* stdCode, const char* userTag) override`
- L410: `virtual doublestra_get_detail_profit(const char* stdCode, const char* userTag, int flag = 0) override`
- L418: `virtual WTSCommodityInfo*stra_get_comminfo(const char* stdCode) override`
- L426: `virtual WTSSessionInfo*stra_get_sessinfo(const char* stdCode) override`
- L436: `virtual WTSKlineSlice*stra_get_bars(const char* stdCode, const char* period, uint32_t count) override`
- L445: `virtual WTSTickSlice*stra_get_ticks(const char* stdCode, uint32_t count) override`
- L453: `virtual WTSTickData*stra_get_last_tick(const char* stdCode) override`
- L461: `virtual std::stringstra_get_rawcode(const char* stdCode) override`
- L468: `virtual voidstra_sub_ticks(const char* stdCode) override`
- L475: `virtual voidstra_log_info(const char* message) override`
- L482: `virtual voidstra_log_debug(const char* message) override`
- L489: `virtual voidstra_log_warn(const char* message) override`
- L496: `virtual voidstra_log_error(const char* message) override`
- L504: `virtual voidstra_save_user_data(const char* key, const char* val) override`
- L513: `virtual const char*stra_load_user_data(const char* key, const char* defVal = "") override`

### `src/WtCore/SelStraContext.cpp` (60 行)
- L18: `SelStraBaseCtx::on_init()`
- L26: `SelStraBaseCtx::on_session_begin(uTDate)`
- L37: `SelStraBaseCtx::on_session_end(uTDate)`

### `src/WtCore/SelStraContext.h` (33 行)
- 类型: SelStrategy:6, SelStraContext:8
- L12: `virtual ~SelStraContext()`
- L14: `voidset_strategy(SelStrategy* stra) _strategy = stra; }`
- L15: `SelStrategy*get_stragety() return _strategy; }`
- L18: `virtual voidon_init() override`
- L20: `virtual voidon_session_begin(uint32_t uTDate) override`
- L22: `virtual voidon_session_end(uint32_t uTDate) override`
- L24: `virtual voidon_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override`
- L26: `virtual voidon_tick_updated(const char* stdCode, WTSTickData* newTick) override`
- L28: `virtual voidon_strategy_schedule(uint32_t curDate, uint32_t curTime) override`

### `src/WtCore/SelStrategyMgr.cpp` (134 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L102: `SelStrategyPtrret(new SelStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L121: `SelStrategyPtrret(new SelStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

### `src/WtCore/SelStrategyMgr.h` (69 行)
- 类型: SelStraWrapper:19, SelStrategyMgr:41
- L31: `SelStrategy*self() return _stra; }`
- L48: `boolloadFactories(const char* path)`
- L50: `SelStrategyPtrcreateStrategy(const char* name, const char* id)`
- L51: `SelStrategyPtrcreateStrategy(const char* factname, const char* unitname, const char* id)`
- L53: `SelStrategyPtrgetStrategy(const char* id)`

### `src/WtCore/TraderAdapter.cpp` (2423 行)
- L245: `fs::create_directories(folder.c_str())`
- L305: `rj::Documentroot(rj::kObjectType)`
- L309: `rj::ValuejPos(rj::kArrayType)`
- L329: `rj::ValuepItem(rj::kObjectType)`
- L332: `rj::ValuelongItem(rj::kObjectType)`
- L339: `rj::ValueshortItem(rj::kObjectType)`
- L353: `rj::ValuejFunds(rj::kObjectType)`
- L360: `rj::ValuefItem(rj::kObjectType)`
- L384: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L474: `SpinLocklock(_mtx_orders)`
- L1278: `SpinLocklock(_mtx_orders)`
- L1454: `WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR, err->getMessage())`
- L1706: `SpinLocklock(_mtx_orders)`
- L2119: `SpinLocklock(_mtx_orders)`
- L2353: `WTSLogger::log_dyn_raw("trader", _id.c_str(), ll, message)`

### `src/WtCore/TraderAdapter.h` (610 行)
- 类型: WTSVariant:21, ActionPolicyMgr:22, WTSContractInfo:23, WTSCommodityInfo:24, WtLocalExecuter:25, EventNotifier:26, ITrdNotifySink:28, TraderAdapter:47, TraderAdapterMgr:567
- L59: `virtual ~TraderAdapter()`
- L165: `boolinit(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)`
- L175: `boolinitExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr)`
- L180: `voidrelease()`
- L186: `boolrun()`
- L192: `inline const char*id() }`
- L198: `AdapterStatestate() const{ return _state; }`
- L213: `inline boolisReady() const { return _state == AS_ALLREADY; }`
- L218: `voidqueryFund()`
- L221: `uint32_tdoEntrust(WTSEntrust* entrust)`
- L222: `booldoCancel(WTSOrderInfo* ordInfo)`
- L224: `inline voidprintPosition(const char* stdCode, const PosItem& pItem)`
- L226: `inline WTSContractInfo*getContract(const char* stdCode)`
- L227: `inline WTSCommodityInfo*getCommodify(const char* stdCommID)`
- L229: `const RiskParams*getRiskParams(const char* stdCode)`
- L231: `voidinitSaveData()`
- L233: `inline voidlogTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)`
- L234: `inline voidlogOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)`
- L236: `voidsaveData(WTSArray* ayFunds = NULL)`
- L238: `inline voidupdateUndone(const char* stdCode, double qty, bool bOuput = true)`
- L248: `doublegetPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3)`
- L255: `OrderMap*getOrders(const char* stdCode)`
- L275: `voidenumPosition(FuncEnumChnlPosCallBack cb)`
- L286: `uint32_topenLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL)`
- L297: `uint32_topenShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL)`
- L309: `uint32_tcloseLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL)`
- L321: `uint32_tcloseShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL)`
- L333: `OrderIDsbuy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL)`
- L345: `OrderIDssell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL)`
- L352: `boolcancel(uint32_t localid)`
- L361: `OrderIDscancel(const char* stdCode, bool isBuy, double qty = 0)`
- L368: `inline boolisTradeEnabled(const char* stdCode) const`
- L375: `boolcheckCancelLimits(const char* stdCode)`
- L382: `boolcheckOrderLimits(const char* stdCode)`
- L390: `boolcheckSelfMatch(const char* stdCode, WTSTradeInfo* tInfo)`
- L415: `virtual voidhandleEvent(WTSTraderEvent e, int32_t ec) override`
- L423: `virtual voidonLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override`
- L428: `virtual voidonLogout() override`
- L435: `virtual voidonRspEntrust(WTSEntrust* entrust, WTSError *err) override`
- L441: `virtual voidonRspAccount(WTSArray* ayAccounts) override`
- L447: `virtual voidonRspPosition(const WTSArray* ayPositions) override`
- L453: `virtual voidonRspOrders(const WTSArray* ayOrders) override`
- L459: `virtual voidonRspTrades(const WTSArray* ayTrades) override`
- L465: `virtual voidonPushOrder(WTSOrderInfo* orderInfo) override`
- L471: `virtual voidonPushTrade(WTSTradeInfo* tradeRecord) override`
- L478: `virtual voidonTraderError(WTSError* err, void* pData = NULL) override`
- L484: `virtual IBaseDataMgr*getBaseDataMgr() override`
- L491: `virtual voidhandleTraderLog(WTSLogLevel ll, const char* message) override`
- L573: `voidrelease()`
- L578: `voidrun()`
- L584: `const TraderAdapterMap&getAdapters() const { return _adapters; }`
- L591: `TraderAdapterPtrgetAdapter(const char* tname)`
- L599: `booladdAdapter(const char* tname, TraderAdapterPtr& adapter)`
- L604: `voidrefresh_funds()`

### `src/WtCore/WtArbiExecuter.cpp` (891 行)
- L173: `SpinLocklock(_mtx_units)`
- L357: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`
- L710: `SpinLocklock(_mtx_units)`
- L741: `SpinLocklock(_mtx_units)`
- L782: `SpinLocklock(_mtx_units)`

### `src/WtCore/WtArbiExecuter.h` (439 行)
- 类型: WTSVariant:20, IDataManager:21, TraderAdapter:22, IHotMgr:23, WtArbiExecuter:30
- L45: `virtual ~WtArbiExecuter()`
- L54: `boolinit(WTSVariant* params)`
- L61: `voidsetTrader(TraderAdapter* adapter)`
- L71: `ExecuteUnitPtrgetUnit(const char* code, bool bAutoCreate = true)`
- L84: `virtual WTSTickSlice*getTicks(const char* code, uint32_t count, uint64_t etime = 0) override`
- L92: `virtual WTSTickData*grabLastTick(const char* code) override`
- L102: `virtual doublegetPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override`
- L110: `virtual OrderMap*getOrders(const char* code) override`
- L118: `virtual doublegetUndoneQty(const char* code) override`
- L129: `virtual OrderIDsbuy(const char* code, double price, double qty, bool bForceClose = false) override`
- L140: `virtual OrderIDssell(const char* code, double price, double qty, bool bForceClose = false) override`
- L148: `virtual boolcancel(uint32_t localid) override`
- L158: `virtual OrderIDscancel(const char* code, bool isBuy, double qty) override`
- L165: `virtual voidwriteLog(const char* message) override`
- L173: `virtual WTSCommodityInfo*getCommodityInfo(const char* stdCode) override`
- L181: `virtual WTSSessionInfo*getSessionInfo(const char* stdCode) override`
- L188: `virtual uint64_tgetCurTime() override`
- L198: `virtual voidset_position(const wt_hashmap<std::string, double>& targets) override`
- L206: `virtual voidon_position_changed(const char* stdCode, double diffPos) override`
- L214: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick) override`
- L227: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L240: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override`
- L253: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`
- L263: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L269: `virtual voidon_channel_ready() override`
- L275: `virtual voidon_channel_lost() override`

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

### `src/WtCore/WtCtaEngine.h` (99 行)
- 类型: WTSVariant:17, WtCtaRtTicker:20, WtCtaEngine:22
- L26: `virtual ~WtCtaEngine()`
- L31: `virtual voidhandle_push_quote(WTSTickData* newTick) override`
- L33: `virtual voidon_tick(const char* stdCode, WTSTickData* curTick) override`
- L35: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L37: `virtual voidon_init() override`
- L38: `virtual voidon_session_begin() override`
- L39: `virtual voidon_session_end() override`
- L41: `virtual voidrun() override`
- L43: `virtual voidrelease() override`
- L45: `virtual voidinit(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override`
- L47: `virtual boolisInTrading() override`
- L48: `virtual uint32_ttransTimeToMin(uint32_t uTime) override`
- L52: `virtual uint64_tget_real_time() override`
- L53: `virtual WTSCommodityInfo*get_comm_info(const char* stdCode) override`
- L54: `virtual WTSSessionInfo*get_sess_info(const char* stdCode) override`
- L55: `virtual IHotMgr*get_hot_mon() override { return _hot_mgr; }`
- L56: `virtual uint32_tget_trading_day() override { return _cur_tdate; }`
- L60: `voidon_schedule(uint32_t curDate, uint32_t curTime)`
- L62: `voidhandle_pos_change(const char* straName, const char* stdCode, double diffPos)`
- L64: `voidaddContext(CtaContextPtr ctx)`
- L66: `CtaContextPtrgetContext(uint32_t id)`
- L80: `voidnotify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag)`
- L81: `voidnotify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val)`
- L82: `voidnotify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag)`

### `src/WtCore/WtCtaTicker.cpp` (299 行)
- L37: `TimeUtils::getDateTime(_date, _time)`
- L122: `StdUniqueLocklock(_mtx)`
- L196: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L202: `StdUniqueLocklock(_mtx)`
- L250: `StdUniqueLocklock(_mtx)`
- L271: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtCore/WtCtaTicker.h` (133 行)
- 类型: WTSSessionInfo:22, IDataReader:23, WTSTickData:24, WtCtaEngine:26, WtCtaRtTicker:32
- L64: `voidinit(IDataReader* store, const char* sessionID)`
- L74: `voidon_tick(WTSTickData* curTick)`
- L80: `voidrun()`
- L86: `voidstop()`
- L95: `boolis_in_trading() const`
- L104: `uint32_ttime_to_mins(uint32_t uTime) const`
- L113: `voidtrigger_price(WTSTickData* curTick)`

### `src/WtCore/WtDiffExecuter.cpp` (902 行)
- L125: `StdFile::read_file_content(filename.c_str(), content)`
- L185: `rj::Documentroot(rj::kObjectType)`
- L189: `rj::ValuejTarget(rj::kArrayType)`
- L193: `rj::ValuejItem(rj::kObjectType)`
- L204: `rj::ValuejDiff(rj::kArrayType)`
- L208: `rj::ValuejItem(rj::kObjectType)`
- L226: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L444: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`

### `src/WtCore/WtDiffExecuter.h` (350 行)
- 类型: WTSVariant:21, IDataManager:22, IBaseDataMgr:23, TraderAdapter:24, IHotMgr:25, WtDiffExecuter:33
- L51: `virtual ~WtDiffExecuter()`
- L60: `boolinit(WTSVariant* params)`
- L67: `voidsetTrader(TraderAdapter* adapter)`
- L77: `ExecuteUnitPtrgetUnit(const char* code, bool bAutoCreate = true)`
- L83: `voidsave_data()`
- L89: `voidload_data()`
- L102: `virtual WTSTickSlice*getTicks(const char* code, uint32_t count, uint64_t etime = 0) override`
- L110: `virtual WTSTickData*grabLastTick(const char* code) override`
- L120: `virtual doublegetPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override`
- L128: `virtual OrderMap*getOrders(const char* code) override`
- L136: `virtual doublegetUndoneQty(const char* code) override`
- L147: `virtual OrderIDsbuy(const char* code, double price, double qty, bool bForceClose = false) override`
- L158: `virtual OrderIDssell(const char* code, double price, double qty, bool bForceClose = false) override`
- L166: `virtual boolcancel(uint32_t localid) override`
- L176: `virtual OrderIDscancel(const char* code, bool isBuy, double qty) override`
- L183: `virtual voidwriteLog(const char* message) override`
- L191: `virtual WTSCommodityInfo*getCommodityInfo(const char* stdCode) override`
- L199: `virtual WTSSessionInfo*getSessionInfo(const char* stdCode) override`
- L206: `virtual uint64_tgetCurTime() override`
- L215: `virtual voidset_position(const wt_hashmap<std::string, double>& targets) override`
- L224: `virtual voidon_position_changed(const char* stdCode, double diffPos) override`
- L232: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick) override`
- L244: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L258: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override`
- L271: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`
- L282: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L289: `virtual voidon_channel_ready() override`
- L296: `virtual voidon_channel_lost() override`

### `src/WtCore/WtDistExecuter.h` (92 行)
- 类型: WTSVariant:16, WtDistExecuter:23
- L35: `virtual ~WtDistExecuter()`
- L43: `boolinit(WTSVariant* params)`
- L54: `virtual voidset_position(const wt_hashmap<std::string, double>& targets) override`
- L62: `virtual voidon_position_changed(const char* stdCode, double targetPos) override`
- L70: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick) override`

### `src/WtCore/WtDtMgr.cpp` (542 行)
- L72: `DLLHelper::free_library(hInst)`
- L80: `DLLHelper::free_library(hInst)`
- L109: `WTSLogger::debug("All bars updated, on_bar will be triggered")`
- L146: `WTSLogger::log_raw(ll, message)`
- L295: `std::stringpureStdCode(stdCode, len - 1)`

### `src/WtCore/WtDtMgr.h` (356 行)
- 类型: WTSVariant:23, WTSTickData:24, WTSKlineSlice:25, WTSTickSlice:26, IBaseDataMgr:27, IBaseDataMgr:28, WtEngine:29, WtDtMgr:38
- L60: `boolinitStore(WTSVariant* cfg)`
- L71: `boolinit(WTSVariant* cfg, WtEngine* engine, bool bForceCache = false)`
- L78: `voidregsiter_loader(IHisDataLoader* loader) _loader = loader; }`
- L86: `voidhandle_push_quote(const char* stdCode, WTSTickData* newTick)`
- L98: `virtual WTSTickSlice*get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L107: `virtual WTSOrdQueSlice*get_order_queue_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L117: `virtual WTSOrdDtlSlice*get_order_detail_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L127: `virtual WTSTransSlice*get_transaction_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override`
- L138: `virtual WTSKlineSlice*get_kline_slice(const char* stdCode, WTSKlinePeriod period, uint32_t times, uint32_t count, uint64_t etime = 0) override`
- L146: `virtual WTSTickData*grab_last_tick(const char* stdCode) override`
- L155: `virtual doubleget_adjusting_factor(const char* stdCode, uint32_t uDate) override`
- L162: `virtual uint32_tget_adjusting_flag() override`
- L173: `virtual voidon_bar(const char* code, WTSKlinePeriod period, WTSBarStruct* newBar) override`
- L180: `virtual voidon_all_bar_updated(uint32_t updateTime) override`
- L187: `virtual IBaseDataMgr*get_basedata_mgr() override`
- L194: `virtual IHotMgr*get_hot_mgr() override`
- L201: `virtual uint32_tget_date() override`
- L208: `virtual uint32_tget_min_time() override`
- L215: `virtual uint32_tget_secs() override`
- L223: `virtual voidreader_log(WTSLogLevel ll, const char* message) override`
- L230: `inline IDataReader*reader() return _reader; }`
- L237: `inline IHisDataLoader*loader() return _loader; }`

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

### `src/WtCore/WtEngine.h` (339 行)
- 类型: WTSSessionInfo:30, WTSCommodityInfo:31, WTSContractInfo:32, IBaseDataMgr:34, IHotMgr:35, WTSVariant:37, WTSTickData:39, WTSBarStruct:40, WTSTickSlice:41, WTSKlineSlice:42, WTSPortFundInfo:43, WtDtMgr:45, TraderAdapterMgr:46, EventNotifier:48, WtRiskMonWrapper:52, IEngineEvtListener:73, WtEngine:81
- L64: `WtRiskMonitor*self() return _mon; }`
- L76: `virtual voidon_initialize_event() }`
- L77: `virtual voidon_schedule_event(uint32_t /*uDate*/, uint32_t /*uTime*/) }`
- L78: `virtual voidon_session_event(uint32_t /*uDate*/, bool /*isBegin*/ = true) }`
- L86: `inline voidset_adapter_mgr(TraderAdapterMgr* mgr) _adapter_mgr = mgr; }`
- L88: `voidset_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs = 0, uint32_t rawTime = 0)`
- L90: `voidset_trading_date(uint32_t curTDate)`
- L92: `inline uint32_tget_date() return _cur_date; }`
- L93: `inline uint32_tget_min_time() return _cur_time; }`
- L94: `inline uint32_tget_raw_time() return _cur_raw_time; }`
- L95: `inline uint32_tget_secs() return _cur_secs; }`
- L96: `inline uint32_tget_trading_date() return _cur_tdate; }`
- L98: `inline IBaseDataMgr*get_basedata_mgr() return _base_data_mgr; }`
- L99: `inline IHotMgr*get_hot_mgr() return _hot_mgr; }`
- L100: `WTSSessionInfo*get_session_info(const char* sid, bool isCode = false)`
- L101: `WTSCommodityInfo*get_commodity_info(const char* stdCode)`
- L102: `WTSContractInfo*get_contract_info(const char* stdCode)`
- L103: `std::stringget_rawcode(const char* stdCode)`
- L105: `WTSTickData*get_last_tick(uint32_t sid, const char* stdCode)`
- L106: `WTSTickSlice*get_tick_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L107: `WTSKlineSlice*get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times = 1, uint64_t etime = 0)`
- L109: `voidsub_tick(uint32_t sid, const char* code)`
- L111: `doubleget_cur_price(const char* stdCode)`
- L113: `doubleget_day_price(const char* stdCode, int flag = 0)`
- L120: `doubleget_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo = NULL)`
- L122: `uint32_tget_adjusting_flag()`
- L124: `doublecalc_fee(const char* stdCode, double price, double qty, uint32_t offset)`
- L138: `virtual WTSPortFundInfo*getFundInfo() override`
- L140: `virtual voidsetVolScale(double scale) override`
- L142: `virtual boolisInTrading() override`
- L144: `virtual voidwriteRiskLog(const char* message) override`
- L146: `virtual uint32_tgetCurDate() override`
- L147: `virtual uint32_tgetCurTime() override`
- L148: `virtual uint32_tgetTradingDate() override`
- L149: `virtual uint32_ttransTimeToMin(uint32_t /*uTime*/) override{ return 0; }`
- L153: `virtual voidhandle_push_quote(WTSTickData* newTick) override`
- L156: `virtual voidinit(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier)`
- L158: `virtual voidrun() = 0`
- L161: `virtual voidrelease()`
- L163: `virtual voidon_tick(const char* stdCode, WTSTickData* curTick)`
- L165: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0`
- L167: `virtual voidon_init() }`
- L168: `virtual voidon_session_begin()`
- L169: `virtual voidon_session_end()`
- L173: `voidload_fees(const char* filename)`
- L175: `voidload_datas()`
- L177: `voidsave_datas()`
- L179: `voidappend_signal(const char* stdCode, double qty, bool bStandBy)`
- L181: `voiddo_set_position(const char* stdCode, double qty, double curPx = -1)`
- L183: `voidtask_loop()`
- L185: `voidpush_task(TaskItem task)`
- L187: `voidupdate_fund_dynprofit()`
- L189: `boolinit_riskmon(WTSVariant* cfg)`
- L192: `voidinit_outputs()`
- L193: `inline voidlog_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee = 0.0)`

### `src/WtCore/WtExecMgr.h` (87 行)
- 类型: WtFilterMgr:6, WtExecuterMgr:10
- L15: `inline voidset_filter_mgr(WtFilterMgr* mgr) _filter_mgr = mgr; }`
- L22: `voidenum_executer(EnumExecuterCb cb)`
- L24: `voidset_positions(wt_hashmap<std::string, double> target_pos)`
- L25: `voidhandle_pos_change(const char* stdCode, double targetPos, double diffPos, const char* execid = "ALL")`
- L26: `voidhandle_tick(const char* stdCode, WTSTickData* curTick)`
- L31: `boolload_router_rules(WTSVariant* config)`
- L66: `voidadd_target_to_cache(const char* stdCode, double targetPos, const char* execid = "ALL")`
- L72: `voidcommit_cached_targets(double scale = 1.0)`

### `src/WtCore/WtExecuterFactory.cpp` (186 行)
- L23: `fs::pathmyPath(path)`
- L52: `DLLHelper::free_library(hInst)`

### `src/WtCore/WtExecuterFactory.h` (185 行)
- 类型: ExeUnitWrapper:21, WtExecuterFactory:77
- L47: `ExecuteUnit*self() return _unit; }`
- L91: `boolloadFactories(const char* path)`
- L98: `ExecuteUnitPtrcreateExeUnit(const char* name)`
- L105: `ExecuteUnitPtrcreateDiffExeUnit(const char* name)`
- L112: `ExecuteUnitPtrcreateArbiExeUnit(const char* name)`
- L120: `ExecuteUnitPtrcreateExeUnit(const char* factname, const char* unitname)`
- L128: `ExecuteUnitPtrcreateDiffExeUnit(const char* factname, const char* unitname)`
- L136: `ExecuteUnitPtrcreateArbiExeUnit(const char* factname, const char* unitname)`

### `src/WtCore/WtFilterMgr.h` (91 行)
- 类型: EventNotifier:11, WtFilterMgr:13
- L18: `voidset_notifier(EventNotifier* notifier) _notifier = notifier; }`
- L23: `voidload_filters(const char* fileName = "")`
- L35: `boolis_filtered_by_strategy(const char* straName, double& targetPos, bool isDiff = false)`
- L46: `boolis_filtered_by_code(const char* stdCode, double& targetPos)`
- L55: `boolis_filtered_by_executer(const char* execid)`

### `src/WtCore/WtHelper.cpp` (101 行)
- L59: `fs::create_directories(folder)`
- L67: `fs::create_directories(folder)`
- L75: `fs::create_directories(folder)`
- L83: `fs::create_directories(folder)`
- L91: `fs::create_directories(folder)`
- L99: `fs::create_directories(folder)`

### `src/WtCore/WtHelper.h` (165 行)
- 类型: WtHelper:19
- L26: `static std::stringgetCWD()`
- L35: `static std::stringgetModulePath(const char* moduleName, const char* subDir, bool isCWD = true)`
- L41: `static const char*getBaseDir()`
- L47: `static const char*getOutputDir()`
- L53: `static const char*getStraDataDir()`
- L59: `static const char*getStraUsrDatDir()`
- L65: `static const char*getPortifolioDir()`
- L71: `static const char*getExecDataDir()`
- L90: `static inline voidsetTDate(uint32_t tDate) _cur_tdate = tDate; }`
- L96: `static inline uint32_tgetDate() return _cur_date; }`
- L102: `static inline uint32_tgetTime() return _cur_time; }`
- L108: `static inline uint32_tgetSecs() return _cur_secs; }`
- L114: `static inline uint32_tgetTradingDate() return _cur_tdate; }`
- L120: `static const std::string&getInstDir() return _inst_dir; }`
- L126: `static voidsetInstDir(const char* inst_dir) _inst_dir = inst_dir; }`
- L132: `static voidsetGenerateDir(const char* gen_dir) _gen_dir = gen_dir; }`

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

### `src/WtCore/WtHftEngine.h` (80 行)
- 类型: WTSVariant:18, WtHftRtTicker:19, WtHftEngine:23
- L27: `virtual ~WtHftEngine()`
- L32: `virtual voidinit(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override`
- L34: `virtual voidrun() override`
- L36: `virtual voidrelease() override`
- L38: `virtual voidhandle_push_quote(WTSTickData* newTick) override`
- L39: `virtual voidhandle_push_order_detail(WTSOrdDtlData* curOrdDtl) override`
- L40: `virtual voidhandle_push_order_queue(WTSOrdQueData* curOrdQue) override`
- L41: `virtual voidhandle_push_transaction(WTSTransData* curTrans) override`
- L43: `virtual voidon_tick(const char* stdCode, WTSTickData* curTick) override`
- L45: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L47: `virtual voidon_session_begin() override`
- L49: `virtual voidon_session_end() override`
- L52: `WTSOrdQueSlice*get_order_queue_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L53: `WTSOrdDtlSlice*get_order_detail_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L54: `WTSTransSlice*get_transaction_slice(uint32_t sid, const char* stdCode, uint32_t count)`
- L57: `voidon_minute_end(uint32_t curDate, uint32_t curTime)`
- L59: `voidaddContext(HftContextPtr ctx)`
- L61: `HftContextPtrgetContext(uint32_t id)`
- L63: `voidsub_order_queue(uint32_t sid, const char* stdCode)`
- L64: `voidsub_order_detail(uint32_t sid, const char* stdCode)`
- L65: `voidsub_transaction(uint32_t sid, const char* stdCode)`

### `src/WtCore/WtHftTicker.cpp` (259 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L114: `StdUniqueLocklock(_mtx)`
- L177: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L183: `StdUniqueLocklock(_mtx)`
- L228: `StdUniqueLocklock(_mtx)`
- L246: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtCore/WtHftTicker.h` (135 行)
- 类型: WTSSessionInfo:20, IDataReader:21, WTSTickData:22, WtHftEngine:24, WtHftRtTicker:30
- L50: `voidinit(IDataReader* store, const char* sessionID)`
- L56: `voidon_tick(WTSTickData* curTick)`
- L62: `voidrun()`
- L68: `voidstop()`
- L76: `voidtrigger_price(WTSTickData* curTick)`

### `src/WtCore/WtLocalExecuter.cpp` (995 行)
- L186: `SpinLocklock(_mtx_units)`
- L409: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`
- L803: `SpinLocklock(_mtx_units)`
- L836: `SpinLocklock(_mtx_units)`
- L879: `SpinLocklock(_mtx_units)`

### `src/WtCore/WtLocalExecuter.h` (435 行)
- 类型: WTSVariant:20, IDataManager:21, TraderAdapter:22, IHotMgr:23, WtLocalExecuter:29
- L45: `virtual ~WtLocalExecuter()`
- L54: `boolinit(WTSVariant* params)`
- L61: `voidsetTrader(TraderAdapter* adapter)`
- L71: `ExecuteUnitPtrgetUnit(const char* code, bool bAutoCreate = true)`
- L84: `virtual WTSTickSlice*getTicks(const char* code, uint32_t count, uint64_t etime = 0) override`
- L92: `virtual WTSTickData*grabLastTick(const char* code) override`
- L102: `virtual doublegetPosition(const char* stdCode, bool validOnly = true, int32_t flag = 3) override`
- L110: `virtual OrderMap*getOrders(const char* code) override`
- L118: `virtual doublegetUndoneQty(const char* code) override`
- L129: `virtual OrderIDsbuy(const char* code, double price, double qty, bool bForceClose = false) override`
- L140: `virtual OrderIDssell(const char* code, double price, double qty, bool bForceClose = false) override`
- L148: `virtual boolcancel(uint32_t localid) override`
- L158: `virtual OrderIDscancel(const char* code, bool isBuy, double qty) override`
- L165: `virtual voidwriteLog(const char* message) override`
- L173: `virtual WTSCommodityInfo*getCommodityInfo(const char* stdCode) override`
- L181: `virtual WTSSessionInfo*getSessionInfo(const char* stdCode) override`
- L188: `virtual uint64_tgetCurTime() override`
- L196: `virtual voidset_position(const wt_hashmap<std::string, double>& targets) override`
- L205: `virtual voidon_position_changed(const char* stdCode, double diffPos) override`
- L213: `virtual voidon_tick(const char* stdCode, WTSTickData* newTick) override`
- L224: `virtual voidon_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override`
- L237: `virtual voidon_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override`
- L250: `virtual voidon_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday) override`
- L260: `virtual voidon_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override`
- L266: `virtual voidon_channel_ready() override`
- L272: `virtual voidon_channel_lost() override`

### `src/WtCore/WtSelEngine.cpp` (509 行)
- L56: `WtEngine::release()`
- L107: `WtEngine::on_tick(stdCode, curTick)`
- L330: `StdThreadPtrthrd(new StdThread([ctx, curDate, curTime, nextTime]()`
- L347: `rj::Documentroot(rj::kObjectType)`
- L350: `rj::ValuejStraList(rj::kArrayType)`
- L359: `rj::ValuejChnlList(rj::kArrayType)`
- L374: `rj::PrettyWriter<rj::StringBuffer>writer(sb)`
- L376: `StdFile::write_file_content(filename.c_str(), sb.GetString())`
- L384: `WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier)`
- L402: `TaskInfoPtrtInfo(new TaskInfo)`
- L459: `SpinLocklock(_pos_mtx)`

### `src/WtCore/WtSelEngine.h` (108 行)
- 类型: WtSelRtTicker:40, WtSelEngine:43
- L52: `virtual voidinit(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier) override`
- L54: `virtual voidrun() override`
- L56: `virtual voidrelease() override`
- L58: `virtual voidon_tick(const char* stdCode, WTSTickData* curTick) override`
- L60: `virtual voidon_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override`
- L62: `virtual voidhandle_push_quote(WTSTickData* newTick) override`
- L64: `virtual voidon_init() override`
- L66: `virtual voidon_session_begin() override`
- L68: `virtual voidon_session_end() override`
- L72: `virtual uint64_tget_real_time() override`
- L73: `virtual WTSCommodityInfo*get_comm_info(const char* stdCode) override`
- L74: `virtual WTSSessionInfo*get_sess_info(const char* stdCode) override`
- L75: `virtual IHotMgr*get_hot_mon() override { return _hot_mgr; }`
- L76: `virtual uint32_tget_trading_day() override { return _cur_tdate; }`
- L80: `voidaddContext(SelContextPtr ctx, uint32_t date, uint32_t time, TaskPeriodType period, bool bStrict = true, const char* trdtpl = "CHINA", const char* sessionID="TRADING")`
- L82: `SelContextPtrgetContext(uint32_t id)`
- L90: `voidon_minute_end(uint32_t uDate, uint32_t uTime)`
- L92: `voidhandle_pos_change(const char* straName, const char* stdCode, double diffQty)`

### `src/WtCore/WtSelTicker.cpp` (246 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L117: `StdUniqueLocklock(_mtx)`
- L180: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L186: `StdUniqueLocklock(_mtx)`
- L226: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`

### `src/WtCore/WtSelTicker.h` (100 行)
- 类型: WTSSessionInfo:19, IDataReader:20, WTSTickData:21, WtSelEngine:23, WtSelRtTicker:30
- L51: `voidinit(IDataReader* store, const char* sessionID)`
- L59: `voidon_tick(WTSTickData* curTick, uint32_t hotFlag = 0)`
- L65: `voidrun()`
- L71: `voidstop()`
- L80: `voidtrigger_price(WTSTickData* curTick, uint32_t hotFlag = 0)`

