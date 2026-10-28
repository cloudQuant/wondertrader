/*
 * r134_btporter_pos_orders_test.cpp  (lane R134 · wave21)
 *
 * 靶子（回测侧 mocker 非空、且信号/委托表非空的公开面）：
 *   - WtBtPorter.cpp:589-603   cta_get_all_position 的逐项 lambda 与终止回调
 *   - WtBtPorter.cpp:1113-1127 sel_get_all_position 的逐项 lambda 与终止回调
 *   - WtBtPorter.cpp:1868-1887 hft_cancel_all 的非空 OrderIDs 拼串 + 尾逗号裁剪
 *   - WtBtPorter.cpp:1900-1917 hft_buy 的非空 OrderIDs
 *   - WtBtPorter.cpp:1931-1948 hft_sell 的非空 OrderIDs
 *
 * 前置：replayer.basefiles 装载 sessions/commodities/contracts，合约代码使用
 * 三段式 SHFE.rb.2510（既有 R113/R124 用两段式 SHFE.rb2510，commodity 解析不到，
 * 信号/委托表一直为空，见 bugs/R134.md BUG-R134-003）。
 *
 * 边界：同步、无行情回放、无线程；mocker 由 config 根据 env.mocker 建立；
 * 临时目录用完清理；每个用例 reset 单例。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	const char* kCode = "SHFE.rb.2510";

	struct PosEntry
	{
		std::string code;
		double		qty = 0.0;
		bool		last = false;
	};

	std::vector<PosEntry> g_pos;

	void pos_cb(CtxHandler, const char* code, double qty, bool isLast)
	{
		PosEntry e;
		e.code = code ? code : "";
		e.qty = qty;
		e.last = isLast;
		g_pos.push_back(e);
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
			section = "\"hft\":{\"name\":\"r134_hft_ord\"}";
		else
			section = "\"" + std::string(mode) + "\":{\"name\":\"r134_" +
				std::string(mode) + "_pos\",\"slippage\":0}";

		return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
			"\"stime\":20240103000000,\"etime\":20240104000000,\"tick\":false,"
			"\"basefiles\":{"
			"\"session\":\"" + dir + "/sessions.json\","
			"\"commodity\":\"" + dir + "/commodities.json\","
			"\"contract\":\"" + dir + "/contracts.json\"}},"
			"\"env\":{\"mocker\":\"" + std::string(mode) + "\"},"
			+ section + "}";
	}

	struct OrderFixture
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

			g_pos.clear();
			reset_singleton();
			getRunner().init((dir + "/r134_absent_logcfg.yaml").c_str(), true, dir.c_str());
			getRunner().config(make_cfg(dir, mode).c_str(), false);

			if (strcmp(mode, "cta") == 0)
				return getRunner().cta_mocker() != nullptr;
			if (strcmp(mode, "sel") == 0)
				return getRunner().sel_mocker() != nullptr;
			return getRunner().hft_mocker() != nullptr;
		}

		~OrderFixture() { teardown(); }

		void teardown()
		{
			if (cleaned)
				return;
			cleaned = true;
			reset_singleton();
			std::error_code ec;
			stdfs::remove_all(dir, ec);
		}
	};
}

/*
 * 生产: WtBtPorter.cpp:589-603（cta_get_all_position 的 lambda 回调 + 终止符）
 */
TEST(R134BtPorterPositions, CtaSetPositionEnumeratesOneSignal)
{
	OrderFixture fx;
	ASSERT_TRUE(fx.setup("cta_pos", "cta"));

	cta_set_position(1, kCode, 2.0, "r134", 0.0, 0.0);
	EXPECT_DOUBLE_EQ(2.0, cta_get_position(1, kCode, false, ""));

	g_pos.clear();
	cta_get_all_position(1, pos_cb);

	ASSERT_EQ(2U, g_pos.size());
	EXPECT_EQ(std::string(kCode), g_pos[0].code);
	EXPECT_DOUBLE_EQ(2.0, g_pos[0].qty);
	EXPECT_FALSE(g_pos[0].last);
	EXPECT_EQ(std::string(""), g_pos[1].code);
	EXPECT_DOUBLE_EQ(0.0, g_pos[1].qty);
	EXPECT_TRUE(g_pos[1].last);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1113-1127（sel_get_all_position 的 lambda 回调 + 终止符）
 */
TEST(R134BtPorterPositions, SelSetPositionEnumeratesOneSignal)
{
	OrderFixture fx;
	ASSERT_TRUE(fx.setup("sel_pos", "sel"));

	sel_set_position(1, kCode, 3.0, "r134");
	EXPECT_DOUBLE_EQ(3.0, sel_get_position(1, kCode, false, ""));

	g_pos.clear();
	sel_get_all_position(1, pos_cb);

	ASSERT_EQ(2U, g_pos.size());
	EXPECT_EQ(std::string(kCode), g_pos[0].code);
	EXPECT_DOUBLE_EQ(3.0, g_pos[0].qty);
	EXPECT_FALSE(g_pos[0].last);
	EXPECT_EQ(std::string(""), g_pos[1].code);
	EXPECT_TRUE(g_pos[1].last);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1868-1887 / 1900-1917 / 1931-1948
 *       hft_buy / hft_sell 的非空 OrderIDs，以及 hft_cancel_all 的拼串与裁剪
 */
TEST(R134BtPorterOrders, HftOrderIdsAreJoinedAndTrimmed)
{
	OrderFixture fx;
	ASSERT_TRUE(fx.setup("hft_ord", "hft"));

	std::string buy = hft_buy(1, kCode, 4500.0, 2.0, "r134", 0);
	ASSERT_FALSE(buy.empty());
	const unsigned long buyId = std::strtoul(buy.c_str(), NULL, 10);
	ASSERT_GT(buyId, 0UL);
	EXPECT_EQ(std::string::npos, buy.find(','));

	std::string sell = hft_sell(1, kCode, 4500.0, 1.0, "r134", 0);
	ASSERT_FALSE(sell.empty());
	const unsigned long sellId = std::strtoul(sell.c_str(), NULL, 10);
	ASSERT_GT(sellId, 0UL);
	EXPECT_NE(buyId, sellId);
	EXPECT_EQ(std::string::npos, sell.find(','));

	std::string canceled = hft_cancel_all(1, kCode, true);
	EXPECT_EQ(std::to_string(buyId), canceled);
	EXPECT_EQ(std::string::npos, canceled.find(','));

	fx.teardown();
}
