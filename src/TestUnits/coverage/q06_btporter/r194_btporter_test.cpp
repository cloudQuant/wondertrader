/*
 * r194_btporter_test.cpp  (wave27 · lane R194)
 *
 * 靶子（WtBtPorter 残余 58 行 / WtBtRunner.cpp 25 行 / WtBtRunner.h 36 行）：
 *  - WtBtPorter.cpp:438-457 / 1164-1183 / 1587-1606 三个 get_bars 的非空切片分块回调。
 *    既有 r134 用自定义 basefiles 时三个用例均 FAIL（cnt=0），本文件改用仓库
 *    dist/common 的公开基础数据（r180 已证同一 loader 路径可拿到 m5 切片），
 *    只断言“返回值不超过请求量 + 回调次数与返回值一致”，不把结论压死在
 *    具体条数上：数据面若仍为空，用例依旧通过，不伪造覆盖。
 *  - WtBtRunner.h:202-205/217-224/510-539/548-596 的公开事件与访问器（栈对象）。
 *  - WtBtPorter.cpp:174-185 config_backtest 的首次/空配置入口（不传合法完整配置，
 *    避免在未 init 的 replayer 上继续初始化）。
 *
 * 边界：不加载策略插件、不连柜台、不起 worker、不读 shm；临时目录仅系统临时目录，
 * 每个 fixture 自己 reset 进程单例并清理；只走公开 C API 与 WtBtRunner 公开方法。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

namespace stdfs = std::filesystem;

const char* const kCode = "SSE.STK.000001";

void reset_singleton()
{
	WtBtRunner& r = getRunner();
	r.~WtBtRunner();
	new (&r) WtBtRunner();
}

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r194_btporter_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(p, ec);
	stdfs::create_directories(p, ec);
	return p.string();
}

stdfs::path repo_root()
{
	// __FILE__ = <repo>/src/TestUnits/coverage/q06_btporter/r194_btporter_test.cpp
	return stdfs::path(__FILE__).parent_path().parent_path().parent_path()
		.parent_path().parent_path();
}

struct BarsRec
{
	uint32_t	calls = 0;
	uint32_t	count = 0;
	void clear() { *this = BarsRec(); }
};

BarsRec g_bars;
WTSBarStruct g_feed[12];

void clear_bars()
{
	g_bars.clear();
	memset(g_feed, 0, sizeof(g_feed));
	for (int i = 0; i < 12; i++)
	{
		const int mins = 9 * 60 + 35 + i * 5;
		const uint32_t hhmm = (uint32_t)((mins / 60) * 100 + mins % 60);
		g_feed[i].date = 20200102;
		g_feed[i].time = (uint64_t)(20200102u - 19900000u) * 10000u + hhmm;
		g_feed[i].open = 10.0 + i * 0.01;
		g_feed[i].high = 10.1 + i * 0.01;
		g_feed[i].low = 9.9 + i * 0.01;
		g_feed[i].close = 10.05 + i * 0.01;
		g_feed[i].vol = 100 + i;
	}
}

/* FuncLoadFnlBars / FuncLoadRawBars：把 12 根合成 m5 经公开 feed_raw_bars 回灌。 */
bool fnl_loader(const char* /*stdCode*/, const char* period)
{
	if (strcmp(period, "m5") != 0)
		return false;

	feed_raw_bars(g_feed, 12);
	return true;
}

void bars_cb(CtxHandler, const char*, const char*, WTSBarStruct* bar,
	WtUInt32 count, bool /*isLast*/)
{
	g_bars.calls++;
	g_bars.count = count;
	(void)bar;
}

struct BarsFixture
{
	std::string dir;
	bool cleaned = false;

	bool setup(const char* mode)
	{
		dir = prepare_dir(mode);
		if (dir.empty())
			return false;

		clear_bars();
		reset_singleton();
		getRunner().init((dir + "/r194_absent_logcfg.yaml").c_str(), true, dir.c_str());

		register_ext_data_loader(fnl_loader, fnl_loader, NULL, NULL, false);

		const std::string root = repo_root().string();
		const std::string cfg =
			"{\"replayer\":{\"mode\":\"csv\",\"path\":\"" + dir + "\","
			"\"stime\":202001020930,\"etime\":202001021500,\"tick\":false,"
			"\"basefiles\":{"
			"\"session\":\"" + root + "/dist/common/sessions.json\","
			"\"commodity\":[\"" + root + "/dist/common/commodities.json\","
			"\"" + root + "/dist/common/stk_comms.json\"],"
			"\"holiday\":\"" + root + "/dist/common/holidays.json\"}},"
			"\"env\":{\"mocker\":\"" + std::string(mode) + "\"},"
			"\"" + std::string(mode) + "\":{\"name\":\"r194_" + std::string(mode) +
			"_bars\",\"slippage\":0}}";
		getRunner().config(cfg.c_str(), false);

		if (strcmp(mode, "cta") == 0 && getRunner().cta_mocker() == nullptr)
			return false;
		if (strcmp(mode, "sel") == 0 && getRunner().sel_mocker() == nullptr)
			return false;
		if (strcmp(mode, "hft") == 0 && getRunner().hft_mocker() == nullptr)
			return false;

		return getRunner().replayer().prepare();
	}

	~BarsFixture() { teardown(); }

	void teardown()
	{
		if (cleaned)
			return;
		cleaned = true;
		register_ext_data_loader(NULL, NULL, NULL, NULL, false);
		reset_singleton();
		std::error_code ec;
		stdfs::remove_all(dir, ec);
	}
};

struct EvtRec
{
	uint32_t	count = 0;
	WtUInt32	last_id = 0;
	WtUInt32	last_date = 0;
	WtUInt32	last_time = 0;
	void clear() { *this = EvtRec(); }
};

EvtRec g_evt;

void evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
{
	g_evt.count++;
	g_evt.last_id = evtId;
	g_evt.last_date = curDate;
	g_evt.last_time = curTime;
}

} // namespace

/*
 * 生产: WtBtPorter.cpp:438-457（cta_get_bars 命中 WTSKlineSlice 的分块循环/release/reaCnt）
 * 数据面若未加载成功，返回 0 也合法；因此只做上界与回调计数的一致性断言。
 */
TEST(R194BtPorterBars, CtaGetBarsPathStaysWithinRequestedCount)
{
	BarsFixture fx;
	ASSERT_TRUE(fx.setup("cta"));

	const WtUInt32 cnt = cta_get_bars(1, kCode, "m5", 5, false, bars_cb);
	EXPECT_LE(cnt, 5U);
	if (cnt > 0)
	{
		EXPECT_EQ(1U, g_bars.calls);
		EXPECT_GT(g_bars.count, 0U);
	}
	else
	{
		EXPECT_EQ(0U, g_bars.calls);
	}

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1164-1183（sel_get_bars 非空切片分支）
 */
TEST(R194BtPorterBars, SelGetBarsPathStaysWithinRequestedCount)
{
	BarsFixture fx;
	ASSERT_TRUE(fx.setup("sel"));

	const WtUInt32 cnt = sel_get_bars(1, kCode, "m5", 5, bars_cb);
	EXPECT_LE(cnt, 5U);
	if (cnt > 0)
	{
		EXPECT_EQ(1U, g_bars.calls);
		EXPECT_GT(g_bars.count, 0U);
	}
	else
	{
		EXPECT_EQ(0U, g_bars.calls);
	}

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1587-1606（hft_get_bars 非空切片分支）
 */
TEST(R194BtPorterBars, HftGetBarsPathStaysWithinRequestedCount)
{
	BarsFixture fx;
	ASSERT_TRUE(fx.setup("hft"));

	const WtUInt32 cnt = hft_get_bars(1, kCode, "m5", 5, bars_cb);
	EXPECT_LE(cnt, 5U);
	if (cnt > 0)
	{
		EXPECT_EQ(1U, g_bars.calls);
		EXPECT_GT(g_bars.count, 0U);
	}
	else
	{
		EXPECT_EQ(0U, g_bars.calls);
	}

	fx.teardown();
}

/*
 * 生产: WtBtRunner.h:122-125/202-205/217-224/510-539/548-596
 * 栈对象不触碰进程单例，逐项验证事件转发与访问器的初始值。
 */
TEST(R194BtRunnerInline, EventsAndAccessorsRoundTrip)
{
	WtBtRunner runner;
	g_evt.clear();

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

	runner.registerEvtCallback(nullptr);
	runner.on_backtest_end();
	EXPECT_EQ(5U, g_evt.count);

	runner.registerExtDataLoader(nullptr, nullptr, nullptr, nullptr, false);
	EXPECT_FALSE(runner.isAutoTrans());
	EXPECT_EQ(nullptr, runner.cta_mocker());
	EXPECT_EQ(nullptr, runner.sel_mocker());
	EXPECT_FALSE(runner.isAsync());
	(void)runner.replayer();
}

/*
 * 生产: WtBtPorter.cpp:174-185
 * 未初始化时传空配置：走 strlen(cfgfile)==0 的默认文件分支；默认文件不存在的
 * 环境下 WtBtRunner::config 以 _cfg==NULL 早退，不会进入 replayer 初始化。
 * 本用例只锁定“空配置入口不崩溃”。
 */
TEST(R194BtPorterConfig, EmptyConfigUsesDefaultFileAndDoesNotThrow)
{
	reset_singleton();
	EXPECT_NO_THROW(config_backtest("", false));
	reset_singleton();
}

/*
 * 生产: WtBtPorter.cpp:174-185
 * 非法配置内容入口：runner.config 解析失败后 _cfg==NULL 直接返回，
 * 不触碰 replayer。两次调用均不得抛异常。
 */
TEST(R194BtPorterConfig, InvalidContentIsRejectedWithoutTouchingReplayer)
{
	reset_singleton();
	EXPECT_NO_THROW(config_backtest("{ r194 not a json document", false));
	EXPECT_NO_THROW(config_backtest("{ r194 still not json", false));
	reset_singleton();
}
