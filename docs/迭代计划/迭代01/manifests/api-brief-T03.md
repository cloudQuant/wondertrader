# API 简报 T03

覆盖模块: ParserShm, ParserUDP, ParserXTP, ParserXeleSkt。
每条形如 `L<行号>: <签名>`，可直接 Read 该文件对应行段，不要整模块通读。语义/分支以源码为准，本表只给入口。

## `ParserShm`

### `src/ParserShm/ParserShm.cpp` (329 行)
- L29: `fmtutil::format_to(buffer, format, args...)`
- L103: `std::this_thread::sleep_for(std::chrono::seconds(2))`
- L124: `CpuHelper::bind_core(_cpu - 1)`
- L141: `std::this_thread::sleep_for(std::chrono::microseconds(_check_span))`
- L149: `std::this_thread::sleep_for(std::chrono::microseconds(_check_span))`
- L159: `std::this_thread::sleep_for(std::chrono::microseconds(_check_span))`

### `src/ParserShm/ParserShm.h` (95 行)
- 类型: ParserShm:24, _DataQueue:46
- L62: `virtual boolinit(WTSVariant* config) override`
- L64: `virtual voidrelease() override`
- L66: `virtual boolconnect() override`
- L68: `virtual booldisconnect() override`
- L70: `virtual boolisConnected() override`
- L72: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L73: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L75: `virtual voidregisterSpi(IParserSpi* listener) override`

## `ParserUDP`

### `src/ParserUDP/ParserUDP.cpp` (458 行)
- 类型: UDPDataPacket:51
- L26: `fmtutil::format_to(buffer, format, args...)`
- L153: `StdUniqueLocklock(_mtx_queue)`
- L198: `StdUniqueLocklock(_mtx_queue)`
- L209: `StdUniqueLocklock(_mtx_queue)`
- L218: `StdUniqueLocklock(_mtx_queue)`
- L226: `boost::bind(&ParserUDP::handle_write, this, boost::asio::placeholders::error))`
- L247: `StdUniqueLocklock(_mtx_queue)`
- L278: `boost::asio::post(_strand, boost::bind(&ParserUDP::doOnDisconnected, this))`
- L329: `std::this_thread::sleep_for(std::chrono::seconds(2))`

### `src/ParserUDP/ParserUDP.h` (94 行)
- 类型: ParserUDP:24
- L32: `virtual boolinit(WTSVariant* config) override`
- L34: `virtual voidrelease() override`
- L36: `virtual boolconnect() override`
- L38: `virtual booldisconnect() override`
- L40: `virtual boolisConnected() override`
- L42: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L43: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L45: `virtual voidregisterSpi(IParserSpi* listener) override`
- L49: `voidhandle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad)`
- L50: `voidhandle_write(const boost::system::error_code& e)`
- L52: `boolreconnect(uint32_t flag = 3)`
- L54: `voidsubscribe()`
- L56: `voidextract_buffer(uint32_t length, bool isBroad)`
- L59: `voiddoOnConnected()`
- L60: `voiddoOnDisconnected()`
- L62: `voiddo_send()`

## `ParserXTP`

### `src/ParserXTP/ParserXTP.cpp` (696 行)
- L34: `fmt::format_to(buffer, format, args...)`
- L117: `fs::create_directories(path.c_str())`
- L153: `std::this_thread::sleep_for(std::chrono::milliseconds(2))`
- L191: `boost::asio::post(_asyncio, [this]()`
- L193: `std::this_thread::sleep_for(std::chrono::seconds(2))`

### `src/ParserXTP/ParserXTP.h` (132 行)
- 类型: WTSTickData:24, ParserXTP:29
- L33: `virtual ~ParserXTP()`
- L45: `virtual boolinit(WTSVariant* config) override`
- L47: `virtual voidrelease() override`
- L49: `virtual boolconnect() override`
- L51: `virtual booldisconnect() override`
- L53: `virtual boolisConnected() override`
- L55: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L56: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L58: `virtual voidregisterSpi(IParserSpi* listener) override`
- L70: `virtual voidOnDisconnected(int reason) override`
- L73: `virtual voidOnError(XTPRI *error_info) override`
- L75: `virtual voidOnSubTickByTick(XTPST *ticker, XTPRI *error_info, bool is_last) override`
- L76: `virtual voidOnSubMarketData(XTPST *ticker, XTPRI *error_info, bool is_last) override`
- L77: `virtual voidOnUnSubMarketData(XTPST *ticker, XTPRI *error_info, bool is_last) override`
- L79: `virtual voidOnDepthMarketData(XTPMD *market_data, int64_t bid1_qty[], int32_t bid1_count, int32_t max_bid1_count, int64_t ask1_qty[], int32_t ask1_count, int32_t max_ask1_count) override`
- L80: `virtual voidOnTickByTick(XTPTBT *tbt_data) override`
- L86: `voidDoLogin()`
- L90: `voidDoSubscribeMD()`
- L94: `boolIsErrorRspInfo(XTPRI *error_info)`

## `ParserXeleSkt`

### `src/ParserXeleSkt/ParserXeleSkt.cpp` (541 行)
- 类型: tm:391, tm:451
- L32: `fmt::format_to(buffer, format, args...)`
- L159: `ip::tcp::sockets(io_context)`
- L289: `boost::asio::post(_strand, boost::bind(&ParserXeleSkt::doOnDisconnected, this))`
- L343: `std::this_thread::sleep_for(std::chrono::seconds(2))`

### `src/ParserXeleSkt/ParserXeleSkt.h` (95 行)
- 类型: ParserXeleSkt:26
- L34: `virtual boolinit(WTSVariant* config) override`
- L36: `virtual voidrelease() override`
- L38: `virtual boolconnect() override`
- L40: `virtual booldisconnect() override`
- L42: `virtual boolisConnected() override`
- L44: `virtual voidsubscribe(const CodeSet &vecSymbols) override`
- L45: `virtual voidunsubscribe(const CodeSet &vecSymbols) override`
- L47: `virtual voidregisterSpi(IParserSpi* listener) override`
- L51: `voidhandle_udp_read(const boost::system::error_code& e, std::size_t bytes_transferred)`
- L53: `boolprepare()`
- L55: `boolreconnect()`
- L57: `voidextract_buffer(uint32_t length)`
- L60: `voiddoOnConnected()`
- L61: `voiddoOnDisconnected()`

### `src/ParserXeleSkt/md_struct.h` (244 行)
- 类型: CXeleShfeMarketHead:54, CXeleShfeMarketData:59, CXeleShfeHeartBeat:72, CXeleShfeDepthMarketData:82, CXeleShfeMarketDataStatic:112, CXeleShfeSnapShot:140, CXeleShfeDepthSnapShot:182

