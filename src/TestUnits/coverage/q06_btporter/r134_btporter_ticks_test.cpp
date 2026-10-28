/*
 * r134_btporter_ticks_test.cpp  (lane R134 · wave21)
 *
 * 靶子（回测侧 Tick 切片的非空回调支）：
 *   - WtBtPorter.cpp:476-496   cta_get_ticks  的非空 WTSTickSlice 分支
 *   - WtBtPorter.cpp:1220-1239 sel_get_ticks  的非空 WTSTickSlice 分支
 *   - WtBtPorter.cpp:1625-1644 hft_get_ticks  的非空 WTSTickSlice 分支
 *
 * 数据来源：公开 register_ext_data_loader 注册 tick loader（fnl loader 同时给一个
 * 返回 false 的桩，使回放器把 this 作为 IBtDataLoader）；hepler->prepare() 只做
 * reset + handle_init + 当前日期初始化，不进入 run()；随后 get_tick_slice 懒加载
 * 时调用 loadRawHisTicks，loader 内用公开 feed_raw_ticks 喂两根合成 tick。
 *
 * 边界：prepare 为同步初始化，无 worker；无行情循环；临时目录用完清理。
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

	struct TicksRec
	{
		uint32_t	calls = 0;
		uint32_t	count = 0;
		double		price = 0.0;
		bool		last = false;
		std::string	code;

		void clear() { *this = TicksRec(); }
	};

	TicksRec g_ticks_rec;
	WTSTickStruct g_ticks[2];

	void clear_ticks()
	{
		g_ticks_rec.clear();
		memset(g_ticks, 0, sizeof(g_ticks));
		for (int i = 0; i < 2; i++)
		{
			wt_strcpy(g_ticks[i].code, "rb2510");
			wt_strcpy(g_ticks[i].exchg, "SHFE");
			g_ticks[i].action_date = 20240102;
			g_ticks[i].action_time = 93000000 + i * 1000;
			g_ticks[i].price = 4501.0 + i;
		}
	}

	/* fnl 桩：只让回放器拿到非空 _bt_loader，不提供 K 线。 */
	bool no_bars(const char* /*stdCode*/, const char* /*period*/)
	{
		return false;
	}

	/* FuncLoadRawTicks：两根合成 tick 经公开 C API 喂回读取器。 */
	bool tick_loader(const char* /*stdCode*/, uint32_t /*uDate*/)
	{
		feed_raw_ticks(g_ticks, 2);
		return true;
	}

	void ticks_cb(CtxHandler, const char* code, WTSTickStruct* tick, WtUInt32 count, bool isLast)
	{
		g_ticks_rec.calls++;
		g_ticks_rec.count = count;
		g_ticks_rec.last = isLast;
		g_ticks_rec.code = code ? code : "";
		if (tick != NULL)
			g_ticks_rec.price = tick->price;
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
			section = "\"hft\":{\"name\":\"r134_hft_ticks\"}";
		else
			section = "\"" + std::string(mode) + "\":{\"name\":\"r134_" +
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
			getRunner().init((dir + "/r134_absent_logcfg.yaml").c_str(), true, dir.c_str());

			register_ext_data_loader(no_bars, NULL, NULL, tick_loader, false);
			getRunner().config(make_cfg(dir, mode).c_str(), false);

			if (strcmp(mode, "cta") == 0 && getRunner().cta_mocker() == NULL)
				return false;
			if (strcmp(mode, "sel") == 0 && getRunner().sel_mocker() == NULL)
				return false;
			if (strcmp(mode, "hft") == 0 && getRunner().hft_mocker() == NULL)
				return false;

			return getRunner().replayer().prepare();
		}

		~TickFixture() { teardown(); }

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
}

/*
 * 生产: WtBtPorter.cpp:476-496（cta_get_ticks 命中 WTSTickSlice 后的 thisCnt!=0
 *       回调、release 与返回）
 */
TEST(R134BtPorterTicks, CtaGetTicksReturnsLoadedSlice)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("cta_ticks", "cta"));

	const WtUInt32 cnt = cta_get_ticks(1, kCode, 5, ticks_cb);
	EXPECT_EQ(1U, cnt);
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_TRUE(g_ticks_rec.last);
	EXPECT_EQ(std::string(kCode), g_ticks_rec.code);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1220-1239（sel_get_ticks 命中 WTSTickSlice 后的 thisCnt!=0
 *       回调、release 与返回）
 */
TEST(R134BtPorterTicks, SelGetTicksReturnsLoadedSlice)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("sel_ticks", "sel"));

	const WtUInt32 cnt = sel_get_ticks(1, kCode, 5, ticks_cb);
	EXPECT_EQ(1U, cnt);
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_TRUE(g_ticks_rec.last);
	EXPECT_EQ(std::string(kCode), g_ticks_rec.code);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1625-1644（hft_get_ticks 命中 WTSTickSlice 后的 thisCnt!=0
 *       回调、release 与返回）
 */
TEST(R134BtPorterTicks, HftGetTicksReturnsLoadedSlice)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("hft_ticks", "hft"));

	const WtUInt32 cnt = hft_get_ticks(1, kCode, 5, ticks_cb);
	EXPECT_EQ(1U, cnt);
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_TRUE(g_ticks_rec.last);
	EXPECT_EQ(std::string(kCode), g_ticks_rec.code);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	fx.teardown();
}
