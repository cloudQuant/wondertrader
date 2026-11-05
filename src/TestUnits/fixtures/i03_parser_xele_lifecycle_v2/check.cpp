// ParserXeleSkt lifecycle V2 checks against the real dynamic module and the
// real ParserAdapter. Endpoints are local (see xele_fixture.hpp).
#include <gtest/gtest.h>
#include "xele_fixture.hpp"
#include "Includes/IParserLifecycleV2.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WtCore/ParserAdapter.h"

using namespace xelefx;

namespace {
struct Module
{
	explicit Module(const char* path = "./parsers/libParserXeleSkt.dylib") : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {}
	void* handle;
	FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle, "createParser")) : nullptr;
	FuncDeleteParser remove = handle ? reinterpret_cast<FuncDeleteParser>(dlsym(handle, "deleteParser")) : nullptr;
	WtParserLifecycleV2GetterFn get = handle ? reinterpret_cast<WtParserLifecycleV2GetterFn>(dlsym(handle, WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL)) : nullptr;
	const WtParserLifecycleApiV2* table = get ? get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR) : nullptr;
	int live = 0;
	~Module() { if (handle && live == 0) dlclose(handle); }
};

WtParserLifecycleV2Status waitResult(const WtParserLifecycleApiV2* table, IParserApi* api)
{
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	WtParserLifecycleV2Status status;
	do {
		status = table->wait_drained(static_cast<void*>(api), 0);
		if (status != WT_PARSER_LIFECYCLE_V2_PENDING) return status;
		std::this_thread::sleep_for(2ms);
	} while (std::chrono::steady_clock::now() < deadline);
	return status;
}

struct Owner
{
	Module& module; const WtParserLifecycleApiV2* table; IParserApi* api;
	explicit Owner(Module& m) : module(m), table(m.table), api(m.create ? m.create() : nullptr) { if (api) ++module.live; }
	bool stopAndDestroy()
	{
		if (!api) return true;
		if (table->request_stop(static_cast<void*>(api)) != WT_PARSER_LIFECYCLE_V2_OK) return false;
		if (waitResult(table, api) != WT_PARSER_LIFECYCLE_V2_DRAINED) return false;
		void* opaque = api;
		if (table->destroy_api(&opaque) != WT_PARSER_LIFECYCLE_V2_OK || opaque) return false;
		api = nullptr; --module.live; return true;
	}
	~Owner() { (void)stopAndDestroy(); }
};

class BlockingLogSink : public Sink
{
public:
	const WtParserLifecycleApiV2* table = nullptr; IParserApi* api = nullptr;
	bool entered = false, release = false; std::atomic<int> canWait{ 999 };
	void handleParserLog(WTSLogLevel, const char*) override
	{
		canWait = table->can_wait(api);
		std::unique_lock<std::mutex> lock(mutex); entered = true; cv.notify_all();
		cv.wait(lock, [this] { return release; });
	}
};

void logInit()
{
	static bool once = [] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr); return true; }();
	(void)once;
}
}

TEST(ParserXeleV2, TableAndPartialInit)
{
	Module module; ASSERT_NE(nullptr, module.handle); ASSERT_NE(nullptr, module.table); ASSERT_NE(nullptr, module.create);
	EXPECT_GE(module.table->struct_size, WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR, module.table->abi_version >> 16);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, module.table->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->preflight_factory());
	EXPECT_EQ(nullptr, module.get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR + 1));
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	EXPECT_FALSE(owner.api->init(nullptr));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT, module.table->wait_drained(owner.api, 0)) << "wait before stop";
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api));
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api)) << "idempotent stop";
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, module.table->wait_drained(owner.api, 0));
	EXPECT_FALSE(owner.api->connect());
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, RepeatedInitIsRejectedAndKeepsOriginal)
{
	Module module; ASSERT_NE(nullptr, module.table);
	SnapshotServer original(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	const uint16_t port = freeUdpPort(); ASSERT_NE(0, port);
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config first(original.port(), port), second(1, port); // port 1: nothing listens there
	ASSERT_TRUE(owner.api->init(first.value));
	EXPECT_FALSE(owner.api->init(second.value));
	owner.api->subscribe(subscription());
	ASSERT_TRUE(owner.api->connect()) << "original TCP endpoint must be retained";
	EXPECT_EQ(1, original.served());
	EXPECT_TRUE(pumpUntilQuotes(sink, port, 1));
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, ReconnectCycleThenDrain)
{
	Module module; ASSERT_NE(nullptr, module.table);
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	const uint16_t port = freeUdpPort(); ASSERT_NE(0, port);
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config cfg(server.port(), port); ASSERT_TRUE(owner.api->init(cfg.value)); owner.api->subscribe(subscription());
	for (int cycle = 1; cycle <= 3; ++cycle)
	{
		ASSERT_TRUE(owner.api->connect()) << "cycle " << cycle;
		ASSERT_TRUE(owner.api->connect()) << "connect while connected is a no-op";
		ASSERT_TRUE(pumpUntilQuotes(sink, port, sink.quoteCount() + 1)) << "cycle " << cycle;
		ASSERT_TRUE(owner.api->disconnect());
		ASSERT_TRUE(owner.api->disconnect()) << "repeated disconnect";
		ASSERT_TRUE(sink.waitCloses(cycle));
	}
	EXPECT_EQ(3, sink.closeCount()) << "one Close per active connection";
	// Card 14a=C (decided 2026-11-03): one transport Connect and one confirmed
	// Login (after peer data) per connection epoch.
	EXPECT_EQ(3, sink.connects);
	EXPECT_EQ(3, sink.logins);
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, SnapshotFailureStartsNoWorkerAndDrains)
{
	Module module; ASSERT_NE(nullptr, module.table);
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config cfg(1, freeUdpPort()); ASSERT_TRUE(owner.api->init(cfg.value));
	EXPECT_FALSE(owner.api->connect());
	EXPECT_FALSE(owner.api->isConnected());
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, module.table->wait_drained(owner.api, 0)) << "no worker to wait for";
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, CallbackStopDefersDrainUntilCallbackAndWorkerExit)
{
	Module module; ASSERT_NE(nullptr, module.table);
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	const uint16_t port = freeUdpPort(); ASSERT_NE(0, port);
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api);
	std::atomic<int> stopStatus{ 999 }, canWaitStatus{ 999 }, waitStatus{ 999 }, destroyStatus{ 999 };
	std::atomic<bool> once{ false };
	sink.hold = true;
	sink.onQuote = [&] {
		if (once.exchange(true)) return;
		stopStatus = module.table->request_stop(owner.api);
		canWaitStatus = module.table->can_wait(owner.api);
		waitStatus = module.table->wait_drained(owner.api, 0);
		void* opaque = owner.api; destroyStatus = module.table->destroy_api(&opaque);
	};
	owner.api->registerSpi(&sink); Config cfg(server.port(), port); ASSERT_TRUE(owner.api->init(cfg.value));
	owner.api->subscribe(subscription()); ASSERT_TRUE(owner.api->connect());
	const bool arrived = pumpUntilQuotes(sink, port, 1);
	if (!arrived) { sink.resume(); EXPECT_TRUE(owner.stopAndDestroy()); FAIL() << "loopback callback timed out"; }
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, stopStatus.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK, canWaitStatus.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK, waitStatus.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, destroyStatus.load()) << "destroy on the worker must refuse";
	const auto start = std::chrono::steady_clock::now();
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api));
	EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, module.table->wait_drained(owner.api, 0));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, module.table->wait_drained(owner.api, 30)) << "callback still held";
	sink.resume();
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, ConcurrentHostApiCallbackKeepsPollPending)
{
	Module module; ASSERT_NE(nullptr, module.table); Sink original; BlockingLogSink replacement;
	Owner owner(module); ASSERT_NE(nullptr, owner.api); replacement.table = module.table; replacement.api = owner.api;
	owner.api->registerSpi(&original);
	std::thread host([&] { owner.api->registerSpi(&replacement); }); // logs "Listener is replaced"
	bool entered;
	{ std::unique_lock<std::mutex> lock(replacement.mutex); entered = replacement.cv.wait_for(lock, 2s, [&] { return replacement.entered; }); }
	if (entered)
	{
		const auto start = std::chrono::steady_clock::now();
		EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api));
		EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
		EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK, replacement.canWait.load());
		EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, module.table->wait_drained(owner.api, 0));
	}
	{ std::lock_guard<std::mutex> lock(replacement.mutex); replacement.release = true; replacement.cv.notify_all(); }
	host.join(); EXPECT_TRUE(entered); EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, LegacyReleaseThenV2DrainAndDestroy)
{
	Module module; ASSERT_NE(nullptr, module.table);
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	const uint16_t port = freeUdpPort(); ASSERT_NE(0, port);
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config cfg(server.port(), port); ASSERT_TRUE(owner.api->init(cfg.value)); owner.api->subscribe(subscription());
	ASSERT_TRUE(owner.api->connect()); ASSERT_TRUE(pumpUntilQuotes(sink, port, 1));
	owner.api->release(); owner.api->release();
	EXPECT_FALSE(owner.api->connect());
	EXPECT_TRUE(owner.stopAndDestroy()) << "legacy join completed the stop proof";
}

TEST(ParserXeleV2, MultipleInstancesDrainIndependently)
{
	Module module; ASSERT_NE(nullptr, module.table);
	SnapshotServer s1(snapshotFrame("ag2512", kInstrumentNo, 1.0)), s2(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	uint16_t a = freeUdpPort(), b = freeUdpPort(); if (a == b) b = freeUdpPort();
	ASSERT_NE(0, a); ASSERT_NE(0, b); ASSERT_NE(a, b);
	Sink first, second; Owner one(module), two(module); ASSERT_NE(nullptr, one.api); ASSERT_NE(nullptr, two.api);
	one.api->registerSpi(&first); two.api->registerSpi(&second);
	Config c1(s1.port(), a), c2(s2.port(), b);
	ASSERT_TRUE(one.api->init(c1.value)); ASSERT_TRUE(two.api->init(c2.value));
	one.api->subscribe(subscription()); two.api->subscribe(subscription());
	ASSERT_TRUE(one.api->connect()); ASSERT_TRUE(two.api->connect());
	EXPECT_TRUE(pumpUntilQuotes(first, a, 1)); EXPECT_TRUE(pumpUntilQuotes(second, b, 1));
	EXPECT_TRUE(one.stopAndDestroy());
	EXPECT_TRUE(two.api->isConnected());
	EXPECT_TRUE(pumpUntilQuotes(second, b, second.quoteCount() + 1));
	EXPECT_TRUE(two.stopAndDestroy());
}

TEST(ParserXeleV2, InjectedThreadCreationFailureHasNoAsyncWork)
{
	Module module("./fault/parsers/libParserXeleSkt.dylib"); ASSERT_NE(nullptr, module.table);
	auto fail = reinterpret_cast<void (*)()>(dlsym(module.handle, "parserXeleTestFailNextWorkerStart"));
	ASSERT_NE(nullptr, fail);
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config cfg(server.port(), freeUdpPort()); ASSERT_TRUE(owner.api->init(cfg.value)); owner.api->subscribe(subscription());
	fail(); EXPECT_FALSE(owner.api->connect());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, module.table->request_stop(owner.api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, module.table->wait_drained(owner.api, 0));
	EXPECT_TRUE(owner.stopAndDestroy());
}

TEST(ParserXeleV2, InjectedTimerFailureReturnsErrorAndRetainsHandle)
{
	Module module("./fault/parsers/libParserXeleSkt.dylib"); ASSERT_NE(nullptr, module.table);
	auto fail = reinterpret_cast<void (*)()>(dlsym(module.handle, "parserXeleTestFailNextStopTimer"));
	auto legacyShutdown = reinterpret_cast<bool (*)(IParserApi*)>(dlsym(module.handle, "parserXeleTestLegacyShutdown"));
	ASSERT_NE(nullptr, fail); ASSERT_NE(nullptr, legacyShutdown);
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	Sink sink; Owner owner(module); ASSERT_NE(nullptr, owner.api); owner.api->registerSpi(&sink);
	Config cfg(server.port(), freeUdpPort()); ASSERT_TRUE(owner.api->init(cfg.value)); owner.api->subscribe(subscription());
	fail(); EXPECT_FALSE(owner.api->connect());
	// Join the deliberately failed worker before testing the sticky V2 error;
	// this test-only cleanup proof does not generalize to an uncertain provider.
	ASSERT_TRUE(legacyShutdown(owner.api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, module.table->request_stop(owner.api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, module.table->wait_drained(owner.api, 0));
	void* opaque = owner.api;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, module.table->destroy_api(&opaque)); EXPECT_EQ(owner.api, opaque);
	delete owner.api; owner.api = nullptr; --module.live;
}

TEST(ParserXeleV2, RealParserAdapterTwoPhase)
{
	logInit();
	SnapshotServer server(snapshotFrame("ag2512", kInstrumentNo, 1.0));
	auto* mgr = new WTSBaseDataMgr; auto* manager = new ParserAdapterMgr;
	auto adapter = std::make_shared<ParserAdapter>();
	Config cfg(server.port(), freeUdpPort()); cfg.value->append("module", "ParserXeleSkt");
	ASSERT_TRUE(adapter->init("xele", cfg.value, nullptr, mgr));
	ASSERT_TRUE(manager->addAdapter("xele", adapter));
	ASSERT_TRUE(manager->runWithResult());
	EXPECT_EQ(1, server.served());
	ASSERT_TRUE(manager->requestStopLifecycleAll());
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (!manager->finishReleaseLifecycleAll() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
	if (manager->hasQuarantinedAdapters()) { FAIL() << "adapter remains quarantined; owner graph intentionally retained"; }
	delete manager; delete mgr;
}
