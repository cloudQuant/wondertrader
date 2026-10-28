/*
 * r124_btporter_selpos_test.cpp  (lane R124 · wave20)
 *
 * 靶子：WtBtPorter.cpp:1113-1127
 *   sel_get_all_position 在 sel_mocker 非空时的逐项回调（L1122-1124 lambda）
 *   以及固定终止符。
 *
 * 既有 R83 只覆盖 sel 空表/空 mocker；R113 只覆盖 cta_get_all_position 的信号枚举。
 * 本用例按 R113 的底座配置装载 sessions/commodities/contracts，再通过公开
 * sel_set_position 写入一条信号，使 enum_position 真正进入回调体。
 *
 * 边界：同步、无行情回放、无线程；只写系统临时目录并清理。
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

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	struct EnumRec
	{
		int		calls = 0;
		bool	last = false;
		double	qty = 0.0;
		std::string code;

		void clear() { *this = EnumRec(); }
	};

	EnumRec g_enum;

	void enum_cb(CtxHandler, const char* code, double qty, bool isLast)
	{
		g_enum.calls++;
		g_enum.last = isLast;
		g_enum.qty = qty;
		g_enum.code = code ? code : "";
	}

	void reset_singleton()
	{
		WtBtRunner& r = getRunner();
		r.~WtBtRunner();
		new (&r) WtBtRunner();
	}

	std::string prepare_dir()
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ ("r124_btporter_selpos_" + std::to_string((long)::getpid()));
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
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";
}

/*
 * 生产: WtBtPorter.cpp:1113-1127（sel_get_all_position 的逐项回调 + 终止符）
 */
TEST(R124BtPorterSelPositions, SelSetPositionEnumeratesOneSignal)
{
	const std::string dir = prepare_dir();
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	reset_singleton();
	getRunner().init((dir + "/r124_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"none\"}}";
	getRunner().config(cfg.c_str(), false);

	const CtxHandler h = init_sel_mocker("r124_sel_pos", 20240102, 93000, "d");
	ASSERT_NE(h, 0U);
	ASSERT_NE(getRunner().sel_mocker(), nullptr);

	sel_set_position(h, "SHFE.rb2510", 3.0, "r124");

	g_enum.clear();
	sel_get_all_position(h, enum_cb);

	EXPECT_EQ(2, g_enum.calls);
	EXPECT_TRUE(g_enum.last);
	EXPECT_EQ(std::string("SHFE.rb2510"), g_enum.code);
	EXPECT_DOUBLE_EQ(3.0, g_enum.qty);

	reset_singleton();

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}
