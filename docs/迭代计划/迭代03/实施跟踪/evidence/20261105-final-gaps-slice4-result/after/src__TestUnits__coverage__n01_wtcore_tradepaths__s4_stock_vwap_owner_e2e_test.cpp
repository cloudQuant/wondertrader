/*!
 * \file s4_stock_vwap_owner_e2e_test.cpp
 * BUG-I02-NEW-008 (slice4 final gaps, supplementary end-to-end): the real
 * WtExeFact.WtStockVWapExeUnit in a real WtLocalExecuter on a TraderAdapter with a
 * local fake API (no vendor API). StockVWap's channel-ready reconciliation reads the
 * account-level same-contract snapshot (getUndoneQty) and, when it finds live quantity
 * without own orders, cancels "unmanaged" orders by code/side (cancel(code, isBuy)).
 * With owner routing that snapshot is the unit's own owner scope, so a live order of
 * another owner on the same contract and side is neither treated as unmanaged nor
 * cancelled. The control without owner routing records the legacy (K44) account-level
 * view: the unit sees the foreign quantity as unmanaged; the owner-checked cancel still
 * cannot touch the other order.
 *
 * The do_calc cases drive the unit's own order path with controlled ticks, a controlled
 * executer clock and a cumulative VWAP profile (no product gate is changed): two
 * StockVWap executers on one TraderAdapter and one contract, owner routing on, each
 * submit, receive reports and cancel only under their own owner.
 */
#include "r71_wtcore_env.h"

#include <WtCore/ExecOwnerModes.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtLocalExecuter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* const kStk = "SSE.STK.600000";

TraderAdapter::OwnerRoutingCapabilities s4VwapCaps()
{
	TraderAdapter::OwnerRoutingCapabilities caps;
	caps.durable_journal = true;
	caps.local_order_id_floor = true;
	caps.provider_user_tag_round_trip = true;
	caps.stable_trade_id_across_push_and_query = true;
	return caps;
}

class S4VwapApi final : public r71::R71FakeTraderApi
{
public:
	int orderInsert(WTSEntrust* entrust) override
	{
		usertags.push_back(entrust->getUserTag());
		entrustIds.push_back(entrust->getEntrustID());
		prices.push_back(entrust->getPrice());
		volumes.push_back(entrust->getVolume());
		return r71::R71FakeTraderApi::orderInsert(entrust);
	}
	int orderAction(WTSEntrustAction* action) override
	{
		cancels.push_back(action->getEntrustID());
		return r71::R71FakeTraderApi::orderAction(action);
	}
	std::vector<std::string> usertags;
	std::vector<std::string> entrustIds;
	std::vector<double> prices;
	std::vector<double> volumes;
	std::vector<std::string> cancels;
};

// Executer clock under test control (getCurTime drives the unit's fire span).
class S4ClockStub final : public r71::R71ExecStub
{
public:
	uint64_t now = 20250110093100000ull;
	uint64_t get_real_time() override { return now; }
};

class S4LoggingLocal : public WtLocalExecuter
{
public:
	using WtLocalExecuter::WtLocalExecuter;
	void writeLog(const char* message) override
	{
		if (message != nullptr)
			logs.emplace_back(message);
	}
	bool logged(const char* part) const
	{
		for (const std::string& line : logs)
			if (line.find(part) != std::string::npos)
				return true;
		return false;
	}
	std::vector<std::string> logs;
};

// Ends the executers' lifetime on every exit path (also after a failed ASSERT): sinks
// are removed, the manager is quiesced, then the executers are destroyed with it.
struct S4ExecTeardown
{
	TraderAdapter& trader;
	WtExecuterMgr& mgr;
	std::vector<std::shared_ptr<S4LoggingLocal>*> locals;
	~S4ExecTeardown()
	{
		for (std::shared_ptr<S4LoggingLocal>* local : locals)
		{
			if (*local)
				trader.removeSink(local->get());
			local->reset();
		}
		WtExecQuiesceCoordinator coordinator({ &mgr });
		const bool quiesced = coordinator.quiesce_all() && coordinator.is_complete();
		EXPECT_TRUE(quiesced) << "executers did not quiesce";
	}
};

class S4StockVWapOwnerE2E : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71PluginDir plug;
	r71::R71TraderEnv traderEnv;
	S4ClockStub stub;
	r71::R71DataMgr dm;
	WtExecuterFactory factory;
	WTSContractInfo* stk = NULL;
	S4VwapApi api;

	void SetUp() override
	{
		ASSERT_TRUE(plug.ok()) << "libWtExeFact is required for the real unit";
		ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));
		traderEnv.bd.regisSession("STOCK_TEST");
		WTSSessionInfo* sess = traderEnv.bd.session("STOCK_TEST");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(930, 1130);
		sess->addTradingSection(1300, 1500);
		WTSCommodityInfo* comm = r71::makeComm("SSE", "STK", "STOCK_TEST", TM_LongT1, CC_Stock);
		comm->setSessionInfo(sess);
		stk = r71::makeCt("SSE", "STK", "600000");
		traderEnv.bd.regis(comm, stk);
		traderEnv.bd.regisAlias(kStk, stk);
		stub.comm_ = comm;
		stub.sess_ = sess;
		stub.hot_ = &traderEnv.hot;
		// The unit loads Vwap_<commodity name>.txt from the working directory.
		std::ofstream out("Vwap_STK.txt", std::ios::binary | std::ios::trunc);
		out << profileFor(*sess);
		out.close();
		ASSERT_FALSE(out.fail());
	}

	void TearDown() override
	{
		std::remove("Vwap_STK.txt");
	}

	// stepPerMinute == 0: constant 1.0 (channel-ready cases); otherwise a cumulative
	// profile reaching stepPerMinute * minute shares at each trading minute.
	static std::string profileFor(WTSSessionInfo& session, double stepPerMinute = 0.0)
	{
		std::ostringstream out;
		out << "{\"version\":2,\"profiles\":[{\"session_id\":\"" << session.id()
			<< "\",\"offset_minutes\":" << session.getOffsetMins() << ",\"sections\":[";
		const WTSSessionInfo::TradingTimes& sections = session.getTradingSections();
		for (size_t i = 0; i < sections.size(); ++i)
			out << (i != 0 ? "," : "") << "{\"from\":" << sections[i].first_raw
				<< ",\"to\":" << sections[i].second_raw << '}';
		out << "],\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
			<< "\"close_inclusive\":true,\"values\":[";
		for (uint32_t i = 0; i <= session.getTradingMins(); ++i)
			out << (i != 0 ? "," : "") << (stepPerMinute == 0.0 ? 1.0 : stepPerMinute * i);
		out << "]}]}";
		return out.str();
	}

	// completeEmptyOrders: the login order query is a K42 COMPLETE empty snapshot (how a
	// migrated provider reports an account without orders); otherwise the legacy NULL.
	bool build(TraderAdapter& trader, const char* id, bool routing, bool completeEmptyOrders = false)
	{
		if (!traderEnv.policy_ok() || !trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !trader.run())
			return false;
		trader.onLoginResult(true, "s4", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		if (completeEmptyOrders)
		{
			WTSArray* none = WTSArray::create();
			trader.onRspOrdersWithStatus(none, WT_QUERY_STATUS_COMPLETE);
			none->release();
		}
		else
			trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		if (trader.state() != TraderAdapter::AS_ALLREADY)
			return false;
		return !routing || trader.configureOwnerRoutingCapabilities(s4VwapCaps());
	}

	// The provider reports the other owner's order as live (queuing, nothing traded).
	void pushLive(TraderAdapter& trader, const std::string& usertag, const char* orderId,
		const std::string& entrustId = std::string())
	{
		pushOrder(trader, usertag, orderId, 0.0, WOS_NotTraded_Queuing, entrustId);
	}

	// Order report as a provider echoes it (usertag and entrust id of the insert).
	void pushOrder(TraderAdapter& trader, const std::string& usertag, const char* orderId,
		double traded, WTSOrderState state, const std::string& entrustId = std::string(),
		double price = 10.0, double volume = 100.0)
	{
		WTSOrderInfo* order = WTSOrderInfo::create();
		if (!entrustId.empty())
			order->setEntrustID(entrustId.c_str());
		order->setContractInfo(stk);
		order->setDirection(WDT_LONG);
		order->setOffsetType(WOT_OPEN);
		order->setPrice(price);
		order->setVolume(volume);
		order->setVolTraded(traded);
		order->setVolLeft(state == WOS_Canceled ? 0.0 : volume - traded);
		order->setUserTag(usertag.c_str());
		order->setOrderID(orderId);
		order->setOrderState(state);
		trader.onPushOrder(order);
		order->release();
	}

	void pushTrade(TraderAdapter& trader, const std::string& usertag, const char* tradeId,
		const char* orderId, double volume, double price = 10.0)
	{
		WTSTradeInfo* trade = WTSTradeInfo::create(stk->getCode(), stk->getExchg());
		trade->setContractInfo(stk);
		trade->setUserTag(usertag.c_str());
		trade->setTradeID(tradeId);
		trade->setRefOrder(orderId);
		trade->setDirection(WDT_LONG);
		trade->setOffsetType(WOT_OPEN);
		trade->setPrice(price);
		trade->setVolume(volume);
		trade->setTradeDate(20250110);
		trade->setTradeTime(93300000);
		trader.onPushTrade(trade);
		trade->release();
	}

	// A controlled in-session tick: the executer clock follows the tick time.
	void tickTo(S4LoggingLocal& local, uint32_t actionTime, double price)
	{
		stub.now = 20250110000000000ull + actionTime;
		WTSTickData* tick = r71::makeTick("SSE", kStk, 20250110, 20250110, actionTime, price, price - 0.01, price + 0.01);
		ASSERT_NE(tick, nullptr);
		local.on_tick(kStk, tick);
		tick->release();
	}

	// The localid is the numeric suffix of the adapter usertag.
	static uint32_t localidOf(const std::string& usertag)
	{
		return (uint32_t)std::strtoul(usertag.substr(usertag.rfind('.') + 1).c_str(), NULL, 10);
	}

	static std::size_t ownedOrders(TraderAdapter& trader, const char* executer)
	{
		OrderMap* orders = trader.getOrdersForOwner(kStk, ExecOwnerModes::ownerId(executer));
		if (orders == NULL)
			return 0;
		const std::size_t n = orders->size();
		orders->release();
		return n;
	}

	// totalTimes/minOpenLots default to the channel-ready cases; the do_calc cases use
	// 240 slices (one per trading minute, fire span 60 s) and a 100-share minimum lot.
	std::shared_ptr<S4LoggingLocal> makeVwap(const char* name, TraderAdapter& trader, WtExecuterMgr& mgr,
		uint32_t totalTimes = 1, double minOpenLots = 1.0, bool asSink = false)
	{
		std::shared_ptr<S4LoggingLocal> local(new S4LoggingLocal(&factory, name, &dm));
		mgr.add_executer(local);
		local->setStub(&stub);
		const std::string json = "{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{"
			"\"name\":\"WtExeFact.WtStockVWapExeUnit\",\"begin_time\":930,\"end_time\":1500,"
			"\"ord_sticky\":0,\"tail_secs\":0,\"total_times\":" + std::to_string(totalTimes)
			+ ",\"price_mode\":0,\"price_offset\":0,"
			"\"lots\":1.0,\"minopenlots\":" + std::to_string(minOpenLots) + ",\"offset\":0}}}";
		WTSVariant* params = WTSCfgLoader::load_from_content(json);
		EXPECT_NE(params, nullptr);
		if (params == nullptr)
			return local;
		EXPECT_TRUE(local->init(params));
		params->release();
		local->setTrader(&trader);
		if (asSink)
			trader.addSink(local.get());	// as WtRunner binds executers to their trader
		return local;
	}

	// Creates the unit for a 100-share target and opens its channel.
	static void arm(S4LoggingLocal& local, WtExecuterMgr& mgr, const char* name)
	{
		local.on_position_changed(kStk, 100.0);
		const ExecUnitInitStatus status = mgr.query_exec_unit_init_status(name, kStk);
		ASSERT_EQ(ExecUnitInitState::READY, status.state) << status.error;
		local.on_channel_ready();
	}

	void rewriteProfile(double stepPerMinute)
	{
		WTSSessionInfo* sess = traderEnv.bd.session("STOCK_TEST");
		ASSERT_NE(sess, nullptr);
		std::ofstream out("Vwap_STK.txt", std::ios::binary | std::ios::trunc);
		out << profileFor(*sess, stepPerMinute);
		out.close();
		ASSERT_FALSE(out.fail());
	}

	static void quiesce(WtExecuterMgr& mgr)
	{
		WtExecQuiesceCoordinator coordinator({ &mgr });
		ASSERT_TRUE(coordinator.quiesce_all());
		ASSERT_TRUE(coordinator.is_complete());
	}
};

} // namespace

TEST_F(S4StockVWapOwnerE2E, ChannelReadyIgnoresAnotherOwnersLiveOrderOnTheSameContract)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "s4_svwap_routed", true));
	ASSERT_EQ(1u, trader.buyAsOwner("exec:s4_other", true, kStk, 10.0, 100.0, 0, false).size());
	ASSERT_EQ(1u, api.usertags.size());
	pushLive(trader, api.usertags.back(), "B1");
	ASSERT_DOUBLE_EQ(100.0, trader.getUndoneQty(kStk)) << "fixture: the other owner's order is live";
	{
		WtExecuterMgr mgr;
		std::shared_ptr<S4LoggingLocal> vwap = makeVwap("s4_svwap", trader, mgr);
		ASSERT_TRUE(trader.isOwnerRoutingEnabled());
		vwap->on_position_changed(kStk, 100.0);
		const ExecUnitInitStatus status = mgr.query_exec_unit_init_status("s4_svwap", kStk);
		ASSERT_EQ(ExecUnitInitState::READY, status.state) << status.error;

		vwap->on_channel_ready();
		EXPECT_DOUBLE_EQ(0.0, vwap->getUndoneQty(kStk)) << "the unit's snapshot is its own owner scope";
		EXPECT_FALSE(vwap->logged("unmanaged orders")) << "another owner's order is not unmanaged for this unit";
		EXPECT_TRUE(api.cancels.empty()) << "StockVWap never cancels another owner's order";
		EXPECT_DOUBLE_EQ(100.0, trader.getUndoneQtyForOwner(kStk, "exec:s4_other"));
		EXPECT_DOUBLE_EQ(100.0, trader.getUndoneQty(kStk));

		// The executer-level code/side cancel used by the unit stays inside its owner.
		EXPECT_TRUE(vwap->cancel(kStk, true, 0).empty());
		EXPECT_TRUE(api.cancels.empty());
		vwap.reset();
		ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	}
	trader.release();
}

// Control (records the K44 legacy view, not a red case of this fix).
TEST_F(S4StockVWapOwnerE2E, WithoutOwnerRoutingTheAccountSnapshotLooksUnmanagedButIsNotCancelled)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "s4_svwap_legacy", false));
	ASSERT_EQ(1u, trader.buy(kStk, 10.0, 100.0, 0, false).size());
	ASSERT_EQ(1u, api.usertags.size());
	pushLive(trader, api.usertags.back(), "B1");
	ASSERT_DOUBLE_EQ(100.0, trader.getUndoneQty(kStk));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<S4LoggingLocal> vwap = makeVwap("s4_svwap_l", trader, mgr);
		ASSERT_FALSE(trader.isOwnerRoutingEnabled());
		vwap->on_position_changed(kStk, 100.0);
		const ExecUnitInitStatus status = mgr.query_exec_unit_init_status("s4_svwap_l", kStk);
		ASSERT_EQ(ExecUnitInitState::READY, status.state) << status.error;

		vwap->on_channel_ready();
		EXPECT_DOUBLE_EQ(100.0, vwap->getUndoneQty(kStk)) << "legacy: the account-level snapshot";
		EXPECT_TRUE(vwap->logged("unmanaged orders")) << "legacy: the foreign quantity looks unmanaged";
		EXPECT_TRUE(api.cancels.empty()) << "the owner-checked cancel still never reaches the other order";
		EXPECT_DOUBLE_EQ(100.0, trader.getUndoneQty(kStk));
		vwap.reset();
		ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	}
	trader.release();
}

// ---------------------------------------------------------------- do_calc end to end

TEST_F(S4StockVWapOwnerE2E, TwoStockVWapExecutersSubmitReceiveAndCancelOnlyUnderTheirOwnOwner)
{
	ASSERT_NO_FATAL_FAILURE(rewriteProfile(100.0));
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "s4_svwap_two", true, true));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<S4LoggingLocal> b = makeVwap("s4_vw_b", trader, mgr, 240, 100.0, true);
		std::shared_ptr<S4LoggingLocal> a = makeVwap("s4_vw_a", trader, mgr, 240, 100.0, true);
		S4ExecTeardown teardown{ trader, mgr, { &a, &b } };
		ASSERT_TRUE(trader.isOwnerRoutingEnabled());
		ASSERT_NO_FATAL_FAILURE(arm(*b, mgr, "s4_vw_b"));
		ASSERT_NO_FATAL_FAILURE(arm(*a, mgr, "s4_vw_a"));
		ASSERT_TRUE(api.usertags.empty()) << "no order before the first in-session tick";

		// B's first in-session tick: its own VWAP slice (100 shares at minute 1).
		ASSERT_NO_FATAL_FAILURE(tickTo(*b, 93100000, 10.0));
		ASSERT_EQ(1u, api.usertags.size()) << "StockVWap B submits through do_calc";
		const std::string tagB = api.usertags[0];
		EXPECT_DOUBLE_EQ(100.0, api.volumes[0]);
		EXPECT_DOUBLE_EQ(10.0, api.prices[0]) << "price_mode 0 uses the tick price";
		pushLive(trader, tagB, "S4-VB-1", api.entrustIds[0]);
		EXPECT_EQ(1u, ownedOrders(trader, "s4_vw_b"));
		EXPECT_DOUBLE_EQ(100.0, b->getUndoneQty(kStk));

		// A on the same contract and side: B's live order is neither A's gross order nor
		// A's undone quantity, so it does not hold A (the audited blocking risk).
		ASSERT_NO_FATAL_FAILURE(tickTo(*a, 93100500, 10.0));
		ASSERT_EQ(2u, api.usertags.size()) << "A is not blocked by B's same-contract order";
		const std::string tagA = api.usertags[1];
		EXPECT_NE(tagA, tagB);
		EXPECT_FALSE(a->logged("gross same-code order")) << "B's order is not in A's gross scan";
		pushLive(trader, tagA, "S4-VA-1", api.entrustIds[1]);
		EXPECT_EQ(1u, ownedOrders(trader, "s4_vw_a"));
		EXPECT_EQ(1u, ownedOrders(trader, "s4_vw_b"));
		EXPECT_DOUBLE_EQ(100.0, a->getUndoneQty(kStk));
		EXPECT_DOUBLE_EQ(100.0, b->getUndoneQty(kStk));
		EXPECT_DOUBLE_EQ(200.0, trader.getUndoneQty(kStk)) << "the account still holds both";

		// A's own target drops to 50 shares: on its next slice its live 100-share order
		// exceeds the remainder, A cancels by code/side (cancel(code, isBuy)) and only its
		// own order goes to the provider.
		a->on_position_changed(kStk, -50.0);
		ASSERT_NO_FATAL_FAILURE(tickTo(*a, 93200000, 10.0));
		ASSERT_EQ(1u, api.cancels.size()) << "A cancels its live order after its target dropped";
		EXPECT_EQ(api.entrustIds[1], api.cancels[0]) << "the cancel is A's own entrust";
		EXPECT_EQ(2u, api.usertags.size()) << "no new order while A's cancel is outstanding";
		pushOrder(trader, tagA, "S4-VA-1", 0.0, WOS_Canceled, api.entrustIds[1]);
		EXPECT_EQ(0u, ownedOrders(trader, "s4_vw_a"));
		EXPECT_EQ(1u, ownedOrders(trader, "s4_vw_b")) << "B's same-contract order stays live";
		EXPECT_DOUBLE_EQ(100.0, b->getUndoneQty(kStk));
		EXPECT_DOUBLE_EQ(0.0, a->getUndoneQty(kStk));
		const std::string canceledA = "Order " + std::to_string(localidOf(tagA)) + " of " + kStk + " canceled";
		EXPECT_TRUE(a->logged(canceledA.c_str())) << "A's unit receives its own cancel report";
		EXPECT_FALSE(b->logged(canceledA.c_str())) << "B's unit never sees A's cancel report";

		// B's fill is booked by B only.
		pushTrade(trader, tagB, "S4-VT-B1", "S4-VB-1", 100.0);
		pushOrder(trader, tagB, "S4-VB-1", 100.0, WOS_AllTraded, api.entrustIds[0]);
		const std::string tradedB = "Order " + std::to_string(localidOf(tagB)) + " of " + kStk + " traded";
		const std::string filledB = "Order " + std::to_string(localidOf(tagB)) + " has filled";
		EXPECT_TRUE(b->logged(tradedB.c_str())) << "B's unit receives its own trade";
		EXPECT_TRUE(b->logged(filledB.c_str()));
		EXPECT_FALSE(a->logged(tradedB.c_str())) << "A's unit never sees B's trade";
		EXPECT_FALSE(a->logged(filledB.c_str()));
		EXPECT_DOUBLE_EQ(0.0, b->getUndoneQty(kStk));
		EXPECT_EQ(1u, api.cancels.size()) << "nobody cancelled B's order";
	}
	trader.release();
}

// Control (records the K44 legacy view, not a red case of this fix): without owner
// routing the account-level snapshot holds A behind B's same-contract order.
TEST_F(S4StockVWapOwnerE2E, WithoutOwnerRoutingAnotherExecutersOrderHoldsTheStockVWap)
{
	ASSERT_NO_FATAL_FAILURE(rewriteProfile(100.0));
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "s4_svwap_two_legacy", false, true));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<S4LoggingLocal> b = makeVwap("s4_vwl_b", trader, mgr, 240, 100.0, true);
		std::shared_ptr<S4LoggingLocal> a = makeVwap("s4_vwl_a", trader, mgr, 240, 100.0, true);
		S4ExecTeardown teardown{ trader, mgr, { &a, &b } };
		ASSERT_FALSE(trader.isOwnerRoutingEnabled());
		ASSERT_NO_FATAL_FAILURE(arm(*b, mgr, "s4_vwl_b"));
		ASSERT_NO_FATAL_FAILURE(arm(*a, mgr, "s4_vwl_a"));
		ASSERT_NO_FATAL_FAILURE(tickTo(*b, 93100000, 10.0));
		ASSERT_EQ(1u, api.usertags.size());
		pushLive(trader, api.usertags[0], "S4-VLB-1", api.entrustIds[0]);

		ASSERT_NO_FATAL_FAILURE(tickTo(*a, 93100500, 10.0));
		EXPECT_EQ(1u, api.usertags.size()) << "legacy: A treats B's order as its own pending order";
		EXPECT_DOUBLE_EQ(100.0, a->getUndoneQty(kStk));
		EXPECT_TRUE(api.cancels.empty());
	}
	trader.release();
}

// Gate note (not a product change): with the legacy NULL login order snapshot the
// adapter has no order map, getOrders is NULL and the unit fails closed by design until
// a reliable snapshot or an order report exists; a K42 COMPLETE empty query satisfies it.
TEST_F(S4StockVWapOwnerE2E, WithoutAnOrderSnapshotTheUnitFailsClosedBeforeItsFirstOrder)
{
	ASSERT_NO_FATAL_FAILURE(rewriteProfile(100.0));
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "s4_svwap_nosnap", true, false));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<S4LoggingLocal> a = makeVwap("s4_vwn_a", trader, mgr, 240, 100.0, true);
		S4ExecTeardown teardown{ trader, mgr, { &a } };
		ASSERT_NO_FATAL_FAILURE(arm(*a, mgr, "s4_vwn_a"));
		ASSERT_NO_FATAL_FAILURE(tickTo(*a, 93100000, 10.0));
		EXPECT_TRUE(api.usertags.empty());
		EXPECT_TRUE(a->logged("getOrders returned null"));
	}
	trader.release();
}
