/**
 * 迭代03 第五会话主线程：决策 K44（2026-11-04 用户裁定，BUG-I02-NEW-008 / I03-NEW-088）
 * owner 路由开启时，HFT 与人工裸 API 订单参与合约归属：
 *   - HFT 上下文以 "hft:<名称>" 独占认领所下合约，并声明为该 owner 的 sink，只收到自己订单的回报；
 *   - 无 owner 作用域的裸下单以账户级 "manual" 独占认领；
 *   - 路由关闭时两者行为不变（不认领）。
 * 改前：路由开启时 HFT/裸下单不认领合约，执行器可与之交错交易同一合约；HFT 上下文作为非 owner 观察者
 * 收到执行器订单的成交并计入自身持仓（I03-NEW-088）。
 */
#include "r71_wtcore_env.h"

#include <WtCore/HftStraContext.h>

#include <climits>
#include <map>

using namespace wtp;

namespace {

TraderAdapter::OwnerRoutingCapabilities k44Capabilities()
{
	TraderAdapter::OwnerRoutingCapabilities caps;
	caps.durable_journal = true;
	caps.local_order_id_floor = true;
	caps.provider_user_tag_round_trip = true;
	caps.stable_trade_id_across_push_and_query = true;
	return caps;
}

// Every accepted insert is filled synchronously with a trade that carries the
// entrust's user tag, as a provider reports it.
class K44FillingApi : public r71::R71FakeTraderApi
{
public:
	ITraderSpi* spi = nullptr;
	r71::R71BaseDataMgr* bd = nullptr;
	uint32_t trade_seq = 0;

	void registerSpi(ITraderSpi* listener) override { spi = listener; }

	int orderInsert(WTSEntrust* entrust) override
	{
		r71::R71FakeTraderApi::orderInsert(entrust);
		if (spi == nullptr || bd == nullptr)
			return 0;
		WTSContractInfo* ct = bd->getContract(entrust->getCode(), entrust->getExchg());
		WTSTradeInfo* trade = WTSTradeInfo::create(entrust->getCode(), entrust->getExchg());
		trade->setContractInfo(ct);
		trade->setUserTag(entrust->getUserTag());
		const std::string tid = "K44-T" + std::to_string(++trade_seq);
		trade->setTradeID(tid.c_str());
		trade->setRefOrder(tid.c_str());
		trade->setVolume(entrust->getVolume());
		trade->setPrice(entrust->getPrice());
		trade->setDirection(entrust->getDirection());
		trade->setOffsetType(entrust->getOffsetType());
		trade->setTradeDate(20250110);
		trade->setTradeTime(93100000);
		spi->onPushTrade(trade);
		trade->release();
		return 0;
	}
};

class K44HftCtx : public HftStraContext
{
public:
	K44HftCtx(WtHftEngine* engine, const char* name) : HftStraContext(engine, name, true, 0) {}

	std::map<std::string, double> booked;	// volume this context accounted per code

	void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override
	{
		booked[stdCode ? stdCode : ""] += vol;
		HftStraContext::on_trade(localid, stdCode, isBuy, vol, price);
	}
};

class S5MainK44OwnerParticipation : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	r71::R71DataMgr dm;
	r71::R71HftEngine engine;
	K44FillingApi api;

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
		api.bd = &traderEnv.bd;
	}

	bool build(TraderAdapter& trader, const char* id, bool routing)
	{
		if (!traderEnv.policy_ok() || !trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !trader.run())
			return false;
		trader.onLoginResult(true, "k44", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		if (trader.state() != TraderAdapter::AS_ALLREADY)
			return false;
		return !routing || trader.configureOwnerRoutingCapabilities(k44Capabilities());
	}
};

} // namespace

TEST_F(S5MainK44OwnerParticipation, ManualEntryUnderRoutingClaimsTheContractExclusively)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k44_manual_first", true));
	ASSERT_NE(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 1.0, 0));
	EXPECT_TRUE(trader.buyAsOwner("exec:k44", true, "SHFE.ag.2512", 3500.0, 1.0, 0, false).empty())
		<< "an executer may not trade a contract the manual owner holds";
	EXPECT_EQ(1u, trader.buyAsOwner("exec:k44", true, "SHFE.au.2212", 400.0, 1.0, 0, false).size())
		<< "other contracts stay free";
	trader.release();
}

TEST_F(S5MainK44OwnerParticipation, ExecuterOwnedContractRejectsManualEntryUnderRouting)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k44_exec_first", true));
	ASSERT_EQ(1u, trader.buyAsOwner("exec:k44", true, "SHFE.ag.2512", 3500.0, 1.0, 0, false).size());
	EXPECT_EQ(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 1.0, 0));
	EXPECT_NE(UINT_MAX, trader.openLong("SHFE.au.2212", 400.0, 1.0, 0));
	trader.release();
}

TEST_F(S5MainK44OwnerParticipation, WithoutRoutingManualAndHftEntriesDoNotClaim)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k44_no_routing", false));
	K44HftCtx ctx(&engine, "k44_plain");
	ctx.setTrader(&trader);
	ctx.on_init();
	trader.addSink(&ctx);
	ASSERT_EQ(1u, ctx.stra_buy("SHFE.ag.2512", 3500.0, 1.0, "k44").size());
	ASSERT_NE(UINT_MAX, trader.openLong("SHFE.au.2212", 400.0, 1.0, 0));
	EXPECT_EQ(1u, trader.buyAsOwner("exec:k44", false, "SHFE.ag.2512", 3500.0, 1.0, 0, false).size())
		<< "without routing HFT orders do not claim the contract";
	EXPECT_NE(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 1.0, 0))
		<< "without routing manual orders are not restricted by ownership";
	trader.removeSink(&ctx);
	trader.release();
}

TEST_F(S5MainK44OwnerParticipation, HftContextUnderRoutingOwnsItsContractAndSeesOnlyItsOwnFills)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k44_hft", true));
	K44HftCtx ctx(&engine, "k44_hft_a");
	ctx.setTrader(&trader);
	ctx.on_init();
	trader.addSink(&ctx);

	ASSERT_EQ(1u, ctx.stra_buy("SHFE.ag.2512", 3500.0, 1.0, "k44").size());
	EXPECT_DOUBLE_EQ(1.0, ctx.booked["SHFE.ag.2512"]) << "the context receives the fill of its own order";
	EXPECT_TRUE(trader.buyAsOwner("exec:k44", true, "SHFE.ag.2512", 3500.0, 1.0, 0, false).empty())
		<< "an executer may not trade a contract the HFT context holds";
	EXPECT_EQ(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 1.0, 0)) << "neither may a manual entry";

	ASSERT_EQ(1u, trader.buyAsOwner("exec:k44", true, "SHFE.au.2212", 400.0, 2.0, 0, false).size());
	EXPECT_DOUBLE_EQ(0.0, ctx.booked["SHFE.au.2212"])
		<< "I03-NEW-088: an executer's fill is not accounted into the HFT context";
	EXPECT_EQ(UINT_MAX, ctx.stra_enter_long("SHFE.au.2212", 400.0, 1.0, "k44", 0))
		<< "direct HFT entries also respect the executer's claim";
	trader.removeSink(&ctx);
	trader.release();
}
