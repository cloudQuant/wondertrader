/*
 * r214_btporter_l2_catch_test.cpp  (lane R214 · wave29)
 *
 * 靶子：WtBtPorter 剩余可执行行
 *   - WtBtPorter.cpp hft_get_ordque/orddtl/trans 的 catch(...) 支（1678-1679、
 *     1713-1714、1748-1749）
 *   - WtBtPorter.cpp hft_step 的「异步模式 + 无 mocker」支（1996-1998）
 *
 * 与既有 lane 的分工：R205 用同一套 Level2 落盘夹具覆盖了三个入口的
 * 「非空切片 / 缺失合约」支，但回调不抛异常，catch 支仍为 0；本文件只把
 * 回调换成抛异常版本。R173 覆盖了异步空回放，但只对 cta_step 断言；本文件
 * 在同一个异步态下调用 hft_step，走 hft_mocker==NULL 的早退。
 *
 * 边界：不加载策略插件（module 为空）、不接柜台、不写共享内存；Level2 块用
 * WtDtHelper 公开导出写系统临时目录，mode=bin 只读该目录；用例末尾 reset
 * 单例并清理。异步回放只跑空数据，stop 负责 join。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "WtBtCore/WtHelper.h"
#include "Share/DLLHelper.hpp"

#include "Includes/WTSStruct.h"

/* WtDtHelper.h 与 PorterDefs.h 的 FuncGetBarsCallback 同名不同签名，不能同时
 * include；这里只声明三个落盘导出（签名取自 WtDtHelper/WtDtHelper.h）。 */
typedef void(*R214HelperLog)(const char* message);
extern "C"
{
	bool store_order_queues(const char* tickFile, WTSOrdQueStruct* firstItem, int count, R214HelperLog cbLogger);
	bool store_order_details(const char* tickFile, WTSOrdDtlStruct* firstItem, int count, R214HelperLog cbLogger);
	bool store_transactions(const char* tickFile, WTSTransStruct* firstItem, int count, R214HelperLog cbLogger);
}

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
		/ (std::string("r214_btporter_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(p, ec);
	stdfs::create_directories(p, ec);
	return p.string();
}

void write_text(const std::string& path, const std::string& body)
{
	std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
	ofs << body;
}

stdfs::path repo_root()
{
	// __FILE__ = <repo>/src/TestUnits/coverage/q06_btporter/r214_btporter_l2_catch_test.cpp
	return stdfs::path(__FILE__).parent_path().parent_path().parent_path()
		.parent_path().parent_path();
}

bool prepare_inst_dir()
{
	const std::string module = DLLHelper::wrap_module("WtDataStorage");
	const char* const rels[] = {
		"build-cov/build_x64/Debug/bin",
		"build-cov/build_x64/Release/bin",
		"build_x64/Debug/bin",
		"build_x64/Release/bin",
	};
	for (const char* rel : rels)
	{
		stdfs::path dir = repo_root() / rel;
		std::error_code ec;
		if (stdfs::exists(dir / module, ec))
		{
			WtHelper::setInstDir((dir.string() + "/").c_str());
			return true;
		}
	}
	return false;
}

struct L2Boom {};

uint32_t g_ordque_throws = 0;
uint32_t g_orddtl_throws = 0;
uint32_t g_trans_throws = 0;

void throwing_ordque_cb(CtxHandler, const char*, WTSOrdQueStruct*, WtUInt32, bool)
{
	g_ordque_throws++;
	throw L2Boom();
}

void throwing_orddtl_cb(CtxHandler, const char*, WTSOrdDtlStruct*, WtUInt32, bool)
{
	g_orddtl_throws++;
	throw L2Boom();
}

void throwing_trans_cb(CtxHandler, const char*, WTSTransStruct*, WtUInt32, bool)
{
	g_trans_throws++;
	throw L2Boom();
}

const char* kSessions =
	"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
	"\"sections\":[{\"from\":0,\"to\":2359}]}}";

const char* kCommodities =
	"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
	"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

const char* kContracts =
	"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

} // namespace

/* 生产: WtBtPorter.cpp:1665-1679 / 1700-1714 / 1735-1749 */
TEST(R214BtPorterLevel2, ThrowingLevel2CallbacksAreCaught)
{
	const std::string dir = prepare_dir("l2");
	ASSERT_FALSE(dir.empty());

	struct Guard
	{
		std::string dir;
		std::string old_inst;
		~Guard()
		{
			reset_singleton();
			if (!old_inst.empty())
				WtHelper::setInstDir(old_inst.c_str());
			if (!dir.empty())
			{
				std::error_code ec;
				stdfs::remove_all(dir, ec);
			}
		}
	} guard;
	guard.dir = dir;
	guard.old_inst = WtHelper::getInstDir();

	std::error_code ec;
	stdfs::create_directories(dir + "/his/queue/SSE/20200102", ec);
	stdfs::create_directories(dir + "/his/orders/SSE/20200102", ec);
	stdfs::create_directories(dir + "/his/trans/SSE/20200102", ec);

	WTSOrdQueStruct q;
	memset(&q, 0, sizeof(q));
	strcpy(q.exchg, "SSE");
	strcpy(q.code, "600000");
	q.trading_date = 20200102;
	q.action_date = 20200102;
	q.action_time = 90000000;
	q.side = (WTSBSDirectType)0;
	q.price = 10.5;
	q.order_items = 1;
	q.qsize = 2;
	q.volumes[0] = 100;
	q.volumes[1] = 200;
	ASSERT_TRUE(store_order_queues((dir + "/his/queue/SSE/20200102/600000.dsb").c_str(),
		&q, 1, NULL));

	WTSOrdDtlStruct d;
	memset(&d, 0, sizeof(d));
	strcpy(d.exchg, "SSE");
	strcpy(d.code, "600000");
	d.trading_date = 20200102;
	d.action_date = 20200102;
	d.action_time = 90000000;
	d.index = 1;
	d.price = 10.5;
	d.volume = 100;
	d.side = (WTSBSDirectType)0;
	d.otype = (WTSOrdDetailType)0;
	ASSERT_TRUE(store_order_details((dir + "/his/orders/SSE/20200102/600000.dsb").c_str(),
		&d, 1, NULL));

	WTSTransStruct t;
	memset(&t, 0, sizeof(t));
	strcpy(t.exchg, "SSE");
	strcpy(t.code, "600000");
	t.trading_date = 20200102;
	t.action_date = 20200102;
	t.action_time = 90000000;
	t.index = 1;
	t.ttype = (WTSTransType)0;
	t.side = (WTSBSDirectType)0;
	t.price = 10.5;
	t.volume = 100;
	ASSERT_TRUE(store_transactions((dir + "/his/trans/SSE/20200102/600000.dsb").c_str(),
		&t, 1, NULL));

	reset_singleton();
	getRunner().init((dir + "/r214_absent_logcfg.yaml").c_str(), true, dir.c_str());
	ASSERT_TRUE(prepare_inst_dir()) << "找不到 libWtDataStorage，mode=bin 会空指针";

	const std::string root = repo_root().string();
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"bin\",\"path\":\"" + dir + "\","
		"\"stime\":202001020930,\"etime\":202001021500,\"tick\":true,"
		"\"basefiles\":{"
		"\"session\":\"" + root + "/dist/common/sessions.json\","
		"\"commodity\":[\"" + root + "/dist/common/commodities.json\","
		"\"" + root + "/dist/common/stk_comms.json\"],"
		"\"holiday\":\"" + root + "/dist/common/holidays.json\"}},"
		"\"env\":{\"mocker\":\"hft\"},"
		"\"hft\":{\"name\":\"r214_l2\",\"module\":\"\"}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().hft_mocker());
	ASSERT_TRUE(getRunner().replayer().prepare());

	g_ordque_throws = g_orddtl_throws = g_trans_throws = 0;
	EXPECT_EQ(0U, hft_get_ordque(1, "SSE.600000", 5, throwing_ordque_cb));
	EXPECT_EQ(1U, g_ordque_throws);
	EXPECT_EQ(0U, hft_get_orddtl(1, "SSE.600000", 5, throwing_orddtl_cb));
	EXPECT_EQ(1U, g_orddtl_throws);
	EXPECT_EQ(0U, hft_get_trans(1, "SSE.600000", 5, throwing_trans_cb));
	EXPECT_EQ(1U, g_trans_throws);
}

/* 生产: WtBtPorter.cpp:1990-1998 */
TEST(R214BtPorterAsync, HftStepWithoutMockerReturnsInAsyncMode)
{
	const std::string dir = prepare_dir("async");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	struct Guard
	{
		std::string dir;
		~Guard()
		{
			reset_singleton();
			if (!dir.empty())
			{
				std::error_code ec;
				stdfs::remove_all(dir, ec);
			}
		}
	} guard;
	guard.dir = dir;

	reset_singleton();
	getRunner().init((dir + "/r214_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{"
		"\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"tick\":false,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"r214_async_empty\",\"slippage\":0}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().cta_mocker());
	ASSERT_EQ(nullptr, getRunner().hft_mocker());

	run_backtest(false, true);
	ASSERT_TRUE(getRunner().isAsync());
	EXPECT_NO_THROW(hft_step(1));
	stop_backtest();
}
