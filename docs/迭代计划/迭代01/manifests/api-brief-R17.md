# API 简报 R17

测试目标: exefact。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/WtExeFact/WtDiffMinImpactExeUnit.cpp` (522 行)
- L85: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L311: `CalcFlagflag(&_in_calc)`
- L329: `StdUniqueLocklock(_mtx_calc)`

### `src/WtExecMon/WtSimpDataMgr.cpp` (233 行)
- L65: `DLLHelper::free_library(hInst)`
- L73: `DLLHelper::free_library(hInst)`
- L122: `WTSLogger::log_raw(ll, message)`

### `src/WtExeFact/WtMinImpactExeUnit.cpp` (600 行)
- L122: `ExecuteUnit::init(ctx, stdCode, cfg)`
- L343: `CalcFlagflag(&_in_calc)`
- L355: `StdUniqueLocklock(_mtx_calc)`

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

### `src/WtExecMon/WtExecPorter.cpp` (161 行)
- L132: `WTSLogger::log_raw_by_cat(catName, (WTSLogLevel)level, message)`
- L137: `WTSLogger::log_raw((WTSLogLevel)level, message)`

### `src/WtExeFact/WtOrdMon.h` (107 行)
- 类型: WtOrdMon:34
- L45: `voidpush_order(const uint32_t* ids, uint32_t cnt, uint64_t curTime, bool bCanCancel = true)`
- L52: `voiderase_order(uint32_t localid)`
- L79: `voidcheck_orders(uint32_t expiresecs, uint64_t curTime, EnumOrderCallback callback)`
- L95: `voidenumOrder(EnumAllOrderCallback cb)`

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

