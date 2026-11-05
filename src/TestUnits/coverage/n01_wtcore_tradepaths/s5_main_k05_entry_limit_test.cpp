/**
 * 迭代03 第五会话主线程：决策 K05=C（2026-11-04 用户裁定，BUG-P02-005 / BUG-R202-003）
 * 账户净持仓限额：账户级配置 + 执行器目标层（B，既有），再加下单入口“实际持仓 + 在途”复核（C）。
 * 改前下单入口不复核，执行器目标层之外（如直接下单、在途叠加）可越过账户限额。
 */
#include "r71_wtcore_env.h"

#include <WtCore/AccountNetPositionLimits.h>

#include <climits>
#include <memory>

using namespace wtp;

namespace {

class S5MainK05EntryLimit : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	int ownerTag = 0;	// address used as the registry owner key

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
	}

	void TearDown() override { account_net_limits::Registry::instance().release(&ownerTag); }

	// An executer on this account declares `limit` for the contract (published as by init()).
	void declare(TraderAdapter& trader, double limit)
	{
		account_net_limits::Limits limits;
		limits.by_key["SHFE.ag.2512"] = limit;
		account_net_limits::Positions targets;
		account_net_limits::Positions previous;
		const void* previousAccount = nullptr;
		std::string error;
		std::shared_ptr<AccountTradingGate> gate = trader.accountGate();
		ASSERT_TRUE(account_net_limits::Registry::instance().apply(gate.get(), gate, &ownerTag, "k05",
			limits, targets, previous, previousAccount, error, true)) << error;
	}
};

} // namespace

TEST_F(S5MainK05EntryLimit, EntryAboveTheAccountLimitIsRejectedAndReductionsStayAllowed)
{
	TraderAdapter trader;
	ASSERT_TRUE(traderEnv.build(trader, "k05_entry"));
	declare(trader, 2.0);

	EXPECT_EQ(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 3.0, 0)) << "3 lots exceed the account limit 2";
	const uint32_t first = trader.openLong("SHFE.ag.2512", 3500.0, 2.0, 0);
	EXPECT_NE(UINT_MAX, first) << "2 lots are within the limit";
	EXPECT_EQ(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 1.0, 0))
		<< "in-flight 2 plus 1 would exceed the limit even though no position exists yet";
	EXPECT_NE(UINT_MAX, trader.openShort("SHFE.ag.2512", 3500.0, 1.0, 0))
		<< "an entry that reduces |net| is always allowed";
	trader.release();
}

TEST_F(S5MainK05EntryLimit, WithoutADeclaredLimitEntriesAreNotRestricted)
{
	TraderAdapter trader;
	ASSERT_TRUE(traderEnv.build(trader, "k05_none"));
	EXPECT_NE(UINT_MAX, trader.openLong("SHFE.ag.2512", 3500.0, 5.0, 0));
	trader.release();
}
