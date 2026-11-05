// Legacy IParserApi lifecycle scenarios (T2-XELE-02 / R76-08). Only the
// legacy surface (createParser/deleteParser and IParserApi methods) is used, so
// the same source runs against the saved baseline module (red control) and the
// current module (green). XELE_MODULE_PATH selects the module.
#include <gtest/gtest.h>
#include "xele_fixture.hpp"

using namespace xelefx;

namespace {
struct Module
{
	void* handle = dlopen(XELE_MODULE_PATH, RTLD_NOW | RTLD_LOCAL);
	FuncCreateParser create = handle ? reinterpret_cast<FuncCreateParser>(dlsym(handle, "createParser")) : nullptr;
	FuncDeleteParser remove = handle ? reinterpret_cast<FuncDeleteParser>(dlsym(handle, "deleteParser")) : nullptr;
	// Never dlclose: a failing red scenario may leave provider code running.
};

struct Connected
{
	Module module;
	Sink sink;
	SnapshotServer server{ snapshotFrame("ag2512", kInstrumentNo, 1.0) };
	uint16_t port = freeUdpPort();
	IParserApi* api = nullptr;
	bool start()
	{
		if (!module.create || !module.remove || port == 0 || server.port() == 0) return false;
		api = module.create(); if (!api) return false;
		api->registerSpi(&sink);
		Config cfg(server.port(), port);
		if (!api->init(cfg.value)) return false;
		api->subscribe(subscription());
		return api->connect();
	}
	~Connected() { if (api && module.remove) module.remove(api); }
};
}

TEST(XeleLifecycleScenario, ConnectDisconnectConnectResumesData)
{
	Connected c; ASSERT_TRUE(c.start());
	ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
	EXPECT_TRUE(c.api->disconnect());
	EXPECT_TRUE(c.sink.waitCloses(1));
	EXPECT_FALSE(c.api->isConnected());
	const int before = c.sink.quoteCount();
	ASSERT_TRUE(c.api->connect()) << "second connect after disconnect";
	EXPECT_TRUE(c.api->isConnected());
	ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, before + 1)) << "data callback did not resume";
	EXPECT_EQ(c.sink.quoteThreads.front(), c.sink.quoteThreads.back()) << "worker was replaced";
	EXPECT_TRUE(c.api->disconnect());
	EXPECT_TRUE(c.sink.waitCloses(2));
	EXPECT_EQ(2, c.sink.closeCount());
	EXPECT_EQ(1, c.server.served()) << "snapshot is synchronized once per instance";
	c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
}

TEST(XeleLifecycleScenario, ReleaseThenConnectIsRejected)
{
	Connected c; ASSERT_TRUE(c.start());
	ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
	c.api->release();
	EXPECT_FALSE(c.api->isConnected());
	EXPECT_FALSE(c.api->connect()) << "connect after release must be rejected";
	EXPECT_FALSE(c.api->isConnected());
	const int after = c.sink.quoteCount();
	EXPECT_TRUE(sendUnicast(c.port, marketDataFrame(kInstrumentNo, 100)));
	EXPECT_FALSE(c.sink.waitQuotes(after + 1, 100ms));
	c.api->release();
	c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
}

TEST(XeleLifecycleScenario, ReleaseAndRemoverInsideWorkerCallbackDoNotSelfJoin)
{
	Connected c;
	std::atomic<bool> ran{ false }, retained{ false };
	c.sink.onQuote = [&] {
		if (ran.exchange(true)) return;
		c.api->release();
		IParserApi* copy = c.api;
		c.module.remove(copy);
		retained = copy != nullptr;
	};
	ASSERT_TRUE(c.start());
	ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
	EXPECT_TRUE(ran.load());
	EXPECT_TRUE(retained.load()) << "remover on the worker must keep the owner";
	EXPECT_FALSE(c.api->isConnected());
	EXPECT_FALSE(c.api->connect());
	c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
}

TEST(XeleLifecycleScenario, RepeatedReleaseAndDestructorJoinWorker)
{
	{
		Connected c; ASSERT_TRUE(c.start());
		ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
		c.api->release(); c.api->release(); c.api->release();
		c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
		c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
	}
	{
		Connected c; ASSERT_TRUE(c.start());
		ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
		delete c.api; c.api = nullptr; // destructor from a non-worker thread
	}
}

TEST(XeleLifecycleScenario, DisconnectInsideCallbackIsQueuedAndConnectIsBusy)
{
	Connected c;
	std::atomic<int> disconnected{ -1 }, reconnected{ -1 };
	std::atomic<bool> once{ false };
	c.sink.onQuote = [&] {
		if (once.exchange(true)) return;
		disconnected = c.api->disconnect() ? 1 : 0;
		reconnected = c.api->connect() ? 1 : 0;
	};
	ASSERT_TRUE(c.start());
	ASSERT_TRUE(pumpUntilQuotes(c.sink, c.port, 1));
	EXPECT_EQ(1, disconnected.load());
	EXPECT_EQ(0, reconnected.load()) << "sync re-entry must be busy";
	EXPECT_TRUE(c.sink.waitCloses(1));
	EXPECT_FALSE(c.api->isConnected());
	const int before = c.sink.quoteCount();
	ASSERT_TRUE(c.api->connect());
	EXPECT_TRUE(pumpUntilQuotes(c.sink, c.port, before + 1));
	c.module.remove(c.api); EXPECT_EQ(nullptr, c.api);
}
