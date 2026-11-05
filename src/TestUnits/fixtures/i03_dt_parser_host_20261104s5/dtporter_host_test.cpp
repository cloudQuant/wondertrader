// WtDtPorter WtDtRunner teardown (BUG-R5-007 order, card 9). Built against the
// current sources (WT_HOST_NEW) and the pre-change WtDtPorter runner plus the
// pre-change WtDtCore adapter (red control). Local fake only.
#include <gtest/gtest.h>

#include "fake_probe.hpp"

#include "WtDtPorter/WtDtRunner.h"
#include "WtDtCore/WtHelper.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <dlfcn.h>

namespace fs = std::filesystem;
using namespace dtfake;

namespace
{
std::string fixture_dir()
{
	const char* dir = std::getenv("I03_DT_HOST_PARSER_DIR");
	return dir == nullptr ? std::string() : std::string(dir);
}
std::string module_dir() { return fixture_dir() + "/../"; }

bool wait_until(const std::function<bool()>& pred, int ms = 3000)
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

class DtPorterHost : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_FALSE(fixture_dir().empty()) << "I03_DT_HOST_PARSER_DIR is required";
		ASSERT_TRUE(_v2.open(fixture_dir() + "/libI03DtFakeV2.dylib"));
		_v2.reset();
		_dir = fs::current_path() / ("dtporter_" + std::to_string(::getpid()) + "_" +
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
		write_file(_dir / "holidays.json", R"JSON({"CHINA": []})JSON");
		write_file(_dir / "statemonitor.json",
			R"JSON({"SD0930": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	}

	std::string runner_cfg(const std::string& writer = "{}")
	{
		const std::string d = _dir.string();
		const std::string s = std::string("{\"basefiles\": {")
			+ "\"session\": \"" + d + "/sessions.json\","
			+ "\"commodity\": \"" + d + "/commodities.json\","
			+ "\"contract\": \"" + d + "/contracts.json\","
			+ "\"holiday\": \"" + d + "/holidays.json\"},"
			+ "\"allday\": false, \"statemonitor\": \"" + d + "/statemonitor.json\","
			+ "\"writer\": " + writer + ","
			+ "\"parsers\": [{\"id\":\"porter_p1\",\"active\":true,\"module\":\"I03DtFakeV2\"}]}";
		const fs::path p = _dir / "dtcfg.json";
		write_file(p, s);
		return p.string();
	}

	void start(WtDtRunner& runner, const std::string& writer = "{}")
	{
		runner.initialize(runner_cfg(writer).c_str(), (_dir / "no_such_logcfg.yaml").string().c_str(),
			module_dir().c_str(), true, true);
		runner.start(true, false);		// async: parsers connect, the state monitor runs
	}

	Probe _v2;
	fs::path _dir;
};

TEST_F(DtPorterHost, RunnerDestructionStopsParsersBeforeMembers)
{
	WtDtRunner* runner = new WtDtRunner();
	start(*runner);
	EXPECT_EQ(_v2[C_CONNECT], 1);
	ASSERT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 3; }));
	delete runner;
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	const int callbacks = _v2[C_CALLBACKS];
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_EQ(_v2[C_CALLBACKS], callbacks);
}

// I03-NEW-047 / BUG-P10-006: _dumpers is declared after _data_mgr. The fake
// writer flushes through its external dumpers inside release(); the runner must
// release the data manager while the dumpers are still alive (run under ASan
// for the red control: the pre-change runner frees the dumpers first).
TEST_F(DtPorterHost, WriterReleaseReachesLiveExtDumpers)
{
	void* writerModule = dlopen((module_dir() + "libI03DtFakeWriter.dylib").c_str(), RTLD_NOW);
	ASSERT_NE(writerModule, nullptr);
	auto releaseDumps = reinterpret_cast<int (*)()>(dlsym(writerModule, "fake_writer_release_dumps"));
	ASSERT_NE(releaseDumps, nullptr);
	const int before = releaseDumps();

	WtDtRunner* runner = new WtDtRunner();
	start(*runner, R"JSON({"module": "I03DtFakeWriter"})JSON");
	ASSERT_TRUE(runner->createExtDumper("r047_dumper"));
	ASSERT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 0; }));
	delete runner;
	EXPECT_EQ(releaseDumps() - before, 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
}

#ifdef WT_HOST_NEW
TEST_F(DtPorterHost, ShutdownReleasesInOrderAndIsIdempotent)
{
	WtDtRunner* runner = new WtDtRunner();
	start(*runner);
	ASSERT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 3; }));
	EXPECT_TRUE(runner->shutdown());
	EXPECT_EQ(_v2[C_REQUEST_STOP], 1);
	EXPECT_EQ(_v2[C_DESTROY], 1);
	EXPECT_EQ(_v2[C_LIVE], 0);
	const int callbacks = _v2[C_CALLBACKS];
	EXPECT_TRUE(runner->shutdown());		// idempotent
	runner->start(true, false);			// rejected after the registry stopped
	EXPECT_EQ(_v2[C_CONNECT], 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	EXPECT_EQ(_v2[C_CALLBACKS], callbacks);
	delete runner;
	EXPECT_EQ(_v2[C_DESTROY], 1);
}

TEST_F(DtPorterHost, UnprovableParserKeepsRunnerGraph)
{
	_v2.set(K_WAIT_MODE, 2);
	WtDtRunner* runner = new WtDtRunner();		// retained to process exit on failure
	start(*runner);
	ASSERT_TRUE(wait_until([&]() { return _v2[C_CALLBACKS] > 0; }));
	EXPECT_FALSE(runner->shutdown());
	EXPECT_EQ(_v2[C_DESTROY], 0);
	EXPECT_EQ(_v2[C_DELETE_UNSAFE], 0);
	EXPECT_EQ(_v2[C_LIVE], 1);
	EXPECT_FALSE(runner->shutdown());
	EXPECT_EQ(_v2[C_WAIT], 1);					// uncertain owner is not retried
}
#endif
}
