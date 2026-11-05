// Parser lifecycle V2 checks for one vendor parser family: the exported table,
// honest drain reporting (DRAINED only without an SDK object; ERROR =
// quarantine once an SDK object existed), wait safety inside callbacks, and the
// real WtCore ParserAdapter / ParserAdapterMgr two-phase stop.
#include "family.hpp"

#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "WtCore/ParserAdapter.h"

using namespace vfx;

namespace
{
	void logInit()
	{
		static bool once = [] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr); return true; }();
		(void)once;
	}

	bool moduleLoaded()
	{
		void* h = dlopen(PARSER_MODULE, RTLD_NOW | RTLD_NOLOAD);
		if (h)
			dlclose(h);
		return h != nullptr;
	}

	class V2 : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ASSERT_TRUE(fake().ok()) << FAKE_LIB << ": " << dlerror();
			fake().reset();
			ASSERT_NE(nullptr, module.handle) << dlerror();
			ASSERT_NE(nullptr, module.table) << "no " << WT_PARSER_LIFECYCLE_V2_GETTER_SYMBOL << " export";
			table = module.table;
			api = module.create();
			ASSERT_NE(nullptr, api);
			api->registerSpi(&sink);
			cfg = makeConfig(tmp.path + "/flow");
		}
		void TearDown() override
		{
			if (cfg)
				cfg->release();
		}
		WtParserLifecycleV2Status waitSettled()
		{
			const auto deadline = std::chrono::steady_clock::now() + 3s;
			WtParserLifecycleV2Status status;
			do
			{
				status = table->wait_drained(api, 0);
				if (status != WT_PARSER_LIFECYCLE_V2_PENDING)
					return status;
				std::this_thread::sleep_for(2ms);
			} while (std::chrono::steady_clock::now() < deadline);
			return status;
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
		Sink& sink = *new Sink;	// kept alive with quarantined parsers
		const WtParserLifecycleApiV2* table = nullptr;
		IParserApi* api = nullptr;
		WTSVariant* cfg = nullptr;
	};
}

// Runs first, before any parser of this module is quarantined.
TEST(V2Adapter, A_FailedInitBeforeSdkDrainsAndReleasesOwner)
{
	logInit();
	ASSERT_TRUE(fake().ok());
	fake().reset();
	fake().set(OPT_CREATE_NULL, 1);
	const bool loadedBefore = moduleLoaded();
	TempDir tmp;
	WTSVariant* cfg = makeConfig(tmp.path + "/flow", true);
	WTSBaseDataMgr* bd = new WTSBaseDataMgr;
	auto adapter = std::make_shared<ParserAdapter>();
	EXPECT_FALSE(adapter->init(kName, cfg, nullptr, bd));
	EXPECT_TRUE(adapter->hasApi()) << "failed candidate is retained until drained";
	EXPECT_TRUE(adapter->requestStopLifecycle());
	EXPECT_TRUE(adapter->finishReleaseLifecycle());
	EXPECT_FALSE(adapter->hasApi()) << "drained owner, handle and module reference are released";
	// macOS dyld never unmaps an image that has thread-local variables (every
	// one of these parsers, baseline included, has them), so dlclose cannot be
	// observed as an unmap here; record the observation instead of asserting it.
	::testing::Test::RecordProperty("module_loaded_before", loadedBefore ? 1 : 0);
	::testing::Test::RecordProperty("module_still_mapped_after_release", moduleLoaded() ? 1 : 0);
	EXPECT_EQ(0, fake().count(C_CREATED));
	cfg->release();
	adapter.reset();
	delete bd;
}

TEST_F(V2, TableShape)
{
	EXPECT_GE(table->struct_size, WT_PARSER_LIFECYCLE_V2_REQUIRED_SIZE);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR, table->abi_version >> 16);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, table->capabilities & WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES);
	EXPECT_EQ(0u, table->reserved[0]);
	EXPECT_EQ(0u, table->reserved[1]);
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->preflight_factory());
	EXPECT_EQ(nullptr, module.get(WT_PARSER_LIFECYCLE_V2_ABI_MAJOR + 1));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT, table->request_stop(nullptr));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT, table->wait_drained(nullptr, 0));
	void* null = nullptr;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT, table->destroy_api(&null));
}

TEST_F(V2, NeverInitializedDrainsAndDestroys)
{
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT, table->wait_drained(api, 0)) << "wait before stop";
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api)) << "idempotent stop";
	EXPECT_FALSE(api->init(cfg)) << "init after stop";
	EXPECT_FALSE(api->connect()) << "connect after stop";
	EXPECT_FALSE(api->isConnected());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->can_wait(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, table->wait_drained(api, 0));
	void* opaque = api;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->destroy_api(&opaque));
	EXPECT_EQ(nullptr, opaque);
	EXPECT_EQ(0, fake().count(C_CREATED));
	api = nullptr;
}

TEST_F(V2, FailedInitBeforeSdkDrainsAndDestroys)
{
	fake().set(OPT_CREATE_NULL, 1);
	EXPECT_FALSE(api->init(cfg));
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, table->wait_drained(api, 0));
	void* opaque = api;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->destroy_api(&opaque));
	EXPECT_EQ(nullptr, opaque);
	api = nullptr;
}

// Honest reporting: provider work drained, SDK-owned threads unprovable.
TEST_F(V2, SdkCreatedIsReportedUncertainAndQuarantined)
{
	ASSERT_TRUE(initOnline());
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(1, sink.quoteCount());
	fake().set(OPT_LATE_CALLBACK, 1);
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_FALSE(api->connect());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, waitSettled()) << "SDK-owned callback threads cannot be proven drained";
	EXPECT_EQ(1, fake().count(C_RELEASE)) << "vendor API released once on the waiting thread";
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
	EXPECT_EQ(0, fake().count(C_RELEASE_DURING_CALLBACK));
	EXPECT_EQ(1, fake().count(C_LATE_DISPATCHED));
	EXPECT_EQ(1, sink.quoteCount()) << "late SDK callback reached the host after stop";
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, table->wait_drained(api, 0));
	EXPECT_EQ(1, fake().count(C_RELEASE)) << "no second vendor release";
	void* opaque = api;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, table->destroy_api(&opaque));
	EXPECT_EQ(api, opaque) << "handle must stay live for quarantine";
}

// Wait-safety inside an SDK callback; stop itself is accepted there.
TEST_F(V2, StopInsideSdkCallbackIsNonBlockingAndWaitIsUnsafe)
{
	ASSERT_TRUE(initOnline());
	std::atomic<int> stop{99}, can{99}, wait{99}, destroy{99};
	sink.onQuote = [&]() {
		stop = table->request_stop(api);
		can = table->can_wait(api);
		wait = table->wait_drained(api, 0);
		void* opaque = api;
		destroy = table->destroy_api(&opaque);
	};
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, stop.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK, can.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK, wait.load());
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, destroy.load());
	sink.onQuote = nullptr;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->can_wait(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, waitSettled());
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
}

TEST_F(V2, PendingWhileCallbackInFlight)
{
	ASSERT_TRUE(initOnline());
	probeSink(sink);
	sink.blockQuotes = true;
	ASSERT_TRUE(fake().post(K_DEPTH));
	ASSERT_TRUE(sink.waitQuoteEntered());
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, table->wait_drained(api, 0));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, table->wait_drained(api, 30));
	EXPECT_EQ(0, fake().count(C_RELEASE));
	sink.openQuotes();
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, table->wait_drained(api, 3000));
	EXPECT_EQ(1, fake().count(C_RELEASE));
	EXPECT_EQ(0, fake().count(C_RELEASE_DURING_CALLBACK));
}

TEST_F(V2, PendingWhileHostApiCallInFlight)
{
	ASSERT_TRUE(initOnline());
	const int subscribed = fake().count(C_SUB_REQ);
	fake().set(OPT_BLOCK_SUBSCRIBE, 1);
	std::thread host([this]() {
		CodeSet more = subscription();
		api->subscribe(more);
	});
	ASSERT_TRUE(fake().subscribeEntered());
	EXPECT_EQ(subscribed + 1, fake().count(C_SUB_REQ));
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_PENDING, table->wait_drained(api, 0));
	EXPECT_EQ(0, fake().count(C_RELEASE)) << "vendor API released under an in-flight host call";
	fake().unblockSubscribe();
	host.join();
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, waitSettled());
	EXPECT_EQ(1, fake().count(C_RELEASE));
}

// Legacy release followed by the V2 path still reports uncertainty.
TEST_F(V2, LegacyReleaseThenV2StaysQuarantined)
{
	ASSERT_TRUE(initOnline());
	api->release();
	EXPECT_EQ(1, fake().count(C_RELEASE));
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, table->wait_drained(api, 0));
	EXPECT_EQ(1, fake().count(C_RELEASE));
	void* opaque = api;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, table->destroy_api(&opaque));
	EXPECT_EQ(api, opaque);
}

// Independent instances: stopping one does not stop the other's admission.
TEST_F(V2, IndependentInstances)
{
	IParserApi* other = module.create();
	ASSERT_NE(nullptr, other);
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(other));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_DRAINED, table->wait_drained(other, 0));
	void* opaque = other;
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->destroy_api(&opaque));
	ASSERT_TRUE(initOnline()) << "the first instance is unaffected";
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_EQ(1, sink.quoteCount());
	ASSERT_EQ(WT_PARSER_LIFECYCLE_V2_OK, table->request_stop(api));
	EXPECT_EQ(WT_PARSER_LIFECYCLE_V2_ERROR, waitSettled());
}

// Real WtCore ParserAdapter + ParserAdapterMgr: the SDK-backed parser is
// accepted (V2 table valid), runs, and on two-phase stop remains quarantined
// with its module loaded; repeated release does not release the SDK again.
TEST(V2Adapter, B_RealParserAdapterQuarantinesSdkParser)
{
	logInit();
	ASSERT_TRUE(fake().ok());
	fake().reset();
	TempDir tmp;
	WTSVariant* cfg = makeConfig(tmp.path + "/flow", true);
	WTSBaseDataMgr* bd = new WTSBaseDataMgr;
	ParserAdapterMgr* mgr = new ParserAdapterMgr;
	auto adapter = std::make_shared<ParserAdapter>();
	ASSERT_TRUE(adapter->init(kName, cfg, nullptr, bd)) << "V2 table must be accepted by the real adapter";
	ASSERT_TRUE(mgr->addAdapter(kName, adapter));
	ASSERT_TRUE(mgr->runWithResult());
	EXPECT_EQ(1, kXtp ? fake().count(C_LOGIN_REQ) : fake().count(C_INIT));
	if (!kXtp)
	{
		ASSERT_TRUE(postAndWait(K_FRONT_CONNECTED));
		ASSERT_TRUE(postAndWait(K_LOGIN));
	}
	ASSERT_TRUE(postAndWait(K_DEPTH));
	EXPECT_LE(1, fake().count(C_DISPATCHED));
	EXPECT_TRUE(mgr->requestStopLifecycleAll());
	// PENDING (a provider task still in flight) is retried, as a runner would.
	bool finished = false;
	const auto deadline = std::chrono::steady_clock::now() + 3s;
	do
	{
		finished = mgr->finishReleaseLifecycleAll();
		if (finished || fake().count(C_RELEASE) != 0)
			break;
		std::this_thread::sleep_for(2ms);
	} while (std::chrono::steady_clock::now() < deadline);
	EXPECT_FALSE(finished) << "SDK-backed parser must stay quarantined";
	EXPECT_FALSE(mgr->finishReleaseLifecycleAll());
	EXPECT_TRUE(mgr->hasQuarantinedAdapters());
	EXPECT_TRUE(adapter->hasApi());
	EXPECT_TRUE(moduleLoaded());
	EXPECT_EQ(1, fake().count(C_RELEASE));
	EXPECT_EQ(0, fake().count(C_RELEASE_ON_SDK_THREAD));
	adapter->release();
	EXPECT_FALSE(mgr->finishReleaseLifecycleAll());
	EXPECT_EQ(1, fake().count(C_RELEASE)) << "repeated release must not release the SDK again";
	// Owner graph (adapter, manager, base data, cfg) intentionally retained.
}
