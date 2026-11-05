/**
 * 迭代03 第五会话主线程：决策 K07 d（2026-11-04 用户裁定，BUG-Q04-002 / BUG-I02-NEW-030）
 * WtCore 同步回报口径统一为 UFT 现行口径：orderInsert 调用期间只见到拒单/撤单且无接受证据才返回 UINT_MAX；
 * 一旦见到存活委托或成交，即使随后撤单且返回负值也保留订单。改前 WtCore 见到拒单/撤单即返回 UINT_MAX。
 */
#include "r71_wtcore_env.h"

#include <climits>

using namespace wtp;

namespace {

class K07SyncApi : public r71::R71FakeTraderApi
{
public:
	ITraderSpi* spi = nullptr;
	WTSContractInfo* contract = nullptr;	// providers attach the contract to every report
	bool reportAcceptance = true;

	void registerSpi(ITraderSpi* listener) override { spi = listener; }

	int orderInsert(WTSEntrust* entrust) override
	{
		r71::R71FakeTraderApi::orderInsert(entrust);
		if (spi != nullptr)
		{
			if (reportAcceptance)
			{
				WTSOrderInfo* live = WTSOrderInfo::create(entrust);
				live->setCode("ag2512");	// raw contract code, as a provider reports it
				live->setContractInfo(contract);
				live->setOrderID("K07D-1");
				live->setOrderState(WOS_PartTraded_Queuing);
				live->setVolTraded(1);
				live->setVolLeft(entrust->getVolume() - 1);
				spi->onPushOrder(live);
				live->release();

				WTSTradeInfo* trade = WTSTradeInfo::create("ag2512", "SHFE");
				trade->setContractInfo(contract);
				trade->setUserTag(entrust->getUserTag());
				trade->setTradeID("K07D-T1");
				trade->setRefOrder("K07D-1");
				trade->setVolume(1);
				trade->setPrice(entrust->getPrice());
				trade->setDirection(entrust->getDirection());
				trade->setOffsetType(entrust->getOffsetType());
				spi->onPushTrade(trade);
				trade->release();
			}

			WTSOrderInfo* dead = WTSOrderInfo::create(entrust);
			dead->setCode("ag2512");
			dead->setContractInfo(contract);
			dead->setOrderID("K07D-1");
			dead->setOrderState(WOS_Canceled);
			dead->setVolTraded(reportAcceptance ? 1 : 0);
			dead->setVolLeft(0);
			spi->onPushOrder(dead);
			dead->release();
		}
		return -1;
	}
};

class S5MainK07dSyncReport : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	K07SyncApi api;

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
		traderEnv.bd.regisAlias("SHFE.ag.2512", ct);
		api.contract = ct;
	}

	bool build(TraderAdapter& trader, const char* id)
	{
		if (!traderEnv.policy_ok() || !trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !trader.run())
			return false;
		trader.onLoginResult(true, "k07d", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		return trader.state() == TraderAdapter::AS_ALLREADY;
	}
};

} // namespace

TEST_F(S5MainK07dSyncReport, AcceptedThenCanceledDuringInsertKeepsTheOrder)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k07d_keep"));
	api.reportAcceptance = true;
	const uint32_t localid = trader.openLong("SHFE.ag.2512", 3500.0, 2.0, 0);
	EXPECT_NE(UINT_MAX, localid) << "a fill reported during orderInsert is evidence of acceptance; the order is kept";
	trader.release();
}

TEST_F(S5MainK07dSyncReport, RejectedWithoutAcceptanceDuringInsertFails)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k07d_reject"));
	api.reportAcceptance = false;
	EXPECT_EQ(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 2.0, 0));
	trader.release();
}
