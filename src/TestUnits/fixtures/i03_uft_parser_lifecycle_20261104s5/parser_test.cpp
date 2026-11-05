/*
 * BUG-R78-001 (iteration 03, card 9): WtUftCore ParserAdapter lifecycle fixture.
 *
 * Built twice by CMakeLists.txt: against the current src/WtUftCore/ParserAdapter
 * (WT_HOST_NEW) and against the pre-change copy (red control). Providers are the
 * local fake modules from provider.cpp; nothing connects to a network or vendor.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/WtHelper.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"

using namespace wtp;

namespace {

std::string fixtureRoot()
{
	const char* dir = getenv("I03_UFT_PARSER_ROOT");
	return dir == nullptr ? std::string("./") : std::string(dir) + "/";
}

struct Probe
{
	void* module = nullptr;
	void (*reset)() = nullptr;
	int (*count)(int) = nullptr;
	void (*set)(int, int) = nullptr;
	explicit Probe(int mode)
	{
		const std::string path = fixtureRoot() + "parsers/libPvt2" + std::to_string(mode) + ".dylib";
		module = dlopen(path.c_str(), RTLD_NOW);
		if (module == nullptr)
			return;
		reset = reinterpret_cast<void (*)()>(dlsym(module, "probe_reset"));
		count = reinterpret_cast<int (*)(int)>(dlsym(module, "probe_count"));
		set = reinterpret_cast<void (*)(int, int)>(dlsym(module, "probe_set"));
	}
	~Probe() { if (module != nullptr) dlclose(module); }
};

struct Cfg
{
	WTSVariant* p;
	explicit Cfg(int mode) : p(WTSVariant::createObject())
	{
		const std::string name = "Pvt2" + std::to_string(mode);
		p->append("module", name.c_str());
	}
	~Cfg() { p->release(); }
};

class FakeBd : public IBaseDataMgr
{
public:
	FakeBd()
	{
		_comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "NONE", "NONE");
		_ag = WTSContractInfo::create("ag2406", "ag2406", "SHFE", "ag");
		_ag->setAltCode("ag2406");
		_ag->setCommInfo(_comm);
	}
	virtual ~FakeBd() { _ag->release(); _comm->release(); }
	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		return (strcmp(code, "ag2406") == 0 && strcmp(exchg, "SHFE") == 0) ? _ag : nullptr;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override
	{
		WTSArray* ret = WTSArray::create();
		ret->append(_ag, true);
		return ret;
	}
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

private:
	WTSCommodityInfo* _comm;
	WTSContractInfo* _ag;
};

class CountingStub : public IParserStub
{
public:
	std::atomic<uint32_t> quotes{ 0 };
	void handle_push_quote(WTSTickData*) override { quotes.fetch_add(1); }
};

class ExternalParser : public IParserApi
{
public:
	static std::atomic<int> destroyed;
	~ExternalParser() override { destroyed.fetch_add(1); }
	bool init(WTSVariant*) override { return true; }
	bool connect() override { return true; }
	void release() override {}
};
std::atomic<int> ExternalParser::destroyed{ 0 };

std::atomic<int> g_removed{ 0 };
void confirmingRemover(IParserApi*& api)
{
	g_removed.fetch_add(1);
	delete api;
	api = nullptr;
}

WTSTickData* makeTick()
{
	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	strcpy(st.exchg, "SHFE");
	strcpy(st.code, "ag2406");
	st.action_date = 20240315;
	st.action_time = 93000000;
	st.trading_date = 20240315;
	st.price = 5000;
	return WTSTickData::create(st);
}

class UftParserLifecycle : public ::testing::Test
{
protected:
	FakeBd bd;
	void SetUp() override
	{
		static bool once = []() {
			WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr);
			return true;
		}();
		(void)once;
		WtHelper::setInstDir(fixtureRoot().c_str());
	}
};

}	// namespace

// Card 9a=A: a module without a valid V2 table (legacy module, malformed table
// or rejected preflight) is rejected before createParser and leaves no API.
TEST_F(UftParserLifecycle, ModulesWithoutValidLifecycleV2AreRejectedBeforeCreate)
{
	for (int mode : { 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12 })
	{
		Probe probe(mode);
		ASSERT_NE(nullptr, probe.module) << mode;
		probe.reset();
		Cfg cfg(mode);
		ParserAdapter adapter;
		EXPECT_FALSE(adapter.init("invalid", cfg.p, nullptr, &bd)) << mode;
		EXPECT_EQ(0, probe.count(0)) << "createParser called for mode " << mode;
		EXPECT_FALSE(adapter.hasApi()) << mode;
		adapter.release();
		EXPECT_EQ(1u, cfg.p->retainCount()) << mode;
	}
}

// Card 9b=A: a PENDING drain keeps the API, configuration and module; a later
// release() retries and destroys exactly once through destroy_api.
TEST_F(UftParserLifecycle, PendingDrainIsRetainedAndRetried)
{
	Probe probe(0);
	ASSERT_NE(nullptr, probe.module);
	probe.reset();
	probe.set(1, 1);	// one PENDING poll
	Cfg cfg(0);
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.init("pending", cfg.p, nullptr, &bd));
	adapter.release();
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_TRUE(adapter.isTeardownPending());
	EXPECT_EQ(0, probe.count(7));
	EXPECT_EQ(0, probe.count(5)) << "legacy release/deleteParser used without drain proof";
	EXPECT_EQ(2u, cfg.p->retainCount());

	adapter.release();
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_EQ(1, probe.count(7));
	EXPECT_EQ(1, probe.count(2)) << "request_stop is sent once";
	EXPECT_EQ(1u, cfg.p->retainCount());
	adapter.release();
	EXPECT_EQ(1, probe.count(7));
}

// can_wait WOULD_DEADLOCK on the releasing thread is retryable, not a drain.
TEST_F(UftParserLifecycle, UnsafeWaitingThreadKeepsTeardownPending)
{
	Probe probe(0);
	ASSERT_NE(nullptr, probe.module);
	probe.reset();
	probe.set(2, 0);
	Cfg cfg(0);
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.init("unsafe", cfg.p, nullptr, &bd));
	adapter.release();
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_EQ(0, probe.count(4));
	EXPECT_EQ(0, probe.count(7));
	probe.set(2, 1);
	adapter.release();
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_EQ(1, probe.count(7));
}

// A failed destroy_api is uncertain: the handle is kept and never destroyed again.
TEST_F(UftParserLifecycle, DestroyErrorIsStickyQuarantine)
{
	Probe probe(0);
	ASSERT_NE(nullptr, probe.module);
	probe.reset();
	probe.set(4, 2);
	Cfg cfg(0);
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.init("destroy-error", cfg.p, nullptr, &bd));
	adapter.release();
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_TRUE(adapter.isTeardownUncertain());
	adapter.release();
	EXPECT_EQ(1, probe.count(7));
	EXPECT_EQ(0, probe.count(5));
}

// An external API without a remover has no drain proof: release() closes
// admission but never deletes it.
TEST_F(UftParserLifecycle, ExternalApiWithoutRemoverIsQuarantined)
{
	ExternalParser::destroyed = 0;
	CountingStub stub;
	ExternalParser* api = new ExternalParser();
	{
		ParserAdapter adapter;
		ASSERT_TRUE(adapter.initExt("legacy-ext", api, &stub, &bd));
		adapter.release();
		adapter.release();
		EXPECT_TRUE(adapter.hasApi());
		EXPECT_EQ(0, ExternalParser::destroyed.load());
		WTSTickData* tick = makeTick();
		adapter.handleQuote(tick, 0);
		tick->release();
		EXPECT_EQ(0u, stub.quotes.load());
	}
	delete api;	// quiescent test fake, reclaimed after its owner is gone
}

// Card 9: release() from inside this adapter's callback only requests stop.
// Decision K18=否 (2026-11-04, card 9a): a remover that nulls the external API is not
// accepted as a callback-drain proof either, so the later release() at a safe point
// quarantines the API until process exit (admission stays closed) and never calls the
// remover. (The 2026-11-04 s5 oracle expected the remover to run once and hasApi()=false.)
TEST_F(UftParserLifecycle, ReleaseInsideCallbackIsDeferred)
{
	g_removed = 0;
	ExternalParser::destroyed = 0;
	class ReentrantStub : public IParserStub
	{
	public:
		ParserAdapter* adapter = nullptr;
		int removedInside = -1;
		bool pendingInside = false;
		std::atomic<uint32_t> quotes{ 0 };
		void handle_push_quote(WTSTickData*) override
		{
			quotes.fetch_add(1);
			adapter->release();
			removedInside = g_removed.load();
			pendingInside = adapter->isTeardownPending();
		}
	} stub;
	ExternalParser* api = new ExternalParser();
	{
		ParserAdapter adapter;
		stub.adapter = &adapter;
		ASSERT_TRUE(adapter.initExt("reentrant", api, &stub, &bd, confirmingRemover));
		WTSTickData* tick = makeTick();
		adapter.handleQuote(tick, 0);
		EXPECT_EQ(1u, stub.quotes.load());
		EXPECT_EQ(0, stub.removedInside) << "no teardown inside the callback";
		EXPECT_TRUE(stub.pendingInside) << "a release from the callback only requests stop";
		EXPECT_TRUE(adapter.hasApi());
		adapter.release();	// safe point
		EXPECT_EQ(0, g_removed.load()) << "K18: the remover is never accepted as a drain proof";
		EXPECT_TRUE(adapter.hasApi());
		EXPECT_TRUE(adapter.isResidentQuarantined());
		EXPECT_EQ(0, ExternalParser::destroyed.load());
		adapter.handleQuote(tick, 0);
		EXPECT_EQ(1u, stub.quotes.load()) << "callback admission stays closed";
		tick->release();
	}
	EXPECT_EQ(0, g_removed.load());
	delete api;	// quiescent test fake, reclaimed after its owner is gone
}

// Provider threads keep delivering callbacks while the owner releases the
// adapter. Teardown must never overlap an admitted callback; under
// -fsanitize=thread any overlap is reported. K18=否: the end state is the
// resident quarantine (API kept, remover never called) with admission closed.
TEST_F(UftParserLifecycle, ConcurrentCallbacksNeverOverlapTeardown)
{
	for (int round = 0; round < 20; ++round)
	{
		g_removed = 0;
		CountingStub stub;
		ExternalParser* api = new ExternalParser();
		{
			ParserAdapter adapter;
			ASSERT_TRUE(adapter.initExt("concurrent", api, &stub, &bd, confirmingRemover));
			std::atomic<bool> stop{ false };
			std::vector<std::thread> providers;
			for (int t = 0; t < 3; ++t)
			{
				providers.emplace_back([&]() {
					while (!stop.load())
					{
						WTSTickData* tick = makeTick();
						adapter.handleQuote(tick, 0);
						tick->release();
						adapter.handleParserLog(LL_DEBUG, "concurrent provider log");
					}
				});
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			// release() closes admission at once; while callbacks are still running it only
			// leaves the teardown pending and a later call completes it at a safe point.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!adapter.isResidentQuarantined() && std::chrono::steady_clock::now() < deadline)
				adapter.release();
			EXPECT_TRUE(adapter.isResidentQuarantined()) << round;
			EXPECT_TRUE(adapter.hasApi()) << round;
			const uint32_t afterTeardown = stub.quotes.load();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			EXPECT_EQ(afterTeardown, stub.quotes.load()) << "callback admitted after teardown, round " << round;
			stop = true;
			for (auto& th : providers)
				th.join();
			EXPECT_EQ(0, g_removed.load()) << "K18: the remover is never called, round " << round;
		}
		delete api;	// quiescent test fake, reclaimed after its owner and providers are gone
	}
}
