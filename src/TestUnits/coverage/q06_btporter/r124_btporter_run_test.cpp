/*
 * r124_btporter_run_test.cpp  (lane R124 · wave20)
 *
 * 靶子：
 *   - WtBtPorter.cpp:214-217  run_backtest 的同步转发（无数据空回放）
 *   - WtBtRunner.cpp:509-527  run() 的同步分支（_running 守卫、_async 赋值、
 *     mocker hook、prepare、_replayer.run）
 *   - WtBtPorter.cpp:176-185  config_backtest 的一次性门（第二次调用 L179 早退）
 *
 * 空回放：replayer.mode=none、无任何行情数据，prepare 只回调 sink 的
 * handle_init，run 找不到主 K 线后直接 handle_replay_done；同步路径，
 * 不创建 worker 线程，也不触发任何真实撮合。
 *
 * 边界：只走公开 API；临时文件只写系统临时目录并清理；不触碰 src 生产目录。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include <unistd.h>

#include "WtBtCore/HisDataReplayer.h"
#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	void reset_singleton()
	{
		WtBtRunner& r = getRunner();
		r.~WtBtRunner();
		new (&r) WtBtRunner();
	}

	std::string prepare_dir(const char* tag)
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ (std::string("r124_btporter_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(p, ec);
		stdfs::create_directories(p, ec);
		return p.string();
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	class R124TempDir
	{
	public:
		explicit R124TempDir(const char* tag) : _path(prepare_dir(tag)) {}
		~R124TempDir()
		{
			std::error_code ec;
			stdfs::remove_all(_path, ec);
		}

		const std::string& path() const { return _path; }

	private:
		std::string _path;
	};

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":0,\"to\":2359}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

	/* 只记录回放器生命周期，不消费任何行情。 */
	class R124EmptySink : public IDataSink
	{
	public:
		uint32_t init = 0;
		uint32_t done = 0;

		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_init() override { init++; }
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override { done++; }
	};

	int run_second_config_backtest_probe()
	{
		R124TempDir temp("config_once");
		const std::string& dir = temp.path();
		if (dir.empty() || !stdfs::is_directory(dir))
			return 10;

		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);
		if (!stdfs::exists(dir + "/sessions.json")
			|| !stdfs::exists(dir + "/commodities.json")
			|| !stdfs::exists(dir + "/contracts.json"))
			return 11;

		/* A valid, silent logger profile keeps this probe self-contained. */
		const std::string logCfg = R"JSON({"root":{"level":"off","async":false,"sinks":[]}})JSON";
		init_backtest(logCfg.c_str(), false, dir.c_str());
		enable_tick(true);
		if (!getRunner().replayer().is_tick_enabled())
		{
			std::fprintf(stderr, "could not establish initial tick-enabled state\n");
			return 12;
		}

		auto make_config = [&dir](bool tick) {
			return std::string("{\"replayer\":{")
				+ "\"mode\":\"none\",\"path\":\"" + dir + "\","
				+ "\"stime\":20240102000000,\"etime\":20240103000000,"
				+ "\"tick\":" + (tick ? "true" : "false") + ","
				+ "\"basefiles\":{"
				+ "\"session\":\"" + dir + "/sessions.json\","
				+ "\"commodity\":\"" + dir + "/commodities.json\","
				+ "\"contract\":\"" + dir + "/contracts.json\"}},"
				+ "\"env\":{\"mocker\":\"none\"}}";
		};

		const std::string firstCfg = make_config(false);
		config_backtest(firstCfg.c_str(), false);
		const bool firstConfigDisabledTicks = !getRunner().replayer().is_tick_enabled();

		const std::string secondCfg = make_config(true);
		config_backtest(secondCfg.c_str(), false);
		const bool secondConfigWasIgnored = !getRunner().replayer().is_tick_enabled();

		release_backtest();
		reset_singleton();

		if (!firstConfigDisabledTicks)
		{
			std::fprintf(stderr, "first valid config did not disable tick replay\n");
			return 13;
		}
		if (!secondConfigWasIgnored)
		{
			std::fprintf(stderr, "second config_backtest call changed tick replay state\n");
			return 14;
		}
		return 0;
	}
}

/*
 * 生产: WtBtPorter.cpp:214-217 + WtBtRunner.cpp:509-527
 * 空回放（无行情）同步跑完：prepare -> handle_init -> run -> handle_replay_done。
 * 断言 sink 各命中一次，证明 run() 的同步分支确实执行且没有起 worker。
 */
TEST(R124BtRunnerRun, EmptySyncReplayCompletesWithoutWorker)
{
	const std::string dir = prepare_dir("run");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	reset_singleton();
	getRunner().init((dir + "/r124_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{"
		"\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"tick\":false,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"none\"}}";
	getRunner().config(cfg.c_str(), false);

	R124EmptySink sink;
	getRunner().replayer().register_sink(&sink, "r124_sink");

	EXPECT_NO_THROW(run_backtest(false, false));
	EXPECT_EQ(1U, sink.init);
	EXPECT_EQ(1U, sink.done);

	/* 同步 run 结束后 _running 已复位，stop 只走早退，必须可重复。 */
	EXPECT_NO_THROW(stop_backtest());
	EXPECT_NO_THROW(stop_backtest());

	getRunner().replayer().register_sink(NULL, "");
	reset_singleton();

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/*
 * WtBtPorter.cpp documents config_backtest as a once-only operation. Run in a
 * threadsafe death-test subprocess because its function-local static state is
 * process-wide and cannot be reset through the public API. Both configs and
 * their basefiles are valid; no replay or trading is started.
 */
TEST(R124BtPorterConfig, SecondConfigBacktestCallIsNoOp)
{
	const std::string previousDeathTestStyle = GTEST_FLAG_GET(death_test_style);
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(
		std::exit(run_second_config_backtest_probe()),
		::testing::ExitedWithCode(0), "");
	GTEST_FLAG_SET(death_test_style, previousDeathTestStyle);
}
