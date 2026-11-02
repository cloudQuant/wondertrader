#include "r54_trader_fixture.h"

using namespace wtp;

TEST(q04_trader_contract_info, push_trade_uses_embedded_contract_when_base_data_misses)
{
	r54::Env env("contract_info_priority");
	env.add("SHFE", "ag", "ag2406");
	ASSERT_TRUE(env.bd.removeContract("SHFE", "ag2406"));

	// 模拟 provider 在成交回报中携带合同元数据，但适配器自己的基础数据已没有该合约。
	q04::FakeBaseDataMgr callbackMetadata;
	WTSContractInfo* embeddedContract = callbackMetadata.add("DCE", "rb", "rb2510");
	ASSERT_NE(nullptr, embeddedContract);
	const uint32_t accountQueriesBefore = env.api._qAccCnt;

	WTSTradeInfo* trade = WTSTradeInfo::create("rb2510", "DCE");
	trade->setContractInfo(embeddedContract);
	trade->setDirection(WDT_LONG);
	trade->setOffsetType(WOT_OPEN);
	trade->setVolume(2.0);
	trade->setPrice(3500.0);
	EXPECT_EQ(nullptr, env.bd.getContract("rb2510", "DCE"));

	env.adapter.onPushTrade(trade);

	ASSERT_EQ(1u, env.sink._trades.size());
	EXPECT_EQ("DCE.rb2510", env.sink._trades.front()._code);
	EXPECT_TRUE(env.sink._trades.front()._long);
	EXPECT_DOUBLE_EQ(2.0, env.adapter.getPosition("DCE.rb2510", false, 1));
	EXPECT_EQ(accountQueriesBefore + 1, env.api._qAccCnt);
	trade->release();
}

TEST(q04_trader_contract_info, push_trade_without_embedded_or_registered_contract_is_ignored)
{
	r54::Env env("contract_info_missing");
	const uint32_t accountQueriesBefore = env.api._qAccCnt;
	WTSTradeInfo* trade = WTSTradeInfo::create("r2510", "DCE");
	ASSERT_EQ(nullptr, trade->getContractInfo());

	env.adapter.onPushTrade(trade);

	EXPECT_TRUE(env.sink._trades.empty());
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getPosition("DCE.r2510", false, 1));
	EXPECT_EQ(accountQueriesBefore, env.api._qAccCnt);
	trade->release();
}
