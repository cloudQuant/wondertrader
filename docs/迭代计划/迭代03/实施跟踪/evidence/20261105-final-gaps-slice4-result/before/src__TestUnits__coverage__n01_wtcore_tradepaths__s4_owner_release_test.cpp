/**
 * I03-NEW-089 (iteration 03, 2026-11-05): with owner routing enabled an
 * executer or HFT context claims the contracts it trades on the account gate
 * (K44, card 5c), but nothing ever called releaseOwner(), so a destroyed
 * executer or context kept the contract blocked for every other owner for the
 * rest of the process. Contract implemented here: an executer (Local, Arbi) or
 * HFT context returns its own claims when its lifetime ends (destruction) or
 * when it is rebound to a trader of another account gate. Account-level manual
 * claims are not released (their release is not decided; see the control case).
 *
 * WtDiffExecuter is covered by the same contract but its files were locked by
 * another work package when this case was written.
 */
#include "r71_wtcore_env.h"

#include <WtCore/ExecOwnerModes.h>
#include <WtCore/HftStraContext.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtExecMgr.h>
#include <WtCore/WtLocalExecuter.h>

#include <climits>
#include <memory>

using namespace wtp;

namespace {

TraderAdapter::OwnerRoutingCapabilities s4Capabilities()
{
	TraderAdapter::OwnerRoutingCapabilities caps;
	caps.durable_journal = true;
	caps.local_order_id_floor = true;
	caps.provider_user_tag_round_trip = true;
	caps.stable_trade_id_across_push_and_query = true;
	return caps;
}

class S4HftCtx : public HftStraContext
{
public:
	S4HftCtx(WtHftEngine* engine, const char* name) : HftStraContext(engine, name, true, 0) {}
};

const char* const kAg = "SHFE.ag.2512";
const char* const kAu = "SHFE.au.2212";

class S4OwnerRelease : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	r71::R71DataMgr dm;
	r71::R71HftEngine engine;
	r71::R71FakeTraderApi apiA;
	r71::R71FakeTraderApi apiB;
	WtExecuterFactory factory;

	void SetUp() override
	{
		traderEnv.bd.regisSession("TRADING");
		WTSSessionInfo* sess = traderEnv.bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		WTSCommodityInfo* ag = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		ag->setSessionInfo(sess);
		traderEnv.bd.regis(ag, r71::makeCt("SHFE", "ag", "ag2512"));
		WTSCommodityInfo* au = r71::makeComm("SHFE", "au", "TRADING", TM_Both);
		au->setSessionInfo(sess);
		traderEnv.bd.regis(au, r71::makeCt("SHFE", "au", "au2212"));
		engine.attach(&traderEnv.bd, &traderEnv.hot, &dm);
	}

	bool build(TraderAdapter& trader, r71::R71FakeTraderApi& api, const char* id)
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
		return trader.configureOwnerRoutingCapabilities(s4Capabilities());
	}

	static bool otherOwnerCanTrade(TraderAdapter& trader, const char* code)
	{
		return trader.buyAsOwner("exec:s4_other", true, code, 3500.0, 1.0, 0, false).size() == 1u;
	}

	// The manager owns the executer; destroying the manager after a completed
	// quiesce destroys the executer as a host teardown does.
	static void quiesce(WtExecuterMgr& mgr)
	{
		WtExecQuiesceCoordinator coordinator({ &mgr });
		ASSERT_TRUE(coordinator.quiesce_all());
		ASSERT_TRUE(coordinator.is_complete());
	}
};

} // namespace

TEST_F(S4OwnerRelease, HftContextReturnsItsClaimsWhenDestroyed)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, apiA, "s4_hft_destroy"));
	{
		S4HftCtx ctx(&engine, "s4_hft_a");
		ctx.setTrader(&trader);
		ctx.on_init();
		trader.addSink(&ctx);
		ASSERT_EQ(1u, ctx.stra_buy(kAg, 3500.0, 1.0, "s4").size());
		EXPECT_FALSE(otherOwnerCanTrade(trader, kAg)) << "the live context holds the contract";
		trader.removeSink(&ctx);
	}
	EXPECT_TRUE(otherOwnerCanTrade(trader, kAg)) << "a destroyed context no longer blocks the contract";
	trader.release();
}

TEST_F(S4OwnerRelease, HftContextReturnsClaimsOfThePreviousTraderWhenRebound)
{
	TraderAdapter traderA;
	TraderAdapter traderB;
	ASSERT_TRUE(build(traderA, apiA, "s4_hft_rebind_a"));
	ASSERT_TRUE(build(traderB, apiB, "s4_hft_rebind_b"));
	S4HftCtx ctx(&engine, "s4_hft_rebind");
	ctx.setTrader(&traderA);
	ctx.on_init();
	ASSERT_EQ(1u, ctx.stra_buy(kAg, 3500.0, 1.0, "s4").size());
	EXPECT_FALSE(otherOwnerCanTrade(traderA, kAg));

	ctx.setTrader(&traderB);
	EXPECT_TRUE(otherOwnerCanTrade(traderA, kAg)) << "the binding to trader A has ended";
	ASSERT_EQ(1u, ctx.stra_buy(kAu, 400.0, 1.0, "s4").size());
	EXPECT_FALSE(otherOwnerCanTrade(traderB, kAu)) << "the new binding claims on trader B as before";
	traderA.release();
	traderB.release();
}

TEST_F(S4OwnerRelease, LocalExecuterReturnsItsClaimsWhenDestroyed)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, apiA, "s4_local_destroy"));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<WtLocalExecuter> local(new WtLocalExecuter(&factory, "s4_local", &dm));
		mgr.add_executer(local);
		local->setTrader(&trader);
		// The executer's own entrust path (WtLocalExecuter::buy) submits under this owner id.
		ASSERT_EQ(1u, trader.buyAsOwner(ExecOwnerModes::ownerId("s4_local"), false, kAg, 3500.0, 1.0, 0, false).size());
		EXPECT_FALSE(otherOwnerCanTrade(trader, kAg));
		local.reset();
		ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	}
	EXPECT_TRUE(otherOwnerCanTrade(trader, kAg)) << "a destroyed executer no longer blocks the contract";
	trader.release();
}

TEST_F(S4OwnerRelease, ArbiExecuterReturnsItsClaimsWhenDestroyed)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, apiA, "s4_arbi_destroy"));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<WtArbiExecuter> arbi(new WtArbiExecuter(&factory, "s4_arbi", &dm));
		mgr.add_executer(arbi);
		arbi->setTrader(&trader);
		ASSERT_EQ(1u, trader.buyAsOwner(ExecOwnerModes::ownerId("s4_arbi"), false, kAg, 3500.0, 1.0, 0, false).size());
		EXPECT_FALSE(otherOwnerCanTrade(trader, kAg));
		arbi.reset();
		ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	}
	EXPECT_TRUE(otherOwnerCanTrade(trader, kAg));
	trader.release();
}

// Control (not a red case): account-level manual claims are outside this fix.
TEST_F(S4OwnerRelease, ManualClaimIsNotReleasedByAnExecuterTeardown)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, apiA, "s4_manual_kept"));
	ASSERT_NE(UINT_MAX, trader.openLong(kAg, 3500.0, 1.0, 0));
	{
		WtExecuterMgr mgr;
		std::shared_ptr<WtLocalExecuter> local(new WtLocalExecuter(&factory, "s4_local_manual", &dm));
		mgr.add_executer(local);
		local->setTrader(&trader);
		local.reset();
		ASSERT_NO_FATAL_FAILURE(quiesce(mgr));
	}
	EXPECT_FALSE(otherOwnerCanTrade(trader, kAg)) << "the manual claim keeps its current (undecided) lifetime";
	trader.release();
}
