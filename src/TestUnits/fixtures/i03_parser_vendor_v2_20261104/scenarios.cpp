// Legacy-API lifecycle scenarios (createParser/deleteParser + IParserApi only)
// for one vendor parser family, run against the current module (green) and the
// saved pre-change module (red). The fake SDK delivers callbacks on its own
// thread; see fake_sdk.hpp. Card 9: 9a quarantine, 9b remover keeps an owner
// without SDK thread-exit proof, 9c repeated init rejected, 9d busy/deferred
// re-entry.
#include "family.hpp"

using namespace vfx;

namespace
{
	class Scenario : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ASSERT_TRUE(fake().ok()) << FAKE_LIB << ": " << dlerror();
			fake().reset();
			ASSERT_NE(nullptr, module.handle) << dlerror();
			ASSERT_NE(nullptr, module.create);
			ASSERT_NE(nullptr, module.remove);
			api = module.create();
			ASSERT_NE(nullptr, api);
			api->registerSpi(&sink);
			cfg = makeConfig(tmp.path + "/flow");
		}
		void TearDown() override
		{
			// Parsers that created an SDK object are intentionally leaked
			// (quarantine); the sink stays alive with them.
			if (cfg)
				cfg->release();
		}
		bool initOnline()
		{
			if (!api->init(cfg))
				return false;
			api->subscribe(subscription());
			return bringOnline(api);
		}

		TempDir tmp;
		Module module;
		Sink& sink = *new Sink;	// leaked with possibly quarantined parsers
		IParserApi* api = nullptr;
		WTSVariant* cfg = nullptr;
	};
}

// 9d: release() inside an SDK callback must not run the vendor Release() on
// the SDK thread (a real SDK joins its own thread there). The release is
// deferred, later callbacks are dropped, and a safe-thread release completes it.
TEST_F(Scenario, ReleaseInsideSdkCallbackIsDeferred)
{
	ASSERT_TRUE(initOnline());
	sink.onQuote = [this]() { api->release(); };
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(1, sink.quoteCount());
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD)) << "vendor Release ran on the SDK callback thread";
	sink.onQuote = nullptr;
	if (fake().post(K_DEPTH))
		ASSERT_TRUE(fake().idle());
	EXPECT_EQ(1, sink.quoteCount()) << "callback after release reached the host";
	api->release();
	EXPECT_EQ(1, fake().count(C_RELEASE));
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
}

// 9d: disconnect() inside an SDK callback reports busy (false) and defers.
TEST_F(Scenario, DisconnectInsideSdkCallbackIsBusy)
{
	ASSERT_TRUE(initOnline());
	std::atomic<int> result{-1};
	sink.onQuote = [this, &result]() { result = api->disconnect() ? 1 : 0; };
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(0, result.load()) << "disconnect inside callback must not report completion";
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
	sink.onQuote = nullptr;
	EXPECT_TRUE(api->disconnect());
	EXPECT_EQ(1, fake().count(C_RELEASE));
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
}

// A release on a host thread waits for an admitted SDK callback to leave
// before the vendor Release() is called.
TEST_F(Scenario, ReleaseWaitsForInFlightCallback)
{
	ASSERT_TRUE(initOnline());
	probeSink(sink);
	sink.blockQuotes = true;
	ASSERT_TRUE(fake().post(K_DEPTH));
	ASSERT_TRUE(sink.waitQuoteEntered());
	std::atomic<bool> returned{false};
	std::thread releaser([this, &returned]() { api->release(); returned = true; });
	std::this_thread::sleep_for(150ms);
	EXPECT_FALSE(returned.load()) << "release returned while a callback was in flight";
	sink.openQuotes();
	releaser.join();
	EXPECT_TRUE(returned.load());
	EXPECT_EQ(1, fake().count(C_RELEASE));
	EXPECT_EQ(0, fake().count(C_RELEASE_DURING_CALLBACK)) << "vendor Release overlapped the host callback";
}

// A late callback the SDK still delivers through its captured SPI pointer
// after release must not reach the host.
TEST_F(Scenario, LateSdkCallbackAfterReleaseIsDropped)
{
	ASSERT_TRUE(initOnline());
	ASSERT_TRUE(postAndWait(K_DEPTH));
	const int before = sink.quoteCount();
	fake().set(OPT_LATE_CALLBACK, 1);
	api->release();
	EXPECT_EQ(1, fake().count(C_LATE_DISPATCHED));
	EXPECT_EQ(before, sink.quoteCount()) << "late SDK callback reached the host after release";
}

// 9c: a second init is rejected and the original SDK object is kept.
TEST_F(Scenario, RepeatedInitIsRejectedAndKeepsOriginal)
{
	ASSERT_TRUE(api->init(cfg));
	EXPECT_FALSE(api->init(cfg));
	EXPECT_EQ(1, fake().count(C_CREATED));
	api->subscribe(subscription());
	ASSERT_TRUE(bringOnline(api));
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(1, sink.quoteCount());
	api->release();
	EXPECT_EQ(1, fake().count(C_RELEASE));
}

// 9d: connect() re-entered from a parser callback is busy and does not start
// the vendor session again.
TEST_F(Scenario, ConnectInsideCallbackIsBusy)
{
	ASSERT_TRUE(api->init(cfg));
	api->subscribe(subscription());
	std::atomic<int> result{-1};
	sink.onEvent = [this, &result](WTSParserEvent e) {
		if (e == WPE_Connect && result.load() < 0)
			result = api->connect() ? 1 : 0;
	};
	ASSERT_TRUE(bringOnline(api));
	ASSERT_TRUE(sink.waitEvents(WPE_Connect, 1));
	ASSERT_TRUE(fake().idle());
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (result.load() < 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(2ms);
	EXPECT_EQ(0, result.load()) << "connect inside a callback must report busy";
	EXPECT_EQ(1, kXtp ? fake().count(C_LOGIN_REQ) : fake().count(C_INIT)) << "vendor session restarted from a callback";
	// release first: it drains admitted callbacks (and joins the XTP worker)
	// before the hook is cleared on this thread.
	api->release();
	sink.onEvent = nullptr;
}

// Repeated release is idempotent; 9b: the remover keeps a parser whose SDK
// object existed (no SDK thread-exit confirmation) instead of deleting it.
TEST_F(Scenario, RepeatedReleaseAndRemoverKeepsSdkOwner)
{
	ASSERT_TRUE(initOnline());
	api->release();
	api->release();
	EXPECT_EQ(1, fake().count(C_RELEASE));
	IParserApi* handle = api;
	module.remove(handle);
	EXPECT_EQ(api, handle) << "remover deleted a parser without SDK thread-exit proof";
	if (handle != nullptr)
		EXPECT_FALSE(api->connect()) << "released parser must not reconnect";
}

// A parser that never created an SDK object is deleted by the remover.
TEST_F(Scenario, RemoverDeletesParserWithoutSdkObject)
{
	IParserApi* handle = api;
	module.remove(handle);
	EXPECT_EQ(nullptr, handle);
	EXPECT_EQ(0, fake().count(C_CREATED));
	api = nullptr;
}

// Direct delete from a host thread releases the vendor API off the SDK thread
// before the parser memory goes away.
TEST_F(Scenario, DirectDeleteReleasesSdkFirst)
{
	ASSERT_TRUE(initOnline());
	ASSERT_TRUE(postAndWait(K_DEPTH));
	delete api;
	api = nullptr;
	EXPECT_EQ(1, fake().count(C_RELEASE)) << "destructor left the SDK holding a pointer to freed parser memory";
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
	EXPECT_LE(1, fake().count(C_SPI_NULL));
}
