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
 */
#include "r71_wtcore_env.h"

#include <WtCore/ExecOwnerModes.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WtCore/WtLocalExecuter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <cstdio>
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
		return r71::R71FakeTraderApi::orderInsert(entrust);
	}
	int orderAction(WTSEntrustAction* action) override
	{
		cancels.push_back(action->getEntrustID());
		return r71::R71FakeTraderApi::orderAction(action);
	}
	std::vector<std::string> usertags;
	std::vector<std::string> cancels;
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

class S4StockVWapOwnerE2E : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71PluginDir plug;
	r71::R71TraderEnv traderEnv;
	r71::R71ExecStub stub;
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

	static std::string profileFor(WTSSessionInfo& session)
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
			out << (i != 0 ? "," : "") << "1.0";
		out << "]}]}";
		return out.str();
	}

	bool build(TraderAdapter& trader, const char* id, bool routing)
	{
		if (!traderEnv.policy_ok() || !trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !trader.run())
			return false;
		trader.onLoginResult(true, "s4", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		if (trader.state() != TraderAdapter::AS_ALLREADY)
			return false;
		return !routing || trader.configureOwnerRoutingCapabilities(s4VwapCaps());
	}

	// The provider reports the other owner's order as live (queuing, nothing traded).
	void pushLive(TraderAdapter& trader, const std::string& usertag, const char* orderId)
	{
		WTSOrderInfo* order = WTSOrderInfo::create();
		order->setContractInfo(stk);
		order->setDirection(WDT_LONG);
		order->setOffsetType(WOT_OPEN);
		order->setPrice(10.0);
		order->setVolume(100.0);
		order->setVolTraded(0.0);
		order->setVolLeft(100.0);
		order->setUserTag(usertag.c_str());
		order->setOrderID(orderId);
		order->setOrderState(WOS_NotTraded_Queuing);
		trader.onPushOrder(order);
		order->release();
	}

	std::shared_ptr<S4LoggingLocal> makeVwap(const char* name, TraderAdapter& trader, WtExecuterMgr& mgr)
	{
		std::shared_ptr<S4LoggingLocal> local(new S4LoggingLocal(&factory, name, &dm));
		mgr.add_executer(local);
		local->setStub(&stub);
		const std::string json = "{\"scale\":1.0,\"poolsize\":0,\"policy\":{\"default\":{"
			"\"name\":\"WtExeFact.WtStockVWapExeUnit\",\"begin_time\":930,\"end_time\":1500,"
			"\"ord_sticky\":0,\"tail_secs\":0,\"total_times\":1,\"price_mode\":0,\"price_offset\":0,"
			"\"lots\":1.0,\"minopenlots\":1.0,\"offset\":0}}}";
		WTSVariant* params = WTSCfgLoader::load_from_content(json);
		EXPECT_NE(params, nullptr);
		if (params == nullptr)
			return local;
		EXPECT_TRUE(local->init(params));
		params->release();
		local->setTrader(&trader);
		return local;
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
