/*!
 * \file r143_porter_bars_test.cpp
 * \brief 第二十二波 R143 - WtPorter K 线回调成功臂（修正 R134 的 InstDir 路径）
 *
 * 覆盖（只读生产代码）:
 *  - WtPorter.cpp:508-532  cta_get_bars 命中 WTSKlineSlice 后的分块回调/release/reaCnt
 *  - WtPorter.cpp:1302-1322 sel_get_bars 同上
 *  - WtPorter.cpp:1805-1825 hft_get_bars 同上
 *
 * 数据链:
 *  - register_ext_data_loader 注册 fnl loader；config 的 data.store 指向本机构建产物
 *    libWtDataStorage（module=WtDataStorage）。
 *  - WtDataReader 读不到历史文件时回调 IHisDataLoader::loadFinalHisBars，loader 内用
 *    公开 C API feed_raw_bars 把两根合成日线写回读取器。
 *  - 关键修正：R134 的 point_inst_dir_to_bin 对带尾斜杠的 InstDir 只做一次
 *    parent_path()，实际仍停在 bin/TestUnits，导致 dlopen 找不到 libWtDataStorage。
 *    本用例直接用 dladdr 取可执行文件目录的父目录（bin/）。
 *
 * 边界:
 *  - 不开线程、不跑行情/回放、不连柜台；每次 reset 进程单例，用例间无状态残留。
 *  - 文件只写系统临时目录，teardown 先 reset 单例再 remove_all。
 *  - 不依赖执行顺序：本文件每个用例自己 init/config。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <dlfcn.h>
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

namespace {

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
	for (int i = 0; i < 2; i++)
	{
		g_feed[i].date = 20240102 + i;
		g_feed[i].time = 1500;
		g_feed[i].open = 4500.0;
		g_feed[i].high = 4510.0;
		g_feed[i].low = 4490.0;
		g_feed[i].close = 4505.0 + i * 15.0;
		g_feed[i].vol = 10;
	}
}

/* FuncLoadFnlBars：两根合成日线经公开 C API 喂回读取器。 */
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
	if (bar != nullptr && count > 0)
		g_bars.last_close = bar[count - 1].close;
}

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	stdfs::path p = stdfs::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

void reset_singleton()
{
	WtRtRunner& r = getRunner();
	r.~WtRtRunner();
	new (&r) WtRtRunner();
}

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r143_porter_") + tag + "_" + std::to_string((long)::getpid()));
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
	std::string inst_before;
	bool cleaned = false;

	bool setup(const char* tag, const char* env)
	{
		const std::string bin = binDir();
		if (bin.empty() || !stdfs::exists(bin + "libWtDataStorage.dylib"))
			return false;

		dir = prepare_dir(tag);
		if (dir.empty())
			return false;
		write_text(dir + "/sessions.json", kSessions);
		write_text(dir + "/commodities.json", kCommodities);
		write_text(dir + "/contracts.json", kContracts);
		write_text(dir + "/actions.json", "{}");

		clear_bars();
		reset_singleton();
		getRunner().init((dir + "/r143_absent_logcfg.yaml").c_str(), true, dir.c_str());
		inst_before = WtHelper::getInstDir();
		//init() 会把 InstDir 设成可执行文件目录；这里上跳一层到 bin/
		WtHelper::setInstDir(bin.c_str());

		register_ext_data_loader(fnl_loader, raw_loader, nullptr, nullptr);
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
		register_ext_data_loader(nullptr, nullptr, nullptr, nullptr);
		reset_singleton();
		WtHelper::setInstDir(inst_before.c_str());
		std::error_code ec;
		stdfs::remove_all(dir, ec);
	}
};

} // namespace

// WtPorter.cpp:508-532
TEST(R143PorterBars, cta_get_bars_returns_loaded_blocks)
{
	LiveFixture fx;
	if (!fx.setup("cta", "cta"))
		GTEST_SKIP() << "libWtDataStorage.dylib not found in this build";

	const CtxHandler h = create_cta_context("r143_cta_bars", 0);
	ASSERT_NE(0U, h);

	const WtUInt32 cnt = cta_get_bars(h, kCode, "d", 5, false, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_EQ(std::string("d"), g_bars.period);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);
}

// WtPorter.cpp:1302-1322
TEST(R143PorterBars, sel_get_bars_returns_loaded_blocks)
{
	LiveFixture fx;
	if (!fx.setup("sel", "sel"))
		GTEST_SKIP() << "libWtDataStorage.dylib not found in this build";

	const CtxHandler h = create_sel_context("r143_sel_bars", 20240102, 93000, "d");
	ASSERT_NE(0U, h);

	const WtUInt32 cnt = sel_get_bars(h, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);
}

// WtPorter.cpp:1805-1825
TEST(R143PorterBars, hft_get_bars_returns_loaded_blocks)
{
	LiveFixture fx;
	if (!fx.setup("hft", "hft"))
		GTEST_SKIP() << "libWtDataStorage.dylib not found in this build";

	const CtxHandler h = create_hft_context("r143_hft_bars", "R143_TRADER_ABSENT", false, 0);
	ASSERT_NE(0U, h);

	const WtUInt32 cnt = hft_get_bars(h, kCode, "d", 5, bars_cb);
	EXPECT_EQ(2U, cnt);
	EXPECT_EQ(1U, g_bars.calls);
	EXPECT_EQ(2U, g_bars.count);
	EXPECT_TRUE(g_bars.last);
	EXPECT_EQ(std::string(kCode), g_bars.code);
	EXPECT_DOUBLE_EQ(4520.0, g_bars.last_close);
}
