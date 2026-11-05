/**
 * Iteration 03: TraderAdapter order/cancel frequency limiting (card 7) and
 * split-order undone accounting (BUG-R3-002/003/008, BUG-I02-NEW-139/180).
 *
 * Built against three TraderAdapter variants selected by WT_I03_VARIANT:
 *   0 = git HEAD (permanent exclusion, last-operation window end)
 *   1 = working tree before this change (FrequencyWindow wired, no rollback)
 *   2 = current change
 * Tests that need the injected monotonic clock seam only exist in variant 2;
 * in older variants they report GTEST_SKIP because the seam is absent.
 *
 * Every provider here is an in-process fake. No real API, broker or network.
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <climits>

#include <gtest/gtest.h>

#include <Includes/WTSCollection.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSTradeDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Includes/IBaseDataMgr.h>
#include <Includes/ITraderApi.h>

#define private public
#define protected public
#include <WtCore/TraderAdapter.h>
#undef protected
#undef private

#include <WtCore/ActionPolicyMgr.h>
#include <WtCore/WtHelper.h>

#ifndef WT_I03_VARIANT
#error "WT_I03_VARIANT must be 0 (HEAD), 1 (pre-change tree) or 2 (fix)"
#endif

USING_NS_WTP;

namespace {

const char* kStdCode = "SHFE.ag.2512";
const char* kRawCode = "ag2512";
const char* kExchg = "SHFE";

std::atomic<uint64_t> g_fake_ms{ 1000000 };
uint64_t fakeNow() { return g_fake_ms.load(); }

class ScriptedApi : public ITraderApi
{
public:
	std::function<int(WTSEntrust*, uint32_t)> onInsert;		// (entrust, 1-based insert index)
	std::function<int(WTSEntrustAction*, uint32_t)> onAction;
	std::atomic<uint32_t> inserts{ 0 };
	std::atomic<uint32_t> actions{ 0 };
	std::atomic<uint32_t> eid{ 0 };

	bool init(WTSVariant*) override { return true; }
	void release() override {}
	void registerSpi(ITraderSpi*) override {}
	void connect() override {}
	void disconnect() override {}
	bool isConnected() override { return true; }
	bool makeEntrustID(char* buffer, int length) override
	{
		std::snprintf(buffer, (size_t)length, "I03E%05u", ++eid);
		return true;
	}
	int orderInsert(WTSEntrust* entrust) override
	{
		const uint32_t n = ++inserts;
		return onInsert ? onInsert(entrust, n) : 0;
	}
	int orderAction(WTSEntrustAction* action) override
	{
		const uint32_t n = ++actions;
		return onAction ? onAction(action, n) : 0;
	}
	int queryAccount() override { return 0; }
	int queryPositions() override { return 0; }
	int queryOrders() override { return 0; }
	int queryTrades() override { return 0; }
};

class OneContractBaseData : public IBaseDataMgr
{
public:
	WTSCommodityInfo* comm = NULL;
	WTSContractInfo* ct = NULL;
	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		return strcmp(comm->getFullPid(), exchgpid) == 0 ? comm : NULL;
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		return (strcmp(exchg, kExchg) == 0 && strcmp(pid, "ag") == 0) ? comm : NULL;
	}
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		return (strcmp(code, kRawCode) == 0 && (exchg[0] == '\0' || strcmp(exchg, kExchg) == 0)) ? ct : NULL;
	}
	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
};

class I03RateLimit : public ::testing::Test
{
protected:
	ScriptedApi api_;
	OneContractBaseData bd_;
	WTSSessionInfo* sess_ = NULL;
	std::unique_ptr<ActionPolicyMgr> policy_;
	std::filesystem::path tmp_;
	std::string id_;

	void SetUp() override
	{
		const std::string name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
		tmp_ = std::filesystem::temp_directory_path() /
			("i03_ratelimit_" + std::to_string((long)::getpid()) + "_" + name);
		std::filesystem::create_directories(tmp_);
		static std::atomic<uint32_t> seq{ 0 };
		id_ = "i03r" + std::to_string(++seq);

		bd_.comm = WTSCommodityInfo::create("ag", "ag", kExchg, "TRADING", "CHINA");
		bd_.comm->setCategory(CC_Future);
		bd_.comm->setCoverMode(CM_CoverToday);
		bd_.comm->setTradingMode(TM_Both);
		bd_.comm->setVolScale(1);
		bd_.comm->setPriceTick(1.0);
		sess_ = WTSSessionInfo::create("TRADING", "TRADING", 0);
		sess_->addTradingSection(900, 1130);
		bd_.comm->setSessionInfo(sess_);
		bd_.ct = WTSContractInfo::create(kRawCode, kRawCode, kExchg, "ag");
		bd_.ct->setAltCode(kRawCode);
		bd_.ct->setVolumeLimits(1, 1);	// every split is one lot
		bd_.ct->setCommInfo(bd_.comm);
		WtHelper::setTime(20250110, 931, 0);
		loadPolicy(R"({"default":{"order":[{"action":"open","limit":0}]}})");
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(std::filesystem::path(WtHelper::getBaseDir()) / "traders" / id_, ec);
		std::filesystem::remove_all(tmp_, ec);
		if (bd_.ct) bd_.ct->release();
		if (bd_.comm) bd_.comm->release();
		if (sess_) sess_->release();
	}

	void loadPolicy(const std::string& body)
	{
		const std::string path = (tmp_ / "policy.json").string();
		std::ofstream(path, std::ios::trunc) << body;
		policy_.reset(new ActionPolicyMgr());	// rules accumulate per manager
		ASSERT_TRUE(policy_->init(path.c_str()));
	}

	void ready(TraderAdapter& trader, uint32_t orderN, uint32_t orderSpan,
		uint32_t cancelN = 0, uint32_t cancelSpan = 0, bool injectClock = true)
	{
		ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, policy_.get()));
		ASSERT_TRUE(trader.run());
		TraderAdapter::RiskParams params;
		params._order_times_boundary = orderN;
		params._order_stat_timespan = orderSpan;
		params._cancel_times_boundary = cancelN;
		params._cancel_stat_timespan = cancelSpan;
		trader._risk_params_map["default"] = params;
		trader._risk_mon_enabled = true;
#if WT_I03_VARIANT >= 2
		if (injectClock)
			trader._rate_clock = &fakeNow;
#else
		(void)injectClock;
#endif
	}

	WTSOrderInfo* makeOrder(uint32_t localid, WTSDirectionType dir, WTSOffsetType off, double vol,
		bool canceled = false)
	{
		WTSOrderInfo* o = WTSOrderInfo::create();
		o->setCode(kRawCode);
		o->setExchange(kExchg);
		o->setContractInfo(bd_.ct);
		o->setDirection(dir);
		o->setOffsetType(off);
		o->setOrderFlag(WOF_NOR);
		o->setPrice(7000.0);
		o->setVolume(vol);
		o->setVolTraded(0);
		o->setVolLeft(canceled ? 0 : vol);
		o->setOrderID(("OID" + std::to_string(localid)).c_str());
		o->setEntrustID("I03E");
		o->setUserTag(("otp." + id_ + "." + std::to_string(localid)).c_str());
		o->setOrderDate(20250110);
		o->setOrderTime(93000);
		if (canceled)
			o->setOrderState(WOS_Canceled);
		return o;
	}

	WTSOrderInfo* fromEntrust(WTSEntrust* e, bool canceled)
	{
		WTSOrderInfo* o = WTSOrderInfo::create(e);
		o->setContractInfo(bd_.ct);
		o->setOrderID("SYNC");
		o->setVolTraded(0);
		o->setVolLeft(canceled ? 0 : e->getVolume());
		o->setOrderDate(20250110);
		o->setOrderTime(93000);
		o->setOrderState(canceled ? WOS_Canceled : WOS_NotTraded_Queuing);
		return o;
	}

	void seedOrders(TraderAdapter& trader, std::initializer_list<uint32_t> ids)
	{
		WTSArray* arr = WTSArray::create();
		for (uint32_t id : ids)
		{
			WTSOrderInfo* o = makeOrder(id, WDT_LONG, WOT_OPEN, 1);
			arr->append(o, false);
		}
		trader.onRspOrders(arr);
		arr->release();
	}

	static bool noFailureMarker(const OrderIDs& ids)
	{
		for (uint32_t id : ids)
			if (id == UINT_MAX)
				return false;
		return true;
	}
};

#define REQUIRE_CLOCK_SEAM()                                                     \
	do {                                                                         \
		if (WT_I03_VARIANT < 2)                                                  \
			GTEST_SKIP() << "variant has no injectable monotonic clock seam";    \
	} while (0)

//////////////////////////////////////////////////////////////////////////
// 7a: N admitted, N+1 rejected before the API (orders), N = 1 and N = 2
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, order_boundary_n_admitted_n_plus_1_rejected_before_api)
{
	for (uint32_t n : { 1u, 2u })
	{
		TraderAdapter trader;
		ready(trader, n, 60);
		api_.inserts = 0;
		for (uint32_t i = 0; i < n; ++i)
			EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0)) << "N=" << n << " i=" << i;
		EXPECT_FALSE(trader.checkOrderLimits(kStdCode)) << "N=" << n;
		EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0)) << "N=" << n;
		EXPECT_EQ(n, api_.inserts.load()) << "N+1 must not reach the API, N=" << n;
		trader.release();
	}
}

//////////////////////////////////////////////////////////////////////////
// 7d: cancels use the same boundary (single and batch), N = 1 and N = 2
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, cancel_boundary_single_n_admitted_n_plus_1_rejected)
{
	for (uint32_t n : { 1u, 2u })
	{
		TraderAdapter trader;
		ready(trader, 0, 0, n, 60);
		seedOrders(trader, { 5001, 5002, 5003 });
		api_.actions = 0;
		uint32_t accepted = 0;
		for (uint32_t id : { 5001u, 5002u, 5003u })
			accepted += trader.cancel(id) ? 1 : 0;
		EXPECT_EQ(n, accepted) << "N=" << n;
		EXPECT_EQ(n, api_.actions.load()) << "N+1 cancel must not reach the API, N=" << n;
		EXPECT_FALSE(trader.checkCancelLimits(kStdCode));
		trader.release();
	}
}

TEST_F(I03RateLimit, cancel_boundary_batch_uses_same_window)
{
	for (uint32_t n : { 1u, 2u })
	{
		TraderAdapter trader;
		ready(trader, 0, 0, n, 60);
		seedOrders(trader, { 6001, 6002, 6003 });
		api_.actions = 0;
		OrderIDs ids = trader.cancel(kStdCode, true);
		EXPECT_EQ(n, ids.size()) << "N=" << n;
		EXPECT_EQ(n, api_.actions.load()) << "N=" << n;
		auto it = trader._cancel_time_cache.find(kStdCode);
		ASSERT_NE(trader._cancel_time_cache.end(), it) << "batch cancel must record its requests";
		EXPECT_EQ(n, it->second.size());
		// A single cancel after the batch shares the same exhausted window.
		EXPECT_FALSE(trader.cancel(6003u));
		EXPECT_EQ(n, api_.actions.load());
		trader.release();
	}
}

//////////////////////////////////////////////////////////////////////////
// 7b: temporary ban until window expiry, automatic recovery (real clock)
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, order_window_recovers_after_expiry_real_clock)
{
	TraderAdapter trader;
	ready(trader, 2, 1, 0, 0, false);
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_FALSE(trader.isTradeEnabled(kStdCode)) << "full window is a temporary ban";
	std::this_thread::sleep_for(std::chrono::milliseconds(1150));
	EXPECT_TRUE(trader.isTradeEnabled(kStdCode)) << "ban must lift after the window expires";
	EXPECT_TRUE(trader.checkOrderLimits(kStdCode));
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(3u, api_.inserts.load());
	trader.release();
}

TEST_F(I03RateLimit, cancel_window_recovers_after_expiry_real_clock)
{
	TraderAdapter trader;
	ready(trader, 0, 0, 1, 1, false);
	seedOrders(trader, { 7001, 7002 });
	EXPECT_TRUE(trader.cancel(7001u));
	EXPECT_FALSE(trader.cancel(7002u));
	std::this_thread::sleep_for(std::chrono::milliseconds(1150));
	EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
	EXPECT_TRUE(trader.cancel(7002u));
	EXPECT_TRUE(trader.isTradeEnabled(kStdCode)) << "cancel limit must not permanently exclude the code";
	EXPECT_EQ(2u, api_.actions.load());
	trader.release();
}

//////////////////////////////////////////////////////////////////////////
// 7a/7c: exact window end and injected monotonic clock
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, order_window_exact_expiry_with_injected_clock)
{
	REQUIRE_CLOCK_SEAM();
	TraderAdapter trader;
	ready(trader, 2, 10);
	const uint64_t t0 = g_fake_ms.load();
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	g_fake_ms = t0 + 4000;
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	g_fake_ms = t0 + 9999;
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_FALSE(trader.isTradeEnabled(kStdCode));
	g_fake_ms = t0 + 10000;	// first request leaves the window (now - 10s, now]
	EXPECT_TRUE(trader.isTradeEnabled(kStdCode));
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0)) << "second still in window";
	g_fake_ms = t0 + 14000;
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(4u, api_.inserts.load());
	// Long idle: the window ends at "now", never at the last operation.
	g_fake_ms = t0 + 3600 * 1000;
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(6u, api_.inserts.load());
	trader.release();
}

TEST_F(I03RateLimit, window_uses_injected_monotonic_clock_not_real_time)
{
	REQUIRE_CLOCK_SEAM();
	TraderAdapter trader;
	ready(trader, 1, 1, 1, 1);
	seedOrders(trader, { 8001, 8002 });
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_TRUE(trader.cancel(8001u));
	// Real time passes, the injected monotonic clock does not.
	std::this_thread::sleep_for(std::chrono::milliseconds(1150));
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_FALSE(trader.cancel(8002u));
	g_fake_ms += 1000;
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_TRUE(trader.cancel(8002u));
	EXPECT_EQ(2u, api_.inserts.load());
	EXPECT_EQ(2u, api_.actions.load());
	trader.release();
}

TEST_F(I03RateLimit, cancel_window_exact_expiry_with_injected_clock)
{
	REQUIRE_CLOCK_SEAM();
	TraderAdapter trader;
	ready(trader, 0, 0, 1, 5);
	seedOrders(trader, { 8101, 8102, 8103 });
	const uint64_t t0 = g_fake_ms.load();
	EXPECT_TRUE(trader.cancel(8101u));
	g_fake_ms = t0 + 4999;
	EXPECT_TRUE(trader.cancel(kStdCode, true).empty());
	g_fake_ms = t0 + 5000;
	OrderIDs ids = trader.cancel(kStdCode, true);
	EXPECT_EQ(1u, ids.size());
	EXPECT_EQ(2u, api_.actions.load());
	trader.release();
}

//////////////////////////////////////////////////////////////////////////
// Per-request reservation: synchronous API failure returns the slot
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, order_sync_failure_rolls_back_reservation)
{
	TraderAdapter trader;
	ready(trader, 1, 60);
	api_.onInsert = [](WTSEntrust*, uint32_t n) { return n == 1 ? -1 : 0; };
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_TRUE(trader.checkOrderLimits(kStdCode)) << "failed request must not consume the slot";
	EXPECT_NE(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(UINT_MAX, trader.openLong(kStdCode, 7000.0, 1, 0));
	EXPECT_EQ(2u, api_.inserts.load());
	trader.release();
}

TEST_F(I03RateLimit, cancel_sync_failure_rolls_back_reservation)
{
	TraderAdapter trader;
	ready(trader, 0, 0, 1, 60);
	seedOrders(trader, { 9001, 9002 });
	api_.onAction = [](WTSEntrustAction*, uint32_t n) { return n == 1 ? -1 : 0; };
	EXPECT_FALSE(trader.cancel(9001u));
	EXPECT_TRUE(trader.checkCancelLimits(kStdCode));
	EXPECT_TRUE(trader.cancel(9001u));
	EXPECT_FALSE(trader.cancel(9002u));
	EXPECT_EQ(2u, api_.actions.load());
	trader.release();
}

//////////////////////////////////////////////////////////////////////////
// Re-entry: provider calls back inside the submit call
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, nested_submission_counts_pending_reservation)
{
	TraderAdapter trader;
	ready(trader, 2, 60);
	uint32_t nested = 0, nestedNested = 0;
	api_.onInsert = [&](WTSEntrust*, uint32_t n) {
		if (n == 1)
			nested = trader.openLong(kStdCode, 7000.0, 1, 0);
		else if (n == 2)
			nestedNested = trader.openLong(kStdCode, 7000.0, 1, 0);
		return 0;
	};
	const uint32_t outer = trader.openLong(kStdCode, 7000.0, 1, 0);
	EXPECT_NE(UINT_MAX, outer);
	EXPECT_NE(UINT_MAX, nested);
	EXPECT_EQ(UINT_MAX, nestedNested) << "pending outer request occupies a slot";
	EXPECT_EQ(2u, api_.inserts.load());
	trader.release();
}

TEST_F(I03RateLimit, sync_rejection_report_inside_insert_is_not_returned_as_live)
{
	TraderAdapter trader;
	ready(trader, 0, 0);
	api_.onInsert = [&](WTSEntrust* e, uint32_t n) {
		if (n == 2)
		{
			WTSError* err = WTSError::create(WEC_ORDERINSERT, "sync reject");
			trader.onRspEntrust(e, err);
			err->release();
		}
		return 0;
	};
	OrderIDs ids = trader.buy(kStdCode, 7000.0, 3, 0, false);
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_EQ(1u, ids.size()) << "rejected split is dead and submission stops";
	EXPECT_EQ(2u, api_.inserts.load());
	EXPECT_DOUBLE_EQ(1.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, sync_rejection_with_negative_return_is_not_rolled_back_twice)
{
	TraderAdapter trader;
	ready(trader, 0, 0);
	api_.onInsert = [&](WTSEntrust* e, uint32_t) {
		WTSError* err = WTSError::create(WEC_ORDERINSERT, "sync reject");
		trader.onRspEntrust(e, err);
		trader.onRspEntrust(e, err);	// duplicate push during the same call
		err->release();
		return -1;
	};
	OrderIDs ids = trader.sell(kStdCode, 7000.0, 1, 0, false);
	EXPECT_TRUE(ids.empty());
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, sync_cancel_report_inside_insert_consumes_ledger_once)
{
	TraderAdapter trader;
	ready(trader, 0, 0);
	api_.onInsert = [&](WTSEntrust* e, uint32_t n) {
		if (n == 2)
		{
			WTSOrderInfo* o = fromEntrust(e, true);
			trader.onPushOrder(o);
			o->release();
		}
		return 0;
	};
	OrderIDs ids = trader.sell(kStdCode, 7000.0, 3, 0, false);
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(-1.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, negative_return_after_live_report_keeps_order)
{
	TraderAdapter trader;
	ready(trader, 1, 60);
	api_.onInsert = [&](WTSEntrust* e, uint32_t) {
		WTSOrderInfo* o = fromEntrust(e, false);
		trader.onPushOrder(o);
		o->release();
		return -1;
	};
	OrderIDs ids = trader.buy(kStdCode, 7000.0, 1, 0, false);
	ASSERT_EQ(1u, ids.size());
	EXPECT_NE(UINT_MAX, ids[0]);
	EXPECT_DOUBLE_EQ(1.0, trader.getUndoneQty(kStdCode));
	EXPECT_FALSE(trader.checkOrderLimits(kStdCode)) << "request reached the provider; slot stays";
	trader.release();
}

TEST_F(I03RateLimit, nested_buy_inside_split_keeps_both_ledgers)
{
	TraderAdapter trader;
	ready(trader, 0, 0);
	OrderIDs nested;
	api_.onInsert = [&](WTSEntrust*, uint32_t n) {
		if (n == 1)
			nested = trader.buy(kStdCode, 7000.0, 1, 0, false);
		return 0;
	};
	OrderIDs ids = trader.buy(kStdCode, 7000.0, 2, 0, false);
	EXPECT_EQ(2u, ids.size());
	EXPECT_EQ(1u, nested.size());
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_TRUE(noFailureMarker(nested));
	EXPECT_EQ(3u, api_.inserts.load());
	EXPECT_DOUBLE_EQ(3.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, batch_cancel_survives_sync_cancel_reports)
{
	TraderAdapter trader;
	ready(trader, 0, 0, 0, 0);
	seedOrders(trader, { 9101, 9102, 9103, 9104 });
	EXPECT_DOUBLE_EQ(4.0, trader.getUndoneQty(kStdCode));
	api_.onAction = [&](WTSEntrustAction* action, uint32_t) {
		const std::string oid = action->getOrderID();
		const uint32_t localid = (uint32_t)std::stoul(oid.substr(3));
		WTSOrderInfo* o = makeOrder(localid, WDT_LONG, WOT_OPEN, 1, true);
		trader.onPushOrder(o);	// removes the order from _orders synchronously
		o->release();
		return 0;
	};
	OrderIDs ids = trader.cancel("", true);
	EXPECT_EQ(4u, ids.size());
	EXPECT_EQ(4u, api_.actions.load());
	std::set<uint32_t> unique(ids.begin(), ids.end());
	EXPECT_EQ(4u, unique.size());
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode));
	OrderMap* all = trader.getOrders("");
	if (all) { EXPECT_EQ(0u, all->size()); all->release(); }
	trader.release();
}

TEST_F(I03RateLimit, concurrent_submissions_never_exceed_boundary)
{
	TraderAdapter trader;
	ready(trader, 5, 60);
	api_.onInsert = [](WTSEntrust*, uint32_t) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return 0;
	};
	std::atomic<uint32_t> ok{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t)
		threads.emplace_back([&]() {
			for (int i = 0; i < 10; ++i)
				if (trader.openLong(kStdCode, 7000.0, 1, 0) != UINT_MAX)
					++ok;
		});
	for (auto& th : threads)
		th.join();
	EXPECT_EQ(5u, ok.load());
	EXPECT_EQ(5u, api_.inserts.load());
	trader.release();
}

//////////////////////////////////////////////////////////////////////////
// R3-008: split orders, undone qty only for splits accepted by the API
//////////////////////////////////////////////////////////////////////////
TEST_F(I03RateLimit, buy_split_partial_rate_reject)
{
	TraderAdapter trader;
	ready(trader, 2, 60);
	OrderIDs ids = trader.buy(kStdCode, 7000.0, 3, 0, false);
	EXPECT_EQ(2u, ids.size());
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_EQ(2u, api_.inserts.load());
	EXPECT_DOUBLE_EQ(2.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, sell_split_partial_rate_reject)
{
	TraderAdapter trader;
	ready(trader, 2, 60);
	OrderIDs ids = trader.sell(kStdCode, 7000.0, 3, 0, false);
	EXPECT_EQ(2u, ids.size());
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_EQ(2u, api_.inserts.load());
	EXPECT_DOUBLE_EQ(-2.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

TEST_F(I03RateLimit, buy_and_sell_split_partial_api_failure)
{
	for (bool isBuy : { true, false })
	{
		TraderAdapter trader;
		ready(trader, 0, 0);
		api_.inserts = 0;
		api_.onInsert = [](WTSEntrust*, uint32_t n) { return n == 2 ? -1 : 0; };
		OrderIDs ids = isBuy ? trader.buy(kStdCode, 7000.0, 3, 0, false)
			: trader.sell(kStdCode, 7000.0, 3, 0, false);
		EXPECT_EQ(1u, ids.size()) << (isBuy ? "buy" : "sell");
		EXPECT_TRUE(noFailureMarker(ids)) << (isBuy ? "buy" : "sell");
		EXPECT_EQ(2u, api_.inserts.load()) << "submission stops after the failed split";
		EXPECT_DOUBLE_EQ(isBuy ? 1.0 : -1.0, trader.getUndoneQty(kStdCode));
		trader.release();
	}
}

TEST_F(I03RateLimit, buy_and_sell_split_all_rejected)
{
	for (bool isBuy : { true, false })
	{
		TraderAdapter trader;
		ready(trader, 0, 0);
		api_.onInsert = [](WTSEntrust*, uint32_t) { return -1; };
		OrderIDs ids = isBuy ? trader.buy(kStdCode, 7000.0, 3, 0, false)
			: trader.sell(kStdCode, 7000.0, 3, 0, false);
		EXPECT_TRUE(ids.empty()) << (isBuy ? "buy" : "sell");
		EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(kStdCode)) << (isBuy ? "buy" : "sell");
		trader.release();
	}
}

TEST_F(I03RateLimit, buy_and_sell_rule_cap_records_only_submitted_qty)
{
	loadPolicy(R"({"default":{"order":[{"action":"open","limit":3,"limit_l":0,"limit_s":0}]}})");
	for (bool isBuy : { true, false })
	{
		TraderAdapter trader;
		ready(trader, 0, 0);
		OrderIDs ids = isBuy ? trader.buy(kStdCode, 7000.0, 10, 0, false)
			: trader.sell(kStdCode, 7000.0, 10, 0, false);
		EXPECT_EQ(3u, ids.size());
		EXPECT_DOUBLE_EQ(isBuy ? 3.0 : -3.0, trader.getUndoneQty(kStdCode)) << (isBuy ? "buy" : "sell");
		trader.release();
	}
}

TEST_F(I03RateLimit, sell_close_long_split_partial_failure)
{
	loadPolicy(R"({"default":{"order":[{"action":"close"}]}})");
	TraderAdapter trader;
	ready(trader, 0, 0);
	WTSPositionItem* p = WTSPositionItem::create(kRawCode, "CNY", kExchg);
	p->setContractInfo(bd_.ct);
	p->setDirection(WDT_LONG);
	p->setPrePosition(0);
	p->setAvailPrePos(0);
	p->setNewPosition(3);
	p->setAvailNewPos(3);
	p->setInitPosition(3);
	p->setAvgPrice(7000.0);
	WTSArray* arr = WTSArray::create();
	arr->append(p, false);
	trader.onRspPosition(arr);
	arr->release();
	api_.onInsert = [](WTSEntrust* e, uint32_t n) {
		EXPECT_EQ(WOT_CLOSETODAY, e->getOffsetType());
		return n == 3 ? -1 : 0;
	};
	OrderIDs ids = trader.sell(kStdCode, 7000.0, 3, 0, false);
	EXPECT_EQ(2u, ids.size());
	EXPECT_TRUE(noFailureMarker(ids));
	EXPECT_DOUBLE_EQ(-2.0, trader.getUndoneQty(kStdCode));
	trader.release();
}

} // namespace
