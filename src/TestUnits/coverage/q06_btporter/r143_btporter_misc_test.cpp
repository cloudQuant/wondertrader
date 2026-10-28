/*!
 * \file r143_btporter_misc_test.cpp
 * \brief 第二十二波 R143 - WtBtPorter 剩余公开面
 *
 * 覆盖（只读生产代码）:
 *  - WtBtPorter.cpp:482-483  cta_get_ticks 非空切片但 thisCnt==0 的空回调
 *  - WtBtPorter.cpp:1226-1227 sel_get_ticks 同上
 *  - WtBtPorter.cpp:1631-1632 hft_get_ticks 同上
 *  - WtBtRunner.h:202-205     registerEvtCallback
 *  - WtBtRunner.h:217-224     registerExtDataLoader
 *  - WtBtRunner.h:510-539     cta_mocker/sel_mocker/hft_mocker/replayer/isAsync
 *  - WtBtRunner.h:548-596     on_initialize_event/on_schedule_event/on_session_event/on_backtest_end
 *
 * 边界:
 *  - tick 数据由公开 register_ext_data_loader + feed_raw_ticks 注入；prepare() 只做
 *    同步初始化，不调用 run()，无 worker、无行情循环。
 *  - tickCnt 传 0：回放器仍返回非空切片（realCnt 由 eIdx 决定），导出层 min(0,size)=0，
 *    因此命中 thisCnt==0 的 NULL 回调分支。
 *  - 只写系统临时目录并清理；每个 fixture 自己 reset 进程单例。
 *  - 不加载策略插件、不连柜台、不读共享内存。
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

const char* kCode = "SHFE.rb.2510";

//////////////////////////////////////////////////////////////////////////
// 事件回调（WtBtRunner.h:548-596）
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

//////////////////////////////////////////////////////////////////////////
// tick 注入（WtBtPorter.cpp:469-496 / :1618-1644）
struct TicksRec
{
	uint32_t	calls = 0;
	uint32_t	count = 0;
	bool		last = false;
	bool		null_tick = false;
	void clear() { *this = TicksRec(); }
};

TicksRec g_ticks;
WTSTickStruct g_tick_feed[2];

void clear_ticks()
{
	g_ticks.clear();
	memset(g_tick_feed, 0, sizeof(g_tick_feed));
	for (int i = 0; i < 2; i++)
	{
		wt_strcpy(g_tick_feed[i].code, "rb2510");
		wt_strcpy(g_tick_feed[i].exchg, "SHFE");
		g_tick_feed[i].action_date = 20240102;
		g_tick_feed[i].action_time = 93000000 + i * 1000;
		g_tick_feed[i].price = 4501.0 + i;
	}
}

/* fnl 桩：只让回放器拿到非空 _bt_loader，不提供 K 线。 */
bool no_bars(const char* /*stdCode*/, const char* /*period*/)
{
	return false;
}

bool tick_loader(const char* /*stdCode*/, uint32_t /*uDate*/)
{
	feed_raw_ticks(g_tick_feed, 2);
	return true;
}

void ticks_cb(CtxHandler, const char* /*code*/, WTSTickStruct* tick, WtUInt32 count, bool isLast)
{
	g_ticks.calls++;
	g_ticks.count = count;
	g_ticks.last = isLast;
	g_ticks.null_tick = (tick == nullptr);
}

void reset_singleton()
{
	WtBtRunner& r = getRunner();
	r.~WtBtRunner();
	new (&r) WtBtRunner();
}

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r143_btporter_") + tag + "_" + std::to_string((long)::getpid()));
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

std::string make_cfg(const std::string& dir, const char* mode)
{
	std::string section;
	if (strcmp(mode, "hft") == 0)
		section = "\"hft\":{\"name\":\"r143_hft_ticks\"}";
	else
		section = "\"" + std::string(mode) + "\":{\"name\":\"r143_" +
			std::string(mode) + "_ticks\",\"slippage\":0}";

	return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240103000000,\"etime\":20240104000000,\"tick\":true,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"" + std::string(mode) + "\"},"
		+ section + "}";
}

struct TickFixture
{
	std::string dir;
	bool cleaned = false;

	bool setup(const char* tag, const char* mode)
	{
		dir = prepare_dir(tag);
		if (dir.empty())
			return false;
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);

		clear_ticks();
		reset_singleton();
		getRunner().init((dir + "/r143_absent_logcfg.yaml").c_str(), true, dir.c_str());

		register_ext_data_loader(no_bars, nullptr, nullptr, tick_loader, false);
		getRunner().config(make_cfg(dir, mode).c_str(), false);

		if (strcmp(mode, "cta") == 0 && getRunner().cta_mocker() == nullptr)
			return false;
		if (strcmp(mode, "sel") == 0 && getRunner().sel_mocker() == nullptr)
			return false;
		if (strcmp(mode, "hft") == 0 && getRunner().hft_mocker() == nullptr)
			return false;

		return getRunner().replayer().prepare();
	}

	~TickFixture() { teardown(); }

	void teardown()
	{
		if (cleaned)
			return;
		cleaned = true;
		register_ext_data_loader(nullptr, nullptr, nullptr, nullptr, false);
		reset_singleton();
		std::error_code ec;
		stdfs::remove_all(dir, ec);
	}
};

} // namespace

// WtBtRunner.h:202-205 + :217-224 + :510-539 + :548-596
TEST(R143BtRunnerInline, event_callbacks_and_accessors_are_wired)
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

	//回调槽可清空；清空后事件不再转发
	runner.registerEvtCallback(nullptr);
	runner.on_backtest_end();
	EXPECT_EQ(5U, g_evt.count);

	//setter/getter 全走公开面（未配置时 cta/sel 为可断言的空；hft_mocker 见 BUG-Q06-003
	//未进构造初始化列表，对新建实例读取是 UB，故不在此断言）
	runner.registerExtDataLoader(nullptr, nullptr, nullptr, nullptr, false);
	EXPECT_EQ(nullptr, runner.cta_mocker());
	EXPECT_EQ(nullptr, runner.sel_mocker());
	EXPECT_FALSE(runner.isAsync());
	(void)runner.replayer();
}

// WtBtPorter.cpp:1226-1227
TEST(R143BtPorterTicksEmpty, sel_zero_count_takes_null_callback)
{
	TickFixture fx;
	if (!fx.setup("sel_ticks", "sel"))
		GTEST_SKIP() << "sel mocker/replayer not available";

	const WtUInt32 cnt = sel_get_ticks(1, kCode, 0, ticks_cb);
	EXPECT_EQ(0U, cnt);
	ASSERT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);
	EXPECT_TRUE(g_ticks.last);
	EXPECT_TRUE(g_ticks.null_tick);
}

// WtBtPorter.cpp:482-483
TEST(R143BtPorterTicksEmpty, cta_zero_count_takes_null_callback)
{
	TickFixture fx;
	if (!fx.setup("cta_ticks", "cta"))
		GTEST_SKIP() << "cta mocker/replayer not available";

	const WtUInt32 cnt = cta_get_ticks(1, kCode, 0, ticks_cb);
	EXPECT_EQ(0U, cnt);
	ASSERT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);
	EXPECT_TRUE(g_ticks.last);
	EXPECT_TRUE(g_ticks.null_tick);
}

// WtBtPorter.cpp:1631-1632
TEST(R143BtPorterTicksEmpty, hft_zero_count_takes_null_callback)
{
	TickFixture fx;
	if (!fx.setup("hft_ticks", "hft"))
		GTEST_SKIP() << "hft mocker/replayer not available";

	const WtUInt32 cnt = hft_get_ticks(1, kCode, 0, ticks_cb);
	EXPECT_EQ(0U, cnt);
	ASSERT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);
	EXPECT_TRUE(g_ticks.last);
	EXPECT_TRUE(g_ticks.null_tick);
}
