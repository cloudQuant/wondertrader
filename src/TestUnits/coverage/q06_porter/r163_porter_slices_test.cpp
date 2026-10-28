/*!
 * \file r163_porter_slices_test.cpp
 * \brief 第二十四波 R163 - WtPorter C 导出层切片/异常/执行器分支
 *
 * 覆盖（只读生产代码）:
 *  - WtPorter.cpp:544-568   cta_get_ticks 命中切片后回调/release，以及 stra_get_ticks 抛异常
 *  - WtPorter.cpp:501-532   cta_get_bars 的 catch(...) 早退
 *  - WtPorter.cpp:1354-1381 sel_get_ticks 非空/空切片两条回调路径 + catch
 *  - WtPorter.cpp:1296-1322 sel_get_bars 的 catch(...)
 *  - WtPorter.cpp:1839-1866 hft_get_ticks 非空/空切片回调 + catch
 *  - WtPorter.cpp:1797-1825 hft_get_bars 的 catch(...)
 *  - WtPorter.cpp:1881-1983 hft_get_ordque/orddtl/trans 命中切片后的回调/release
 *  - WtRtRunner.cpp:954-1033 initExecuters 的 local/diff/arbi 命中已配置 trader 的分支
 *
 * 接缝说明:
 *  - 这里没有改产品代码，也没有 #define private public。ExpCtaContext / ExpSelContext /
 *    ExpHftContext 的 stra_get_* 在基类里就是 virtual（CtaStraBaseCtx.h:486/494、
 *    SelStraBaseCtx.h:436/445、HftStraBaseCtx.h:247/256/265/274/283），
 *    测试侧只继承公开类并 override，再用公开的 Engine::addContext 注入。
 *    切片由公开工厂 WTSTickSlice::create / WTSOrdQueSlice::create 等构造，
 *    回调是同步的，片内指针只在一次调用内有效。
 *  - 不启动行情/交易引擎线程，不连接任何柜台；临时文件只写系统临时目录并清理。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include <boost/noncopyable.hpp>

#include "WtCore/ActionPolicyMgr.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtHelper.h"
#include "WtCore/WtSelEngine.h"
#include "WtPorter/ExpCtaContext.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/ExpSelContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSTypes.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

namespace stdfs = std::filesystem;

struct SliceCalls
{
	uint32_t	calls = 0;
	uint32_t	count = 0;
	bool		last = false;
	bool		null_ptr = false;
	double		price = 0.0;

	void clear() { *this = SliceCalls(); }
};

SliceCalls g_bars;
SliceCalls g_ticks;
SliceCalls g_ordque;
SliceCalls g_orddtl;
SliceCalls g_trans;

void reset_calls()
{
	g_bars.clear();
	g_ticks.clear();
	g_ordque.clear();
	g_orddtl.clear();
	g_trans.clear();
}

void bars_cb(CtxHandler, const char*, const char*, WTSBarStruct* bar, WtUInt32 count, bool last)
{
	g_bars.calls++;
	g_bars.count = count;
	g_bars.last = last;
	g_bars.null_ptr = (bar == nullptr);
	if (bar != nullptr && count > 0)
		g_bars.price = bar[count - 1].close;
}

void ticks_cb(CtxHandler, const char*, WTSTickStruct* tick, WtUInt32 count, bool last)
{
	g_ticks.calls++;
	g_ticks.count = count;
	g_ticks.last = last;
	g_ticks.null_ptr = (tick == nullptr);
	if (tick != nullptr && count > 0)
		g_ticks.price = tick[count - 1].price;
}

void ordque_cb(CtxHandler, const char*, WTSOrdQueStruct* item, WtUInt32 count, bool last)
{
	g_ordque.calls++;
	g_ordque.count = count;
	g_ordque.last = last;
	g_ordque.null_ptr = (item == nullptr);
}

void orddtl_cb(CtxHandler, const char*, WTSOrdDtlStruct* item, WtUInt32 count, bool last)
{
	g_orddtl.calls++;
	g_orddtl.count = count;
	g_orddtl.last = last;
	g_orddtl.null_ptr = (item == nullptr);
}

void trans_cb(CtxHandler, const char*, WTSTransStruct* item, WtUInt32 count, bool last)
{
	g_trans.calls++;
	g_trans.count = count;
	g_trans.last = last;
	g_trans.null_ptr = (item == nullptr);
}

/* CTA：THROW 触发导出层 catch；count==0 返回空切片；否则返回 2 条 tick。 */
class R163CtaCtx : public ExpCtaContext
{
public:
	explicit R163CtaCtx(WtCtaEngine* eng) : ExpCtaContext(eng, "r163_cta_slice", 0) {}

	WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override
	{
		if (std::strcmp(stdCode, "THROW") == 0)
			throw std::runtime_error("r163 cta tick");
		(void)count;
		fillTicks();
		return WTSTickSlice::create(stdCode, _ticks, 2);
	}

	WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period,
		uint32_t count, bool isMain = false) override
	{
		(void)stdCode; (void)period; (void)count; (void)isMain;
		throw std::runtime_error("r163 cta bars");
	}

private:
	void fillTicks()
	{
		memset(_ticks, 0, sizeof(_ticks));
		for (int i = 0; i < 2; i++)
		{
			_ticks[i].action_date = 20240102;
			_ticks[i].action_time = 93000000 + i * 1000;
			_ticks[i].price = 4500.0 + i;
		}
	}

	WTSTickStruct _ticks[2];
};

class R163SelCtx : public ExpSelContext
{
public:
	explicit R163SelCtx(WtSelEngine* eng) : ExpSelContext(eng, "r163_sel_slice", 0) {}

	WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override
	{
		if (std::strcmp(stdCode, "THROW") == 0)
			throw std::runtime_error("r163 sel tick");
		(void)count;
		fillTicks();
		return WTSTickSlice::create(stdCode, _ticks, 2);
	}

	WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period,
		uint32_t count) override
	{
		(void)stdCode; (void)period; (void)count;
		throw std::runtime_error("r163 sel bars");
	}

private:
	void fillTicks()
	{
		memset(_ticks, 0, sizeof(_ticks));
		for (int i = 0; i < 2; i++)
		{
			_ticks[i].action_date = 20240102;
			_ticks[i].action_time = 93000000 + i * 1000;
			_ticks[i].price = 4600.0 + i;
		}
	}

	WTSTickStruct _ticks[2];
};

class R163HftCtx : public ExpHftContext
{
public:
	explicit R163HftCtx(WtHftEngine* eng)
		: ExpHftContext(eng, "r163_hft_slice", false, 0) {}

	WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override
	{
		if (std::strcmp(stdCode, "THROW") == 0)
			throw std::runtime_error("r163 hft tick");
		(void)count;
		fillTicks();
		return WTSTickSlice::create(stdCode, _ticks, 2);
	}

	WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period,
		uint32_t count) override
	{
		(void)stdCode; (void)period; (void)count;
		throw std::runtime_error("r163 hft bars");
	}

	WTSOrdQueSlice* stra_get_order_queue(const char* stdCode, uint32_t count) override
	{
		(void)count;
		memset(_oqs, 0, sizeof(_oqs));
		return WTSOrdQueSlice::create(stdCode, _oqs, 2);
	}

	WTSOrdDtlSlice* stra_get_order_detail(const char* stdCode, uint32_t count) override
	{
		(void)count;
		memset(_odts, 0, sizeof(_odts));
		return WTSOrdDtlSlice::create(stdCode, _odts, 2);
	}

	WTSTransSlice* stra_get_transaction(const char* stdCode, uint32_t count) override
	{
		(void)count;
		memset(_trans, 0, sizeof(_trans));
		return WTSTransSlice::create(stdCode, _trans, 2);
	}

private:
	void fillTicks()
	{
		memset(_ticks, 0, sizeof(_ticks));
		for (int i = 0; i < 2; i++)
		{
			_ticks[i].action_date = 20240102;
			_ticks[i].action_time = 93000000 + i * 1000;
			_ticks[i].price = 4700.0 + i;
		}
	}

	WTSTickStruct _ticks[2];
	WTSOrdQueStruct _oqs[2];
	WTSOrdDtlStruct _odts[2];
	WTSTransStruct _trans[2];
};

std::string prepare_dir(const char* tag)
{
	stdfs::path p = stdfs::temp_directory_path()
		/ (std::string("r163_porter_") + tag + "_" + std::to_string((long)::getpid()));
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

WtRtRunner* new_runner(const std::string& dir)
{
	WtRtRunner* r = new WtRtRunner();
	r->init((dir + "/r163_absent_logcfg.yaml").c_str(), true, dir.c_str());
	return r;
}

void reset_singleton()
{
	WtRtRunner& r = getRunner();
	r.~WtRtRunner();
	new (&r) WtRtRunner();
}

const char* kSessions =
	"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
	"\"sections\":[{\"from\":0,\"to\":2359}]}}";

const char* kCommodities =
	"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
	"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

const char* kContracts =
	"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

const char* kActions =
	"{\"default\":{\"order\":[{\"action\":\"open\"},{\"action\":\"close\"}]}}";

std::string cfg_plain(const std::string& dir, const char* env, const std::string& extra)
{
	return "{\"basefiles\":{},\"env\":{\"name\":\"" + std::string(env) + "\"},"
		"\"data\":{},\"bspolicy\":\"" + dir + "/actions.json\"" + extra + "}";
}

std::string cfg_with_contracts(const std::string& dir, const char* env, const std::string& extra)
{
	return "{\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"},"
		"\"env\":{\"name\":\"" + std::string(env) + "\"},"
		"\"data\":{},\"bspolicy\":\"" + dir + "/actions.json\"" + extra + "}";
}

} // namespace

// WtPorter.cpp:544-568 + :501-532
TEST(R163PorterCtaSlices, TickSliceCallbackAndBarsThrowAreGuarded)
{
	const std::string dir = prepare_dir("cta_slice");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	reset_singleton();
	getRunner().init((dir + "/r163_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_plain(dir, "cta", "").c_str(), false));

	WtCtaEngine* eng = dynamic_cast<WtCtaEngine*>(getRunner().getEngine());
	ASSERT_NE(nullptr, eng);
	CtaContextPtr ctx(new R163CtaCtx(eng));
	eng->addContext(ctx);
	const CtxHandler h = ctx->id();
	ASSERT_NE(0U, h);

	reset_calls();
	EXPECT_EQ(2U, cta_get_ticks(h, "SHFE.rb.2510", 2, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(2U, g_ticks.count);
	EXPECT_FALSE(g_ticks.null_ptr);
	EXPECT_TRUE(g_ticks.last);
	EXPECT_DOUBLE_EQ(4501.0, g_ticks.price);

	// 非空切片 + tickCnt=0：仍是非空分支，回调 count=0
	reset_calls();
	EXPECT_EQ(0U, cta_get_ticks(h, "SHFE.rb.2510", 0, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);

	// stra_get_ticks / stra_get_bars 抛异常都必须在导出层被吞掉
	reset_calls();
	EXPECT_EQ(0U, cta_get_ticks(h, "THROW", 5, ticks_cb));
	EXPECT_EQ(0U, g_ticks.calls);
	EXPECT_EQ(0U, cta_get_bars(h, "THROW", "d", 5, false, bars_cb));
	EXPECT_EQ(0U, g_bars.calls);

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

// WtPorter.cpp:1354-1381 + :1296-1322
TEST(R163PorterSelSlices, TickSliceEmptyBranchAndBarsThrowAreGuarded)
{
	const std::string dir = prepare_dir("sel_slice");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	reset_singleton();
	getRunner().init((dir + "/r163_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_plain(dir, "sel", "").c_str(), false));

	WtSelEngine* eng = dynamic_cast<WtSelEngine*>(getRunner().getEngine());
	ASSERT_NE(nullptr, eng);
	SelContextPtr ctx(new R163SelCtx(eng));
	eng->addContext(ctx, 20240102, 930, TPT_Daily);
	const CtxHandler h = ctx->id();
	ASSERT_NE(0U, h);

	reset_calls();
	EXPECT_EQ(2U, sel_get_ticks(h, "SHFE.rb.2510", 2, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(2U, g_ticks.count);
	EXPECT_DOUBLE_EQ(4601.0, g_ticks.price);

	// 1363-1368: 非空切片但 tickCnt=0 -> 走 cb(NULL, 0, true)
	reset_calls();
	EXPECT_EQ(0U, sel_get_ticks(h, "SHFE.rb.2510", 0, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);
	EXPECT_TRUE(g_ticks.null_ptr);

	reset_calls();
	EXPECT_EQ(0U, sel_get_ticks(h, "THROW", 5, ticks_cb));
	EXPECT_EQ(0U, g_ticks.calls);
	EXPECT_EQ(0U, sel_get_bars(h, "THROW", "d", 5, bars_cb));
	EXPECT_EQ(0U, g_bars.calls);

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

// WtPorter.cpp:1839-1866 + :1797-1825 + :1881-1983
TEST(R163PorterHftSlices, TickOrderQueueDetailAndTransSlicesAreConsumed)
{
	const std::string dir = prepare_dir("hft_slice");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);

	reset_singleton();
	getRunner().init((dir + "/r163_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_plain(dir, "hft", "").c_str(), false));

	WtHftEngine* eng = dynamic_cast<WtHftEngine*>(getRunner().getEngine());
	ASSERT_NE(nullptr, eng);
	HftContextPtr ctx(new R163HftCtx(eng));
	eng->addContext(ctx);
	const CtxHandler h = ctx->id();
	ASSERT_NE(0U, h);

	reset_calls();
	EXPECT_EQ(2U, hft_get_ticks(h, "SHFE.rb.2510", 2, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(2U, g_ticks.count);
	EXPECT_DOUBLE_EQ(4701.0, g_ticks.price);

	// 1849-1853: 非空切片 + tickCnt=0 -> cb(NULL, 0, true)
	reset_calls();
	EXPECT_EQ(0U, hft_get_ticks(h, "SHFE.rb.2510", 0, ticks_cb));
	EXPECT_EQ(1U, g_ticks.calls);
	EXPECT_EQ(0U, g_ticks.count);
	EXPECT_TRUE(g_ticks.null_ptr);

	reset_calls();
	EXPECT_EQ(2U, hft_get_ordque(h, "SHFE.rb.2510", 2, ordque_cb));
	EXPECT_EQ(1U, g_ordque.calls);
	EXPECT_EQ(2U, g_ordque.count);

	reset_calls();
	EXPECT_EQ(2U, hft_get_orddtl(h, "SHFE.rb.2510", 2, orddtl_cb));
	EXPECT_EQ(1U, g_orddtl.calls);
	EXPECT_EQ(2U, g_orddtl.count);

	reset_calls();
	EXPECT_EQ(2U, hft_get_trans(h, "SHFE.rb.2510", 2, trans_cb));
	EXPECT_EQ(1U, g_trans.calls);
	EXPECT_EQ(2U, g_trans.count);

	reset_calls();
	EXPECT_EQ(0U, hft_get_ticks(h, "THROW", 5, ticks_cb));
	EXPECT_EQ(0U, hft_get_bars(h, "THROW", "d", 5, bars_cb));
	EXPECT_EQ(0U, g_bars.calls);

	reset_singleton();
	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

// WtRtRunner.cpp:954-1033
TEST(R163PorterExecuters, LocalDiffArbiBindConfiguredTraderAdapters)
{
	const std::string dir = prepare_dir("executers");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	// trader 没有 module，TraderAdapter::init 返回 false，但 initTraders 仍把它加入
	// _traders；随后三种执行器的 `if (trader)` 成功分支都能命中。
	const std::string cfg = cfg_plain(dir, "cta",
		",\"traders\":[{\"active\":true,\"id\":\"r163_trd\"}],"
		"\"executers\":["
		"{\"active\":true,\"id\":\"r163_exec_local\",\"name\":\"local\",\"trader\":\"r163_trd\"},"
		"{\"active\":true,\"id\":\"r163_exec_diff\",\"name\":\"diff\",\"trader\":\"r163_trd\"},"
		"{\"active\":true,\"id\":\"r163_exec_arbi\",\"name\":\"arbi\",\"trader\":\"r163_trd\"}"
		"]");

	EXPECT_TRUE(r->config(cfg.c_str(), false));
	EXPECT_NE(nullptr, r->getEngine());

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

// WtPorter.cpp:2132-2138 + :2168-2174 + :2204-2210
namespace {

/* 只实现导出层需要的三件事：init 成功、发委托号、下单/撤单返回 0。
 * 不连接任何柜台，也不创建线程。 */
class R163FakeTraderApi : public ITraderApi
{
public:
	bool init(WTSVariant*) override { return true; }
	void registerSpi(ITraderSpi*) override {}
	void connect() override {}
	bool isConnected() override { return true; }
	bool makeEntrustID(char* buffer, int length) override
	{
		if (buffer == nullptr || length <= 0)
			return false;
		std::snprintf(buffer, (std::size_t)length, "r163.%u", ++_seq);
		return true;
	}
	int login(const char*, const char*, const char*) override { return 0; }
	int logout() override { return 0; }
	int orderInsert(WTSEntrust*) override { return 0; }
	int orderAction(WTSEntrustAction*) override { return 0; }
	int queryAccount() override { return 0; }
	int queryPositions() override { return 0; }
	int queryOrders() override { return 0; }
	int queryTrades() override { return 0; }
	void release() override {}

private:
	unsigned int _seq = 0;
};

} // namespace

TEST(R163PorterHftOrders, BuySellAndCancelAllBuildNonEmptyLocalIdLists)
{
	const std::string dir = prepare_dir("hft_orders");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	reset_singleton();
	getRunner().init((dir + "/r163_absent_logcfg.yaml").c_str(), true, dir.c_str());
	WtHelper::setGenerateDir(dir.c_str());
	ASSERT_TRUE(getRunner().config(cfg_with_contracts(dir, "hft", "").c_str(), false));

	WtHftEngine* eng = dynamic_cast<WtHftEngine*>(getRunner().getEngine());
	ASSERT_NE(nullptr, eng);
	const CtxHandler h = create_hft_context("r163_hft_order", "R163_TRADER_ABSENT", false, 0);
	ASSERT_NE(0U, h);
	HftContextPtr base = getRunner().getHftContext(h);
	ASSERT_TRUE(base != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(base.get());
	ASSERT_NE(nullptr, ctx);

	IBaseDataMgr* bdMgr = getRunner().getEngine()->get_basedata_mgr();
	ASSERT_NE(nullptr, bdMgr);
	WTSContractInfo* ct = bdMgr->getContract("rb2510", "SHFE");
	ASSERT_NE(nullptr, ct);

	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init((dir + "/actions.json").c_str()));

	R163FakeTraderApi api;
	{
		TraderAdapter adapter(nullptr);
		ASSERT_TRUE(adapter.initExt("r163_tr", &api, bdMgr, &policy));
		// run() 创建 _stat_map；不 run 时 buy 会在 L693 解引用空表
		ASSERT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);
		ctx->setTrader(&adapter);

		const char* code = "SHFE.rb.2510";

		const std::string buy = hft_buy(h, code, 4500.0, 2.0, "r163", 0);
		ASSERT_FALSE(buy.empty());
		const uint32_t buyId = (uint32_t)std::strtoul(buy.c_str(), nullptr, 10);
		ASSERT_NE(0U, buyId);

		// 给 adapter 回灌多头可用持仓，sell 的平仓规则才有量
		WTSArray* ayp = WTSArray::create();
		WTSPositionItem* pos = WTSPositionItem::create("rb2510", "CNY", "SHFE");
		ASSERT_NE(nullptr, pos);
		pos->setContractInfo(ct);
		pos->setDirection(WDT_LONG);
		pos->setNewPosition(2.0);
		pos->setAvailNewPos(2.0);
		ayp->append(pos);
		pos->release();
		adapter.onRspPosition(ayp);
		ayp->release();

		const std::string sell = hft_sell(h, code, 4500.0, 2.0, "r163", 0);
		ASSERT_FALSE(sell.empty());
		const uint32_t sellId = (uint32_t)std::strtoul(sell.c_str(), nullptr, 10);
		ASSERT_NE(0U, sellId);
		EXPECT_NE(buyId, sellId);

		// 回灌买入委托，使 cancel_all 能枚举到该 localid
		WTSOrderInfo* oi = WTSOrderInfo::create();
		ASSERT_NE(nullptr, oi);
		oi->setContractInfo(ct);
		oi->setDirection(WDT_LONG);
		oi->setOffsetType(WOT_OPEN);
		oi->setPrice(4500.0);
		oi->setVolume(2.0);
		oi->setVolLeft(2.0);
		oi->setVolTraded(0.0);
		oi->setOrderID("r163_oid_1");
		char tag[64] = { 0 };
		std::snprintf(tag, sizeof(tag), "otp.r163_tr.%u", buyId);
		oi->setUserTag(tag);
		oi->setOrderState(WOS_NotTraded_Queuing);
		adapter.onPushOrder(oi);
		oi->release();

		const std::string canceled = hft_cancel_all(h, code, true);
		EXPECT_EQ(std::to_string(buyId), canceled);

		ctx->setTrader(nullptr);
	}

	reset_singleton();

	const std::string baseDir = WtHelper::getBaseDir();
	std::error_code ec;
	stdfs::remove_all(stdfs::path(baseDir) / "traders" / "r163_tr", ec);
	stdfs::remove_all(dir, ec);
}
