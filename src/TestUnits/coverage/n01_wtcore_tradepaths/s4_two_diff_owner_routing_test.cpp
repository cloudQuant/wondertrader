/**
 * BUG-I02-NEW-008 (iteration 03, 2026-11-05): end-to-end owner routing with two
 * real WtDiffExecuter instances sharing one TraderAdapter and one standard
 * contract (card 5c: same account + same contract may be shared by identity-
 * capable executers; every report is booked by the executer that sent the
 * order). The units are the real WtExeFact.WtDiffMinImpactExeUnit (it declares
 * WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY); the provider is a local fake API.
 * Covered: targets through the ALL route and through cached targets
 * (WtExecuterMgr::add_target_to_cache / commit_cached_targets with one explicit
 * and one ALL entry), order / trade / entrust-failure reports, the owner-scoped
 * order snapshot (ExecuteContext::getOrders) and the owner-scoped account-level
 * cancel ExecuteContext::cancel(code, isBuy) that StockVWap and the other units
 * use, and an unresolvable-owner trade that must not reach either ledger.
 * Before owner routing every report reached both executers (2026-10-31 probe:
 * A's fill moved B's diff 3/5 -> 2/4); the probe is reproduced here with routing
 * disabled as the red control case.
 */
#include "r71_wtcore_env.h"

#include <WtCore/ExecOwnerModes.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtExecuterFactory.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* const kCode = "SHFE.ag.2512";

TraderAdapter::OwnerRoutingCapabilities s4Caps()
{
	TraderAdapter::OwnerRoutingCapabilities caps;
	caps.durable_journal = true;
	caps.local_order_id_floor = true;
	caps.provider_user_tag_round_trip = true;
	caps.stable_trade_id_across_push_and_query = true;
	return caps;
}

struct S4Insert
{
	std::string usertag;
	double price = 0;
	double volume = 0;
	WTSDirectionType dir = WDT_LONG;
	WTSOffsetType off = WOT_OPEN;
};

class S4RecordingApi final : public r71::R71FakeTraderApi
{
public:
	int orderInsert(WTSEntrust* entrust) override
	{
		S4Insert ins;
		ins.usertag = entrust->getUserTag();
		ins.price = entrust->getPrice();
		ins.volume = entrust->getVolume();
		ins.dir = entrust->getDirection();
		ins.off = entrust->getOffsetType();
		inserts.push_back(ins);
		return r71::R71FakeTraderApi::orderInsert(entrust);
	}
	int orderAction(WTSEntrustAction* action) override
	{
		cancels.push_back(action->getEntrustID());
		return r71::R71FakeTraderApi::orderAction(action);
	}
	std::vector<S4Insert> inserts;
	std::vector<std::string> cancels;
};

class S4TwoDiffOwnerRouting : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71PluginDir plug;
	r71::R71TraderEnv traderEnv;
	r71::R71ExecStub stub;
	r71::R71DataMgr dm;
	WtExecuterFactory factory;
	WTSContractInfo* ag = NULL;

	void SetUp() override
	{
		ASSERT_TRUE(plug.ok()) << "libWtExeFact is required for the real units";
		ASSERT_TRUE(factory.loadFactories(plug.dir().c_str()));
		traderEnv.bd.regisSession("TRADING");
		WTSSessionInfo* sess = traderEnv.bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		WTSCommodityInfo* comm = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		comm->setSessionInfo(sess);
		ag = r71::makeCt("SHFE", "ag", "ag2512");
		traderEnv.bd.regis(comm, ag);
		traderEnv.bd.regisAlias(kCode, ag);
		stub.comm_ = comm;
		stub.sess_ = sess;
		stub.hot_ = &traderEnv.hot;
	}

	bool build(TraderAdapter& trader, S4RecordingApi& api, const char* id, bool routing)
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
		return !routing || trader.configureOwnerRoutingCapabilities(s4Caps());
	}

	WTSVariant* cfg(const char* name)
	{
		const std::string json = std::string("{\"scale\":1.0,\"poolsize\":0,")
			+ "\"policy\":{\"default\":{\"name\":\"WtExeFact.WtDiffMinImpactExeUnit\","
			  "\"lots\":5,\"minopenlots\":1,\"offset\":0,\"expire\":0,"
			  "\"pricemode\":0,\"span\":0,\"byrate\":false}}}";
		const std::string file = r71::temp_root() + name + "_" + std::to_string((long)::getpid()) + ".json";
		if (!r71::write_file(file, json))
			return NULL;
		return WTSCfgLoader::load_from_file(file.c_str());
	}

	std::shared_ptr<WtDiffExecuter> makeDiff(const char* name, TraderAdapter& trader, WtExecuterMgr& mgr)
	{
		std::shared_ptr<WtDiffExecuter> diff(new WtDiffExecuter(&factory, name, &dm, &traderEnv.bd));
		mgr.add_executer(diff);
		diff->setStub(&stub);
		WTSVariant* c = cfg(name);
		EXPECT_NE(c, nullptr);
		if (c == NULL)
			return diff;
		EXPECT_TRUE(diff->init(c));
		c->release();
		diff->setTrader(&trader);
		trader.addSink(diff.get());
		return diff;
	}

	void tickTo(WtDiffExecuter& diff, uint32_t atime, double price)
	{
		WTSTickData* t = r71::makeTick("SHFE", kCode, 20250110, 20250110, atime, price);
		ASSERT_NE(t, nullptr);
		diff.on_tick(kCode, t);
		t->release();
	}

	// Executer diff ledger as persisted in its own snapshot (WtHelper exec data dir).
	static double persistedDiff(const char* executer)
	{
		const std::string file = WtHelper::getExecDataDir() + std::string(executer) + ".json";
		WTSVariant* root = WTSCfgLoader::load_from_file(file.c_str());
		if (root == NULL)
			return 0.0;
		double value = 0.0;
		WTSVariant* diffs = root->get("diffs");
		for (uint32_t i = 0; diffs != NULL && i < diffs->size(); i++)
		{
			WTSVariant* item = diffs->get(i);
			if (item != NULL && strcmp(item->getCString("code"), kCode) == 0)
				value = item->getDouble("diff");
		}
		root->release();
		return value;
	}

	static std::size_t ownOrders(WtDiffExecuter& diff)
	{
		OrderMap* orders = diff.getOrders(kCode);
		if (orders == NULL)
			return 0;
		const std::size_t n = orders->size();
		orders->release();
		return n;
	}

	void pushOrder(TraderAdapter& trader, const S4Insert& ins, const char* orderId, double traded, WTSOrderState state)
	{
		WTSOrderInfo* order = WTSOrderInfo::create();
		order->setContractInfo(ag);
		order->setDirection(ins.dir);
		order->setOffsetType(ins.off);
		order->setPrice(ins.price);
		order->setVolume(ins.volume);
		order->setVolTraded(traded);
		order->setVolLeft(ins.volume - traded);
		order->setUserTag(ins.usertag.c_str());
		order->setOrderID(orderId);
		order->setOrderState(state);
		trader.onPushOrder(order);
		order->release();
	}

	void pushTrade(TraderAdapter& trader, const S4Insert& ins, const char* tradeId, const char* orderId, double vol)
	{
		WTSTradeInfo* trade = WTSTradeInfo::create(ag->getCode(), ag->getExchg());
		trade->setContractInfo(ag);
		trade->setUserTag(ins.usertag.c_str());
		trade->setTradeID(tradeId);
		trade->setRefOrder(orderId);
		trade->setDirection(ins.dir);
		trade->setOffsetType(ins.off);
		trade->setPrice(ins.price);
		trade->setVolume(vol);
		trade->setTradeDate(20250110);
		trade->setTradeTime(93100000);
		trader.onPushTrade(trade);
		trade->release();
	}

	static void quiesce(WtExecuterMgr& mgr)
	{
		WtExecQuiesceCoordinator coordinator({ &mgr });
		ASSERT_TRUE(coordinator.quiesce_all());
		ASSERT_TRUE(coordinator.is_complete());
	}
};

} // namespace

TEST_F(S4TwoDiffOwnerRouting, ReportsCancelsAndSnapshotsStayWithTheSendingExecuter)
{
	S4RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s4_two_diff", true));
	WtExecuterMgr mgr;
	auto a = makeDiff("s4_diff_a", trader, mgr);
	auto b = makeDiff("s4_diff_b", trader, mgr);
	ASSERT_TRUE(a && b);

	// ALL route: both executers receive the increment; each sends its own order.
	mgr.handle_pos_change(kCode, 3.0, 3.0, "ALL");
	ASSERT_DOUBLE_EQ(3.0, persistedDiff("s4_diff_a"));
	ASSERT_DOUBLE_EQ(3.0, persistedDiff("s4_diff_b"));
	tickTo(*a, 93100000, 3500.0);
	ASSERT_EQ(1u, api.inserts.size()) << "executer A sends";
	const S4Insert oa = api.inserts.back();
	tickTo(*b, 93100500, 3500.0);
	ASSERT_EQ(2u, api.inserts.size()) << "card 5c: B may trade the same contract on the same account";
	const S4Insert ob = api.inserts.back();
	EXPECT_NE(oa.usertag, ob.usertag);
	EXPECT_DOUBLE_EQ(oa.volume, a->getUndoneQty(kCode));
	EXPECT_DOUBLE_EQ(ob.volume, b->getUndoneQty(kCode));

	// Order reports are visible only to the executer that sent the order.
	pushOrder(trader, oa, "S4-A-1", 0.0, WOS_NotTraded_Queuing);
	pushOrder(trader, ob, "S4-B-1", 0.0, WOS_NotTraded_Queuing);
	EXPECT_EQ(1u, ownOrders(*a));
	EXPECT_EQ(1u, ownOrders(*b));

	// A's fill moves only A's diff (the 2026-10-31 probe moved both).
	pushTrade(trader, oa, "S4-T-A1", "S4-A-1", 1.0);
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_diff_a"));
	EXPECT_DOUBLE_EQ(3.0, persistedDiff("s4_diff_b")) << "B's ledger must not see A's fill";
	EXPECT_DOUBLE_EQ(oa.volume - 1.0, a->getUndoneQty(kCode));
	EXPECT_DOUBLE_EQ(ob.volume, b->getUndoneQty(kCode));

	// Owner-scoped account-level cancel (the unit path used by StockVWap): A's
	// cancel(code, isBuy) touches only A's live order.
	const std::size_t cancelsBefore = api.cancels.size();
	OrderIDs cancelled = a->cancel(kCode, oa.dir == WDT_LONG && oa.off == WOT_OPEN, 0);
	EXPECT_EQ(1u, cancelled.size());
	EXPECT_EQ(cancelsBefore + 1, api.cancels.size());
	EXPECT_EQ(1u, ownOrders(*b)) << "B's same-contract order stays live";

	// B's entrust failure releases only B's reservation.
	WTSEntrust* rejected = WTSEntrust::create(ag->getCode(), ob.volume, ob.price, ag->getExchg());
	rejected->setUserTag(ob.usertag.c_str());
	rejected->setDirection(ob.dir);
	rejected->setOffsetType(ob.off);
	WTSError* err = WTSError::create(WEC_ORDERINSERT, "s4 rejected");
	trader.onRspEntrust(rejected, err);
	err->release();
	rejected->release();
	EXPECT_DOUBLE_EQ(0.0, b->getUndoneQty(kCode));
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_diff_a"));
	EXPECT_DOUBLE_EQ(3.0, persistedDiff("s4_diff_b"));

	// A trade whose owner cannot be resolved (tag of no ledger entry) reaches neither ledger.
	S4Insert stranger = oa;
	stranger.usertag = oa.usertag.substr(0, oa.usertag.rfind('.') + 1) + "999999";
	pushTrade(trader, stranger, "S4-T-X", "S4-X", 1.0);
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_diff_a"));
	EXPECT_DOUBLE_EQ(3.0, persistedDiff("s4_diff_b"));
	EXPECT_TRUE(trader.isUnresolvedReportPaused()) << "an unresolvable owner pauses the account (card 6 / F4)";

	trader.removeSink(a.get());
	trader.removeSink(b.get());
	a.reset();
	b.reset();
	ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	trader.release();
}

TEST_F(S4TwoDiffOwnerRouting, CachedTargetsRouteExplicitAndAllEntriesToTheirOwnLedgers)
{
	S4RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s4_two_diff_cached", true));
	WtExecuterMgr mgr;
	auto a = makeDiff("s4_cdiff_a", trader, mgr);
	auto b = makeDiff("s4_cdiff_b", trader, mgr);
	ASSERT_TRUE(a && b);

	mgr.add_target_to_cache(kCode, 2.0, "s4_cdiff_a");
	mgr.add_target_to_cache(kCode, 4.0, "ALL");
	mgr.commit_cached_targets();
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_cdiff_a")) << "the explicit entry wins for A";
	EXPECT_DOUBLE_EQ(4.0, persistedDiff("s4_cdiff_b")) << "B falls back to the ALL entry";

	tickTo(*a, 93100000, 3500.0);
	tickTo(*b, 93100500, 3500.0);
	ASSERT_EQ(2u, api.inserts.size());
	const S4Insert oa = api.inserts[0];
	const S4Insert ob = api.inserts[1];
	pushOrder(trader, ob, "S4-CB-1", 0.0, WOS_NotTraded_Queuing);
	pushTrade(trader, ob, "S4-CT-B1", "S4-CB-1", 1.0);
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_cdiff_a")) << "A's ledger must not see B's fill";
	EXPECT_DOUBLE_EQ(3.0, persistedDiff("s4_cdiff_b"));
	EXPECT_EQ(0u, ownOrders(*a));
	EXPECT_EQ(1u, ownOrders(*b));
	EXPECT_DOUBLE_EQ(oa.volume, a->getUndoneQty(kCode));

	trader.removeSink(a.get());
	trader.removeSink(b.get());
	a.reset();
	b.reset();
	ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	trader.release();
}

// Red control: the 2026-10-31 cross-owner probe. Without owner routing the
// adapter broadcasts to both executers and A's fill moves B's diff too. This
// documents the pre-routing defect; it passes because it asserts the old
// behaviour of the routing-disabled (default) mode, which K44 keeps unchanged.
TEST_F(S4TwoDiffOwnerRouting, WithoutRoutingAFillReachesBothLedgersAsBefore)
{
	S4RecordingApi api;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, api, "s4_two_diff_plain", false));
	WtExecuterMgr mgr;
	auto a = makeDiff("s4_pdiff_a", trader, mgr);
	auto b = makeDiff("s4_pdiff_b", trader, mgr);
	ASSERT_TRUE(a && b);

	mgr.handle_pos_change(kCode, 3.0, 3.0, "ALL");
	tickTo(*a, 93100000, 3500.0);
	ASSERT_EQ(1u, api.inserts.size());
	const S4Insert oa = api.inserts.back();
	pushOrder(trader, oa, "S4-PA-1", 0.0, WOS_NotTraded_Queuing);
	pushTrade(trader, oa, "S4-PT-A1", "S4-PA-1", 1.0);
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_pdiff_a"));
	EXPECT_DOUBLE_EQ(2.0, persistedDiff("s4_pdiff_b")) << "routing disabled: the broadcast also books into B";

	trader.removeSink(a.get());
	trader.removeSink(b.get());
	a.reset();
	b.reset();
	ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	trader.release();
}
