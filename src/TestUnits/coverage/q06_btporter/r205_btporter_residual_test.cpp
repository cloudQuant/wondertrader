/*
 * r205_btporter_residual_test.cpp  (wave28 · lane R205)
 *
 * 靶子：
 *  - WtBtPorter.cpp 三个 get_bars 的 catch(...) 支：456-457 / 1182-1183 / 1605-1606
 *  - WtBtPorter.cpp hft_step 的异步空 mocker 支：1996-1998
 *  - WtBtPorter.cpp cta_step 的异步空 mocker 支：901
 *  - WtBtPorter.cpp hft_step 的异步非空 mocker 支：2000
 *  - WtBtRunner.cpp 异步 worker 的 catch(...) 支：538
 *  - WtBtRunner.cpp hft 分支 enable_hook：521
 *  - WtBtRunner.cpp loadFinalHisBars/loadRawHisBars 的不支持周期 default：117 / 142
 *  - WtBtPorter.cpp hft_get_ordque/orddtl/trans 的非空切片与无数据支：
 *    1665-1670/1678-1679、1700-1705/1713-1714、1735-1740/1748-1749
 *
 * 边界：不加载策略插件（module 为空，load_library 失败即返回）、不接柜台、
 * 不写共享内存；临时目录只落系统临时目录并在 fixture 内清理。异步回放只
 * 跑空数据（无主 K 线、无订阅），worker 在 prepare/run 后立即结束，stop 负责 join。
 * 三个 get_bars 的 catch 用例用真实 loader 灌 12 根 m5，确保切片非空后由
 * 回调抛异常进入 C API 的 catch，只用返回值证明异常被吞掉。
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

#include "WtBtCore/WtHelper.h"
#include "Share/DLLHelper.hpp"

#include "Includes/WTSStruct.h"

/* WtDtHelper.h 与 PorterDefs.h 都定义了同名但签名不同的 FuncGetBarsCallback，
 * 不能同时 include。这里只声明本用例要用的三个导出函数（签名取自
 * WtDtHelper/WtDtHelper.h:253-277，WtString == const char*）。 */
typedef void(*R205HelperLog)(const char* message);
extern "C"
{
	bool store_order_queues(const char* tickFile, WTSOrdQueStruct* firstItem, int count, R205HelperLog cbLogger);
	bool store_order_details(const char* tickFile, WTSOrdDtlStruct* firstItem, int count, R205HelperLog cbLogger);
	bool store_transactions(const char* tickFile, WTSTransStruct* firstItem, int count, R205HelperLog cbLogger);
}

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

namespace stdfs = std::filesystem;

const char* const kCode = "SSE.STK.000001";

/* ------------------------------------------------------------------ 单例/目录 */

void reset_singleton()
{
	WtBtRunner& r = getRunner();
	r.~WtBtRunner();
	new (&r) WtBtRunner();
}

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r205_btporter_") + tag + "_" + std::to_string((long)::getpid()));
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

stdfs::path repo_root()
{
	// __FILE__ = <repo>/src/TestUnits/coverage/q06_btporter/r205_btporter_residual_test.cpp
	return stdfs::path(__FILE__).parent_path().parent_path().parent_path()
		.parent_path().parent_path();
}

/* r173 的最小基础数据：只覆盖一个交易日、一个品种/合约，mode=none */
const char* kSessions =
	"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
	"\"sections\":[{\"from\":0,\"to\":2359}]}}";

const char* kCommodities =
	"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
	"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

const char* kContracts =
	"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

void write_min_basefiles(const std::string& dir)
{
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);
}

/* HisDataMgr 按 WtHelper::getInstDir() + libWtDataStorage.dylib 装载模块。
 * 测试可执行体在 bin/TestUnits 下，模块在上一级 bin 下；沿用 r91 的做法，
 * 在构建目录候选里找到模块后把 inst_dir 钉过去。 */
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

/* ------------------------------------------------------------------ 合成 m5 */

WTSBarStruct g_feed[12];

void fill_feed()
{
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

/* FuncLoadFnlBars/FuncLoadRawBars：只认 m5，把 12 根合成 K 线经公开接口回灌 */
bool bars_loader(const char* /*stdCode*/, const char* period)
{
	if (strcmp(period, "m5") != 0)
		return false;

	feed_raw_bars(g_feed, 12);
	return true;
}

/* ------------------------------------------------------------------ 抛异常回调 */

struct BarsBoom {};
struct EvtBoom {};

uint32_t g_throw_calls = 0;

void throwing_bars_cb(CtxHandler, const char*, const char*, WTSBarStruct*,
	WtUInt32, bool)
{
	g_throw_calls++;
	throw BarsBoom();
}

/* prepare() 会先同步发出 EVENT_ENGINE_INIT；若在首个事件就抛出，异常落在
 * run_backtest 的调用栈上而不是 worker 的 catch(...) 里。这里只在
 * EVENT_BACKTEST_END 抛出，确保异常来自 worker 内的回测结束回调。 */
void throwing_evt_cb(WtUInt32 evtId, WtUInt32, WtUInt32)
{
	if (evtId == EVENT_BACKTEST_END)
		throw EvtBoom();
}

uint32_t g_ordque_calls = 0;
uint32_t g_orddtl_calls = 0;
uint32_t g_trans_calls = 0;
uint32_t g_ordque_cnt = 0;
uint32_t g_orddtl_cnt = 0;
uint32_t g_trans_cnt = 0;

void ordque_cb(CtxHandler, const char*, WTSOrdQueStruct*, WtUInt32 count, bool)
{
	g_ordque_calls++;
	g_ordque_cnt = count;
}

void orddtl_cb(CtxHandler, const char*, WTSOrdDtlStruct*, WtUInt32 count, bool)
{
	g_orddtl_calls++;
	g_orddtl_cnt = count;
}

void trans_cb(CtxHandler, const char*, WTSTransStruct*, WtUInt32 count, bool)
{
	g_trans_calls++;
	g_trans_cnt = count;
}

/* ------------------------------------------------------------------ 三种 get_bars 夹具 */

class R205BarsCatchFixture : public ::testing::Test
{
protected:
	std::string	dir_;
	bool		cleaned_ = false;

	void TearDown() override
	{
		if (cleaned_)
			return;
		cleaned_ = true;
		register_ext_data_loader(NULL, NULL, NULL, NULL, false);
		reset_singleton();
		if (!dir_.empty())
		{
			std::error_code ec;
			stdfs::remove_all(dir_, ec);
		}
	}

	bool configure(const char* mode, const char* tag)
	{
		fill_feed();
		dir_ = prepare_dir(tag);
		if (dir_.empty())
			return false;

		reset_singleton();
		getRunner().init((dir_ + "/r205_absent_logcfg.yaml").c_str(), true, dir_.c_str());

		register_ext_data_loader(bars_loader, bars_loader, NULL, NULL, false);

		const std::string root = repo_root().string();
		const std::string cfg =
			"{\"replayer\":{\"mode\":\"csv\",\"path\":\"" + dir_ + "\","
			"\"stime\":202001020930,\"etime\":202001021500,\"tick\":false,"
			"\"basefiles\":{"
			"\"session\":\"" + root + "/dist/common/sessions.json\","
			"\"commodity\":[\"" + root + "/dist/common/commodities.json\","
			"\"" + root + "/dist/common/stk_comms.json\"],"
			"\"holiday\":\"" + root + "/dist/common/holidays.json\"}},"
			"\"env\":{\"mocker\":\"" + std::string(mode) + "\"},"
			"\"" + std::string(mode) + "\":{\"name\":\"r205_" + std::string(tag) +
			"\",\"slippage\":0}}";
		getRunner().config(cfg.c_str(), false);

		if (strcmp(mode, "cta") == 0 && getRunner().cta_mocker() == nullptr)
			return false;
		if (strcmp(mode, "sel") == 0 && getRunner().sel_mocker() == nullptr)
			return false;
		if (strcmp(mode, "hft") == 0 && getRunner().hft_mocker() == nullptr)
			return false;

		return getRunner().replayer().prepare();
	}
};

} // namespace

/* 生产: WtBtPorter.cpp:438-457, 回调在 try 内抛出，C API 必须 catch(...) 并返回 0 */
TEST_F(R205BarsCatchFixture, CtaGetBarsCallbackThrowIsCaught)
{
	ASSERT_TRUE(configure("cta", "cta_catch"));

	g_throw_calls = 0;
	EXPECT_EQ(0U, cta_get_bars(1, kCode, "m5", 12, false, throwing_bars_cb));
	EXPECT_EQ(1U, g_throw_calls) << "切片必须非空才会回调，catch 才可达";
}

/* 生产: WtBtPorter.cpp:1164-1183 */
TEST_F(R205BarsCatchFixture, SelGetBarsCallbackThrowIsCaught)
{
	ASSERT_TRUE(configure("sel", "sel_catch"));

	g_throw_calls = 0;
	EXPECT_EQ(0U, sel_get_bars(1, kCode, "m5", 12, throwing_bars_cb));
	EXPECT_EQ(1U, g_throw_calls) << "切片必须非空才会回调，catch 才可达";
}

/* 生产: WtBtPorter.cpp:1587-1606 */
TEST_F(R205BarsCatchFixture, HftGetBarsCallbackThrowIsCaught)
{
	ASSERT_TRUE(configure("hft", "hft_catch"));

	g_throw_calls = 0;
	EXPECT_EQ(0U, hft_get_bars(1, kCode, "m5", 12, throwing_bars_cb));
	EXPECT_EQ(1U, g_throw_calls) << "切片必须非空才会回调，catch 才可达";
}

/* 生产: WtBtPorter.cpp:1665-1670/1678-1679、1700-1705/1713-1714、
 * 1735-1740/1748-1749。
 * 用 WtDtHelper 的公开导出把符合 his/queue|orders|trans/[exchg]/[date]/[code].dsb
 * 格式的三类 Level2 块写到临时目录，replayer 以 mode=bin 从同一目录读取；
 * 非空切片走分派/释放/返回，缺失合约走 NULL 的 return 0 支。 */
TEST(R205BtPorterLevel2, HftLevel2GettersServeStoredBlocksAndRejectMissing)
{
	const std::string dir = prepare_dir("l2");
	ASSERT_FALSE(dir.empty());

	struct Guard
	{
		std::string	dir;
		std::string	old_inst;
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
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());
	ASSERT_TRUE(prepare_inst_dir()) << "找不到 libWtDataStorage，mode=bin 会空指针";

	const std::string root = repo_root().string();
	/* Level2 块的时间是 09:00；回放起始时间取 09:00，让 lower_bound 正好落在
	 * 首条记录上（_cursor=1），get_order_queue_slice 返回 1 条。取 09:30 时
	 * lower_bound 返回 end，_cursor=size+1，realCnt 会多数出一条不存在的数据。 */
	const std::string cfg =
		"{\"replayer\":{\"mode\":\"bin\",\"path\":\"" + dir + "\","
		"\"stime\":202001020900,\"etime\":202001021500,\"tick\":true,"
		"\"basefiles\":{"
		"\"session\":\"" + root + "/dist/common/sessions.json\","
		"\"commodity\":[\"" + root + "/dist/common/commodities.json\","
		"\"" + root + "/dist/common/stk_comms.json\"],"
		"\"holiday\":\"" + root + "/dist/common/holidays.json\"}},"
		"\"env\":{\"mocker\":\"hft\"},"
		"\"hft\":{\"name\":\"r205_l2\",\"module\":\"\"}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().hft_mocker());
	ASSERT_TRUE(getRunner().replayer().prepare());

	g_ordque_calls = g_orddtl_calls = g_trans_calls = 0;
	g_ordque_cnt = g_orddtl_cnt = g_trans_cnt = 0;

	EXPECT_EQ(1U, hft_get_ordque(1, "SSE.600000", 5, ordque_cb));
	EXPECT_EQ(1U, g_ordque_calls);
	EXPECT_EQ(1U, g_ordque_cnt);

	EXPECT_EQ(1U, hft_get_orddtl(1, "SSE.600000", 5, orddtl_cb));
	EXPECT_EQ(1U, g_orddtl_calls);
	EXPECT_EQ(1U, g_orddtl_cnt);

	EXPECT_EQ(1U, hft_get_trans(1, "SSE.600000", 5, trans_cb));
	EXPECT_EQ(1U, g_trans_calls);
	EXPECT_EQ(1U, g_trans_cnt);

	/* 无对应 dsb：checkOrderQueues/Details/Transactions 返回 false -> NULL 支 */
	EXPECT_EQ(0U, hft_get_ordque(1, "SSE.600001", 5, ordque_cb));
	EXPECT_EQ(0U, hft_get_orddtl(1, "SSE.600001", 5, orddtl_cb));
	EXPECT_EQ(0U, hft_get_trans(1, "SSE.600001", 5, trans_cb));
	EXPECT_EQ(1U, g_ordque_calls);
	EXPECT_EQ(1U, g_orddtl_calls);
	EXPECT_EQ(1U, g_trans_calls);
}

TEST(R205BtPorterLevel2, HftOrderQueueQueryAfterLastStoredRecordDoesNotOvercount)
{
	const std::string dir = prepare_dir("l2_after_last_record");
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
	WTSOrdQueStruct q{};
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

	reset_singleton();
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());
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
		"\"hft\":{\"name\":\"r205_l2_after_last\",\"module\":\"\"}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().hft_mocker());
	ASSERT_TRUE(getRunner().replayer().prepare());

	g_ordque_calls = 0;
	g_ordque_cnt = 0;
	const WtUInt32 returned = hft_get_ordque(1, "SSE.600000", 5, ordque_cb);
	// At 09:30 the only stored block (09:00) is still eligible and must be returned once.
	// ordque_cb records only the count and never dereferences the callback pointer,
	// so this regression assertion cannot read past the single stored item.
	EXPECT_EQ(1U, returned);
	EXPECT_EQ(1U, g_ordque_calls);
	EXPECT_EQ(1U, g_ordque_cnt);
}

/* 产品缺陷 BUG-R205-003：HisDataReplayer::checkOrderQueues 失败时把空记录写进
 * `_trans_cache`（checkOrderDetails 同样），污染逐笔成交缓存。启用本用例即可看到
 * 一次 ordque 缺失查询之后，随后写入的 trans 文件被 `_count == 0` 的脏缓存挡住。
 * 当前实现返回 0，故保持 DISABLED_，不改产品代码换取通过。 */
TEST(R205BtPorterLevel2, DISABLED_QueueMissPoisonsTransCacheAndHidesLaterTransFile)
{
	const std::string dir = prepare_dir("l2_poison");
	ASSERT_FALSE(dir.empty());

	struct Guard
	{
		std::string	dir;
		std::string	old_inst;
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

	reset_singleton();
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());
	ASSERT_TRUE(prepare_inst_dir());

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
		"\"hft\":{\"name\":\"r205_l2_poison\",\"module\":\"\"}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().hft_mocker());
	ASSERT_TRUE(getRunner().replayer().prepare());

	/* 没有 queue 文件：失败支把空记录写进 _trans_cache */
	EXPECT_EQ(0U, hft_get_ordque(1, "SSE.600000", 5, ordque_cb));

	/* 之后再落一份合法的 trans 文件；若缓存未被污染应按文件读出 1 条 */
	std::error_code ec;
	stdfs::create_directories(dir + "/his/trans/SSE/20200102", ec);
	WTSTransStruct t;
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

	EXPECT_EQ(1U, hft_get_trans(1, "SSE.600000", 5, trans_cb));
}

/* 生产: WtBtRunner.cpp:530-538, 空回放在 worker 内触发 backtest-end，
 * 事件回调抛出后必须被 worker 的 catch(...) 吞掉，主线程 stop 仍能 join。 */
TEST(R205BtRunnerAsync, BacktestEndThrowIsCaughtInsideWorker)
{
	const std::string dir = prepare_dir("cta_evt_throw");
	ASSERT_FALSE(dir.empty());
	write_min_basefiles(dir);

	reset_singleton();
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{"
		"\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"tick\":false,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"cta\"},"
		"\"cta\":{\"name\":\"r205_evt_throw\",\"slippage\":0}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().cta_mocker());

	register_evt_callback(&throwing_evt_cb);
	run_backtest(false, true);
	EXPECT_TRUE(getRunner().isAsync());
	stop_backtest();

	/* 事件回调清空后再 stop 一次：worker 已结束时只走 join/reset 早退 */
	register_evt_callback(NULL);
	EXPECT_NO_THROW(stop_backtest());

	/* cta 作为 listener 保证空回放安全，同时 hft 为 NULL：hft_step 走 1996-1998 */
	EXPECT_NO_THROW(hft_step(1));

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/* 生产: WtBtRunner.cpp:518-521 的 hft 分支、WtBtPorter.cpp:901 与 2000。
 * HFT 模式的空回放让 _async 保持 true 且 hft_mocker 非空；step_tick 在未装
 * hook 时立即返回，可安全触发。 */
TEST(R205BtRunnerAsync, HftMockerAsyncHookAndStepTick)
{
	const std::string dir = prepare_dir("hft_async");
	ASSERT_FALSE(dir.empty());
	write_min_basefiles(dir);

	reset_singleton();
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"replayer\":{"
		"\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,"
		"\"tick\":false,"
		"\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"hft\"},"
		"\"hft\":{\"name\":\"r205_hft_async\",\"module\":\"\"}}";
	getRunner().config(cfg.c_str(), false);
	ASSERT_NE(nullptr, getRunner().hft_mocker());

	run_backtest(false, true);
	EXPECT_TRUE(getRunner().isAsync());
	stop_backtest();

	/* cta_mocker 为空：异步下 cta_step 走 901 */
	EXPECT_FALSE(cta_step(1));
	/* hft_mocker 非空、未装 hook：step_tick 立即返回（2000/2001） */
	EXPECT_NO_THROW(hft_step(1));

	EXPECT_NO_THROW(stop_backtest());
	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/* ------------------------------------------------------------------ loader 周期 */

namespace {

bool fnl_ok(const char* /*stdCode*/, const char* /*period*/) { return true; }
bool raw_ok(const char* /*stdCode*/, const char* /*period*/) { return true; }
bool adj_ok(const char* /*stdCode*/) { return true; }
bool tick_ok(const char* /*stdCode*/, uint32_t /*uDate*/) { return true; }

void read_bars_noop(void*, WTSBarStruct*, uint32_t) {}
void read_fcts_noop(void*, const char*, uint32_t*, double*, uint32_t) {}
void read_ticks_noop(void*, WTSTickStruct*, uint32_t) {}

} // namespace

/* 生产: WtBtRunner.cpp:96-144 的 default 支（117/142）。
 * 栈对象不碰进程单例；已注册 ext loader 才会走到周期 switch。 */
TEST(R205BtRunnerLoader, UnsupportedPeriodsFallThroughToDefault)
{
	WtBtRunner runner;
	runner.registerExtDataLoader(&fnl_ok, &raw_ok, &adj_ok, &tick_ok, false);
	EXPECT_FALSE(runner.isAutoTrans());

	EXPECT_FALSE(runner.loadFinalHisBars(NULL, "SHFE.rb2510", KP_Hour, &read_bars_noop));
	EXPECT_FALSE(runner.loadRawHisBars(NULL, "SHFE.rb2510", KP_Half, &read_bars_noop));

	/* 受支持周期仍转发给已注册 loader，证明上面的 false 来自 default 而不是空 loader */
	EXPECT_TRUE(runner.loadFinalHisBars(NULL, "SHFE.rb2510", KP_Minute5, &read_bars_noop));
	EXPECT_TRUE(runner.loadRawHisBars(NULL, "SHFE.rb2510", KP_DAY, &read_bars_noop));

	EXPECT_TRUE(runner.loadAllAdjFactors(NULL, &read_fcts_noop));
	EXPECT_TRUE(runner.loadAdjFactors(NULL, "SHFE.rb2510", &read_fcts_noop));
	EXPECT_TRUE(runner.loadRawHisTicks(NULL, "SHFE.rb2510", 20240102, &read_ticks_noop));
}

/* 产品缺陷 BUG-R205-001：WtBtRunner::config 的 `_inited` 只读不写（与
 * WtBtPorter::config_backtest 的 static `inited` 同样），文档承诺的“只初始化
 * 一次”守卫永远为 false。启用本用例即可看到第二次 config 仍会新建 hft_mocker。
 * 当前实现下 EXPECT_EQ(hft_mocker(), nullptr) 会失败，故保持 DISABLED_，
 * 不修改生产代码换取通过。 */
TEST(R205BtRunnerConfig, DISABLED_SecondConfigIsNotIgnoredBecauseInitedIsNeverSet)
{
	const std::string dir = prepare_dir("cfg_guard");
	ASSERT_FALSE(dir.empty());
	write_min_basefiles(dir);

	reset_singleton();
	getRunner().init((dir + "/r205_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string sel = "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,\"tick\":false,"
		"\"basefiles\":{\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"sel\"},\"sel\":{\"name\":\"r205_sel_cfg\",\"slippage\":0}}";
	getRunner().config(sel.c_str(), false);
	ASSERT_NE(nullptr, getRunner().sel_mocker());

	const std::string hft = "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir + "\","
		"\"stime\":20240102000000,\"etime\":20240103000000,\"tick\":false,"
		"\"basefiles\":{\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"}},"
		"\"env\":{\"mocker\":\"hft\"},\"hft\":{\"name\":\"r205_hft_cfg\",\"module\":\"\"}}";
	getRunner().config(hft.c_str(), false);

	/* 若 _inited 守卫生效：第二次 config 应整体早退，sel 保留且 hft 仍为空 */
	EXPECT_NE(nullptr, getRunner().sel_mocker());
	EXPECT_EQ(nullptr, getRunner().hft_mocker());

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}
