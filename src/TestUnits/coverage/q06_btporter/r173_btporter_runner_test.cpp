/*
 * r173_btporter_runner_test.cpp  (lane R173 · wave25)
 *
 * 靶子：
 *  - WtBtRunner.cpp:528-547  run(bAsync=true) 的 worker 分支
 *  - WtBtRunner.cpp:555-558  stop() 在 worker 已结束/未运行时的 join+reset 分支
 *  - WtBtPorter.cpp:899-904  cta_step 的异步模式转发
 *  - WtBtRunner.h:203-205/218-224/548-596 事件注册与四个 inline 事件钩子
 *  - WtBtRunner.cpp:96-180   五个 ext loader 的周期映射/日期转发与 feed 回调
 *
 * 异步用例只做空回放（mode=none、无任何行情）：worker 进入 prepare/run 后
 * 找不到主 K 线即 handle_replay_done，run_backtest 返回后 stop_backtest join。
 * 不加载策略插件、不接柜台、不写共享内存；临时目录只落在系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

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
		/ (std::string("r173_bt_runner_") + tag + "_" + std::to_string((long)::getpid()));
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

/*
 * 生产: WtBtRunner.cpp:528-547（异步 worker）、555-558（stop 的 join/reset 分支）、
 *       WtBtPorter.cpp:899-904（cta_step）
 */
TEST(R173BtRunnerAsync, AsyncEmptyReplayStopJoinsWorkerThenCtaStepIsSafe)
{
	const std::string dir = prepare_dir("async");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	reset_singleton();
	getRunner().init((dir + "/r173_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{"
		"\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"tick\":false,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"r173_async_empty\",\"slippage\":0}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().cta_mocker());

	// 空回放：worker 会很快走到 handle_replay_done；stop 负责 join
	run_backtest(false, true);
	EXPECT_TRUE(getRunner().isAsync());
	stop_backtest();

	// 回放已结束，CtaMocker 没有 install_hook，step_calc 必须安全返回 false
	EXPECT_FALSE(cta_step(1));

	// 再次 stop 只走 _running==false 且 _worker 已 reset 的早退分支
	EXPECT_NO_THROW(stop_backtest());

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

//////////////////////////////////////////////////////////////////////////
// 本地 WtBtRunner 实例的内联事件/loader 测试

struct EvtRec
{
	uint32_t	count = 0;
	WtUInt32	last_id = 0;
	WtUInt32	last_date = 0;
	WtUInt32	last_time = 0;
};

EvtRec g_evt;

void evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
{
	g_evt.count++;
	g_evt.last_id = evtId;
	g_evt.last_date = curDate;
	g_evt.last_time = curTime;
}

struct LoaderRec
{
	std::string final_period;
	std::string raw_period;
	std::string fct_code;
	uint32_t	tick_date = 0;
	uint32_t	bars_calls = 0;
	uint32_t	bars_count = 0;
	uint32_t	fct_calls = 0;
	uint32_t	tick_calls = 0;
	void*		last_obj = nullptr;
};

LoaderRec g_loader;

bool fnl_bars(const char* /*stdCode*/, const char* period)
{
	g_loader.final_period = period ? period : "";
	return true;
}

bool raw_bars(const char* /*stdCode*/, const char* period)
{
	g_loader.raw_period = period ? period : "";
	return true;
}

bool adj_factors(const char* stdCode)
{
	g_loader.fct_code = stdCode ? stdCode : "";
	return true;
}

bool raw_ticks(const char* /*stdCode*/, uint32_t uDate)
{
	g_loader.tick_date = uDate;
	return true;
}

void read_bars(void* obj, WTSBarStruct* /*bars*/, uint32_t count)
{
	g_loader.bars_calls++;
	g_loader.bars_count = count;
	g_loader.last_obj = obj;
}

void read_factors(void* /*obj*/, const char* /*stdCode*/, uint32_t* /*dates*/,
	double* /*factors*/, uint32_t /*count*/)
{
	g_loader.fct_calls++;
}

void read_ticks(void* /*obj*/, WTSTickStruct* /*ticks*/, uint32_t /*count*/)
{
	g_loader.tick_calls++;
}

}	// namespace

/*
 * 生产: WtBtRunner.h:202-224/548-596；WtBtRunner.cpp:96-180
 * 本地实例上验证：事件回调四个入口的 id/日期/时间；ext loader 的 d1/m1/m5 映射、
 * 未知周期返回 false；feedRawBars/feedAdjFactors/feedRawTicks 转发到最近一次
 * 由 load* 设置的 _feeder_* 回调。
 */
TEST(R173BtRunnerInline, EventsAccessorsAndExtLoaderMapping)
{
	WtBtRunner runner;
	g_evt = EvtRec();
	g_loader = LoaderRec();

	runner.registerEvtCallback(&evt_cb);
	runner.on_initialize_event();
	EXPECT_EQ(1U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_INIT, g_evt.last_id);

	runner.on_schedule_event(20240315, 93000);
	EXPECT_EQ(2U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_SCHDL, g_evt.last_id);
	EXPECT_EQ(20240315U, g_evt.last_date);
	EXPECT_EQ(93000U, g_evt.last_time);

	runner.on_session_event(20240315, true);
	EXPECT_EQ(EVENT_SESSION_BEGIN, g_evt.last_id);
	runner.on_session_event(20240315, false);
	EXPECT_EQ(EVENT_SESSION_END, g_evt.last_id);
	EXPECT_EQ(4U, g_evt.count);

	runner.on_backtest_end();
	EXPECT_EQ(EVENT_BACKTEST_END, g_evt.last_id);
	EXPECT_EQ(5U, g_evt.count);

	// 清空回调后事件不再转发
	runner.registerEvtCallback(nullptr);
	runner.on_backtest_end();
	EXPECT_EQ(5U, g_evt.count);

	// 未配置时 cta/sel 槽为空；hft 槽未进构造初始化列表（BUG-Q06-003），不读取
	EXPECT_EQ(nullptr, runner.cta_mocker());
	EXPECT_EQ(nullptr, runner.sel_mocker());
	EXPECT_FALSE(runner.isAsync());
	(void)runner.replayer();

	runner.registerExtDataLoader(fnl_bars, raw_bars, adj_factors, raw_ticks, false);
	EXPECT_FALSE(runner.isAutoTrans());

	int dummy = 0;
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));

	EXPECT_TRUE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_DAY, &read_bars));
	EXPECT_EQ(std::string("d1"), g_loader.final_period);
	EXPECT_TRUE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_Minute1, &read_bars));
	EXPECT_EQ(std::string("m1"), g_loader.final_period);
	EXPECT_TRUE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_Minute5, &read_bars));
	EXPECT_EQ(std::string("m5"), g_loader.final_period);
	EXPECT_FALSE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_Tick, &read_bars));

	EXPECT_TRUE(runner.loadRawHisBars(&dummy, "SHFE.rb2510", KP_DAY, &read_bars));
	EXPECT_EQ(std::string("d1"), g_loader.raw_period);
	EXPECT_FALSE(runner.loadRawHisBars(&dummy, "SHFE.rb2510", KP_Tick, &read_bars));

	EXPECT_TRUE(runner.loadAllAdjFactors(&dummy, &read_factors));
	EXPECT_EQ(std::string(""), g_loader.fct_code);
	EXPECT_TRUE(runner.loadAdjFactors(&dummy, "SHFE.rb2510", &read_factors));
	EXPECT_EQ(std::string("SHFE.rb2510"), g_loader.fct_code);

	EXPECT_TRUE(runner.loadRawHisTicks(&dummy, "SHFE.rb2510", 20240103, &read_ticks));
	EXPECT_EQ(20240103U, g_loader.tick_date);

	// feed* 使用最近一次 load* 装入的 _feed_obj/_feeder_*
	runner.feedRawBars(&bar, 1);
	EXPECT_EQ(1U, g_loader.bars_calls);
	EXPECT_EQ(1U, g_loader.bars_count);
	EXPECT_EQ(&dummy, g_loader.last_obj);

	uint32_t dates[1] = { 20240103 };
	double factors[1] = { 1.0 };
	runner.feedAdjFactors("SHFE.rb2510", dates, factors, 1);
	EXPECT_EQ(1U, g_loader.fct_calls);

	runner.feedRawTicks(&tick, 1);
	EXPECT_EQ(1U, g_loader.tick_calls);
}
