/*
 * r134_btporter_bars_test.cpp  (lane R134 · wave21)
 *
 * 靶子（回测侧 C 导出层的 K 线切片回调支，既有用例只走过 mocker==NULL）：
 *   - WtBtPorter.cpp:438-457   cta_get_bars  的非空 WTSKlineSlice 分块回调
 *   - WtBtPorter.cpp:1164-1183 sel_get_bars  的非空 WTSKlineSlice 分块回调
 *   - WtBtPorter.cpp:1587-1606 hft_get_bars  的非空 WTSKlineSlice 分块回调
 *
 * 数据来源：公开 register_ext_data_loader 注册 fnl loader；HisDataReplayer 的
 * cacheFinalBarsFromLoader 调 loadFinalHisBars，loader 内再调公开 feed_raw_bars
 * 把三根合成日线写入 BarsList；prepare() 只做同步初始化以确定 _cur_tdate，
 * 不调用 run()，不进入行情循环。
 *
 * 边界：不开线程、不跑回放；临时文件只在系统临时目录并清理；每个用例 reset 单例。
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

namespace
{
	namespace stdfs = std::filesystem;

	const char* kCode = "SHFE.rb.2510";

	struct BarsRec
	{
		uint32_t	calls = 0;
		uint32_t	count = 0;
		double		last_close = 0.0;
		bool		last = false;
		std::string	code;
		std::string	period;

		void clear() { *this = BarsRec(); }
	};

	BarsRec g_bars;
	WTSBarStruct g_feed[3];
	uint32_t g_loader_calls = 0;

	void clear_bars()
	{
		g_bars.clear();
		g_loader_calls = 0;
		memset(g_feed, 0, sizeof(g_feed));
		for (int i = 0; i < 3; i++)
		{
			g_feed[i].date = 20240101 + i;
			g_feed[i].time = 1500;
			g_feed[i].open = 4500.0;
			g_feed[i].high = 4510.0;
			g_feed[i].low = 4490.0;
			g_feed[i].close = 4505.0 + i * 15.0;
			g_feed[i].vol = 10;
		}
		g_feed[1].close = 4520.0;
	}

	/* FuncLoadFnlBars：三根合成日线经公开 C API 喂回读取器。 */
	bool fnl_loader(const char* /*stdCode*/, const char* /*period*/)
	{
		g_loader_calls++;
		feed_raw_bars(g_feed, 3);
		return true;
	}

	bool raw_loader(const char* stdCode, const char* period)
	{
		return fnl_loader(stdCode, period);
	}

	void bars_cb(CtxHandler, const char* code, const char* period, WTSBarStruct* bar,
		WtUInt32 count, bool isLast)
	{
		g_bars.calls++;
		g_bars.count = count;
		g_bars.last = isLast;
		g_bars.code = code ? code : "";
		g_bars.period = period ? period : "";
		if (bar != NULL && count > 0)
			g_bars.last_close = bar[count - 1].close;
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
			/ (std::string("r134_btporter_") + tag + "_" + std::to_string((long)::getpid()));
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
			section = "\"hft\":{\"name\":\"r134_hft_bars\"}";
		else
			section = "\"" + std::string(mode) + "\":{\"name\":\"r134_" +
				std::string(mode) + "_bars\",\"slippage\":0}";

		return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
			"\"stime\":202401030000,\"etime\":202401040000,\"tick\":false,"
			"\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\"}},"
			"\"env\":{\"mocker\":\"" + std::string(mode) + "\"},"
			+ section + "}";
	}

	struct BtFixture
	{
		std::string dir;
		bool cleaned = false;

		bool setup(const char* tag, const char* mode, bool documentedFourteenDigitRange = false)
		{
			dir = prepare_dir(tag);
			if (dir.empty())
				return false;
			write_text(dir + "/sessions.json", kSessions);
			write_text(dir + "/commodities.json", kCommodities);
			write_text(dir + "/contracts.json", kContracts);

			clear_bars();
			reset_singleton();
			getRunner().init((dir + "/r134_absent_logcfg.yaml").c_str(), true, dir.c_str());

			register_ext_data_loader(fnl_loader, raw_loader, NULL, NULL, false);
			getRunner().config(make_cfg(dir, mode).c_str(), false);

			if (strcmp(mode, "cta") == 0 && getRunner().cta_mocker() == nullptr)
				return false;
			if (strcmp(mode, "sel") == 0 && getRunner().sel_mocker() == nullptr)
				return false;
			if (strcmp(mode, "hft") == 0 && getRunner().hft_mocker() == nullptr)
				return false;
			if (documentedFourteenDigitRange)
				getRunner().set_time_range(20240103000000ULL, 20240104000000ULL);

			/* 只做同步初始化：确定 _cur_tdate，不进入 run()/行情循环。 */
			return getRunner().replayer().prepare();
		}

		~BtFixture() { teardown(); }

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

	void expect_two_bar_callback()
	{
		EXPECT_GE(g_loader_calls, 1U);
		EXPECT_EQ(1U, g_bars.calls);
		EXPECT_EQ(2U, g_bars.count);
		EXPECT_TRUE(g_bars.last);
		EXPECT_EQ(std::string(kCode), g_bars.code);
		EXPECT_EQ(std::string("d"), g_bars.period);
		EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);
	}
}

/*
 * 生产: WtBtPorter.cpp:438-457（cta_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回；回放器侧见 HisDataReplayer.cpp:3589-3738）
 */
TEST(R134BtPorterBars, CtaGetBarsReturnsLoadedBlocks)
{
	BtFixture fx;
	ASSERT_TRUE(fx.setup("cta_bars", "cta"));

	const WtUInt32 cnt = cta_get_bars(1, kCode, "d", 5, false, bars_cb);
	EXPECT_EQ(2U, cnt);
	expect_two_bar_callback();

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1164-1183（sel_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回）
 */
TEST(R134BtPorterBars, SelGetBarsReturnsLoadedBlocks)
{
	BtFixture fx;
	ASSERT_TRUE(fx.setup("sel_bars", "sel"));

	const WtUInt32 cnt = sel_get_bars(1, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	expect_two_bar_callback();

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1587-1606（hft_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回）
 */
TEST(R134BtPorterBars, HftGetBarsReturnsLoadedBlocks)
{
	BtFixture fx;
	ASSERT_TRUE(fx.setup("hft_bars", "hft"));

	const WtUInt32 cnt = hft_get_bars(1, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	expect_two_bar_callback();

	fx.teardown();
}

/*
 * BUG-P04-011 回归：公开 WtBtRunner::set_time_range 文档约定 YYYYMMDDHHMMSS。
 * 同一 Jan 3 至 Jan 4 区间应能读取已喂入的日线；当前实现按 12 位拆分时会
 * 将 14 位输入解析成错误日期，导致 lower_bound 找不到 cutoff 前的 bar。
 */
TEST(R134BtPorterBars, DocumentedFourteenDigitRangeReturnsLoadedBars)
{
	BtFixture fx;
	ASSERT_TRUE(fx.setup("cta_bars_14digit_contract", "cta", true));

	const WtUInt32 cnt = cta_get_bars(1, kCode, "d", 5, false, bars_cb);
	EXPECT_EQ(2U, cnt) << "BUG-P04-011: documented YYYYMMDDHHMMSS range should include bars";
	expect_two_bar_callback();

	fx.teardown();
}
