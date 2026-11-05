// BUG-R12-13 (ParserXeleSkt trading_date) and I03-NEW-011 (strToTime), S5.
// Local loopback/fake only: no vendor API, no multicast traffic.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "ParserXeleSkt/ParserXeleSkt.h"
#include "ParserXeleSkt/md_struct.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

using namespace wtp;

struct ParserXeleSktTestAccess
{
	// Install one cached tick for instrumentNo, as prepare() would.
	static void seed(ParserXeleSkt& parser, WTSContractInfo* contract, int instrumentNo)
	{
		std::lock_guard<std::recursive_mutex> lock(parser._mtx_io);
		if (parser._tick_cache == nullptr)
			parser._tick_cache = ParserXeleSkt::TickCache::create();
		WTSTickData* tick = WTSTickData::create(contract->getCode());
		tick->setContractInfo(contract);
		std::strncpy(tick->getTickStruct().exchg, contract->getExchg(), sizeof(tick->getTickStruct().exchg) - 1);
		parser._tick_cache->add(instrumentNo, tick, false);
		parser._price_scales[instrumentNo] = 1.0;
	}
	static void feed(ParserXeleSkt& parser, const std::string& bytes)
	{
		std::lock_guard<std::recursive_mutex> lock(parser._mtx_io);
		parser.extract_buffer(static_cast<uint32_t>(bytes.size()), bytes.data(), 0);
	}
	static WTSTickStruct quote(ParserXeleSkt& parser, int instrumentNo)
	{
		std::lock_guard<std::recursive_mutex> lock(parser._mtx_io);
		WTSTickData* tick = static_cast<WTSTickData*>(parser._tick_cache->get(instrumentNo));
		return tick == nullptr ? WTSTickStruct() : tick->getTickStruct();
	}
	static bool hasTick(ParserXeleSkt& parser, int instrumentNo)
	{
		std::lock_guard<std::recursive_mutex> lock(parser._mtx_io);
		return parser._tick_cache != nullptr && parser._tick_cache->get(instrumentNo) != nullptr;
	}
	static bool prepare(ParserXeleSkt& parser) { return parser.prepare(); }
	static void cleanup(ParserXeleSkt& parser)
	{
		std::lock_guard<std::recursive_mutex> lock(parser._mtx_io);
		if (parser._tick_cache != nullptr)
		{
			parser._tick_cache->release();
			parser._tick_cache = nullptr;
		}
	}
};

namespace {
class XeleBaseData : public IBaseDataMgr
{
public:
	XeleBaseData()
	{
		_session = WTSSessionInfo::create("FN0230", "night", 300);
		_session->addTradingSection(2100, 230);
		_session->addTradingSection(900, 1500);
		_commodity = WTSCommodityInfo::create("ag", "silver", "SHFE", "FN0230", "CHINA");
		_commodity->setSessionInfo(_session);
		_contract = WTSContractInfo::create("ag2512", "ag2512", "SHFE", "ag");
		_contract->setCommInfo(_commodity);
	}
	~XeleBaseData() { _contract->release(); _commodity->release(); _session->release(); }
	WTSContractInfo* contract() { return _contract; }

	bool knowsCommodity = true;
	bool throwOnCalc = false;
	uint32_t forcedValue = 0;
	bool forceValue = false;
	std::atomic<int> calcCalls{ 0 };

	WTSCommodityInfo* getCommodity(const char* id) override
	{ return knowsCommodity && id != nullptr && std::strcmp(id, "SHFE.ag") == 0 ? _commodity : nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return knowsCommodity ? _commodity : nullptr; }
	WTSContractInfo* getContract(const char* code, const char* = "", uint32_t = 0) override
	{ return code != nullptr && std::strcmp(code, "ag2512") == 0 ? _contract : nullptr; }
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return _session; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t date, uint32_t time, bool = false) override
	{
		++calcCalls;
		if (throwOnCalc)
			throw std::runtime_error("injected");
		if (forceValue)
			return forcedValue;
		// Night after 21:00 belongs to the next date in this fake (weekday input).
		return time >= 2100 ? date + 1 : date;
	}
	uint64_t getBoundaryTime(const char*, uint32_t date, bool = false, bool = true) override { return static_cast<uint64_t>(date) * 10000 + 900; }

private:
	WTSSessionInfo* _session; WTSCommodityInfo* _commodity; WTSContractInfo* _contract;
};

class XeleSink : public IParserSpi
{
public:
	XeleBaseData base;
	std::atomic<int> quotes{ 0 };
	std::vector<uint32_t> tradingDates;
	IBaseDataMgr* getBaseDataMgr() override { return &base; }
	void handleSymbolList(const WTSArray*) override {}
	void handleQuote(WTSTickData* tick, uint32_t) override { ++quotes; tradingDates.push_back(tick->getTickStruct().trading_date); }
	void handleParserLog(WTSLogLevel, const char*) override {}
	void handleEvent(WTSParserEvent, int32_t) override {}
};

// SnapDateTime: (seconds since XELE_MD_EPOCH) << 4. With TZ=UTC the local
// date/time is the UTC one.
std::string marketData(int instrumentNo, uint32_t secondsSinceEpoch, int32_t price)
{
	std::string frame(sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeMarketData), '\0');
	auto* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = XELE_MD_DATA_VERSION;
	head->Type = MESSAGE_MARKET_DATA;
	auto* md = reinterpret_cast<CXeleShfeMarketData*>(&frame[sizeof(CXeleShfeMarketHead)]);
	md->InstrumentNo = static_cast<int16_t>(instrumentNo);
	md->SnapDateTime = (secondsSinceEpoch << 4);
	md->LastPrice = price;
	md->Volume = 1;
	return frame;
}

constexpr time_t kXeleEpoch = 1546272000; // XELE_MD_EPOCH in ParserXeleSkt.cpp
uint32_t secondsSinceXeleEpoch(time_t utc) { return static_cast<uint32_t>(utc - kXeleEpoch); }

class XeleMapping : public ::testing::Test
{
protected:
	XeleSink sink;
	ParserXeleSkt parser;
	void SetUp() override
	{
		setenv("TZ", "UTC", 1); tzset();
		parser.registerSpi(&sink);
		ParserXeleSktTestAccess::seed(parser, sink.base.contract(), 7);
	}
	void TearDown() override { ParserXeleSktTestAccess::cleanup(parser); }
	// 2024-02-27 10:00:00 UTC (Tuesday)
	std::string tuesdayFrame(int32_t price = 100) { return marketData(7, secondsSinceXeleEpoch(1709028000), price); }
};
}

TEST_F(XeleMapping, ValidMappingWritesTradingDateAndDeliversQuote)
{
	ParserXeleSktTestAccess::feed(parser, tuesdayFrame());
	ASSERT_EQ(1, sink.quotes.load());
	EXPECT_EQ(20240227u, sink.tradingDates.at(0));
	const WTSTickStruct q = ParserXeleSktTestAccess::quote(parser, 7);
	EXPECT_EQ(20240227u, q.trading_date);
	EXPECT_EQ(20240227u, q.action_date);
	EXPECT_EQ(100000000u, q.action_time);
	// trading_date is mapped once per cached tick.
	ParserXeleSktTestAccess::feed(parser, tuesdayFrame(101));
	EXPECT_EQ(2, sink.quotes.load());
	EXPECT_EQ(1, sink.base.calcCalls.load());
}

TEST_F(XeleMapping, FailedMappingNeitherWritesNorDeliversTheQuote)
{
	struct Case { const char* name; bool known; bool raise; bool force; uint32_t value; };
	for (const Case& c : { Case{"missing commodity", false, false, false, 0},
		Case{"legacy zero", true, false, true, 0}, Case{"legacy invalid date", true, false, true, 20240230},
		Case{"legacy throws", true, true, false, 0} })
	{
		sink.base.knowsCommodity = c.known;
		sink.base.throwOnCalc = c.raise;
		sink.base.forceValue = c.force;
		sink.base.forcedValue = c.value;
		const WTSTickStruct before = ParserXeleSktTestAccess::quote(parser, 7);
		ParserXeleSktTestAccess::feed(parser, tuesdayFrame(555));
		const WTSTickStruct after = ParserXeleSktTestAccess::quote(parser, 7);
		EXPECT_EQ(0, sink.quotes.load()) << c.name;
		EXPECT_EQ(0u, after.trading_date) << c.name;
		EXPECT_EQ(before.action_date, after.action_date) << c.name;
		EXPECT_EQ(before.action_time, after.action_time) << c.name;
		EXPECT_DOUBLE_EQ(before.price, after.price) << c.name;
	}
	// Recovery: once mapping works the next frame is delivered.
	sink.base.knowsCommodity = true; sink.base.throwOnCalc = false; sink.base.forceValue = false;
	ParserXeleSktTestAccess::feed(parser, tuesdayFrame(556));
	EXPECT_EQ(1, sink.quotes.load());
	EXPECT_EQ(20240227u, ParserXeleSktTestAccess::quote(parser, 7).trading_date);
}

namespace {
std::string snapshotFrame(const char* code, int instrumentNo, const char* actionDay, std::size_t actionLen,
	const char* updateTime, std::size_t updateLen, int32_t millis)
{
	std::string frame(sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeSnapShot), '\0');
	auto* head = reinterpret_cast<CXeleShfeMarketHead*>(&frame[0]);
	head->Version = XELE_MD_DATA_VERSION;
	head->Type = MESSAGE_SNAP_SHOT;
	auto* snap = reinterpret_cast<CXeleShfeSnapShot*>(&frame[sizeof(CXeleShfeMarketHead)]);
	std::strncpy(snap->InstrumentID, code, sizeof(snap->InstrumentID) - 1);
	snap->InstrumentNo = instrumentNo;
	snap->PriceTick = 1.0;
	std::memcpy(snap->ActionDay, actionDay, actionLen);
	std::memcpy(snap->UpdateTime, updateTime, updateLen);
	snap->UpdateMilliSec = millis;
	return frame;
}

class OneShotServer
{
public:
	explicit OneShotServer(std::string bytes) : _bytes(std::move(bytes))
	{
		_fd = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1; setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(_fd, 4) == 0)
		{ socklen_t len = sizeof(a); getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len); _port = ntohs(a.sin_port); }
		_thread = std::thread([this] {
			for (int i = 0; i < 500 && !_quit; ++i)
			{
				pollfd p{ _fd, POLLIN, 0 };
				if (poll(&p, 1, 10) <= 0) continue;
				int peer = accept(_fd, nullptr, nullptr); if (peer < 0) continue;
				std::size_t written = 0;
				while (written < _bytes.size())
				{
					const ssize_t n = write(peer, _bytes.data() + written, _bytes.size() - written);
					if (n <= 0) break; written += static_cast<std::size_t>(n);
				}
				shutdown(peer, SHUT_WR); close(peer);
				break;
			}
		});
	}
	~OneShotServer() { _quit = true; if (_thread.joinable()) _thread.join(); if (_fd >= 0) close(_fd); }
	uint16_t port() const { return _port; }
private:
	std::string _bytes; int _fd = -1; uint16_t _port = 0; std::atomic<bool> _quit{ false }; std::thread _thread;
};

bool initLoopback(ParserXeleSkt& parser, uint16_t port)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("tcp_host", "127.0.0.1");
	cfg->append("tcp_port", static_cast<int32_t>(port));
	cfg->append("mcast_host", "239.255.0.1");
	cfg->append("mcast_port", static_cast<int32_t>(30001));
	cfg->append("local_host", "127.0.0.1");
	cfg->append("gpsize", static_cast<uint32_t>(1000));
	const bool ok = parser.init(cfg);
	cfg->release();
	return ok;
}

// Runs prepare() against one loopback snapshot and returns the cached quote.
bool prepareWith(const std::string& frame, XeleSink& sink, WTSTickStruct& out)
{
	OneShotServer server(frame);
	ParserXeleSkt parser;
	parser.registerSpi(&sink);
	if (!initLoopback(parser, server.port()))
		return false;
	CodeSet subs;
	subs.insert("SHFE.ag2512");
	parser.subscribe(subs);
	const bool prepared = ParserXeleSktTestAccess::prepare(parser);
	if (prepared && ParserXeleSktTestAccess::hasTick(parser, 7))
		out = ParserXeleSktTestAccess::quote(parser, 7);
	ParserXeleSktTestAccess::cleanup(parser);
	parser.release();
	return prepared;
}
}

TEST(XeleClockField, WellFormedAndEmptyFieldsKeepTheirValues)
{
	XeleSink sink;
	WTSTickStruct q{};
	ASSERT_TRUE(prepareWith(snapshotFrame("ag2512", 7, "20240227", 9, "09:30:15", 9, 500), sink, q));
	EXPECT_EQ(20240227u, q.action_date);
	EXPECT_EQ(93015500u, q.action_time);
	WTSTickStruct empty{};
	ASSERT_TRUE(prepareWith(snapshotFrame("ag2512", 7, "", 1, "", 1, 0), sink, empty));
	EXPECT_EQ(0u, empty.action_date);
	EXPECT_EQ(0u, empty.action_time);
}

// I03-NEW-011: ActionDay and UpdateTime are char[9]; a field without NUL made
// the old strlen() read the following bytes and write past static char[10].
TEST(XeleClockField, UnterminatedOrMalformedFieldsAreClearedWithoutOverflow)
{
	XeleSink sink;
	WTSTickStruct q{};
	q.action_date = 1;
	ASSERT_TRUE(prepareWith(snapshotFrame("ag2512", 7, "202402271", 9, "09:30:15X", 9, 0x31313131), sink, q));
	EXPECT_EQ(0u, q.action_date);
	EXPECT_EQ(0u, q.action_time);
	WTSTickStruct letters{};
	letters.action_date = 1;
	ASSERT_TRUE(prepareWith(snapshotFrame("ag2512", 7, "2024AB27", 9, "09:30:15", 9, 0), sink, letters));
	EXPECT_EQ(0u, letters.action_date);
	EXPECT_EQ(0u, letters.action_time);
}

// I03-NEW-011: the old parser shared one static buffer across instances.
TEST(XeleClockField, ConcurrentPreparesKeepIndependentValues)
{
	std::atomic<int> mismatches{ 0 };
	auto worker = [&](const char* day, const char* time, uint32_t expectDay, uint32_t expectTime) {
		for (int i = 0; i < 20; ++i)
		{
			XeleSink sink;
			WTSTickStruct q{};
			if (!prepareWith(snapshotFrame("ag2512", 7, day, 9, time, 9, 0), sink, q) ||
				q.action_date != expectDay || q.action_time != expectTime)
				++mismatches;
		}
	};
	std::thread a(worker, "20240227", "09:30:15", 20240227u, 93015000u);
	std::thread b(worker, "20231130", "21:05:59", 20231130u, 210559000u);
	a.join();
	b.join();
	EXPECT_EQ(0, mismatches.load());
}
