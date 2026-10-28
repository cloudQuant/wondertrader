/*
 * r134_porter_bars_test.cpp  (lane R134 · wave21)
 *
 * 靶子（实盘侧 C 导出层的 K 线切片回调支，既有用例只走过 NULL ctx / 空数据）：
 *   - WtPorter.cpp:508-532   cta_get_bars  的非空 WTSKlineSlice 分块回调
 *   - WtPorter.cpp:1302-1322 sel_get_bars  的非空 WTSKlineSlice 分块回调
 *   - WtPorter.cpp:1805-1825 hft_get_bars  的非空 WTSKlineSlice 分块回调
 *
 * 数据来源：走产品公开的「外部数据加载器 + 数据存储读取器」链路——
 * register_ext_data_loader 注册 fnl loader，config 的 data.store 指向本机构建产物
 * libWtDataStorage；WtDataReader 先调 IHisDataLoader::loadFinalHisBars，loader 内再
 * 调公开的 feed_raw_bars，把两根合成日线喂回读取器的 BarsList。
 *
 * 边界：不开线程、不跑行情/回放、不连柜台；所有文件只在系统临时目录，
 * 用例末尾 remove_all；每个用例独立 reset 进程单例，不依赖执行顺序。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include <unistd.h>

#include "WtCore/WtHelper.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/WTSStruct.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

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
	WTSBarStruct g_feed[2];

	void clear_bars()
	{
		g_bars.clear();
		memset(g_feed, 0, sizeof(g_feed));
		g_feed[0].date = 20240102;
		g_feed[0].time = 1500;
		g_feed[0].open = 4500.0;
		g_feed[0].high = 4510.0;
		g_feed[0].low = 4490.0;
		g_feed[0].close = 4505.0;
		g_feed[0].vol = 10;
		g_feed[1] = g_feed[0];
		g_feed[1].date = 20240103;
		g_feed[1].close = 4520.0;
	}

	/* FuncLoadFnlBars：把两根合成日线经公开 C API 喂回去。 */
	bool fnl_loader(const char* /*stdCode*/, const char* /*period*/)
	{
		feed_raw_bars(g_feed, 2);
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

	std::string prepare_dir(const char* tag)
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ (std::string("r134_porter_") + tag + "_" + std::to_string((long)::getpid()));
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

	void reset_singleton()
	{
		WtRtRunner& r = getRunner();
		r.~WtRtRunner();
		new (&r) WtRtRunner();
	}

	/* 测试可执行文件在 bin/TestUnits，libWtDataStorage 在 bin 下。 */
	void point_inst_dir_to_bin()
	{
		stdfs::path inst(WtHelper::getInstDir());
		stdfs::path bin = inst.parent_path();
		std::string dir = bin.string();
		if (dir.empty() || dir.back() != '/')
			dir += '/';
		WtHelper::setInstDir(dir.c_str());
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":0,\"to\":2359}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

	std::string make_cfg(const std::string& dir, const char* env)
	{
		return "{\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\"},"
			"\"env\":{\"name\":\"" + std::string(env) + "\"},"
			"\"data\":{\"store\":{\"module\":\"WtDataStorage\",\"path\":\"" + dir + "\"}},"
			"\"bspolicy\":\"" + dir + "/actions.json\"}";
	}

	struct LiveFixture
	{
		std::string dir;
		bool cleaned = false;
		std::string inst_before;

		bool setup(const char* tag, const char* env)
		{
			dir = prepare_dir(tag);
			if (dir.empty())
				return false;
			write_text(dir + "/sessions.json", kSessions);
			write_text(dir + "/commodities.json", kCommodities);
			write_text(dir + "/contracts.json", kContracts);
			write_text(dir + "/actions.json", "{}");

			clear_bars();
			reset_singleton();
			getRunner().init((dir + "/r134_absent_logcfg.yaml").c_str(), true, dir.c_str());
			inst_before = WtHelper::getInstDir();
			point_inst_dir_to_bin();

			register_ext_data_loader(fnl_loader, raw_loader, NULL, NULL);
			if (!getRunner().config(make_cfg(dir, env).c_str(), false))
				return false;
			return getRunner().getEngine() != nullptr;
		}

		~LiveFixture() { teardown(); }

		void teardown()
		{
			if (cleaned)
				return;
			cleaned = true;
			register_ext_data_loader(NULL, NULL, NULL, NULL);
			reset_singleton();
			WtHelper::setInstDir(inst_before.c_str());
			std::error_code ec;
			stdfs::remove_all(dir, ec);
		}
	};
}

/*
 * 生产: WtPorter.cpp:508-532（cta_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回；loader 链路见 WtRtRunner.cpp:203-226）
 */
TEST(R134PorterBars, CtaGetBarsReturnsLoadedBlocks)
{
	LiveFixture fx;
	ASSERT_TRUE(fx.setup("cta_bars", "cta"));

	const CtxHandler h = create_cta_context("r134_cta_bars", 0);
	ASSERT_NE(h, 0U);

	const WtUInt32 cnt = cta_get_bars(h, kCode, "d", 5, false, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_EQ(std::string("d"), g_bars.period);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);

	fx.teardown();
}

/*
 * 生产: WtPorter.cpp:1302-1322（sel_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回）
 */
TEST(R134PorterBars, SelGetBarsReturnsLoadedBlocks)
{
	LiveFixture fx;
	ASSERT_TRUE(fx.setup("sel_bars", "sel"));

	const CtxHandler h = create_sel_context("r134_sel_bars", 20240102, 93000, "d");
	ASSERT_NE(h, 0U);

	const WtUInt32 cnt = sel_get_bars(h, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);

	fx.teardown();
}

/*
 * 生产: WtPorter.cpp:1805-1825（hft_get_bars 命中 WTSKlineSlice 后的分块循环、
 *       release 与 reaCnt 返回）
 */
TEST(R134PorterBars, HftGetBarsReturnsLoadedBlocks)
{
	LiveFixture fx;
	ASSERT_TRUE(fx.setup("hft_bars", "hft"));

	const CtxHandler h = create_hft_context("r134_hft_bars", "R134_TRADER_ABSENT", false, 0);
	ASSERT_NE(h, 0U);

	const WtUInt32 cnt = hft_get_bars(h, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);

	fx.teardown();
}
