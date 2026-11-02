// R22-003 positive controls for a real WtDataStorage writer.
//
// These cases deliberately use the writer module next to the running
// CoverageTests executable. The direct DataManager case observes a successful
// init return and a persisted tick block; the Runner case verifies that a
// structurally complete configuration can initialize the same module and
// return from all-day asynchronous start without leaking its writer thread.
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <string>
#include <system_error>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Includes/FasterDefs.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTypes.h"
#include "Share/DLLHelper.hpp"
#include "Share/ModuleHelper.hpp"
#include "WtDataStorage/DataDefine.h"
#include "WtDtCore/DataManager.h"
#include "WtDtCore/IndexWorkerMgr.h"
#include "WtDtCore/ParserAdapter.h"
#include "WtDtCore/ShmCaster.h"
#include "WtDtCore/StateMonitor.h"
#include "WtDtCore/UDPCaster.h"
#include "WtDtCore/WtHelper.h"
#include "WtDtPorter/ExpDumper.h"
#include "WtDtPorter/PorterDefs.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include <boost/asio.hpp>
#include <vector>

#define private public
#include "WtDtPorter/WtDtRunner.h"
#undef private
#include "WtDtPorter/WtDtPorter.h"
#include "WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

namespace fs = std::filesystem;

namespace {

const char* const kRunnerChildEnv = "WT_R22_WRITER_START_CHILD";

struct RunnerEventHit
{
	uint32_t _event;
	std::string _id;
};

std::vector<RunnerEventHit>& runner_event_hits()
{
	static std::vector<RunnerEventHit> hits;
	return hits;
}

void PORTER_FLAG record_runner_event(WtUInt32 eventId, const char* id)
{
	runner_event_hits().push_back({ eventId, id == NULL ? std::string() : std::string(id) });
}

std::string temp_root()
{
	static std::atomic<uint32_t> sequence { 0 };
	return (fs::temp_directory_path() /
		("r22_writer_" + std::to_string(::getpid()) + "_" + std::to_string(sequence++))).string();
}

std::string json_string(const std::string& value)
{
	std::string out = "\"";
	for (char ch : value)
	{
		if (ch == '\\' || ch == '\"')
			out.push_back('\\');
		if (ch == '\n')
		{
			out += "\\n";
			continue;
		}
		if (ch == '\r')
		{
			out += "\\r";
			continue;
		}
		out.push_back(ch);
	}
	out.push_back('\"');
	return out;
}

bool read_rt_header(const fs::path& path, RTBlockHeader& header)
{
	std::memset(&header, 0, sizeof(header));
	std::ifstream input(path, std::ios::binary);
	return input.read(reinterpret_cast<char*>(&header), sizeof(header)).good();
}

std::string executable_bin_dir()
{
	// getBinDir() resolves the current test executable, not the repository cwd.
	fs::path testBin(getBinDir());
	// ModuleHelper returns its directory with a trailing slash; path::parent_path()
	// would otherwise remove only that empty final component.
	if (testBin.filename().empty())
		testBin = testBin.parent_path();
	fs::path bin = testBin.parent_path();
	std::string path = bin.string();
	if (!path.empty() && path.back() != '/')
		path.push_back('/');
	return path;
}

std::string test_executable_path()
{
	return (fs::path(getBinDir()) / "CoverageTests").string();
}

} // namespace

extern WtDtRunner& getRunner();

class R22WriterStartTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_previousModuleDir = WtHelper::get_module_dir();
		_root = temp_root();
		std::error_code ec;
		fs::remove_all(_root, ec);
		ASSERT_TRUE(fs::create_directories(_root, ec));
		ASSERT_FALSE(ec);

		write_text(_root / "sessions.json",
			R"({"DAY":{"name":"day","offset":0,"sections":[{"from":0,"to":2359}]}})");
		write_text(_root / "commodities.json",
			R"({"SSE":{"STK":{"name":"stock","session":"DAY","pricetick":0.01,"volscale":1,"covermode":0,"pricemode":0,"trademode":1}}})");
		write_text(_root / "contracts.json",
			R"({"SSE":{"600000":{"name":"stock","exchg":"SSE","product":"STK","opendate":20200101,"expiredate":20500101}}})");
		_logCfg = (_root / "missing_log_config.json").string();
	}

	void TearDown() override
	{
		WtHelper::set_module_dir(_previousModuleDir.c_str());
		std::error_code ec;
		fs::remove_all(_root, ec);
		EXPECT_FALSE(ec);
	}

	void write_text(const fs::path& path, const char* content)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << content;
	}

	std::string writerConfig(const fs::path& outputDir) const
	{
		return "{\"module\":\"WtDataStorage\",\"path\":" + json_string(outputDir.string()) +
			",\"cache\":\"r22_writer_cache.dmb\",\"async\":false,\"groupsize\":1,\"disablehis\":true,"
			"\"disabletick\":false,\"disablemin1\":true,\"disablemin5\":true,"
			"\"disableday\":true,\"disabletrans\":true,\"disableordque\":true,"
			"\"disableorddtl\":true}";
	}

	std::string runnerConfig(const fs::path& outputDir) const
	{
		std::string config = "{\"basefiles\":{\"session\":" + json_string((_root / "sessions.json").string()) +
			",\"commodity\":" + json_string((_root / "commodities.json").string()) +
			",\"contract\":" + json_string((_root / "contracts.json").string()) + "},";
		config += "\"allday\":true,\"writer\":" + writerConfig(outputDir) + ",\"parsers\":[]}";
		return config;
	}

	bool writerModuleExists() const
	{
		const fs::path modulePath = fs::path(executable_bin_dir()) / DLLHelper::wrap_module("WtDataStorage");
		std::error_code ec;
		return fs::is_regular_file(modulePath, ec) && !ec;
	}

	fs::path _root;
	std::string _logCfg;
	std::string _previousModuleDir;
};

TEST_F(R22WriterStartTest, RealWriterInitAcceptsAndPersistsTick)
{
	ASSERT_TRUE(writerModuleExists()) << "WtDataStorage module must be beside the running test executable's bin directory";
	const std::string moduleDir = executable_bin_dir();
	WtHelper::set_module_dir(moduleDir.c_str());

	WTSBaseDataMgr base;
	ASSERT_TRUE(base.loadSessions((_root / "sessions.json").string().c_str()));
	ASSERT_TRUE(base.loadCommodities((_root / "commodities.json").string().c_str()));
	ASSERT_TRUE(base.loadContracts((_root / "contracts.json").string().c_str()));
	ASSERT_EQ(1U, base.getContractSize());
	WTSContractInfo* contract = base.getContract("600000", "SSE");
	ASSERT_NE(nullptr, contract);

	const fs::path outputDir = _root / "direct_writer_output";
	const fs::path cachePath = outputDir / "r22_writer_cache.dmb";
	const fs::path tickPath = outputDir / "rt" / "ticks" / "SSE" / "600000.dmb";
	DataManager manager;
	WTSVariant* params = WTSCfgLoader::load_from_content(writerConfig(outputDir).c_str(), false);
	ASSERT_NE(nullptr, params);
	const bool initialized = manager.init(params, &base, nullptr);
	params->release();
	ASSERT_TRUE(initialized) << "DataManager::init must return true for the real WtDataStorage module";

	RTBlockHeader cacheHeader;
	ASSERT_TRUE(read_rt_header(cachePath, cacheHeader));
	EXPECT_EQ(BT_RT_Cache, cacheHeader._type);
	EXPECT_EQ(1U, cacheHeader._capacity);
	EXPECT_EQ(0U, cacheHeader._size);
	EXPECT_EQ(1U, cacheHeader._version);
	EXPECT_EQ(0, std::memcmp(cacheHeader._blk_flag, BLK_FLAG, sizeof(cacheHeader._blk_flag)));

	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strncpy(tick.exchg, "SSE", sizeof(tick.exchg) - 1);
	std::strncpy(tick.code, "600000", sizeof(tick.code) - 1);
	tick.action_date = 20240108;
	tick.action_time = 93000000;
	tick.trading_date = 20240108;
	tick.price = 10.25;
	tick.open = 10.0;
	tick.high = 10.25;
	tick.low = 10.0;
	tick.total_volume = 100;
	tick.total_turnover = 1025;
	WTSTickData* data = WTSTickData::create(tick);
	ASSERT_NE(nullptr, data);
	data->setContractInfo(contract);
	const bool accepted = manager.writeTick(data, 0);
	data->release();
	manager.release();
	ASSERT_TRUE(accepted) << "the initialized real writer must accept a valid tick";

	RTBlockHeader updatedCache;
	ASSERT_TRUE(read_rt_header(cachePath, updatedCache));
	EXPECT_EQ(BT_RT_Cache, updatedCache._type);
	EXPECT_EQ(1U, updatedCache._size);
	EXPECT_TRUE(fs::is_regular_file(tickPath));
	RTDayBlockHeader tickHeader;
	std::memset(&tickHeader, 0, sizeof(tickHeader));
	std::ifstream tickFile(tickPath, std::ios::binary);
	ASSERT_TRUE(tickFile.read(reinterpret_cast<char*>(&tickHeader), sizeof(tickHeader)).good());
	EXPECT_EQ(BT_RT_Ticks, tickHeader._type);
	EXPECT_EQ(20240108U, tickHeader._date);
	EXPECT_EQ(1U, tickHeader._size);
}

TEST_F(R22WriterStartTest, RunnerStartsWithRealWriterAndReleasesItOnScopeExit)
{
	const char* childMode = std::getenv(kRunnerChildEnv);
	if (childMode != nullptr && std::strcmp(childMode, "1") == 0)
	{
		ASSERT_TRUE(writerModuleExists()) << "WtDataStorage module must be beside the running test executable's bin directory";
		runner_event_hits().clear();
		runner_event_hits().reserve(3);
		register_parser_callbacks(record_runner_event, NULL);
		const std::string moduleDir = executable_bin_dir();
		const fs::path outputDir = _root / "runner_writer_output";
		const fs::path cachePath = outputDir / "r22_writer_cache.dmb";
		const fs::path tickPath = outputDir / "rt" / "ticks" / "SSE" / "600000.dmb";
		WtDtRunner& runner = getRunner();
		struct RunnerReleaseGuard
		{
			WtDtRunner& _runner;
			~RunnerReleaseGuard()
			{
				_runner._parsers.release();
				if (_runner._parsers.releaseComplete())
					_runner._data_mgr.release();
			}
		} runnerRelease { runner };

		const std::string config = runnerConfig(outputDir);
		// CoverageTests builds WtDtPorter.cpp into the test executable, whose own
		// directory is bin/TestUnits; pass the located bin directory explicitly so
		// the real writer module is resolved next to the product libraries.
		runner.initialize(config.c_str(), _logCfg.c_str(), moduleDir.c_str(), false, false);
		const fs::path runnerModule = fs::path(WtHelper::get_module_dir())
			/ DLLHelper::wrap_module("WtDataStorage");
		EXPECT_TRUE(fs::is_regular_file(runnerModule)) << runnerModule;
		ASSERT_TRUE(create_ext_parser("r22_real_writer_parser"));
		ASSERT_EQ(1U, runner_event_hits().size());
		EXPECT_EQ(EVENT_PARSER_INIT, runner_event_hits()[0]._event);
		EXPECT_EQ("r22_real_writer_parser", runner_event_hits()[0]._id);
		ASSERT_TRUE(runner._quarantined_ext_parsers.empty());
		// Use the same singleton instance as the public parser_push_quote C bridge.
		// The explicit all-day argument avoids the C start() wrapper's state-monitor path.
		runner.start(true, true);
		ASSERT_EQ(2U, runner_event_hits().size());
		EXPECT_EQ(EVENT_PARSER_CONNECT, runner_event_hits()[1]._event);
		EXPECT_EQ("r22_real_writer_parser", runner_event_hits()[1]._id);
		RTBlockHeader initializedCache;
		ASSERT_TRUE(read_rt_header(cachePath, initializedCache));
		EXPECT_EQ(BT_RT_Cache, initializedCache._type);
		EXPECT_EQ(1U, initializedCache._capacity);
		EXPECT_EQ(0U, initializedCache._size);

		WTSTickStruct tick;
		std::memset(&tick, 0, sizeof(tick));
		std::strncpy(tick.exchg, "SSE", sizeof(tick.exchg) - 1);
		std::strncpy(tick.code, "600000", sizeof(tick.code) - 1);
		tick.action_date = 20240108;
		tick.action_time = 93000000;
		tick.trading_date = 20240108;
		tick.price = 10.25;
		tick.open = 10.0;
		tick.high = 10.25;
		tick.low = 10.0;
		tick.total_volume = 100;
		tick.total_turnover = 1025;
		parser_push_quote("r22_real_writer_parser", &tick, 0);

		// This synchronous fake API has no provider thread. Release the adapter
		// before explicitly releasing the writer and its borrowed base-data graph.
		runner._parsers.release();
		const bool parserReleaseComplete = runner._parsers.releaseComplete();
		EXPECT_TRUE(parserReleaseComplete);
		EXPECT_EQ(0U, runner._parsers.size());
		EXPECT_TRUE(runner._quarantined_ext_parsers.empty());
		ASSERT_EQ(3U, runner_event_hits().size());
		EXPECT_EQ(EVENT_PARSER_RELEASE, runner_event_hits()[2]._event);
		if (parserReleaseComplete)
			runner._data_mgr.release();

		RTBlockHeader cacheHeader;
		ASSERT_TRUE(read_rt_header(cachePath, cacheHeader));
		EXPECT_EQ(BT_RT_Cache, cacheHeader._type);
		EXPECT_GE(cacheHeader._capacity, 1U);
		EXPECT_EQ(1U, cacheHeader._size);
		EXPECT_TRUE(fs::is_regular_file(tickPath));
		RTDayBlockHeader tickHeader;
		std::memset(&tickHeader, 0, sizeof(tickHeader));
		std::ifstream tickFile(tickPath, std::ios::binary);
		ASSERT_TRUE(tickFile.read(reinterpret_cast<char*>(&tickHeader), sizeof(tickHeader)).good());
		EXPECT_EQ(BT_RT_Ticks, tickHeader._type);
		EXPECT_EQ(20240108U, tickHeader._date);
		EXPECT_EQ(1U, tickHeader._size);
		WTSTickStruct storedTick;
		std::memset(&storedTick, 0, sizeof(storedTick));
		tickFile.seekg(sizeof(RTDayBlockHeader));
		ASSERT_TRUE(tickFile.read(reinterpret_cast<char*>(&storedTick), sizeof(storedTick)).good());
		EXPECT_STREQ("600000", storedTick.code);
		EXPECT_DOUBLE_EQ(10.25, storedTick.price);
		register_parser_callbacks(NULL, NULL);
		return;
	}

	// Run the C API scenario in a fresh process so existing coverage tests cannot
	// share or reset the process-wide getRunner() singleton. The child handles its
	// parser and writer release explicitly; process exit also runs static cleanup.
	const std::string executable = test_executable_path();
	ASSERT_TRUE(fs::is_regular_file(executable)) << executable;
	const std::string filter = "--gtest_filter=R22WriterStartTest.RunnerStartsWithRealWriterAndReleasesItOnScopeExit";
	const std::string color = "--gtest_color=no";
	const pid_t child = ::fork();
	ASSERT_GE(child, 0) << "fork failed: " << std::strerror(errno);
	if (child == 0)
	{
		::setenv(kRunnerChildEnv, "1", 1);
		char* const args[] = {
			const_cast<char*>(executable.c_str()),
			const_cast<char*>(filter.c_str()),
			const_cast<char*>(color.c_str()),
			nullptr
		};
		::execv(executable.c_str(), args);
		::_exit(127);
	}

	int childStatus = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	for (;;)
	{
		const pid_t waited = ::waitpid(child, &childStatus, WNOHANG);
		if (waited == child)
			break;
		if (waited == -1 && errno != EINTR)
		{
			ADD_FAILURE() << "waitpid failed: " << std::strerror(errno);
			return;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			::kill(child, SIGKILL);
			while (::waitpid(child, &childStatus, 0) == -1 && errno == EINTR) {}
			ADD_FAILURE() << "isolated Runner C API positive control exceeded 30 seconds";
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	ASSERT_TRUE(WIFEXITED(childStatus)) << "child terminated abnormally, status=" << childStatus;
	EXPECT_EQ(0, WEXITSTATUS(childStatus));
}
