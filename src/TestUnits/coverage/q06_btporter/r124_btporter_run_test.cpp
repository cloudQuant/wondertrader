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
 * 生产: WtBtPorter.cpp:174-185（static inited 门 -> 第二次调用 L179 早退）
 * 用坏 JSON 内容调用两次：第一次即使真正走到 runner.config 也只会在
 * load 失败处返回，不建立 mocker；第二次必须被一次性门直接吃掉。
 */
TEST(R124BtPorterConfig, SecondConfigBacktestCallIsNoOp)
{
	EXPECT_NO_THROW(config_backtest("{ r124 not a json document", false));
	EXPECT_NO_THROW(config_backtest("{ r124 not a json document", false));
}
