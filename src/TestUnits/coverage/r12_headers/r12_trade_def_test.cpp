// Lane R12 - src/Includes/WTSTradeDef.hpp 交易对象
// 覆盖：WTSError 错误码/消息、WTSEntrust 字段读写与信用业务判定、净持仓方向标记、
//       WTSEntrustAction 撤单动作构造、WTSOrderInfo 由委托派生的字段与余量、
//       WTSTradeInfo 成交字段、WTSPositionItem 持仓派生量（总量/可用/冻结）、
//       WTSAccountInfo 资金字段
// 说明：不触碰交易通道，全部是纯内存对象换算；日期时间均为显式入参。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSTypes.h"

USING_NS_WTP;

TEST(r12_trade_error, code_and_message)
{
	WTSError* err = WTSError::create(WEC_ORDERINSERT, "trader not exists");
	ASSERT_NE(err, nullptr);
	EXPECT_EQ(err->getErrorCode(), WEC_ORDERINSERT);
	EXPECT_STREQ(err->getMessage(), "trader not exists");
	// 池对象：release 归还而不是 delete
	err->release();

	WTSError* e2 = WTSError::create(WEC_NONE, "");
	EXPECT_EQ(e2->getErrorCode(), WEC_NONE);
	EXPECT_STREQ(e2->getMessage(), "");
	e2->release();
}

TEST(r12_trade_entrust, create_defaults_and_field_writers)
{
	WTSEntrust* ent = WTSEntrust::create("SHFE.rb2405", 3, 3500.0, "SHFE");
	ASSERT_NE(ent, nullptr);
	EXPECT_STREQ(ent->getCode(), "SHFE.rb2405");
	EXPECT_DOUBLE_EQ(ent->getVolume(), 3.0);
	EXPECT_DOUBLE_EQ(ent->getPrice(), 3500.0);
	EXPECT_STREQ(ent->getExchg(), "SHFE");
	// 默认业务类型是普通买卖
	EXPECT_EQ(ent->getBusinessType(), BT_CASH);
	EXPECT_FALSE(ent->isCreditBusiness());

	ent->setCode("SHFE.rb2406", 11);
	EXPECT_STREQ(ent->getCode(), "SHFE.rb2406");
	ent->setExchange("SHFE", 4);
	EXPECT_STREQ(ent->getExchg(), "SHFE");
	ent->setVolume(5.0);
	ent->setPrice(3600.5);
	EXPECT_DOUBLE_EQ(ent->getVolume(), 5.0);
	EXPECT_DOUBLE_EQ(ent->getPrice(), 3600.5);

	ent->setDirection(WDT_SHORT);
	EXPECT_EQ(ent->getDirection(), WDT_SHORT);
	ent->setOffsetType(WOT_CLOSETODAY);
	EXPECT_EQ(ent->getOffsetType(), WOT_CLOSETODAY);
	ent->setPriceType(WPT_LIMITPRICE);
	EXPECT_EQ(ent->getPriceType(), WPT_LIMITPRICE);
	ent->setOrderFlag(WOF_FAK);
	EXPECT_EQ(ent->getOrderFlag(), WOF_FAK);

	ent->setEntrustID("1001");
	EXPECT_STREQ(ent->getEntrustID(), "1001");
	ent->setUserTag("stra01");
	EXPECT_STREQ(ent->getUserTag(), "stra01");
	// 可变指针重载可用于原地写入
	char* p = ent->getEntrustID();
	ASSERT_NE(p, nullptr);
	wt_strcpy(p, "1002");
	EXPECT_STREQ(ent->getEntrustID(), "1002");
	char* t = ent->getUserTag();
	ASSERT_NE(t, nullptr);
	wt_strcpy(t, "stra02");
	EXPECT_STREQ(ent->getUserTag(), "stra02");
	ent->release();
}

TEST(r12_trade_entrust, credit_business_and_net_direction)
{
	WTSEntrust* ent = WTSEntrust::create("SSE.600000", 100, 8.5, "SSE", BT_CREDIT);
	EXPECT_EQ(ent->getBusinessType(), BT_CREDIT);
	EXPECT_TRUE(ent->isCreditBusiness());
	ent->setBusinessType(BT_CREDIT_FIN);
	EXPECT_TRUE(ent->isCreditBusiness());
	ent->setBusinessType(BT_CREDIT_SLO);
	EXPECT_TRUE(ent->isCreditBusiness());
	ent->setBusinessType(BT_CREDIT_ORD);
	// BT_CREDIT_ORD 不在三个信用业务枚举里，不应被判成信用业务
	EXPECT_FALSE(ent->isCreditBusiness());
	ent->setBusinessType(BT_ETF);
	EXPECT_FALSE(ent->isCreditBusiness());

	ent->setCreditBalance(1234.56);
	EXPECT_DOUBLE_EQ(ent->getCreditBalance(), 1234.56);
	// 默认不是净额业务
	EXPECT_FALSE(ent->isNet());
	ent->setNetDirection(true);
	EXPECT_TRUE(ent->isNet());
	EXPECT_TRUE(ent->isNetBuy());
	ent->setNetDirection(false);
	EXPECT_TRUE(ent->isNet());
	EXPECT_FALSE(ent->isNetBuy());
	// 未绑定合约与扩展信息时为 NULL
	EXPECT_EQ(ent->getContractInfo(), nullptr);
	EXPECT_EQ(ent->getExtras(), nullptr);
	ent->release();
}

TEST(r12_trade_entrust, extras_string_and_contract_binding)
{
	WTSEntrust* ent = WTSEntrust::create();
	// 无参 create 得到全空委托
	EXPECT_STREQ(ent->getCode(), "");
	EXPECT_DOUBLE_EQ(ent->getVolume(), 0.0);
	ent->setextras("{\"k\":1}");
	ent->setContractInfo(NULL);
	EXPECT_EQ(ent->getContractInfo(), nullptr);
	ent->release();
}

TEST(r12_trade_action, cancel_action_fields)
{
	WTSEntrustAction* act = WTSEntrustAction::createCancelAction("1001", "OID01");
	ASSERT_NE(act, nullptr);
	EXPECT_STREQ(act->getEntrustID(), "1001");
	EXPECT_STREQ(act->getOrderID(), "OID01");
	EXPECT_EQ(act->getBusinessType(), BT_CASH);
	// 撤单动作的 actionFlag 必须区别于普通报单动作（0 表示未设置）
	EXPECT_NE(act->getActionFlag(), (WTSActionFlag)0);

	WTSEntrustAction* act2 = WTSEntrustAction::create("DCE.m2409", "DCE", BT_ETF);
	EXPECT_STREQ(act2->getCode(), "DCE.m2409");
	EXPECT_STREQ(act2->getExchg(), "DCE");
	EXPECT_EQ(act2->getBusinessType(), BT_ETF);
	act2->setCode("DCE.m2409", 10);
	act2->setExchange("DCE", 3);
	EXPECT_STREQ(act2->getCode(), "DCE.m2409");
	EXPECT_STREQ(act2->getExchg(), "DCE");
	act2->setEntrustID("2002");
	act2->setOrderID("OID02");
	act2->setUserTag("tag2");
	EXPECT_STREQ(act2->getEntrustID(), "2002");
	EXPECT_STREQ(act2->getOrderID(), "OID02");
	EXPECT_STREQ(act2->getUserTag(), "tag2");
	EXPECT_EQ(act2->getContractInfo(), nullptr);
	// 可变指针重载
	char* eid = act2->getEntrustID();
	wt_strcpy(eid, "2003");
	EXPECT_STREQ(act2->getEntrustID(), "2003");
	char* utag = act2->getUserTag();
	wt_strcpy(utag, "tag3");
	EXPECT_STREQ(act2->getUserTag(), "tag3");
	act->release();
	act2->release();
}

TEST(r12_trade_order, create_from_entrust_copies_fields)
{
	WTSEntrust* ent = WTSEntrust::create("SHFE.rb2405", 5, 3500.0, "SHFE", BT_CASH);
	ent->setDirection(WDT_LONG);
	ent->setOffsetType(WOT_OPEN);
	ent->setPriceType(WPT_LIMITPRICE);
	ent->setOrderFlag(WOF_NOR);
	ent->setEntrustID("7001");
	ent->setUserTag("ut7");

	WTSOrderInfo* ord = WTSOrderInfo::create(ent);
	ASSERT_NE(ord, nullptr);
	EXPECT_STREQ(ord->getCode(), "SHFE.rb2405");
	EXPECT_STREQ(ord->getExchg(), "SHFE");
	EXPECT_DOUBLE_EQ(ord->getPrice(), 3500.0);
	EXPECT_DOUBLE_EQ(ord->getVolume(), 5.0);
	// 初始余量等于委托量
	EXPECT_DOUBLE_EQ(ord->getVolLeft(), 5.0);
	EXPECT_DOUBLE_EQ(ord->getVolTraded(), 0.0);
	EXPECT_EQ(ord->getDirection(), WDT_LONG);
	EXPECT_EQ(ord->getOffsetType(), WOT_OPEN);
	EXPECT_EQ(ord->getPriceType(), WPT_LIMITPRICE);
	EXPECT_EQ(ord->getOrderFlag(), WOF_NOR);
	EXPECT_STREQ(ord->getEntrustID(), "7001");
	EXPECT_STREQ(ord->getUserTag(), "ut7");
	EXPECT_EQ(ord->getBusinessType(), BT_CASH);
	EXPECT_EQ(ord->getContractInfo(), nullptr);
	EXPECT_EQ(ord->getExtras(), nullptr);
	ent->release();

	// 空委托创建得到全零订单对象
	WTSOrderInfo* ord2 = WTSOrderInfo::create();
	EXPECT_STREQ(ord2->getCode(), "");
	EXPECT_DOUBLE_EQ(ord2->getVolume(), 0.0);
	EXPECT_DOUBLE_EQ(ord2->getVolLeft(), 0.0);
	EXPECT_EQ(ord2->getOrderState(), (WTSOrderState)0);
	// 报单日期/时间可由 setter 显式写入
	ord2->setCode("CFFEX.IF2406", 12);
	ord2->setExchange("CFFEX", 5);
	EXPECT_STREQ(ord2->getCode(), "CFFEX.IF2406");
	EXPECT_STREQ(ord2->getExchg(), "CFFEX");
	ord2->setVolume(2.0);
	ord2->setPrice(3800.0);
	EXPECT_DOUBLE_EQ(ord2->getVolume(), 2.0);
	EXPECT_DOUBLE_EQ(ord2->getPrice(), 3800.0);
	// 净额方向标记
	EXPECT_FALSE(ord2->isNet());
	ord2->setNetDirection(true);
	EXPECT_TRUE(ord2->isNet());
	EXPECT_TRUE(ord2->isNetBuy());
	ord2->release();
	ord->release();
}

TEST(r12_trade_info, fields_and_net_direction)
{
	WTSTradeInfo* tr = WTSTradeInfo::create("SHFE.rb2405", "SHFE");
	ASSERT_NE(tr, nullptr);
	EXPECT_STREQ(tr->getCode(), "SHFE.rb2405");
	EXPECT_STREQ(tr->getExchg(), "SHFE");
	EXPECT_EQ(tr->getBusinessType(), BT_CASH);

	tr->setTradeID("T0001");
	tr->setRefOrder("O0001");
	EXPECT_STREQ(tr->getTradeID(), "T0001");
	EXPECT_STREQ(tr->getRefOrder(), "O0001");
	tr->setCode("SHFE.rb2406");
	EXPECT_STREQ(tr->getCode(), "SHFE.rb2406");
	tr->setDirection(WDT_SHORT);
	tr->setOffsetType(WOT_CLOSE);
	tr->setTradeType(WTT_Common);
	EXPECT_EQ(tr->getDirection(), WDT_SHORT);
	EXPECT_EQ(tr->getOffsetType(), WOT_CLOSE);
	EXPECT_EQ(tr->getTradeType(), WTT_Common);
	tr->setVolume(4.0);
	tr->setPrice(3499.5);
	tr->setAmount(13998.0);
	EXPECT_DOUBLE_EQ(tr->getVolume(), 4.0);
	EXPECT_DOUBLE_EQ(tr->getPrice(), 3499.5);
	EXPECT_DOUBLE_EQ(tr->getAmount(), 13998.0);
	tr->setTradeDate(20240506);
	tr->setTradeTime(93000000);
	EXPECT_EQ(tr->getTradeDate(), 20240506u);
	EXPECT_EQ(tr->getTradeTime(), 93000000u);
	tr->setUserTag("tinfo");
	EXPECT_STREQ(tr->getUserTag(), "tinfo");
	EXPECT_FALSE(tr->isNet());
	tr->setNetDirection(false);
	EXPECT_TRUE(tr->isNet());
	EXPECT_FALSE(tr->isNetBuy());
	// 买入动作判定依赖方向与开平组合，不能只看 direction
	EXPECT_EQ(tr->getContractInfo(), nullptr);
	tr->release();
}

TEST(r12_trade_position, derived_totals_and_frozen)
{
	WTSPositionItem* pos = WTSPositionItem::create("SHFE.rb2405", "CNY", "SHFE");
	ASSERT_NE(pos, nullptr);
	EXPECT_STREQ(pos->getCode(), "SHFE.rb2405");
	EXPECT_STREQ(pos->getCurrency(), "CNY");
	EXPECT_STREQ(pos->getExchg(), "SHFE");
	EXPECT_EQ(pos->getBusinessType(), BT_CASH);

	pos->setDirection(WDT_LONG);
	EXPECT_EQ(pos->getDirection(), WDT_LONG);
	pos->setPrePosition(10.0);
	pos->setNewPosition(6.0);
	pos->setAvailPrePos(8.0);
	pos->setAvailNewPos(2.0);
	// 总量 = 昨 + 今
	EXPECT_DOUBLE_EQ(pos->getTotalPosition(), 16.0);
	EXPECT_DOUBLE_EQ(pos->getPrePosition(), 10.0);
	EXPECT_DOUBLE_EQ(pos->getNewPosition(), 6.0);
	// 可用 = 昨可用 + 今可用
	EXPECT_DOUBLE_EQ(pos->getAvailPosition(), 10.0);
	// 冻结 = 总量 - 可用 = 6；分项冻结各自计算
	EXPECT_DOUBLE_EQ(pos->getFrozenPosition(), 6.0);
	EXPECT_DOUBLE_EQ(pos->getFrozenPrePos(), 2.0);
	EXPECT_DOUBLE_EQ(pos->getFrozenNewPos(), 4.0);

	pos->setInitPosition(20.0);
	EXPECT_DOUBLE_EQ(pos->getInitPosition(), 20.0);
	pos->setApplyPosition(1.0);
	EXPECT_DOUBLE_EQ(pos->getApplyPosition(), 1.0);
	pos->setPositionCost(56000.0);
	EXPECT_DOUBLE_EQ(pos->getPositionCost(), 56000.0);
	pos->setMargin(5600.0);
	EXPECT_DOUBLE_EQ(pos->getMargin(), 5600.0);
	pos->setAvgPrice(3500.0);
	EXPECT_DOUBLE_EQ(pos->getAvgPrice(), 3500.0);
	pos->setDynProfit(-200.0);
	EXPECT_DOUBLE_EQ(pos->getDynProfit(), -200.0);
	pos->setBusinessType(BT_CREDIT);
	EXPECT_EQ(pos->getBusinessType(), BT_CREDIT);
	EXPECT_EQ(pos->getContractInfo(), nullptr);
	pos->release();
}

TEST(r12_trade_account, money_fields)
{
	WTSAccountInfo* acc = WTSAccountInfo::create();
	ASSERT_NE(acc, nullptr);
	acc->setCurrency("CNY");
	EXPECT_STREQ(acc->getCurrency(), "CNY");
	acc->setBalance(100000.0);
	acc->setPreBalance(90000.0);
	acc->setMargin(20000.0);
	acc->setFrozenMargin(1000.0);
	acc->setCloseProfit(500.0);
	acc->setDynProfit(-300.0);
	EXPECT_DOUBLE_EQ(acc->getBalance(), 100000.0);
	EXPECT_DOUBLE_EQ(acc->getPreBalance(), 90000.0);
	EXPECT_DOUBLE_EQ(acc->getMargin(), 20000.0);
	EXPECT_DOUBLE_EQ(acc->getFrozenMargin(), 1000.0);
	EXPECT_DOUBLE_EQ(acc->getCloseProfit(), 500.0);
	EXPECT_DOUBLE_EQ(acc->getDynProfit(), -300.0);
	acc->release();
}
