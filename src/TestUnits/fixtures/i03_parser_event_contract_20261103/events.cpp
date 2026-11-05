// Card 14a=C / 14c=C parser event contract for ParserXeleSkt (R76-03) and
// ParserUDP (R76-02), mapped onto the existing WTSParserEvent codes:
//   WPE_Connect = transport established for a connection epoch;
//   WPE_Login   = confirmed: first valid peer message of that epoch (fail-closed);
//   WPE_Close   = once per active connection; a reconnect starts a new epoch.
// MODULE_DIR selects current ("./parsers") or pre-change ("./before/parsers")
// modules; the same source is the red/green control. Local endpoints only.
#include <gtest/gtest.h>
#include "../i03_parser_xele_lifecycle_v2/xele_fixture.hpp"
#include "Includes/IParserLifecycleV2.h"

using namespace xelefx;

namespace {
struct Module
{
	explicit Module(const std::string& path) : handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {}
	void* handle;
	FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle, "createParser")) : nullptr;
	FuncDeleteParser remove = handle ? reinterpret_cast<FuncDeleteParser>(dlsym(handle, "deleteParser")) : nullptr;
	WtParserLifecycleV2GetterFn get = handle ? reinterpret_cast<WtParserLifecycleV2GetterFn>(dlsym(handle, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL)) : nullptr;
	const WtParserLifecycleApiV2* table = get ? get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR) : nullptr;
	// Never dlclose: red runs may leave provider code queued.
};
std::string modulePath(const char* name) { return std::string(MODULE_DIR) + "/lib" + name + ".dylib"; }
std::string faultPath(const char* name) { return std::string(FAULT_DIR) + "/lib" + name + ".dylib"; }

// Ordered event log: C=Connect L=Login X=Close O=Logout Q=quote.
class EventSink : public IParserSpi
{
public:
	BaseData base;
	bool udpBase = false; // ParserUDP needs no base data manager
	std::function<void(const char*)> onLog;
	std::mutex mutex; std::condition_variable cv; std::string log;
	IBaseDataMgr* getBaseDataMgr() override { return udpBase ? nullptr : &base; }
	void handleSymbolList(const WTSArray*) override {}
	void handleParserLog(WTSLogLevel, const char* message) override { if (onLog) onLog(message ? message : ""); }
	void handleEvent(WTSParserEvent e, int32_t) override
	{
		const char c = e == WPE_Connect ? 'C' : e == WPE_Login ? 'L' : e == WPE_Close ? 'X' : 'O';
		std::lock_guard<std::mutex> l(mutex); log.push_back(c); cv.notify_all();
	}
	void handleQuote(WTSTickData*, uint32_t) override { std::lock_guard<std::mutex> l(mutex); log.push_back('Q'); cv.notify_all(); }
	std::string events() { std::lock_guard<std::mutex> l(mutex); std::string r; for (char c : log) if (c != 'Q') r.push_back(c); return r; }
	std::string all() { std::lock_guard<std::mutex> l(mutex); return log; }
	int count(char c) { std::lock_guard<std::mutex> l(mutex); int n = 0; for (char x : log) n += x == c; return n; }
	bool waitCount(char c, int n, std::chrono::milliseconds t = 2s)
	{
		std::unique_lock<std::mutex> l(mutex);
		return cv.wait_for(l, t, [&] { int k = 0; for (char x : log) k += x == c; return k >= n; });
	}
};

// Accepts one peer, waits `delay`, then sends `bytes` and EOF.
class DelayedSnapshotServer
{
public:
	DelayedSnapshotServer(std::string bytes, std::chrono::milliseconds delay) : _bytes(std::move(bytes)), _delay(delay)
	{
		_fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(_fd, 4) == 0)
		{ socklen_t len = sizeof(a); getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len); _port = ntohs(a.sin_port); }
		_thread = std::thread([this] {
			pollfd p{ _fd, POLLIN, 0 };
			if (poll(&p, 1, 5000) <= 0) return;
			int peer = accept(_fd, nullptr, nullptr); if (peer < 0) return;
			std::this_thread::sleep_for(_delay);
			(void)!write(peer, _bytes.data(), _bytes.size());
			shutdown(peer, SHUT_WR); close(peer);
		});
	}
	~DelayedSnapshotServer() { _thread.join(); close(_fd); }
	uint16_t port() const { return _port; }
private:
	std::string _bytes; std::chrono::milliseconds _delay; int _fd = -1; uint16_t _port = 0; std::thread _thread;
};

// ---------------------------------------------------------------- Xele
struct Xele
{
	Module module{ modulePath("ParserXeleSkt") };
	EventSink sink;
	SnapshotServer server{ snapshotFrame("ag2512", kInstrumentNo, 1.0) };
	uint16_t port = freeUdpPort();
	IParserApi* api = nullptr;
	bool create(uint16_t tcpPort = 0)
	{
		if (!module.create || !module.remove || port == 0) return false;
		api = module.create(); if (!api) return false;
		api->registerSpi(&sink);
		Config cfg(tcpPort ? tcpPort : server.port(), port);
		if (!api->init(cfg.value)) return false;
		api->subscribe(subscription());
		return true;
	}
	void frame(const std::string& bytes) { sendUnicast(port, bytes); }
	bool pumpQuote(int n) { for (int i = 0; i < 100; ++i) { frame(marketDataFrame(kInstrumentNo, 100)); if (sink.waitCount('Q', n, 20ms)) return true; } return false; }
	~Xele() { if (api && module.remove) module.remove(api); }
};

TEST(XeleEvents, ConnectThenLoginOnlyAfterFirstValidFrame)
{
	Xele x; ASSERT_TRUE(x.create()); ASSERT_TRUE(x.api->connect());
	ASSERT_TRUE(x.sink.waitCount('C', 1)) << "transport event after snapshot + join";
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("C", x.sink.events()) << "no confirmation without peer data (fail-closed)";
	ASSERT_TRUE(x.pumpQuote(3));
	EXPECT_EQ(0u, x.sink.all().find("CLQ")) << x.sink.all();
	EXPECT_EQ(1, x.sink.count('L'));
	EXPECT_EQ(1, x.sink.count('C'));
}

TEST(XeleEvents, SnapshotFailureEmitsNoSuccessEvent)
{
	Xele x; ASSERT_TRUE(x.create(1)); EXPECT_FALSE(x.api->connect());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("", x.sink.events());
}

TEST(XeleEvents, InvalidFramesDoNotConfirm)
{
	Xele x; ASSERT_TRUE(x.create()); ASSERT_TRUE(x.api->connect());
	ASSERT_TRUE(x.sink.waitCount('C', 1));
	std::string unknown = marketDataFrame(kInstrumentNo, 100); unknown[1] = 0x7f;
	std::string badVersion = marketDataFrame(kInstrumentNo, 100); badVersion[0] = 0x09;
	std::string truncated = marketDataFrame(kInstrumentNo, 100); truncated.resize(truncated.size() - 1);
	x.frame(unknown); x.frame(badVersion); x.frame(truncated);
	std::this_thread::sleep_for(100ms);
	EXPECT_EQ("C", x.sink.events());
	ASSERT_TRUE(x.pumpQuote(1));
	EXPECT_EQ("CL", x.sink.events());
}

TEST(XeleEvents, ReconnectStartsNewEventEpoch)
{
	Xele x; ASSERT_TRUE(x.create());
	for (int epoch = 1; epoch <= 2; ++epoch)
	{
		ASSERT_TRUE(x.api->connect());
		ASSERT_TRUE(x.sink.waitCount('C', epoch));
		ASSERT_TRUE(x.pumpQuote(x.sink.count('Q') + 1));
		ASSERT_TRUE(x.sink.waitCount('L', epoch));
		ASSERT_TRUE(x.api->disconnect());
		ASSERT_TRUE(x.sink.waitCount('X', epoch));
	}
	EXPECT_EQ("CLXCLX", x.sink.events());
}

TEST(XeleEvents, ReleaseFromAnotherThreadDuringConnectEmitsNothing)
{
	DelayedSnapshotServer slow(snapshotFrame("ag2512", kInstrumentNo, 1.0), 300ms);
	Xele x; ASSERT_TRUE(x.create(slow.port()));
	std::atomic<int> connected{ -1 };
	std::thread connector([&] { connected = x.api->connect() ? 1 : 0; });
	std::this_thread::sleep_for(50ms);
	x.api->release();
	connector.join();
	EXPECT_EQ(0, connected.load());
	EXPECT_FALSE(x.api->connect());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("", x.sink.events());
	x.module.remove(x.api); EXPECT_EQ(nullptr, x.api);
}

TEST(XeleEvents, ReleaseFromLogCallbackDuringConnectEmitsNothing)
{
	Xele x;
	std::atomic<bool> once{ false };
	x.sink.onLog = [&](const char* m) { if (std::strstr(m, "Preparing snapshots") && !once.exchange(true)) x.api->release(); };
	ASSERT_TRUE(x.create());
	EXPECT_FALSE(x.api->connect());
	EXPECT_TRUE(once.load());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("", x.sink.events());
	x.module.remove(x.api); EXPECT_EQ(nullptr, x.api);
}

TEST(XeleEvents, V2StopDuringConnectDrainsWithoutEvents)
{
	DelayedSnapshotServer slow(snapshotFrame("ag2512", kInstrumentNo, 1.0), 300ms);
	Xele x; ASSERT_NE(nullptr, x.module.table); ASSERT_TRUE(x.create(slow.port()));
	std::atomic<int> connected{ -1 };
	std::thread connector([&] { connected = x.api->connect() ? 1 : 0; });
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, x.module.table->request_stop(x.api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, x.module.table->wait_drained(x.api, 0)) << "connect still admitted";
	connector.join();
	EXPECT_EQ(0, connected.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, x.module.table->wait_drained(x.api, 1000));
	void* opaque = x.api; EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, x.module.table->destroy_api(&opaque));
	if (!opaque) x.api = nullptr;
	EXPECT_EQ("", x.sink.events());
}

// ---------------------------------------------------------------- UDP
bool sendUdpMessage(uint16_t port, uint32_t type, const void* payload, std::size_t size)
{
	std::string packet(sizeof(type) + size, '\0');
	std::memcpy(&packet[0], &type, sizeof(type)); if (size) std::memcpy(&packet[sizeof(type)], payload, size);
	return sendUnicast(port, packet);
}

struct Udp
{
	explicit Udp(const std::string& path = modulePath("ParserUDP")) : module(path) { sink.udpBase = true; }
	Module module;
	EventSink sink;
	uint16_t port = freeUdpPort();
	IParserApi* api = nullptr;
	bool create()
	{
		if (!module.create || !module.remove || port == 0) return false;
		api = module.create(); if (!api) return false;
		api->registerSpi(&sink);
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("host", "127.0.0.1"); cfg->append("bport", uint32_t(port)); cfg->append("sport", uint32_t(9)); cfg->append("gpsize", uint32_t(1000));
		const bool ok = api->init(cfg); cfg->release();
		CodeSet s; s.insert("EX.CODE"); api->subscribe(s);
		return ok;
	}
	void tick() { WTSTickStruct t; std::strcpy(t.exchg, "EX"); std::strcpy(t.code, "CODE"); sendUdpMessage(port, 0x200, &t, sizeof(t)); }
	bool pumpQuote(int n) { for (int i = 0; i < 100; ++i) { tick(); if (sink.waitCount('Q', n, 20ms)) return true; } return false; }
	~Udp() { if (api && module.remove) module.remove(api); }
};

TEST(UdpEvents, ConnectThenLoginOnlyAfterFirstValidMessage)
{
	Udp u; ASSERT_TRUE(u.create()); ASSERT_TRUE(u.api->connect());
	ASSERT_TRUE(u.sink.waitCount('C', 1)) << "transport event after both channels are armed";
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("C", u.sink.events()) << "subscription send completion is not a confirmation";
	ASSERT_TRUE(u.pumpQuote(3));
	EXPECT_EQ(0u, u.sink.all().find("CLQ")) << u.sink.all();
	EXPECT_EQ(1, u.sink.count('L'));
	EXPECT_EQ(1, u.sink.count('C'));
}

TEST(UdpEvents, PartialConnectFailureEmitsNoSuccessEvent)
{
	Udp u(faultPath("ParserUDP"));
	auto fail = u.module.handle ? reinterpret_cast<void (*)()>(dlsym(u.module.handle, "parserUdpTestFailAfterReceiveArm")) : nullptr;
	ASSERT_NE(nullptr, fail); ASSERT_TRUE(u.create());
	fail(); EXPECT_FALSE(u.api->connect());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("", u.sink.events());
}

TEST(UdpEvents, InvalidMessagesDoNotConfirm)
{
	Udp u; ASSERT_TRUE(u.create()); ASSERT_TRUE(u.api->connect());
	ASSERT_TRUE(u.sink.waitCount('C', 1));
	WTSTickStruct t; std::strcpy(t.exchg, "EX"); std::strcpy(t.code, "CODE");
	sendUdpMessage(u.port, 0x999, &t, sizeof(t));        // unknown type
	sendUdpMessage(u.port, 0x200, &t, sizeof(t) - 1);    // short tick
	sendUdpMessage(u.port, 0x200, nullptr, 0);           // header only
	std::this_thread::sleep_for(100ms);
	EXPECT_EQ("C", u.sink.events());
	ASSERT_TRUE(u.pumpQuote(1));
	EXPECT_EQ("CL", u.sink.events());
}

TEST(UdpEvents, ReconnectStartsNewEventEpoch)
{
	Udp u; ASSERT_TRUE(u.create());
	for (int epoch = 1; epoch <= 2; ++epoch)
	{
		ASSERT_TRUE(u.api->connect());
		ASSERT_TRUE(u.sink.waitCount('C', epoch));
		ASSERT_TRUE(u.pumpQuote(u.sink.count('Q') + 1));
		ASSERT_TRUE(u.sink.waitCount('L', epoch));
		ASSERT_TRUE(u.api->disconnect());
		ASSERT_TRUE(u.sink.waitCount('X', epoch));
	}
	EXPECT_EQ("CLXCLX", u.sink.events());
}

TEST(UdpEvents, ReleaseFromLogCallbackDuringConnectEmitsNothing)
{
	Udp u;
	std::atomic<bool> once{ false };
	u.sink.onLog = [&](const char* m) { if (std::strstr(m, "subscribing packets") && !once.exchange(true)) u.api->release(); };
	ASSERT_TRUE(u.create());
	EXPECT_FALSE(u.api->connect());
	EXPECT_TRUE(once.load());
	std::this_thread::sleep_for(50ms);
	EXPECT_EQ("", u.sink.events());
	u.module.remove(u.api); EXPECT_EQ(nullptr, u.api);
}

TEST(UdpEvents, V2StopFromLogCallbackDuringConnectDrainsWithoutEvents)
{
	Udp u; ASSERT_NE(nullptr, u.module.table);
	std::atomic<bool> once{ false };
	u.sink.onLog = [&](const char* m) { if (std::strstr(m, "subscribing packets") && !once.exchange(true)) u.module.table->request_stop(u.api); };
	ASSERT_TRUE(u.create());
	EXPECT_FALSE(u.api->connect());
	EXPECT_TRUE(once.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, u.module.table->wait_drained(u.api, 1000));
	void* opaque = u.api; EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, u.module.table->destroy_api(&opaque));
	if (!opaque) u.api = nullptr;
	EXPECT_EQ("", u.sink.events());
}
}
