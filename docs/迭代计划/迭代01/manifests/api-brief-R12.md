# API 简报 R12

测试目标: (主目标 CoverageTests)。
本简报只覆盖本轮要动的文件（按报表 missed 行数选出的），
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段。
语义/分支以源码为准；**清单外的文件本轮不要动**。

### `src/Includes/WTSDataDef.hpp` (1648 行)
- 类型: WTSContractInfo:32, WTSValueArray:39, WTSKlineSlice:204, WTSKlineData:444, WTSTickData:858, WTSTickRef:1039, WTSOrdQueData:1189, WTSOrdDtlData:1225, WTSTransData:1262, WTSHisTickData:1302, WTSTickSlice:1390, WTSOrdDtlSlice:1498, WTSOrdQueSlice:1550, WTSTransSlice:1602
- L59: `inline uint32_tsize() }`
- L60: `inline boolempty() }`
- L368: `inline int32_tsize() const{ return _count; }`
- L369: `inline boolempty() const{ return _count == 0; }`
- L374: `inline const char*code() const{ return _code; }`
- L375: `inline voidsetCode(const char* code) }`
- L492: `inline voidsetClosed(bool bClosed) m_bClosed = bClosed; }`
- L493: `inline boolisClosed() const{ return m_bClosed; }`
- L500: `inline voidsetPeriod(WTSKlinePeriod period, uint32_t times = 1) m_kpPeriod = period; m_uTimes = times; }`
- L502: `inline voidsetUnixTime(bool bEnabled = true) m_bUnixTime = bEnabled; }`
- L504: `inline WTSKlinePeriodperiod() const{ return m_kpPeriod; }`
- L505: `inline uint32_ttimes() const{ return m_uTimes; }`
- L506: `inline boolisUnixTime() const{ return m_bUnixTime; }`
- L563: `inline uint32_tsize() }`
- L564: `inline boolIsEmpty() }`
- L569: `inline const char*code() const{ return m_strCode; }`
- L570: `inline voidsetCode(const char* code) }`
- L802: `inline WTSBarList&getDataRef() return m_vecBarData; }`
- L824: `WTSObject::release()`
- L900: `constexpr inline const char*code() const noexcept { return m_tickStruct.code; }`
- L905: `constexpr inline const char*exchg() const noexcept { return m_tickStruct.exchg; }`
- L910: `constexpr inline doubleprice() const noexcept { return m_tickStruct.price; }`
- L912: `constexpr inline doubleopen() const noexcept { return m_tickStruct.open; }`
- L917: `constexpr inline doublehigh() const noexcept { return m_tickStruct.high; }`
- L922: `constexpr inline doublelow() const noexcept { return m_tickStruct.low; }`
- L925: `constexpr inline doublepreclose() const noexcept { return m_tickStruct.pre_close; }`
- L926: `constexpr inline doublepresettle() const noexcept { return m_tickStruct.pre_settle; }`
- L927: `constexpr inline doublepreinterest() const noexcept { return m_tickStruct.pre_interest; }`
- L929: `constexpr inline doubleupperlimit() const noexcept { return m_tickStruct.upper_limit; }`
- L930: `constexpr inline doublelowerlimit() const noexcept { return m_tickStruct.lower_limit; }`
- L932: `constexpr inline doubletotalvolume() const noexcept { return m_tickStruct.total_volume; }`
- L935: `constexpr inline doublevolume() const noexcept { return m_tickStruct.volume; }`
- L938: `constexpr inline doublesettlepx() const noexcept { return m_tickStruct.settle_price; }`
- L941: `constexpr inline doubleopeninterest() const noexcept { return m_tickStruct.open_interest; }`
- L943: `constexpr inline doubleadditional() const noexcept { return m_tickStruct.diff_interest; }`
- L946: `constexpr inline doubletotalturnover() const noexcept { return m_tickStruct.total_turnover; }`
- L949: `constexpr inline doubleturnover() const noexcept { return m_tickStruct.turn_over; }`
- L952: `constexpr inline uint32_ttradingdate() const noexcept { return m_tickStruct.trading_date; }`
- L955: `constexpr inline uint32_tactiondate() const noexcept { return m_tickStruct.action_date; }`
- L958: `constexpr inline uint32_tactiontime() const noexcept { return m_tickStruct.action_time; }`
- L1012: `constexpr inline WTSTickStruct&getTickStruct() noexcept{ return m_tickStruct; }`
- L1014: `constexpr inline voidsetContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }`
- L1015: `constexpr inline WTSContractInfo*getContractInfo() const noexcept { return m_pContract; }`
- L1017: `inline voidsetLimitFlag(uint32_t flag) m_uLimitFlag = flag; }`
- L1018: `constexpr inline uint32_tgetLimitFlag() const { return m_uLimitFlag; }`
- L1020: `constexpr inline boolisNewHigh() const { return m_uLimitFlag & 1; }`
- L1021: `constexpr inline boolisNewLow() const { return m_uLimitFlag & 2; }`
- L1062: `constexpr inline const char*code() const noexcept { return m_tickStruct->code; }`
- L1067: `constexpr inline const char*exchg() const noexcept { return m_tickStruct->exchg; }`
- L1072: `constexpr inline doubleprice() const noexcept { return m_tickStruct->price; }`
- L1074: `constexpr inline doubleopen() const noexcept { return m_tickStruct->open; }`
- L1079: `constexpr inline doublehigh() const noexcept { return m_tickStruct->high; }`
- L1084: `constexpr inline doublelow() const noexcept { return m_tickStruct->low; }`
- L1087: `constexpr inline doublepreclose() const noexcept { return m_tickStruct->pre_close; }`
- L1088: `constexpr inline doublepresettle() const noexcept { return m_tickStruct->pre_settle; }`
- L1089: `constexpr inline doublepreinterest() const noexcept { return m_tickStruct->pre_interest; }`
- L1091: `constexpr inline doubleupperlimit() const noexcept { return m_tickStruct->upper_limit; }`
- L1092: `constexpr inline doublelowerlimit() const noexcept { return m_tickStruct->lower_limit; }`
- L1094: `constexpr inline doubletotalvolume() const noexcept { return m_tickStruct->total_volume; }`
- L1097: `constexpr inline doublevolume() const noexcept { return m_tickStruct->volume; }`
- L1100: `constexpr inline doublesettlepx() const noexcept { return m_tickStruct->settle_price; }`
- L1103: `constexpr inline doubleopeninterest() const noexcept { return m_tickStruct->open_interest; }`
- L1105: `constexpr inline doubleadditional() const noexcept { return m_tickStruct->diff_interest; }`
- L1108: `constexpr inline doubletotalturnover() const noexcept { return m_tickStruct->total_turnover; }`
- L1111: `constexpr inline doubleturnover() const noexcept { return m_tickStruct->turn_over; }`
- L1114: `constexpr inline uint32_ttradingdate() const noexcept { return m_tickStruct->trading_date; }`
- L1117: `constexpr inline uint32_tactiondate() const noexcept { return m_tickStruct->action_date; }`
- L1120: `constexpr inline uint32_tactiontime() const noexcept { return m_tickStruct->action_time; }`
- L1171: `constexpr inline boolvalid() const noexcept { return m_tickStruct != nullptr; }`
- L1173: `constexpr inline operatorbool() const noexcept { return m_tickStruct != nullptr; }`
- （另有 43 个声明，按需用 Read 定位）

### `src/WTSTools/WTSBaseDataMgr.cpp` (888 行)
- L554: `std::sort(_code_list->begin(), _code_list->end(), [](WTSObject* left, WTSObject* right)`
- L677: `TimeUtils::getDateTime(uDate, uTime)`

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

### `src/WTSTools/WTSHotMgr.cpp` (484 行)
- L378: `std::stringfullPid(fullCode, idx)`

### `src/Includes/WTSTradeDef.hpp` (786 行)
- 类型: WTSContractInfo:21, WTSVariant:22, WTSError:23, WTSEntrust:99, WTSEntrustAction:217, WTSOrderInfo:333, WTSTradeInfo:516, WTSPositionItem:649, WTSAccountInfo:747
- L27: `virtual ~WTSError() }`
- L39: `constexpr const char*getMessage() const noexcept { return m_strMsg; }`
- L40: `constexpr WTSErroCodegetErrorCode() const noexcept { return m_errCode; }`
- L107: `virtual ~WTSEntrust() }`
- L140: `constexpr inline boolisCreditBusiness() }`
- L141: `constexpr inline voidsetCreditBalance(double credBal) noexcept { m_dCredBal = credBal; }`
- L142: `constexpr inline doublegetCreditBalance() const noexcept { return m_dCredBal; }`
- L144: `constexpr inline voidsetDirection(WTSDirectionType dType) noexcept {m_direction = dType;}`
- L145: `constexpr inline voidsetPriceType(WTSPriceType pType) noexcept {m_priceType = pType;}`
- L146: `constexpr inline voidsetOrderFlag(WTSOrderFlag oFlag) noexcept {m_orderFlag = oFlag;}`
- L147: `constexpr inline voidsetOffsetType(WTSOffsetType oType) noexcept {m_offsetType = oType;}`
- L149: `constexpr inline WTSDirectionTypegetDirection() const noexcept {return m_direction;}`
- L150: `constexpr inline WTSPriceTypegetPriceType() const noexcept {return m_priceType;}`
- L151: `constexpr inline WTSOrderFlaggetOrderFlag() const noexcept {return m_orderFlag;}`
- L152: `constexpr inline WTSOffsetTypegetOffsetType() const noexcept {return m_offsetType;}`
- L154: `constexpr inline voidsetBusinessType(WTSBusinessType bType) noexcept { m_businessType = bType; }`
- L155: `constexpr inline WTSBusinessTypegetBusinessType() const noexcept { return m_businessType; }`
- L157: `constexpr inline voidsetVolume(double volume) noexcept { m_dVolume = volume; }`
- L158: `constexpr inline voidsetPrice(double price) noexcept { m_iPrice = price; }`
- L160: `constexpr inline doublegetVolume() const noexcept { return m_dVolume; }`
- L161: `constexpr inline doublegetPrice() const noexcept { return m_iPrice; }`
- L163: `constexpr inline const char*getCode() const noexcept { return m_strCode; }`
- L164: `constexpr inline const char*getExchg() const noexcept { return m_strExchg; }`
- L166: `inline voidsetEntrustID(const char* eid) }`
- L167: `constexpr inline const char*getEntrustID() const noexcept { return m_strEntrustID; }`
- L168: `constexpr inline char*getEntrustID() noexcept { return m_strEntrustID; }`
- L170: `inline voidsetUserTag(const char* tag) }`
- L171: `constexpr inline const char*getUserTag() const noexcept { return m_strUserTag; }`
- L172: `constexpr inline char*getUserTag() noexcept { return m_strUserTag; }`
- L174: `constexpr inline voidsetNetDirection(bool isBuy) noexcept { m_bIsNet = true; m_bIsBuy = isBuy; }`
- L175: `constexpr inline boolisNet() const noexcept { return m_bIsNet; }`
- L176: `constexpr inline boolisNetBuy() const noexcept { return m_bIsBuy; }`
- L183: `constexpr inline voidsetContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }`
- L184: `constexpr inline WTSContractInfo*getContractInfo() const noexcept { return m_pContract; }`
- L186: `constexpr inline voidsetExtras(WTSVariant* pExtras) noexcept { m_pExtras = pExtras; }`
- L187: `inline voidsetextras(const char* pExtras) }`
- L189: `constexpr inline WTSVariant*getExtras() const noexcept { return m_pExtras; }`
- L220: `virtual ~WTSEntrustAction() }`
- L251: `constexpr inline const char*getExchg() const noexcept { return m_strExchg; }`
- L252: `constexpr inline const char*getCode() const noexcept {return m_strCode;}`
- L254: `inline voidsetExchange(const char* exchg, std::size_t len = 0) noexcept`
- L258: `inline voidsetCode(const char* code, std::size_t len = 0) noexcept`
- L262: `constexpr inline voidsetActionFlag(WTSActionFlag af) noexcept {m_actionFlag = af;}`
- L263: `constexpr inline WTSActionFlaggetActionFlag() const noexcept {return m_actionFlag;}`
- L265: `inline voidsetEntrustID(const char* eid) }`
- L266: `constexpr inline const char*getEntrustID() const noexcept {return m_strEnturstID;}`
- L267: `constexpr inline char*getEntrustID() noexcept { return m_strEnturstID; }`
- L269: `inline voidsetOrderID(const char* oid) }`
- L270: `constexpr inline const char*getOrderID() const noexcept {return m_strOrderID;}`
- L272: `constexpr inline voidsetBusinessType(WTSBusinessType bType) noexcept { m_businessType = bType; }`
- L273: `constexpr inline WTSBusinessTypegetBusinessType() const noexcept { return m_businessType; }`
- L275: `inline voidsetUserTag(const char* tag) }`
- L276: `constexpr inline const char*getUserTag() const noexcept { return m_strUserTag; }`
- L277: `constexpr inline char*getUserTag() noexcept { return m_strUserTag; }`
- L279: `constexpr inline voidsetContractInfo(WTSContractInfo* cInfo) noexcept { m_pContract = cInfo; }`
- L280: `constexpr inline WTSContractInfo*getContractInfo() const noexcept { return m_pContract; }`
- L336: `virtual ~WTSOrderInfo() }`
- L366: `inline voidsetExchange(const char* exchg, std::size_t len = 0) noexcept`
- L370: `inline voidsetCode(const char* code, std::size_t len = 0) noexcept`
- L374: `constexpr inline voidsetDirection(WTSDirectionType dType) noexcept { m_direction = dType; }`
- L375: `constexpr inline voidsetPriceType(WTSPriceType pType) noexcept { m_priceType = pType; }`
- L376: `constexpr inline voidsetOrderFlag(WTSOrderFlag oFlag) noexcept { m_orderFlag = oFlag; }`
- L377: `constexpr inline voidsetOffsetType(WTSOffsetType oType) noexcept { m_offsetType = oType; }`
- L379: `constexpr inline WTSDirectionTypegetDirection() const noexcept { return m_direction; }`
- L380: `constexpr inline WTSPriceTypegetPriceType() const noexcept { return m_priceType; }`
- L381: `constexpr inline WTSOrderFlaggetOrderFlag() const noexcept { return m_orderFlag; }`
- L382: `constexpr inline WTSOffsetTypegetOffsetType() const noexcept { return m_offsetType; }`
- L402: `constexpr inline voidsetBusinessType(WTSBusinessType bType) noexcept { m_businessType = bType; }`
- L403: `constexpr inline WTSBusinessTypegetBusinessType() const noexcept { return m_businessType; }`
- L405: `constexpr inline voidsetVolume(double volume) noexcept { m_dVolume = volume; }`
- （另有 139 个声明，按需用 Read 定位）

### `src/Includes/WTSVariant.hpp` (744 行)
- 类型: WTSVariant:38
- L107: `std::stringvalue(static_cast<size_t>(length) + 1, '\0')`
- L724: `WTSObject::release()`
- L727: `inline ValueTypetype() const{ return _type; }`
- L729: `inline boolisArray() const{ return _type == VT_Array; }`
- L730: `inline boolisObject() const{ return _type == VT_Object; }`

### `src/Includes/WTSSessionInfo.hpp` (545 行)
- 类型: WTSSessionInfo:20
- L61: `virtual ~WTSSessionInfo() }`
- L64: `const char*id() }`
- L65: `const char*name() }`
- L76: `int32_tgetOffsetMins() const{return m_uOffsetMins;}`
- L103: `voidsetOffsetMins(int32_t offset) m_uOffsetMins = offset;}`
- L105: `const TradingTimes&getTradingSections() const{ return m_tradingTimes; }`
- L106: `const TradingTimes&getAuctionSections() const{ return m_auctionTimes; }`
- L110: `uint32_tgetSectionCount() }`
- L121: `TimeUtils::getDateTime(uDate, uTime)`

### `src/Includes/WTSCollection.hpp` (940 行)
- 类型: WTSArray:32, WTSMap:315, WTSHashMap:595, WTSQueue:755, WTSStack:876
- L58: `inline uint32_tsize() }`
- L60: `inline boolempty() }`
- L177: `std::swap(_vec.at(idx), obj)`
- L295: `std::sort(_vec.begin(), _vec.end(), func)`
- L300: `virtual ~WTSArray() }`
- L340: `uint32_tsize() }`
- L599: `virtual ~WTSHashMap() }`
- L623: `inline uint32_tsize() }`
- L804: `inline uint32_tsize() }`
- L806: `inline boolempty() }`
- L871: `virtual ~WTSQueue() }`
- L885: `inline uint32_tsize() }`
- L886: `inline boolempty() }`
- L926: `WTSObject::release()`

### `src/Share/CodeHelper.hpp` (663 行)
- 类型: CodeHelper:39
- L69: `inline boolisExright() const { return _exright != 0; }`
- L72: `inline boolisForwardAdj() const { return _exright == 1; }`
- L75: `inline boolisBackwardAdj() const { return _exright == 2; }`
- L452: `StrUtil::replace(s, "-", ".")`
- L521: `StrUtil::replace(ret, ".", "-")`
- L523: `StrUtil::replace(ret, ".", "")`

### `src/Share/charconv.hpp` (670 行)
- 类型: UTF8toChar:43, ChartoUTF8:214, URLEncode:388, URLDecode:469, EncodingHelper:543
- L560: `static boolisGBK(unsigned char* data, std::size_t len)`
- L601: `static intpreNUm(unsigned char byte)`
- L635: `static boolisUtf8(unsigned char* data, std::size_t len)`

