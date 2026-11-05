/*
 * Iteration 03 (BUG-Q04-002 / BUG-Q04-007): concurrency of the WtUftCore
 * per-order undone ledger (TraderAdapter) and of the UftStraContext local state
 * across trading-day switches. Built by CMakeLists.txt against the current
 * sources (and optionally the pre-change copies as red control), with
 * -fsanitize=thread or -fsanitize=address,undefined. Local fakes only: no
 * vendor API, no network, no real orders.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtHelper.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

class FakeBd : public IBaseDataMgr
{
public:
	FakeBd()
	{
		_comm = WTSCommodityInfo::create("ag", "ag", "SHFE", "NONE", "NONE");
		_comm->setVolScale(1);
		_comm->setCoverMode(CM_OpenCover);
		_comm->setTradingMode(TM_Both);
		_ag = WTSContractInfo::create("ag2406", "ag2406", "SHFE", "ag");
		_ag->setAltCode("ag2406");
		_ag->setCommInfo(_comm);
	}
	virtual ~FakeBd() { _ag->release(); _comm->release(); }
	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return _comm; }
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		return (strcmp(code, "ag2406") == 0 && strcmp(exchg, "SHFE") == 0) ? _ag : nullptr;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override
	{
		WTSArray* ret = WTSArray::create();
		ret->append(_ag, true);
		return ret;
	}
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }
	WTSContractInfo* contract() const { return _ag; }

private:
	WTSCommodityInfo* _comm;
	WTSContractInfo* _ag;
};

// Thread-safe fake trader API: records submitted entrusts for a report thread.
class QueueApi : public ITraderApi
{
public:
	struct Sent { std::string code; std::string exchg; std::string tag; double vol; };
	std::mutex mtx;
	std::condition_variable cv;
	std::deque<Sent> sent;
	std::atomic<uint32_t> ids{ 0 };

	bool init(WTSVariant*) override { return true; }
	void release() override {}
	void registerSpi(ITraderSpi*) override {}
	void connect() override {}
	void disconnect() override {}
	bool isConnected() override { return true; }
	bool makeEntrustID(char* buffer, int length) override
	{
		snprintf(buffer, (size_t)length, "Q%u", ids.fetch_add(1) + 1);
		return true;
	}
	int login(const char*, const char*, const char*) override { return 0; }
	int logout() override { return 0; }
	int orderInsert(WTSEntrust* entrust) override
	{
		std::lock_guard<std::mutex> lock(mtx);
		sent.push_back({ entrust->getCode(), entrust->getExchg(), entrust->getUserTag(), entrust->getVolume() });
		cv.notify_one();
		return 0;
	}
	int orderAction(WTSEntrustAction*) override { return 0; }
	int queryAccount() override { return 0; }
	int queryPositions() override { return 0; }
	int queryOrders() override { return 0; }
	int queryTrades() override { return 0; }
};

std::string writePolicy(const std::string& dir)
{
	const std::string path = dir + "/policy.json";
	std::ofstream out(path);
	out << R"({"default":{"order":[{"action":"open","limit":0,"limit_s":0,"limit_l":0,"pure":false}],"filters":[]}})";
	return path;
}

std::string workDir(const char* name)
{
	const std::string dir = (std::filesystem::current_path() / name).string();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

void initLogger()
{
	static bool once = []() {
		WTSLogger::init(R"({"root":{"level":"error","async":false,"sinks":[]}})", false, nullptr);
		return true;
	}();
	(void)once;
}

WTSOrderInfo* makeOrder(const QueueApi::Sent& s, double left, WTSOrderState state)
{
	WTSEntrust* e = WTSEntrust::create(s.code.c_str(), s.vol, 5000.0, s.exchg.c_str());
	e->setUserTag(s.tag.c_str());
	e->setDirection(WDT_LONG);
	e->setOffsetType(WOT_OPEN);
	WTSOrderInfo* o = WTSOrderInfo::create(e);
	e->release();
	o->setOrderID(("O-" + s.tag).c_str());
	o->setVolLeft(left);
	o->setVolTraded(s.vol - left);
	o->setOrderState(state);
	return o;
}

WTSTradeInfo* makeTrade(const QueueApi::Sent& s, double vol)
{
	WTSTradeInfo* t = WTSTradeInfo::create(s.code.c_str(), s.exchg.c_str());
	t->setTradeID(("T-" + s.tag).c_str());
	t->setUserTag(s.tag.c_str());
	t->setDirection(WDT_LONG);
	t->setOffsetType(WOT_OPEN);
	t->setVolume(vol);
	t->setPrice(5000.0);
	return t;
}

}	// namespace

// Strategy threads place orders while one SPI thread delivers live, duplicate,
// filled and rejected reports and a reader polls the ledger. Every order ends
// terminal, so the undone total must return to zero exactly.
TEST(UftConcurrency, LedgerUnderConcurrentOrdersReportsAndReaders)
{
	initLogger();
	const std::string dir = workDir("ledger");
	FakeBd bd;
	QueueApi api;
	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init(writePolicy(dir).c_str()));
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("uftcc", &api, &bd, &policy));
	ASSERT_TRUE(adapter.run());
	adapter.onLoginResult(true, "ok", 20240315);

	const int kThreads = 3, kOrders = 200;
	const int total = kThreads * kOrders;
	std::atomic<bool> done{ false };
	std::atomic<int> handled{ 0 };

	std::thread spi([&]() {
		int n = 0;
		while (handled.load() < total)
		{
			QueueApi::Sent s;
			{
				std::unique_lock<std::mutex> lock(api.mtx);
				if (!api.cv.wait_for(lock, std::chrono::milliseconds(50), [&]() { return !api.sent.empty(); }))
					continue;
				s = api.sent.front();
				api.sent.pop_front();
			}
			if (n++ % 5 == 0)
			{
				WTSEntrust* e = WTSEntrust::create(s.code.c_str(), s.vol, 5000.0, s.exchg.c_str());
				e->setUserTag(s.tag.c_str());
				WTSError* err = WTSError::create(WEC_ORDERINSERT, "fixture rejection");
				adapter.onRspEntrust(e, err);
				adapter.onRspEntrust(e, err);	// duplicate
				err->release();
				e->release();
			}
			else
			{
				WTSOrderInfo* live = makeOrder(s, s.vol, WOS_NotTraded_Queuing);
				adapter.onPushOrder(live);
				WTSTradeInfo* trade = makeTrade(s, s.vol);
				adapter.onPushTrade(trade);
				adapter.onPushTrade(trade);	// duplicate
				WTSOrderInfo* filled = makeOrder(s, 0, WOS_AllTraded);
				adapter.onPushOrder(filled);
				adapter.onPushOrder(live);	// late live report after terminal
				trade->release();
				live->release();
				filled->release();
			}
			handled.fetch_add(1);
		}
	});
	std::thread reader([&]() {
		while (!done.load())
		{
			const double undone = adapter.getUndoneQty(kAg);
			EXPECT_GE(undone, 0.0);
#ifdef WT_UFT_UNDONE_LEDGER
			(void)adapter.undoneLedger(api.ids.load());
#endif
			std::this_thread::yield();
		}
	});
	std::vector<std::thread> strategies;
	for (int t = 0; t < kThreads; ++t)
		strategies.emplace_back([&]() {
			for (int i = 0; i < kOrders; ++i)
				EXPECT_NE(UINT_MAX, adapter.openLong(kAg, 5000.0, 1.0, 0));
		});
	for (auto& th : strategies)
		th.join();
	spi.join();
	done = true;
	reader.join();

	EXPECT_EQ(total, handled.load());
	EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kAg));
	// 4 of 5 orders filled 1 lot each; duplicates are counted once.
	EXPECT_DOUBLE_EQ((double)(total - (total + 4) / 5), adapter.getPosition(kAg, false, 1));
}

// Trades, order reports, ticks and readers run while the trading day switches
// twice. The local net position never double counts and stays consistent.
TEST(UftConcurrency, ContextLocalStateAcrossDaySwitches)
{
	initLogger();
	const std::string dir = workDir("ctx");
	WtHelper::setGenerateDir((dir + "/").c_str());
	FakeBd bd;
	QueueApi api;
	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init(writePolicy(dir).c_str()));
	WtUftEngine engine;
	engine.init(nullptr, &bd, nullptr, nullptr);
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("uftctxcc", &api, &bd, &policy));
	ASSERT_TRUE(adapter.run());
	adapter.onLoginResult(true, "ok", 20240315);
	UftStraContext ctx(&engine, "uftctxcc");
	ctx.setTrader(&adapter);
	ctx.on_channel_ready(20240315);

	const int kOrders = 300;
	std::atomic<bool> stop{ false };
	std::mutex idMtx;
	std::deque<uint32_t> ids;
	std::atomic<int> traded{ 0 };

	std::thread strategy([&]() {
		for (int i = 0; i < kOrders; ++i)
		{
			const uint32_t id = ctx.stra_enter_long(kAg, 5000.0, 1.0, 0);
			EXPECT_NE(UINT_MAX, id);
			{
				std::lock_guard<std::mutex> lock(idMtx);
				ids.push_back(id);
			}
			(void)ctx.stra_get_local_position(kAg);
			(void)ctx.stra_get_local_closeprofit(kAg);
		}
	});
	std::thread trader([&]() {
		while (traded.load() < kOrders)
		{
			uint32_t id = 0;
			{
				std::lock_guard<std::mutex> lock(idMtx);
				if (!ids.empty())
				{
					id = ids.front();
					ids.pop_front();
				}
			}
			if (id == 0)
			{
				std::this_thread::yield();
				continue;
			}
			ctx.on_order(id, kAg, true, 0, 1.0, 1.0, 5000.0, false);
			ctx.on_trade(id, kAg, true, 0, 1.0, 5000.0);
			traded.fetch_add(1);
		}
	});
	std::thread ticker([&]() {
		WTSTickStruct st;
		memset(&st, 0, sizeof(st));
		strcpy(st.exchg, "SHFE");
		strcpy(st.code, "ag2406");
		st.action_date = 20240315;
		st.action_time = 93000000;
		st.price = 5001.0;
		while (!stop.load())
		{
			WTSTickData* tick = WTSTickData::create(st);
			tick->setContractInfo(bd.contract());
			ctx.on_tick(kAg, tick);
			tick->release();
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ctx.on_channel_ready(20240318);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ctx.on_channel_ready(20240319);

	strategy.join();
	trader.join();
	stop = true;
	ticker.join();

	EXPECT_DOUBLE_EQ((double)kOrders, ctx.stra_get_local_position(kAg));
	// The same state is derived from the published files by a cold start.
	UftStraContext cold(&engine, "uftctxcc");
	cold.on_channel_ready(20240319);
	EXPECT_DOUBLE_EQ((double)kOrders, cold.stra_get_local_position(kAg));
}
