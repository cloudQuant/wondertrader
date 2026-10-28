/*
 * r173_btporter_ticks_test.cpp  (lane R173 · wave25)
 *
 * 靶子：WtBtPorter.cpp 三个 tick 导出函数的 catch(...) 兜底
 *   - WtBtPorter.cpp:495-496   cta_get_ticks
 *   - WtBtPorter.cpp:1238-1239 sel_get_ticks
 *   - WtBtPorter.cpp:1643-1644 hft_get_ticks
 *
 * 做法：沿用“ext tick loader + prepare() 懒加载”的同步夹具，先确认正常回调
 * 能拿到 1 根合成 tick，再传一个抛 std::runtime_error 的回调。导出层必须
 * 捕获异常并返回 0，而不是把异常穿过 C 边界或崩溃；随后再次正常回调仍可读。
 *
 * 边界：prepare() 只做同步初始化，不调用 run()；无 worker、无行情循环；
 * 临时文件只写系统临时目录并在 fixture 析构时清理；每个用例 reset 进程单例。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
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

/* 用户回调抛异常：导出层 catch(...) 必须兜住并返回 0。 */
void throw_ticks_cb(CtxHandler, const char*, WTSTickStruct*, WtUInt32, bool)
{
	throw std::runtime_error("r173 tick callback failure");
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
		/ (std::string("r173_btporter_") + tag + "_" + std::to_string((long)::getpid()));
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
		section = "\"hft\":{\"name\":\"r173_hft_throws\"}";
	else
		section = "\"" + std::string(mode) + "\":{\"name\":\"r173_" +
			std::string(mode) + "_throws\",\"slippage\":0}";

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
		getRunner().init((dir + "/r173_absent_logcfg.yaml").c_str(), true, dir.c_str());

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

}	// namespace

/*
 * 生产: WtBtPorter.cpp:495-496
 */
TEST(R173BtPorterTicks, CtaThrowingTickCallbackIsCaught)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("cta_throw", "cta"));

	EXPECT_EQ(1U, cta_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	EXPECT_EQ(0U, cta_get_ticks(1, kCode, 5, throw_ticks_cb));

	// 异常被导出层吃掉，缓存切片与游标仍可用
	EXPECT_EQ(1U, cta_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(2U, g_ticks_rec.calls);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1238-1239
 */
TEST(R173BtPorterTicks, SelThrowingTickCallbackIsCaught)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("sel_throw", "sel"));

	EXPECT_EQ(1U, sel_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	EXPECT_EQ(0U, sel_get_ticks(1, kCode, 5, throw_ticks_cb));

	EXPECT_EQ(1U, sel_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(2U, g_ticks_rec.calls);

	fx.teardown();
}

/*
 * 生产: WtBtPorter.cpp:1643-1644
 */
TEST(R173BtPorterTicks, HftThrowingTickCallbackIsCaught)
{
	TickFixture fx;
	ASSERT_TRUE(fx.setup("hft_throw", "hft"));

	EXPECT_EQ(1U, hft_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(1U, g_ticks_rec.calls);
	EXPECT_EQ(1U, g_ticks_rec.count);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks_rec.price);

	EXPECT_EQ(0U, hft_get_ticks(1, kCode, 5, throw_ticks_cb));

	EXPECT_EQ(1U, hft_get_ticks(1, kCode, 5, ticks_cb));
	EXPECT_EQ(2U, g_ticks_rec.calls);

	fx.teardown();
}
