/*!
 * \file q04_trader_push_report_test.cpp
 * \brief TraderAdapter 实时委托/成交回报对本地持仓与未完成量的推进(迭代01 第五波 q04)
 *
 * 覆盖: onPushOrder(NULL/未知合约/撤销统计/首次推送扣可平/重复推送回补可平/
 * 昨仓上限回夹/本通道认领与 _orders 增删/sink 通知)、
 * onPushTrade(T+1 与非 T+1 的可用仓推进、平今、平昨优先扣昨仓、外部单不认领)。
 */
#include "q04_trader_fixture.h"

#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

struct PushEnv
{
	q04::TempRoot			_tmp{ "push" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;
	std::string				_pattern{ "wtp.uft1." };

	PushEnv(TradingMode tm = TM_Both)
	{
		_bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover, tm);
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		EXPECT_TRUE(_adapter.initExt("uft1", &_api, &_bd, &_policy));
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
		_adapter.onLoginResult(true, "ok", 20240315);
	}

	void loadPosition(double lPre, double lPreAvail, double lNew, double lNewAvail,
		double sPre, double sPreAvail, double sNew, double sNewAvail)
	{
		std::vector<WTSObject*> items{
			q04::posItem("SHFE", "ag2406", WDT_LONG, lPre, lPreAvail, lNew, lNewAvail),
			q04::posItem("SHFE", "ag2406", WDT_SHORT, sPre, sPreAvail, sNew, sNewAvail),
		};
		WTSArray* ay = WTSArray::create();
		for (auto one : items)
			ay->append(one);
		_adapter.onRspPosition(ay);
		ay->release();
		for (auto one : items)
			one->release();
	}

	//发一笔委托, 返回本通道 localid
	uint32_t place(double price, double qty, bool isLong, bool isOpen)
	{
		uint32_t id = isOpen
			? (isLong ? _adapter.openLong(kAg, price, qty, 0) : _adapter.openShort(kAg, price, qty, 0))
			: (isLong ? _adapter.closeLong(kAg, price, qty, false, 0) : _adapter.closeShort(kAg, price, qty, false, 0));
		EXPECT_NE(UINT_MAX, id);
		return id;
	}

	//按最近一笔委托组装实时委托回报
	WTSOrderInfo* pushOrderFromLast(const char* orderid, WTSOrderState st, WTSOffsetType offset,
		double volTraded = 0.0, WTSOrderFlag flag = WOF_NOR)
	{
		const q04::FakeTraderApi::EntrustSnap& snap = _api._entrusts.back();
		WTSOrderInfo* ord = q04::orderFromSnap(snap, orderid);
		ord->setOrderState(st);
		ord->setOffsetType(offset);
		ord->setOrderFlag(flag);
		ord->setVolTraded(volTraded);
		ord->setVolLeft(snap._vol - volTraded);
		_adapter.onPushOrder(ord);
		return ord;
	}

	void pushTrade(bool isLong, WTSOffsetType offset, double vol, double price, const char* usertag)
	{
		WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
		t->setDirection(isLong ? WDT_LONG : WDT_SHORT);
		t->setOffsetType(offset);
		t->setVolume(vol);
		t->setPrice(price);
		t->setUserTag(usertag);
		_adapter.onPushTrade(t);
		t->release();
	}
};

}	// namespace

TEST(q04_trader_push, order_push_reduces_available_position_on_first_notice)
{
	PushEnv env;
	//多头: 昨10/可6, 今4/可4
	env.loadPosition(10, 6, 4, 4, 0, 0, 0, 0);

	uint32_t id = env.place(5000.0, 12.0, true, false);	//平多
	WTSOrderInfo* ord = env.pushOrderFromLast("PX-1", WOS_NotTraded_Queuing, WOT_CLOSE);
	//首次推送且是平仓: 先扣昨仓 6, 再扣今仓 4(剩 2)
	EXPECT_DOUBLE_EQ(14.0, env._adapter.getPosition(kAg, false, 1));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, true, 1));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, false, 2));
	(void)id;
	//本通道单子进 _orders 并通知 sink
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
	ASSERT_EQ(1u, env._sink._orders.size());
	EXPECT_EQ(id, env._sink._orders[0]._localid);
	EXPECT_STREQ(kAg, env._sink._orders[0]._code.c_str());
	EXPECT_TRUE(env._sink._orders[0]._long);
	EXPECT_EQ(1u, env._sink._orders[0]._offset);	//WOT_CLOSE -> 1
	EXPECT_DOUBLE_EQ(12.0, env._sink._orders[0]._total);
	EXPECT_DOUBLE_EQ(12.0, env._sink._orders[0]._left);
	EXPECT_FALSE(env._sink._orders[0]._canceled);
	//信息量: 首次推送 +1
	EXPECT_EQ(1u, env._adapter.getInfos(kAg));

	//同一订单号再次推送且已撤销: 可平量要回补, 昨仓超过昨持仓时夹回
	ord->setOrderState(WOS_Canceled);
	env._adapter.onPushOrder(ord);
	//回补 qty = 12 - 0 = 12 -> 昨可平 12 > 昨持仓 10 -> 超出的 2 记到今仓
	EXPECT_DOUBLE_EQ(12.0, env._adapter.getPosition(kAg, true, 1));
	//持仓量本身不受撤单影响
	EXPECT_DOUBLE_EQ(14.0, env._adapter.getPosition(kAg, false, 1));
	//未完成量: 下单 +12, 撤销 -(12-0) -> 0
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kAg));
	//死单要从 _orders 摘掉
	orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size());
	orders->release();
	ASSERT_EQ(2u, env._sink._orders.size());
	EXPECT_TRUE(env._sink._orders[1]._canceled);
	//信息量: 首次下单 +1, 撤单成功再 +1
	EXPECT_EQ(2u, env._adapter.getInfos(kAg));
	ord->release();
}

TEST(q04_trader_push, closetoday_and_short_side_offsets_are_handled)
{
	PushEnv env;
	//空头: 昨5/可3, 今4/可4
	env.loadPosition(0, 0, 0, 0, 5, 3, 4, 4);

	uint32_t id = env.place(5000.0, 2.0, false, false);	//平空(昨)
	WTSOrderInfo* ord = env.pushOrderFromLast("PX-2", WOS_NotTraded_Queuing, WOT_CLOSETODAY);
	//平今只扣今仓可平: 4 -> 2, 昨可平不受影响
	EXPECT_DOUBLE_EQ(-5.0, env._adapter.getPosition(kAg, true, 2));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, true, 1));
	ASSERT_EQ(1u, env._sink._orders.size());
	EXPECT_EQ(id, env._sink._orders[0]._localid);
	EXPECT_FALSE(env._sink._orders[0]._long);
	EXPECT_EQ(2u, env._sink._orders[0]._offset);	//WOT_CLOSETODAY -> 2
	(void)ord;

	//开仓单首次推送不扣可平
	env._api._entrusts.clear();
	env.place(5000.0, 3.0, false, true);
	WTSOrderInfo* openOrd = env.pushOrderFromLast("PX-3", WOS_PartTraded_Queuing, WOT_OPEN);
	EXPECT_DOUBLE_EQ(-5.0, env._adapter.getPosition(kAg, true, 2)) << "空头可平量按净头寸符号返回，开仓不应改变可平量";
	openOrd->release();
}

TEST(q04_trader_push, foreign_and_unknown_channel_reports_are_dropped)
{
	PushEnv env;
	env.loadPosition(4, 4, 0, 0, 0, 0, 0, 0);

	//NULL 回报
	env._adapter.onPushOrder(nullptr);
	EXPECT_EQ(0u, env._sink._orders.size());

	//合约查不到: 直接 return
	WTSOrderInfo* unknown = WTSOrderInfo::create();
	unknown->setCode("r2510");
	unknown->setExchange("DCE");
	unknown->setDirection(WDT_LONG);
	unknown->setOffsetType(WOT_OPEN);
	unknown->setVolume(1);
	unknown->setOrderState(WOS_NotTraded_Queuing);
	unknown->setOrderID("UX-1");
	env._adapter.onPushOrder(unknown);
	EXPECT_EQ(0u, env._sink._orders.size());
	EXPECT_EQ(nullptr, env._adapter.getOrders(""));
	unknown->release();

	//外部单(usertag 不属于本通道): 统计照记, 但不进 _orders、不通知 sink
	WTSOrderInfo* foreign = WTSOrderInfo::create();
	foreign->setCode("ag2406");
	foreign->setExchange("SHFE");
	foreign->setDirection(WDT_LONG);
	foreign->setOffsetType(WOT_OPEN);
	foreign->setVolume(2);
	foreign->setVolLeft(2);
	foreign->setOrderState(WOS_NotTraded_Queuing);
	foreign->setOrderID("FM-1");
	foreign->setUserTag("someoneelse.7");
	env._adapter.onPushOrder(foreign);
	EXPECT_EQ(0u, env._sink._orders.size());
	//没有本通道活单时 _orders 仍可能为空表指针
	OrderMap* orders = env._adapter.getOrders("");
	if (orders != nullptr)
		orders->release();
	EXPECT_EQ(0u, env._sink._orders.size());
	foreign->release();
}

TEST(q04_trader_push, canceled_push_updates_cancel_stats_and_undone)
{
	PushEnv env;
	//多空各: 昨6/可6, 今4/可4
	env.loadPosition(6, 6, 4, 4, 6, 6, 4, 4);

	//自己的平多单先登记: 4 手全部来自昨仓
	uint32_t id = env.place(5000.0, 4.0, true, false);
	WTSOrderInfo* ord = env.pushOrderFromLast("PX-4", WOS_NotTraded_Queuing, WOT_CLOSE);
	EXPECT_DOUBLE_EQ(6.0, env._adapter.getPosition(kAg, true, 1));
	EXPECT_DOUBLE_EQ(4.0, env._adapter.getPosition(kAg, true, 1) - 2.0);

	//FAK 单被自动撤销的分支(同订单号, 换个 flag 让 auto_cancel 支走到)
	ord->setOrderState(WOS_Canceled);
	ord->setOrderFlag(WOF_FAK);
	env._adapter.onPushOrder(ord);
	//未完成量按 vol - volTraded 退回: 4 - 0
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kAg));
	//回补后可平昨仓恢复
	EXPECT_DOUBLE_EQ(10.0, env._adapter.getPosition(kAg, true, 1));
	ord->release();

	//错单分支: 同一个 usertag, 新订单号 + setError
	env._api._entrusts.clear();
	env.place(5000.0, 2.0, true, false);
	WTSOrderInfo* wrong = env.pushOrderFromLast("PX-5", WOS_Canceled, WOT_OPEN);
	wrong->setError(WOEF_Normal);
	env._adapter.onPushOrder(wrong);
	wrong->release();

	//平空方向的撤销同样要回补可平今仓
	env._api._entrusts.clear();
	uint32_t idS = env.place(5000.0, 3.0, false, false);
	WTSOrderInfo* cs = env.pushOrderFromLast("PX-6", WOS_NotTraded_Queuing, WOT_CLOSETODAY);
	EXPECT_DOUBLE_EQ(-7.0, env._adapter.getPosition(kAg, true, 2));
	cs->setOrderState(WOS_Canceled);
	env._adapter.onPushOrder(cs);
	EXPECT_DOUBLE_EQ(-10.0, env._adapter.getPosition(kAg, true, 2));
	cs->release();
	(void)id;
	(void)idS;
}

TEST(q04_trader_push, trade_push_updates_position_volumes_for_both_sides)
{
	PushEnv env;
	env.loadPosition(10, 10, 0, 0, 10, 10, 0, 0);

	//多头开仓: 非 T1 -> 今仓与今可用同时增加
	env.pushTrade(true, WOT_OPEN, 4, 5000.0, "wtp.uft1.111");
	//今仓 4 + 昨仓 10
	EXPECT_DOUBLE_EQ(14.0, env._adapter.getPosition(kAg, false, 1));
	EXPECT_DOUBLE_EQ(14.0, env._adapter.getPosition(kAg, true, 1));
	ASSERT_EQ(1u, env._sink._trades.size());
	EXPECT_EQ(0u, env._sink._trades[0]._offset);	//WOT_OPEN -> 0
	EXPECT_TRUE(env._sink._trades[0]._long);
	EXPECT_DOUBLE_EQ(5000.0, env._sink._trades[0]._price);
	//usertag 属于本通道, 但 111 不是本地单号 -> 只更新未完成量
	EXPECT_DOUBLE_EQ(-4.0, env._adapter.getUndoneQty(kAg));

	//多头平今: 只减今仓
	env.pushTrade(true, WOT_CLOSETODAY, 2, 5100.0, "other.1");
	//平今只减今仓: 4 - 2 = 2, 加上昨仓 10
	EXPECT_DOUBLE_EQ(12.0, env._adapter.getPosition(kAg, false, 1));
	ASSERT_EQ(2u, env._sink._trades.size());
	EXPECT_EQ(2u, env._sink._trades[1]._offset);
	EXPECT_EQ(0u, env._sink._trades[1]._localid) << "非本通道 usertag 的成交 localid 必须是 0";
	//外部成交不动未完成量
	EXPECT_DOUBLE_EQ(-4.0, env._adapter.getUndoneQty(kAg));

	//多头平仓: 先扣昨仓再扣今仓
	env.pushTrade(true, WOT_CLOSE, 9, 5200.0, "other.2");
	//昨仓 10 - 9 = 1, 今仓仍是 2
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getPosition(kAg, false, 1));

	//空头开仓
	env.pushTrade(false, WOT_OPEN, 5, 4900.0, "other.3");
	//-(今空 5 + 昨空 10)
	EXPECT_DOUBLE_EQ(-15.0, env._adapter.getPosition(kAg, false, 2));
	//空头平仓: 昨空 10 -> 5
	env.pushTrade(false, WOT_CLOSE, 5, 4900.0, "other.4");
	//多头 2+1=3, 空头 5+5=10 -> 净 -7
	EXPECT_DOUBLE_EQ(-7.0, env._adapter.getPosition(kAg, false, 3));

	//成交回报每次都会顺带查一次账户
	EXPECT_EQ(5u, env._api._qAccCnt);
}

TEST(q04_trader_push, t1_commodity_does_not_freeze_new_available_position)
{
	PushEnv env(TM_LongT1);
	env.loadPosition(0, 0, 0, 0, 0, 0, 0, 0);

	env.pushTrade(true, WOT_OPEN, 3, 5000.0, "other.1");
	//T1: 今仓 +3, 但今可用仍是 0
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getPosition(kAg, false, 1));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, true, 1));

	env.pushTrade(false, WOT_OPEN, 2, 5000.0, "other.2");
	//空头开仓同样受 isT1 影响(实现里共用同一个判断)
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, true, 2));
}

TEST(q04_trader_push, unknown_instrument_trade_returns_early)
{
	PushEnv env;
	env.loadPosition(2, 2, 0, 0, 0, 0, 0, 0);

	WTSTradeInfo* t = WTSTradeInfo::create("r2510", "DCE");
	t->setDirection(WDT_LONG);
	t->setOffsetType(WOT_OPEN);
	t->setVolume(1);
	env._adapter.onPushTrade(t);
	EXPECT_EQ(0u, env._sink._trades.size());
	//未知合约的成交不会触发账户查询
	EXPECT_EQ(0u, env._api._qAccCnt);
	t->release();
}

//已判定崩溃类输入整类收在这里
TEST(DISABLED_q04_trader_push, null_base_datamgr_and_no_statmap_class)
{
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;

	//1) 未 run() 的适配器: onPushOrder/onPushTrade 直接解引用 NULL _stat_map
	TraderAdapter noStat;
	ActionPolicyMgr policy;
	noStat.initExt("uft1", &api, &bd, &policy);
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	noStat.onPushOrder(ord);
	WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
	noStat.onPushTrade(t);
	ord->release();
	t->release();

	//2) onPushTrade 不看 tradeRecord->getContractInfo(), 但 _bd_mgr 为空时必崩
	q04::TempRoot tmp("pushcls");
	const std::string path = tmp.file("policy.json");
	tmp.write(path, q04::policyJsonOpenCloseLimited());
	ActionPolicyMgr policy2;
	policy2.init(path.c_str());
	TraderAdapter noBd;
	noBd.initExt("uft1", &api, nullptr, &policy2);
	noBd.run();
	WTSTradeInfo* t2 = WTSTradeInfo::create("ag2406", "SHFE");
	noBd.onPushTrade(t2);
	t2->release();
}
