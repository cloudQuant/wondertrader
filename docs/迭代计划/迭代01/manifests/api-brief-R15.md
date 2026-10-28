# API 简报 R15

测试目标: wtcore。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtCore/WtLocalExecuter.cpp` (995 行)
- L186: `SpinLocklock(_mtx_units)`
- L409: `WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf)`
- L803: `SpinLocklock(_mtx_units)`
- L836: `SpinLocklock(_mtx_units)`
- L879: `SpinLocklock(_mtx_units)`

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

### `src/WtCore/WtCtaTicker.cpp` (299 行)
- L37: `TimeUtils::getDateTime(_date, _time)`
- L122: `StdUniqueLocklock(_mtx)`
- L196: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L202: `StdUniqueLocklock(_mtx)`
- L250: `StdUniqueLocklock(_mtx)`
- L271: `std::this_thread::sleep_for(std::chrono::seconds(10))`

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

### `src/WtCore/WtHftTicker.cpp` (259 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L114: `StdUniqueLocklock(_mtx)`
- L177: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L183: `StdUniqueLocklock(_mtx)`
- L228: `StdUniqueLocklock(_mtx)`
- L246: `std::this_thread::sleep_for(std::chrono::seconds(10))`

### `src/WtCore/WtSelTicker.cpp` (246 行)
- L47: `TimeUtils::getDateTime(_date, _time)`
- L117: `StdUniqueLocklock(_mtx)`
- L180: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- L186: `StdUniqueLocklock(_mtx)`
- L226: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`

### `src/WtCore/ParserAdapter.cpp` (425 行)
- L372: `WTSLogger::log_dyn_raw("parser", _id.c_str(), ll, message)`

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

### `src/WtCore/WtExecuterFactory.cpp` (186 行)
- L23: `fs::pathmyPath(path)`
- L52: `DLLHelper::free_library(hInst)`

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

### `src/WtCore/CtaStrategyMgr.cpp` (134 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L102: `CtaStrategyPtrret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L121: `CtaStrategyPtrret(new CtaStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

### `src/WtCore/HftStrategyMgr.cpp` (132 行)
- L39: `fs::pathmyPath(path)`
- L64: `DLLHelper::free_library(hInst)`
- L84: `DLLHelper::free_library(hInst)`
- L101: `HftStrategyPtrret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`
- L120: `HftStrategyPtrret(new HftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact))`

