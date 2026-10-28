// Lane R53 - src/Includes/WTSTradeDef.hpp（补 r12 未覆盖的分支）
// 覆盖：WTSEntrust 买入动作矩阵/信用业务/可变 ID、WTSEntrustAction 普通路径、
//       WTSOrderInfo 开平标志映射/状态机/错误标记/元数据、WTSTradeInfo 开平与扩展、
//       WTSOrderInfo/WTSTradeInfo 的合约与扩展对象绑定。
// 说明：WTSEntrustAction::createCancelAction 的裸 new 缺陷用 DISABLED 记录（BUG-R53-001）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"

using namespace wtp;

namespace
{
struct R53BuyCase
{
	WTSDirectionType direction;
	WTSOffsetType offset;
	bool expected;
};

struct R53OffsetCase
{
	WTSOffsetType offset;
	uint32_t expected;
};
}  // namespace

// WTSTradeDef.hpp:110-189：买入动作矩阵、信用业务、净方向、可变 ID 与长度受控写入
TEST(r53_entrust, buy_action_credit_business_and_mutable_fields)
{
	WTSEntrust* ent = WTSEntrust::create("SHFE.rb2405", 2.0, 3500.0, "SHFE", BT_CREDIT_FIN);
	ASSERT_NE(ent, nullptr);
	EXPECT_STREQ(ent->getCode(), "SHFE.rb2405");
	EXPECT_STREQ(ent->getExchg(), "SHFE");
	EXPECT_DOUBLE_EQ(ent->getVolume(), 2.0);
	EXPECT_DOUBLE_EQ(ent->getPrice(), 3500.0);
	EXPECT_EQ(ent->getBusinessType(), BT_CREDIT_FIN);
	EXPECT_TRUE(ent->isCreditBusiness());

	const R53BuyCase cases[] = {
		{ WDT_LONG, WOT_OPEN, true },
		{ WDT_LONG, WOT_CLOSE, false },
		{ WDT_SHORT, WOT_OPEN, false },
		{ WDT_SHORT, WOT_CLOSE, true },
		{ WDT_SHORT, WOT_CLOSETODAY, true },
		{ WDT_LONG, WOT_CLOSEYESTERDAY, false },
	};
	for (const R53BuyCase& item : cases)
	{
		ent->setDirection(item.direction);
		ent->setOffsetType(item.offset);
		EXPECT_EQ(ent->isBuyAction(), item.expected);
		EXPECT_EQ(ent->getDirection(), item.direction);
		EXPECT_EQ(ent->getOffsetType(), item.offset);
	}

	ent->setBusinessType(BT_CREDIT);
	EXPECT_TRUE(ent->isCreditBusiness());
	ent->setBusinessType(BT_CREDIT_SLO);
	EXPECT_TRUE(ent->isCreditBusiness());
	ent->setBusinessType(BT_CASH);
	EXPECT_FALSE(ent->isCreditBusiness());

	EXPECT_DOUBLE_EQ(ent->getCreditBalance(), 0.0);
	ent->setCreditBalance(12345.5);
	EXPECT_DOUBLE_EQ(ent->getCreditBalance(), 12345.5);

	EXPECT_FALSE(ent->isNet());
	ent->setNetDirection(false);
	EXPECT_TRUE(ent->isNet());
	EXPECT_FALSE(ent->isNetBuy());
	ent->setNetDirection(true);
	EXPECT_TRUE(ent->isNetBuy());

	ent->setVolume(3.5);
	ent->setPrice(3499.0);
	EXPECT_DOUBLE_EQ(ent->getVolume(), 3.5);
	EXPECT_DOUBLE_EQ(ent->getPrice(), 3499.0);

	// 显式长度参数：只复制前缀
	ent->setCode("rb2405-X", 6);
	ent->setExchange("SHFE-X", 4);
	EXPECT_STREQ(ent->getCode(), "rb2405");
	EXPECT_STREQ(ent->getExchg(), "SHFE");

	ent->setEntrustID("E100");
	ent->setUserTag("U100");
	EXPECT_STREQ(ent->getEntrustID(), "E100");
	EXPECT_STREQ(ent->getUserTag(), "U100");
	char* eid = ent->getEntrustID();
	ASSERT_NE(eid, nullptr);
	wt_strcpy(eid, "E200");
	char* tag = ent->getUserTag();
	ASSERT_NE(tag, nullptr);
	wt_strcpy(tag, "U200");
	EXPECT_STREQ(ent->getEntrustID(), "E200");
	EXPECT_STREQ(ent->getUserTag(), "U200");

	WTSContractInfo* comm = WTSContractInfo::create("rb2405", "螺纹钢2405", "SHFE", "rb");
	ASSERT_NE(comm, nullptr);
	ent->setContractInfo(comm);
	EXPECT_EQ(ent->getContractInfo(), comm);
	ent->setContractInfo(nullptr);
	EXPECT_EQ(ent->getContractInfo(), nullptr);

	WTSVariant* extras = WTSVariant::createObject();
	ASSERT_NE(extras, nullptr);
	ent->setExtras(extras);
	EXPECT_EQ(ent->getExtras(), extras);
	ent->setExtras(nullptr);
	EXPECT_EQ(ent->getExtras(), nullptr);
	// setextras 写内部定长缓冲，无 getter；只验证不崩溃
	ent->setextras("tag=abc");

	extras->release();
	comm->release();
	ent->release();
}

// WTSTradeDef.hpp:223-280：普通 create 路径的字段、动作标志、业务类型与绑定
TEST(r53_entrust_action, create_flags_and_business)
{
	WTSEntrustAction* act = WTSEntrustAction::create("DCE.m2409", "DCE", BT_ETF);
	ASSERT_NE(act, nullptr);
	EXPECT_STREQ(act->getCode(), "DCE.m2409");
	EXPECT_STREQ(act->getExchg(), "DCE");
	EXPECT_EQ(act->getBusinessType(), BT_ETF);
	EXPECT_EQ(act->getActionFlag(), WAF_CANCEL);	// 结构体默认撤单

	act->setActionFlag(WAF_MODIFY);
	EXPECT_EQ(act->getActionFlag(), WAF_MODIFY);
	act->setBusinessType(BT_CREDIT);
	EXPECT_EQ(act->getBusinessType(), BT_CREDIT);

	act->setCode("m2501-X", 5);
	act->setExchange("DCE-X", 3);
	EXPECT_STREQ(act->getCode(), "m2501");
	EXPECT_STREQ(act->getExchg(), "DCE");
	act->setEntrustID("E1");
	act->setOrderID("O1");
	act->setUserTag("T1");
	EXPECT_STREQ(act->getEntrustID(), "E1");
	EXPECT_STREQ(act->getOrderID(), "O1");
	EXPECT_STREQ(act->getUserTag(), "T1");
	char* eid = act->getEntrustID();
	wt_strcpy(eid, "E2");
	char* utag = act->getUserTag();
	wt_strcpy(utag, "T2");
	EXPECT_STREQ(act->getEntrustID(), "E2");
	EXPECT_STREQ(act->getUserTag(), "T2");

	WTSContractInfo* comm = WTSContractInfo::create("m2409", "豆粕2409", "DCE", "m");
	ASSERT_NE(comm, nullptr);
	act->setContractInfo(comm);
	EXPECT_EQ(act->getContractInfo(), comm);
	comm->release();

	WTSEntrustAction* empty = WTSEntrustAction::create();
	ASSERT_NE(empty, nullptr);
	EXPECT_STREQ(empty->getCode(), "");
	EXPECT_STREQ(empty->getExchg(), "");
	EXPECT_EQ(empty->getBusinessType(), BT_CASH);
	empty->release();

	act->release();
}

// WTSTradeDef.hpp:384-400：WTSOrderInfo 开平标志映射与买入动作
TEST(r53_order, offset_flag_mapping_and_buy_action)
{
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(ord, nullptr);
	const R53OffsetCase cases[] = {
		{ WOT_OPEN, 0 },
		{ WOT_CLOSE, 1 },
		{ WOT_CLOSEYESTERDAY, 1 },
		{ WOT_CLOSETODAY, 2 },
		{ WOT_FORCECLOSE, 2 },
	};
	for (const R53OffsetCase& item : cases)
	{
		ord->setOffsetType(item.offset);
		EXPECT_EQ(ord->getOffsetFlag(), item.expected);
	}

	ord->setDirection(WDT_LONG);
	ord->setOffsetType(WOT_OPEN);
	EXPECT_TRUE(ord->isBuyAction());
	ord->setOffsetType(WOT_CLOSE);
	EXPECT_FALSE(ord->isBuyAction());
	ord->setDirection(WDT_SHORT);
	ord->setOffsetType(WOT_OPEN);
	EXPECT_FALSE(ord->isBuyAction());
	ord->setOffsetType(WOT_CLOSE);
	EXPECT_TRUE(ord->isBuyAction());

	// WTSTradeDef.hpp:453-471：订单状态与错误标记对 isAlive 的影响
	EXPECT_EQ(ord->getOrderState(), WOS_Submitting);
	EXPECT_TRUE(ord->isAlive());
	EXPECT_FALSE(ord->isError());
	EXPECT_FALSE(ord->isSelfTrade());

	const WTSOrderState deadStates[] = { WOS_AllTraded, WOS_Canceled };
	for (WTSOrderState st : deadStates)
	{
		ord->setOrderState(st);
		EXPECT_FALSE(ord->isAlive());
	}
	const WTSOrderState aliveStates[] = {
		WOS_PartTraded_Queuing,
		WOS_PartTraded_NotQueuing,
		WOS_NotTraded_Queuing,
		WOS_NotTraded_NotQueuing,
		WOS_Submitting,
		WOS_Cancelling,
		WOS_Nottouched,
	};
	for (WTSOrderState st : aliveStates)
	{
		ord->setOrderState(st);
		EXPECT_TRUE(ord->isAlive());
	}

	ord->setError(WOEF_SelfTrade);
	EXPECT_TRUE(ord->isError());
	EXPECT_TRUE(ord->isSelfTrade());
	EXPECT_FALSE(ord->isAlive());	// 错误标记优先于状态
	ord->setError();
	EXPECT_TRUE(ord->isError());
	EXPECT_FALSE(ord->isSelfTrade());
	ord->setError(WOEF_None);
	EXPECT_FALSE(ord->isError());
	ord->setOrderState(WOS_NotTraded_Queuing);
	EXPECT_TRUE(ord->isAlive());
	EXPECT_FALSE(ord->isSelfTrade());
	ord->release();
}

// WTSTradeDef.hpp:429-474：订单元数据、状态消息、可变 OrderID 与合约/扩展绑定
TEST(r53_order, metadata_state_msg_and_bindings)
{
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ASSERT_NE(ord, nullptr);
	ord->setCode("CFFEX.IF2406", 12);
	ord->setExchange("CFFEX", 5);
	EXPECT_STREQ(ord->getCode(), "CFFEX.IF2406");
	EXPECT_STREQ(ord->getExchg(), "CFFEX");

	ord->setOrderDate(20240506);
	ord->setOrderTime(93000123);
	ord->setVolTraded(1.5);
	ord->setVolLeft(3.5);
	EXPECT_EQ(ord->getOrderDate(), 20240506u);
	EXPECT_EQ(ord->getOrderTime(), 93000123u);
	EXPECT_DOUBLE_EQ(ord->getVolTraded(), 1.5);
	EXPECT_DOUBLE_EQ(ord->getVolLeft(), 3.5);

	ord->setOrderID("OID100");
	EXPECT_STREQ(ord->getOrderID(), "OID100");
	char* oid = ord->getOrderID();
	ASSERT_NE(oid, nullptr);
	wt_strcpy(oid, "OID200");
	EXPECT_STREQ(ord->getOrderID(), "OID200");

	ord->setOrderType(WORT_Hedge);
	EXPECT_EQ(ord->getOrderType(), WORT_Hedge);
	ord->setPriceType(WPT_LIMITPRICE);
	ord->setOrderFlag(WOF_NOR);
	EXPECT_EQ(ord->getPriceType(), WPT_LIMITPRICE);
	EXPECT_EQ(ord->getOrderFlag(), WOF_NOR);

	ord->setStateMsg("partially filled");
	EXPECT_STREQ(ord->getStateMsg(), "partially filled");
	char* msg = ord->getStateMsg();
	ASSERT_NE(msg, nullptr);
	wt_strcpy(msg, "filled");
	EXPECT_STREQ(ord->getStateMsg(), "filled");

	WTSContractInfo* comm = WTSContractInfo::create("IF2406", "沪深300", "CFFEX", "IF");
	ASSERT_NE(comm, nullptr);
	ord->setContractInfo(comm);
	EXPECT_EQ(ord->getContractInfo(), comm);
	WTSVariant* extras = WTSVariant::createObject();
	ASSERT_NE(extras, nullptr);
	ord->setExtras(extras);
	EXPECT_EQ(ord->getExtras(), extras);

	extras->release();
	comm->release();
	ord->release();
}

// WTSTradeDef.hpp:556-604：成交记录开平映射、买入动作、可变单号与绑定
TEST(r53_trade_info, offset_flag_buy_action_and_bindings)
{
	WTSTradeInfo* tr = WTSTradeInfo::create("SHFE.rb2405", "SHFE", BT_CREDIT_SLO);
	ASSERT_NE(tr, nullptr);
	EXPECT_EQ(tr->getBusinessType(), BT_CREDIT_SLO);

	const R53OffsetCase cases[] = {
		{ WOT_OPEN, 0 },
		{ WOT_CLOSE, 1 },
		{ WOT_CLOSEYESTERDAY, 1 },
		{ WOT_CLOSETODAY, 2 },
		{ WOT_FORCECLOSE, 2 },
	};
	for (const R53OffsetCase& item : cases)
	{
		tr->setOffsetType(item.offset);
		EXPECT_EQ(tr->getOffsetFlag(), item.expected);
	}

	tr->setDirection(WDT_LONG);
	tr->setOffsetType(WOT_OPEN);
	EXPECT_TRUE(tr->isBuyAction());
	tr->setDirection(WDT_SHORT);
	tr->setOffsetType(WOT_OPEN);
	EXPECT_FALSE(tr->isBuyAction());
	tr->setOffsetType(WOT_CLOSE);
	EXPECT_TRUE(tr->isBuyAction());
	tr->setDirection(WDT_LONG);
	EXPECT_FALSE(tr->isBuyAction());

	tr->setTradeID("T1");
	tr->setRefOrder("O1");
	char* tid = tr->getTradeID();
	ASSERT_NE(tid, nullptr);
	wt_strcpy(tid, "T2");
	char* oid = tr->getRefOrder();
	ASSERT_NE(oid, nullptr);
	wt_strcpy(oid, "O2");
	EXPECT_STREQ(tr->getTradeID(), "T2");
	EXPECT_STREQ(tr->getRefOrder(), "O2");

	WTSContractInfo* comm = WTSContractInfo::create("rb2405", "螺纹钢2405", "SHFE", "rb");
	ASSERT_NE(comm, nullptr);
	tr->setContractInfo(comm);
	EXPECT_EQ(tr->getContractInfo(), comm);
	WTSVariant* extras = WTSVariant::createObject();
	ASSERT_NE(extras, nullptr);
	tr->setExtras(extras);
	EXPECT_EQ(tr->getExtras(), extras);
	tr->setExtras(nullptr);
	EXPECT_EQ(tr->getExtras(), nullptr);

	extras->release();
	comm->release();
	tr->release();
}

// WTSTradeDef.hpp:237-248（BUG-R53-001）：createCancelAction 用裸 new 构造池对象，
// 未设置 WTSPoolObject::_pool/_mutex；release() 归零时经空 _pool 调 destroy 崩溃。
// 期望：release 正常回收；实际：SIGSEGV（另一 lane 的 r12_trade_action 用例已实测 CRASH）。
TEST(DISABLED_r53_entrust_action, cancel_action_release_uses_allocated_pool)
{
	WTSEntrustAction* act = WTSEntrustAction::createCancelAction("1001", "OID01");
	ASSERT_NE(act, nullptr);
	EXPECT_STREQ(act->getEntrustID(), "1001");
	EXPECT_STREQ(act->getOrderID(), "OID01");
	act->release();
}
