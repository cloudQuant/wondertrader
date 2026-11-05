// WtDtServo ParserAdapter / ParserAdapterMgr / WtDtRunner lifecycle matrix
// under card 9 (BUG-R8-006, BUG-I02-NEW-009). Built against the current
// sources (WT_HOST_NEW) and the pre-change copies (red control).
#include <gtest/gtest.h>

#include "fake_probe.hpp"

#include "WtDtServo/ParserAdapter.h"
#include "WtDtServo/WtDtRunner.h"
#include "WtDtServo/WtHelper.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace dtfake;

namespace
{
std::string fixture_dir()
{
	const char* dir = std::getenv("I03_DT_HOST_PARSER_DIR");
	return dir == nullptr ? std::string() : std::string(dir);
}
std::string v2_path() { return fixture_dir() + "/libI03DtFakeV2.dylib"; }
std::string legacy_path() { return fixture_dir() + "/libI03DtFakeLegacy.dylib"; }
std::string module_dir() { return fixture_dir() + "/../"; }

WTSVariant* make_cfg(const std::string& module)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", module.c_str());
	return cfg;
}

bool wait_until(const std::function<bool()>& pred, int ms = 2000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!pred())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
}

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
}

class ServoHost : public ::testing::Test
{
protected:
	static void SetUpTestSuite() { WtHelper::set_module_dir(module_dir().c_str()); }

	void SetUp() override
	{
		ASSERT_FALSE(fixture_dir().empty()) << "I03_DT_HOST_PARSER_DIR is required";
		ASSERT_TRUE(_v2.open(v2_path())) << v2_path();
		ASSERT_TRUE(_legacy.open(legacy_path())) << legacy_path();
		_v2.reset();
		_legacy.reset();
		_dir = fs::current_path() / ("servo_" + std::to_string(::getpid()) + "_" +
			::testing::UnitTest::GetInstance()->current_test_info()->name());
		fs::create_directories(_dir);
		write_file(_dir / "sessions.json",
			R"JSON({"SD0930":{"name":"day","offset":0,"auction":{"from":929,"to":930},
				"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}})JSON");
		write_file(_dir / "commodities.json",
			R"JSON({"CFFEX":{"IC":{"covermode":0,"pricemode":0,"category":1,"precision":1,
				"pricetick":0.2,"volscale":200,"name":"IC","exchg":"CFFEX",
				"session":"SD0930","holiday":"CHINA"}}})JSON");
		write_file(_dir / "contracts.json",
			R"JSON({"CFFEX":{"IC2204":{"name":"IC2204","code":"IC2204","exchg":"CFFEX",
				"product":"IC","maxlimitqty":20,"maxmarketqty":10}}})JSON");
	}

	std::string runner_cfg(const std::string& parserModule)
	{
		const std::string d = _dir.string();
		std::string s = std::string("{\"disable_dump\": true, \"basefiles\": {")
			+ "\"session\": \"" + d + "/sessions.json\","
			+ "\"commodity\": \"" + d + "/commodities.json\","
			+ "\"contract\": \"" + d + "/contracts.json\"}"
			+ ",\"parsers\": [{\"id\":\"servo_p1\",\"active\":true,\"module\":\"" + parserModule + "\"}]}";
		const fs::path p = _dir / "servo.json";
		write_file(p, s);
		return p.string();
	}

	WTSBaseDataMgr _bd;
	Probe _v2;
	Probe _legacy;
	fs::path _dir;
};

TEST_F(ServoHost, LegacyModuleWithoutV2IsRejectedBeforeCreateParser)
{
	WTSVariant* cfg = make_cfg("I03DtFakeLegacy");
	ParserAdapter pa(&_bd, nullptr);
	EXPECT_FALSE(pa.init("legacy", cfg));
	EXPECT_EQ(_legacy[C_CREATE], 0);
	EXPECT_EQ(_legacy[C_INIT], 0);
	EXPECT_EQ(cfg->retainCount(), 1u);
	EXPECT_FALSE(pa.run());
	pa.release();
	cfg->release();
}

TEST_F(ServoHost, FailedProviderInitRollsBackThroughV2AndReturnsConfig)
{
	_v2.set(K_INIT_OK, 0);
	WTSVariant* cfg = make_cfg("I03DtFakeV2");
	ParserAdapter pa(&_bd, nullptr);
	EXPECT_FALSE(pa.init("fail", cfg));
	EXPECT_EQ(_v2[C_CREATE], 1);
	EXPECT_EQ(_v2[C_SUBSCRIBE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(cfg->retainCount(), 1u);
	EXPECT_FALSE(pa.run());
	EXPECT_EQ(_v2[C_CONNECT], 0);
	pa.release();
	EXPECT_EQ(_v2[C_DESTROY], 1);
	cfg->release();
}

TEST_F(ServoHost, SuccessfulInitReleaseDrainsBeforeDestroy)
{
	WTSVariant* cfg = make_cfg("I03DtFakeV2");
	{
		ParserAdapter pa(&_bd, nullptr);
		ASSERT_TRUE(pa.init("ok", cfg));
		EXPECT_TRUE(pa.run());
		EXPECT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 3; }));
		pa.release();
		EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
		EXPECT_EQ(_v2[C_CALLBACKS_AFTER_RELEASE], 0);
		EXPECT_EQ(_v2[C_DESTROY], 1);
		EXPECT_EQ(_v2[C_LIVE], 0);
		EXPECT_EQ(cfg->retainCount(), 1u);
		pa.release();
		EXPECT_EQ(_v2[C_DESTROY], 1);
	}
	cfg->release();
}

TEST_F(ServoHost, ReleaseFromProviderThreadDefersToSafePoint)
{
	WTSVariant* cfg = make_cfg("I03DtFakeV2");
	ParserAdapter* pa = new ParserAdapter(&_bd, nullptr);
	ASSERT_TRUE(pa->init("reentry", cfg));
	std::atomic<int> hookRan{0};
	struct Ctx { ParserAdapter* pa; std::atomic<int>* ran; } ctx{pa, &hookRan};
	_v2.set_hook([](void* p) {
		Ctx* c = static_cast<Ctx*>(p);
		c->pa->release();
		c->ran->store(1);
	}, &ctx);
	ASSERT_TRUE(wait_until([&]() { return hookRan.load() == 1; }));
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 0);
	pa->release();
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(cfg->retainCount(), 1u);
	delete pa;
	cfg->release();
}

TEST_F(ServoHost, RunnerDestructionStopsAndDrainsParsersFirst)
{
	WtDtRunner* runner = new WtDtRunner();
	runner->initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str());
	ASSERT_TRUE(runner->isInitialized());
	// initialize() starts the registered parsers itself.
	EXPECT_EQ(_v2[C_CONNECT], 1);
	EXPECT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 3; }));
	delete runner;
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	const int callbacks = _v2[C_CALLBACKS];
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_EQ(_v2[C_CALLBACKS], callbacks);		// no provider thread left
}

TEST_F(ServoHost, RunnerFailedParserIsNotStartedAndIsRolledBack)
{
	_v2.set(K_INIT_OK, 0);
	WtDtRunner* runner = new WtDtRunner();
	runner->initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str());
	EXPECT_FALSE(runner->isInitialized());
	EXPECT_EQ(runner->activeParserCount(), 0u);
	runner->start();
	EXPECT_EQ(_v2[C_CONNECT], 0);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	delete runner;
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
}

// I03-NEW-044 (card 9c): initialize() already starts the parsers; a second
// start() is rejected and must not send another connect to the same instance.
TEST_F(ServoHost, RepeatedStartDoesNotReconnect)
{
	WtDtRunner* runner = new WtDtRunner();
	runner->initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str());
	ASSERT_TRUE(runner->isInitialized());
	EXPECT_EQ(_v2[C_CONNECT], 1);
	runner->start();
	runner->start();
	EXPECT_EQ(_v2[C_CONNECT], 1);
	EXPECT_TRUE(runner->parserCommandsSubmitted());
	delete runner;
	EXPECT_EQ(_v2[C_LIVE], 0);
}

// BUG-R123-003: a provider callback that is already running inside the runner
// (blocked in the tick callback; afterwards it would continue into
// WtDataManager::update_bars) when the runner is destroyed. Destruction must
// not complete, and no runner member (data manager, reader, base data) may be
// destroyed, until that callback has left the runner.
namespace
{
std::atomic<long> g_seq{0};
std::atomic<long> g_cb_entered_seq{0};
std::atomic<long> g_cb_exited_seq{0};
std::atomic<bool> g_cb_release{false};
std::atomic<int> g_cb_calls{0};
void blocking_tick_cb(const char*, WTSTickStruct*)
{
	if (g_cb_calls.fetch_add(1) != 0)
		return;
	g_cb_entered_seq = ++g_seq;
	while (!g_cb_release.load())
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	g_cb_exited_seq = ++g_seq;
}
}

TEST_F(ServoHost, RunnerDestructionWaitsForInFlightCallbackBeforeMembers)
{
	g_seq = 0; g_cb_entered_seq = 0; g_cb_exited_seq = 0; g_cb_release = false; g_cb_calls = 0;
	_v2.set(K_QUOTE_MODE, 1);
	_v2.set(K_INTERVAL_US, 1000);
	WtDtRunner* runner = new WtDtRunner();
	runner->initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str(), blocking_tick_cb, nullptr);
	ASSERT_TRUE(runner->isInitialized());
	runner->sub_tick("CFFEX.IC.2204", true);			// external callback
	runner->sub_tick("CFFEX.IC.2204", false, true);	// inner: continues into the data manager
	ASSERT_TRUE(wait_until([&]() { return g_cb_entered_seq.load() != 0; }, 5000));

	std::thread releaser([]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		g_cb_release = true;
	});
	delete runner;
	const long deletedSeq = ++g_seq;
	releaser.join();

	EXPECT_NE(0, g_cb_exited_seq.load());
	EXPECT_LT(g_cb_exited_seq.load(), deletedSeq);	// the callback left before teardown finished
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
}

#ifdef WT_HOST_NEW
TEST_F(ServoHost, RunnerShutdownReportsAndRetainsUnprovableOwner)
{
	_v2.set(K_WAIT_MODE, 2);
	WtDtRunner* runner = new WtDtRunner();
	runner->initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str());
	ASSERT_TRUE(runner->isInitialized());
	EXPECT_FALSE(runner->shutdown());
	EXPECT_TRUE(runner->hasRetainedParserAdapters());
	EXPECT_EQ(_v2[C_DESTROY], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_LIVE], 1);
	EXPECT_FALSE(runner->shutdown());		// uncertain owner is not retried
	EXPECT_EQ(_v2[C_WAIT], 1);
	// The runner and its quarantined owner graph are retained to process exit.
}

TEST_F(ServoHost, RepeatedShutdownAfterCompleteTeardownIsNoop)
{
	WtDtRunner runner;
	runner.initialize(runner_cfg("I03DtFakeV2").c_str(), true, module_dir().c_str(),
		(_dir / "no_such_logcfg.yaml").string().c_str());
	ASSERT_TRUE(runner.isInitialized());
	EXPECT_TRUE(runner.shutdown());
	EXPECT_TRUE(runner.shutdown());
	EXPECT_FALSE(runner.hasRetainedParserAdapters());
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
}
#endif
}
