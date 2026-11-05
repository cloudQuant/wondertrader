/**
 * s1_trader_entry_gate_test.cpp - iteration 03, slice1 final gaps
 *
 * TraderAdapter order-entry gates (local fakes only, no provider or network):
 *
 *  - BUG-P02-005 / BUG-R202-003, decision K05=C: the entry re-check of the
 *    account net limit uses the actual net position plus ALL orders in flight on
 *    the account. Adapters bound to the same account_id share one
 *    AccountTradingGate; their in-flight quantity is summed, and the check plus
 *    its reservation is one atomic step (two concurrent entries cannot both pass).
 *  - I03-NEW-005, decision K03 (host side, first step): an order whose
 *    provider call failed synchronously is closed as NOT_SENT; a later live
 *    order report or trade for it means the outcome was not what the host
 *    recorded, so the account is paused for reconciliation (the tombstone is
 *    not reopened).
 *  - I03-NEW-006: without an identity journal, the local order id comes from a
 *    checked clock snapshot; a clock/time-zone failure rejects the entry before
 *    the provider is called (fail-closed) and a later entry succeeds once the
 *    clock is available again.
 */
#include "r71_wtcore_env.h"

#include <WtCore/AccountNetPositionLimits.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace wtp;

namespace {

const char* kS1Code = "SHFE.ag.2512";

/// Provider fake: records the user tag of every insert and returns a configurable result.
class S1RecordingApi : public r71::R71FakeTraderApi
{
public:
	std::atomic<int> insert_ret{ 0 };
	std::string last_usertag;
	std::mutex mtx;

	int orderInsert(WTSEntrust* entrust) override
	{
		{
			std::lock_guard<std::mutex> lock(mtx);
			last_usertag = entrust->getUserTag();
		}
		r71::R71FakeTraderApi::orderInsert(entrust);
		return insert_ret.load();
	}
};

class S1TraderEntryGate : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	int ownerTag = 0;	// registry owner key of the declared account limit

	void SetUp() override
	{
		traderEnv.bd.regisSession("TRADING");
		WTSSessionInfo* sess = traderEnv.bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		WTSCommodityInfo* comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		comm->setSessionInfo(sess);
		WTSContractInfo* ct = r71::makeCt("SHFE", "ag", "ag2512");
		traderEnv.bd.regis(comm, ct);
		traderEnv.bd.regisAlias(kS1Code, ct);
	}

	void TearDown() override { account_net_limits::Registry::instance().release(&ownerTag); }

	bool build(TraderAdapter& trader, ITraderApi& api, const char* id)
	{
		if (!traderEnv.policy_ok())
			return false;
		if (!trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()))
			return false;
		if (!trader.run())
			return false;
		trader.onLoginResult(true, "s1", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		return trader.state() == TraderAdapter::AS_ALLREADY;
	}

	void declare(TraderAdapter& trader, double limit)
	{
		account_net_limits::Limits limits;
		limits.by_key[kS1Code] = limit;
		account_net_limits::Positions targets;
		account_net_limits::Positions previous;
		const void* previousAccount = nullptr;
		std::string error;
		std::shared_ptr<AccountTradingGate> gate = trader.accountGate();
		ASSERT_TRUE(account_net_limits::Registry::instance().apply(gate.get(), gate, &ownerTag, "s1_k05",
			limits, targets, previous, previousAccount, error, true)) << error;
	}

	WTSContractInfo* contract() { return traderEnv.bd.getContract("ag2512", "SHFE"); }

	WTSOrderInfo* liveOrder(const std::string& usertag, double vol, WTSOrderState state = WOS_NotTraded_Queuing)
	{
		WTSOrderInfo* o = WTSOrderInfo::create();
		o->setCode(kS1Code);
		o->setExchange("SHFE");
		o->setContractInfo(contract());
		o->setDirection(WDT_LONG);
		o->setOffsetType(WOT_OPEN);
		o->setOrderState(state);
		o->setPrice(3500.0);
		o->setVolume(vol);
		o->setVolTraded(0);
		o->setVolLeft(vol);
		o->setOrderID("S1OID1");
		o->setEntrustID("S1E001");
		o->setUserTag(usertag.c_str());
		o->setOrderDate(20250110);
		o->setOrderTime(100000);
		return o;
	}

	WTSTradeInfo* trade(const std::string& usertag, double vol)
	{
		WTSTradeInfo* t = WTSTradeInfo::create(kS1Code, "SHFE");
		t->setContractInfo(contract());
		t->setDirection(WDT_LONG);
		t->setOffsetType(WOT_OPEN);
		t->setVolume(vol);
		t->setPrice(3500.0);
		t->setTradeID("S1T001");
		t->setRefOrder("S1OID1");
		t->setUserTag(usertag.c_str());
		t->setTradeDate(20250110);
		t->setTradeTime(100001);
		t->setAmount(vol * 3500.0);
		return t;
	}
};

} // namespace

// K05=C: adapters bound to one account share the in-flight quantity of the account.
TEST_F(S1TraderEntryGate, K05SiblingAdaptersOfOneAccountShareInFlightQuantity)
{
	S1RecordingApi apiA;
	S1RecordingApi apiB;
	TraderAdapter a;
	TraderAdapter b;
	ASSERT_TRUE(build(a, apiA, "s1_k05_a"));
	ASSERT_TRUE(build(b, apiB, "s1_k05_b"));
	std::string error;
	ASSERT_TRUE(a.bindAccount("s1_k05_shared", "", &error)) << error;
	ASSERT_TRUE(b.bindAccount("s1_k05_shared", "", &error)) << error;
	ASSERT_EQ(a.accountGate(), b.accountGate());
	declare(a, 2.0);

	EXPECT_NE(UINT_MAX, a.openLong(kS1Code, 3500.0, 2.0, 0)) << "2 lots on adapter A are within the account limit 2";
	EXPECT_EQ(UINT_MAX, b.openLong(kS1Code, 3500.0, 1.0, 0))
		<< "adapter B must see A's 2 lots in flight: 2 + 1 exceeds the account limit";
	EXPECT_EQ(0u, apiB.n_insert.load()) << "the rejected entry never reached B's provider";
	EXPECT_NE(UINT_MAX, b.openShort(kS1Code, 3500.0, 1.0, 0)) << "a reducing entry is always allowed";
	a.release();
	b.release();
}

// K05=C: the check and its reservation are atomic. Entry 1 passes the check and is
// held (entry observer) before its order reaches the ledger; entry 2 on another
// thread must already see entry 1's reservation.
TEST_F(S1TraderEntryGate, K05ConcurrentEntriesCannotBothPassTheAccountCheck)
{
	S1RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s1_k05_race"));
	declare(trader, 2.0);

	std::mutex m;
	std::condition_variable cv;
	bool firstHeld = false;
	bool secondDone = false;
	uint32_t firstId = UINT_MAX;
	uint32_t secondId = UINT_MAX;

	std::thread first([&]() {
		TraderAdapter::EntrustObserverScope scope(&trader, [&](uint32_t) {
			std::unique_lock<std::mutex> lock(m);
			firstHeld = true;
			cv.notify_all();
			cv.wait_for(lock, std::chrono::seconds(5), [&] { return secondDone; });
		});
		firstId = trader.openLong(kS1Code, 3500.0, 2.0, 0);
	});
	{
		std::unique_lock<std::mutex> lock(m);
		ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return firstHeld; }))
			<< "the first entry did not reach the observer";
	}
	secondId = trader.openLong(kS1Code, 3500.0, 2.0, 0);
	{
		std::lock_guard<std::mutex> lock(m);
		secondDone = true;
	}
	cv.notify_all();
	first.join();

	EXPECT_NE(UINT_MAX, firstId);
	EXPECT_EQ(UINT_MAX, secondId) << "the second entry passed while the first one's 2 lots were already reserved";
	EXPECT_EQ(1u, api.n_insert.load());
	EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty(kS1Code));
	trader.release();
}

// K03 host side: a synchronous provider failure closes the order as NOT_SENT; a
// later live report for that local id pauses the account and does not reopen the
// order's undone quantity.
TEST_F(S1TraderEntryGate, K03LiveOrderReportAfterNotSentClosurePausesAccount)
{
	S1RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s1_k03_order"));
	api.insert_ret = -1;
	EXPECT_EQ(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0));
	ASSERT_EQ(1u, api.n_insert.load());
	ASSERT_FALSE(api.last_usertag.empty());
	EXPECT_FALSE(trader.isTradingAdmissionClosed());
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kS1Code));

	WTSOrderInfo* late = liveOrder(api.last_usertag, 1.0);
	trader.onPushOrder(late);
	late->release();
	EXPECT_TRUE(trader.isTradingAdmissionClosed())
		<< "a live report of an order recorded as not sent leaves the outcome unknown";
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kS1Code)) << "the NOT_SENT tombstone is not reopened";
	api.insert_ret = 0;
	EXPECT_EQ(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0)) << "new orders stay blocked until reconciliation";
	trader.release();
}

// K03 host side: a trade for an order closed as NOT_SENT pauses the account as well.
TEST_F(S1TraderEntryGate, K03TradeAfterNotSentClosurePausesAccount)
{
	S1RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s1_k03_trade"));
	api.insert_ret = -1;
	EXPECT_EQ(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0));
	ASSERT_FALSE(api.last_usertag.empty());

	WTSTradeInfo* fill = trade(api.last_usertag, 1.0);
	trader.onPushTrade(fill);
	fill->release();
	EXPECT_TRUE(trader.isTradingAdmissionClosed());
	trader.release();
}

// Control: a NOT_SENT closure without any later report leaves the account open, and
// a terminal (canceled) report for it is not treated as evidence of a live order.
TEST_F(S1TraderEntryGate, K03NotSentWithoutLiveEvidenceKeepsTheAccountOpen)
{
	S1RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s1_k03_control"));
	api.insert_ret = -1;
	EXPECT_EQ(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0));
	ASSERT_FALSE(api.last_usertag.empty());
	EXPECT_FALSE(trader.isTradingAdmissionClosed());

	WTSOrderInfo* dead = liveOrder(api.last_usertag, 1.0, WOS_Canceled);
	trader.onPushOrder(dead);
	dead->release();
	EXPECT_FALSE(trader.isTradingAdmissionClosed());

	api.insert_ret = 0;
	EXPECT_NE(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0));
	trader.release();
}

namespace {
std::atomic<bool> g_s1_clock_fails{ false };
bool s1OrderClock(uint32_t& date, uint32_t& time, uint64_t& epochMillis)
{
	if (g_s1_clock_fails.load())
		return false;
	return TimeUtils::tryGetDateTime(date, time, epochMillis);
}
} // namespace

// I03-NEW-006: without an identity journal a failed clock snapshot rejects the entry
// before the provider is called; nothing is reserved; recovery needs no restart.
TEST_F(S1TraderEntryGate, New006ClockFailureRejectsEntryBeforeTheProvider)
{
	S1RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s1_new006"));
	trader.setOrderClockForTest(&s1OrderClock);

	g_s1_clock_fails = true;
	EXPECT_EQ(UINT_MAX, trader.openLong(kS1Code, 3500.0, 1.0, 0));
	EXPECT_EQ(0u, api.n_insert.load()) << "no request reaches the provider without a valid clock";
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kS1Code));

	g_s1_clock_fails = false;
	const uint32_t id = trader.openLong(kS1Code, 3500.0, 1.0, 0);
	EXPECT_NE(UINT_MAX, id);
	EXPECT_NE(0u, id);
	EXPECT_EQ(1u, api.n_insert.load());
	trader.setOrderClockForTest(nullptr);
	trader.release();
}
