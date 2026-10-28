/*!
 * \file r54_trader_callback_gaps_test.cpp
 * \brief 第十一波 R54: TraderAdapter 回报链/动作规则/风控未覆盖行的补测
 *
 * 目标: src/WtUftCore/TraderAdapter.cpp
 *  - onRspOrders 卖方统计、onRspTrades 成交统计、onRspAccount 全链就绪
 *  - onRspEntrust 错单分支(动作文本/去重/未完成量回滚)
 *  - buy/sell 的 AT_CloseToday/AT_CloseYestoday/AT_Close 两种 CoverMode
 *  - 开仓 limit_l/limit_s/limit 三条限额 continue
 *  - onPushOrder 平今扣减/撤单回补/昨仓回夹、onPushTrade 平今
 *  - checkCancelLimits 总量排除 + getRiskParams 品种键命中
 *
 * 替身全部来自 q04_trader_fixture.h + r54_trader_fixture.h, 不接柜台/不起线程。
 */
#include "r54_trader_fixture.h"

#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

WTSOrderInfo* makeOrder(const char* code, const char* exchg, WTSContractInfo* cInfo,
	WTSDirectionType dir, WTSOffsetType offset, double vol, WTSOrderState st,
	const char* orderid, const char* usertag = "")
{
	WTSOrderInfo* o = WTSOrderInfo::create();
	o->setCode(code);
	o->setExchange(exchg);
	o->setContractInfo(cInfo);
	o->setDirection(dir);
	o->setOffsetType(offset);
	o->setVolume(vol);
	o->setVolLeft(vol);
	o->setOrderState(st);
	o->setOrderID(orderid);
	o->setUserTag(usertag);
	return o;
}

}	// namespace

// TraderAdapter.cpp:105-106 init 空配置早退
// TraderAdapter.cpp:146-150 风控策略缺少 default 组的告警
// TraderAdapter.cpp:1859-1873 撤单总量超限进入排除队列
// TraderAdapter.cpp:1921-1922 已排除合约的 checkOrderLimits 早退
// TraderAdapter.cpp:1970-1980 品种键命中(键切片为 "ag24", 见 BUG-R11-003)
TEST(R54TraderCallback, risk_exclude_and_product_key_match)
{
	q04::TempRoot tmp("r54risk");
	q04::FakeBaseDataMgr bd;
	q04::FakeTraderApi api;
	ActionPolicyMgr policy;
	q04::FakeSink sink;
	bd.add("SHFE", "ag", "ag2406");
	bd.add("SHFE", "rb", "rb2410");
	const std::string path = tmp.file("policy.json");
	tmp.write(path, q04::policyJsonOpenCloseLimited());
	ASSERT_TRUE(policy.init(path.c_str()));

	WTSVariant* params = WTSVariant::createObject();
	params->append("module", "");
	WTSVariant* riskmon = WTSVariant::createObject();
	riskmon->append("active", true);
	WTSVariant* policyCfg = WTSVariant::createObject();
	WTSVariant* ruleDefault = WTSVariant::createObject();
	ruleDefault->append("cancel_total_limits", (uint32_t)1);
	ruleDefault->append("cancel_times_boundary", (uint32_t)0);
	ruleDefault->append("cancel_stat_timespan", (uint32_t)0);
	ruleDefault->append("order_total_limits", (uint32_t)0);
	ruleDefault->append("order_times_boundary", (uint32_t)0);
	ruleDefault->append("order_stat_timespan", (uint32_t)0);
	WTSVariant* ruleAg = WTSVariant::createObject();
	ruleAg->append("cancel_total_limits", (uint32_t)0);
	ruleAg->append("cancel_times_boundary", (uint32_t)0);
	ruleAg->append("cancel_stat_timespan", (uint32_t)0);
	ruleAg->append("order_total_limits", (uint32_t)0);
	ruleAg->append("order_times_boundary", (uint32_t)0);
	ruleAg->append("order_stat_timespan", (uint32_t)0);
	// getRiskParams 的键是 "ag24"(品种两字母+年份首位), 这是实现现状
	policyCfg->append("ag24", ruleAg);
	policyCfg->append("default", ruleDefault);
	riskmon->append("policy", policyCfg);
	params->append("riskmon", riskmon);

	{
		TraderAdapter adapter;
		EXPECT_FALSE(adapter.init("r54risk", params, &bd, &policy));
		EXPECT_TRUE(adapter.initExt("r54risk", &api, &bd, &policy));
		adapter.addSink(&sink);
		EXPECT_TRUE(adapter.run());
		adapter.onLoginResult(true, "ok", 20240315);

		// rb 的一笔普通撤单, 让 total_cancels()=1
		WTSOrderInfo* rbOrder = makeOrder("rb2410", "SHFE", bd.getContract("rb2410", "SHFE"),
			WDT_LONG, WOT_OPEN, 1, WOS_Canceled, "R54-RB");
		rbOrder->setOrderFlag(WOF_NOR);
		WTSArray* ay = WTSArray::create();
		ay->append(rbOrder);
		adapter.onRspOrders(ay);
		ay->release();
		rbOrder->release();

		// rb 未配置品种键 -> 回落 default(cancel_total_limits=1) -> 超限
		EXPECT_FALSE(adapter.checkCancelLimits("SHFE.rb2410"));
		// ag 命中 "ag24" 组, 限额 0 -> 不超限
		EXPECT_TRUE(adapter.checkCancelLimits("SHFE.ag2406"));
		// rb 已进入排除队列 -> checkOrderLimits 立即 false
		EXPECT_FALSE(adapter.checkOrderLimits("SHFE.rb2410"));
		EXPECT_TRUE(adapter.checkOrderLimits("SHFE.ag2406"));
	}

	// 空配置直接拒绝
	{
		TraderAdapter nullCfg;
		EXPECT_FALSE(nullCfg.init("r54null", nullptr, &bd, &policy));
	}

	// 缺 default 组: 解析完成后告警, 然后在 module 空处返回 false
	{
		WTSVariant* p2 = WTSVariant::createObject();
		p2->append("module", "");
		WTSVariant* r2 = WTSVariant::createObject();
		r2->append("active", true);
		WTSVariant* pc2 = WTSVariant::createObject();
		WTSVariant* only = WTSVariant::createObject();
		only->append("cancel_total_limits", (uint32_t)0);
		pc2->append("only", only);
		r2->append("policy", pc2);
		p2->append("riskmon", r2);
		TraderAdapter noDef;
		EXPECT_FALSE(noDef.init("r54nodef", p2, &bd, &policy));
		p2->release();
	}

	params->release();
}

// TraderAdapter.cpp:1218-1268 持仓回报推进
// TraderAdapter.cpp:1271-1398 卖方错单/撤单统计 + AS_POSITION_QRYED->queryTrades
// TraderAdapter.cpp:1408-1472 六个成交统计组合 + AS_ORDERS_QRYED->queryAccount
// TraderAdapter.cpp:1206-1216 AS_TRADES_QRYED->AS_ALLREADY + on_channel_ready
TEST(R54TraderCallback, response_chain_reaches_channel_ready)
{
	r54::Env env("r54chain", q04::policyJsonOpenCloseLimited());
	env.add("SHFE", "ag", "ag2406");
	env.loadPosition("SHFE", "ag2406", 2, 2, 1, 1, 0, 0, 0, 0);

	WTSContractInfo* ag = env.bd.getContract("ag2406", "SHFE");
	ASSERT_NE(nullptr, ag);

	// 卖方三张撤单: 错单 / 普通撤单 / 自动撤单
	WTSOrderInfo* wrong = makeOrder("ag2406", "SHFE", ag, WDT_SHORT, WOT_OPEN,
		5, WOS_Canceled, "R54-O1");
	wrong->setError(WOEF_Normal);
	WTSOrderInfo* normal = makeOrder("ag2406", "SHFE", ag, WDT_SHORT, WOT_OPEN,
		3, WOS_Canceled, "R54-O2");
	normal->setOrderFlag(WOF_NOR);
	WTSOrderInfo* autoCancel = makeOrder("ag2406", "SHFE", ag, WDT_SHORT, WOT_OPEN,
		2, WOS_Canceled, "R54-O3");
	autoCancel->setOrderFlag(WOF_FAK);
	WTSArray* orders = WTSArray::create();
	orders->append(wrong);
	orders->append(normal);
	orders->append(autoCancel);

	env.adapter.onRspOrders(orders);
	orders->release();
	wrong->release();
	normal->release();
	autoCancel->release();

	// 3 张单各 +1 信息量, 其中 2 张撤单再各 +1
	EXPECT_EQ(5u, env.adapter.getInfos(kAg));
	EXPECT_EQ(TraderAdapter::AS_ORDERS_QRYED, env.adapter.state());
	EXPECT_EQ(1u, env.api._qTrdCnt);

	// 六个方向/开平组合, 全部挂 ContractInfo 才会进入统计
	const WTSDirectionType dirs[6] = { WDT_LONG, WDT_LONG, WDT_LONG, WDT_SHORT, WDT_SHORT, WDT_SHORT };
	const WTSOffsetType offs[6] = { WOT_OPEN, WOT_CLOSE, WOT_CLOSETODAY, WOT_OPEN, WOT_CLOSE, WOT_CLOSETODAY };
	const double vols[6] = { 3, 1, 2, 4, 1, 2 };
	WTSArray* trades = WTSArray::create();
	std::vector<WTSTradeInfo*> tradeObjs;
	for (int i = 0; i < 6; i++)
	{
		WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
		t->setContractInfo(ag);
		t->setDirection(dirs[i]);
		t->setOffsetType(offs[i]);
		t->setVolume(vols[i]);
		t->setPrice(5000.0);
		tradeObjs.push_back(t);
		trades->append(t);
	}
	env.adapter.onRspTrades(trades);
	trades->release();
	for (auto t : tradeObjs)
		t->release();

	EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, env.adapter.state());
	EXPECT_EQ(1u, env.api._qAccCnt);

	env.adapter.onRspAccount(nullptr);
	EXPECT_EQ(TraderAdapter::AS_ALLREADY, env.adapter.state());
	EXPECT_EQ(1u, env.sink._readyCnt);
	EXPECT_EQ(20240315u, env.sink._readyTdate);
}

// TraderAdapter.cpp:1154-1204 onRspEntrust 错单全分支:
// 合约缺失早退、open/closetoday/close 与 long/short 的动作文本、未完成量回滚、
// usertag 为空不通知 sink。
TEST(R54TraderCallback, entrust_errors_cover_action_text)
{
	r54::Env env("r54entrust", q04::policyJsonOpenCloseLimited());
	env.add("SHFE", "ag", "ag2406");

	WTSError* err = WTSError::create(WEC_ORDERINSERT, "reject by counter");
	ASSERT_NE(nullptr, err);

	auto sendError = [&](WTSDirectionType dir, WTSOffsetType offset, const char* tag) {
		const q04::FakeTraderApi::EntrustSnap& snap = env.lastSnap();
		WTSEntrust* e = WTSEntrust::create(snap._code.c_str(), snap._vol, snap._price,
			snap._exchg.c_str());
		e->setUserTag(tag);
		e->setDirection(dir);
		e->setOffsetType(offset);
		env.adapter.onRspEntrust(e, err);
		e->release();
	};

	env.place(5000, 2, true, true);
	sendError(WDT_LONG, WOT_OPEN, env.lastSnap()._usertag.c_str());
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kAg));

	env.place(5000, 2, true, false, true);
	sendError(WDT_LONG, WOT_CLOSETODAY, env.lastSnap()._usertag.c_str());
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kAg));

	env.place(5000, 2, true, false, false);
	sendError(WDT_LONG, WOT_CLOSE, env.lastSnap()._usertag.c_str());

	env.place(5000, 2, false, true);
	sendError(WDT_SHORT, WOT_OPEN, env.lastSnap()._usertag.c_str());

	env.place(5000, 2, false, false, false);
	sendError(WDT_SHORT, WOT_CLOSE, env.lastSnap()._usertag.c_str());
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kAg));
	ASSERT_EQ(5u, env.sink._entrusts.size());
	for (auto& rec : env.sink._entrusts)
		EXPECT_FALSE(rec._succ);

	// usertag 为空: 未完成量回滚但不通知 sink
	env.place(5000, 1, true, true);
	sendError(WDT_LONG, WOT_OPEN, "");
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getUndoneQty(kAg));
	EXPECT_EQ(5u, env.sink._entrusts.size());

	// 合约查不到: 不动未完成量、不通知
	WTSEntrust* unknown = WTSEntrust::create("m2509", 1, 3000.0, "DCE");
	env.adapter.onRspEntrust(unknown, err);
	unknown->release();
	EXPECT_EQ(5u, env.sink._entrusts.size());

	err->release();
}

// TraderAdapter.cpp:525-573 buy AT_CloseToday(非 CoverToday 走全部可平 + else 日志)
// TraderAdapter.cpp:576-620 buy AT_CloseYestoday(非 CoverToday else 日志)
// TraderAdapter.cpp:621-650 buy AT_Close 不区分平昨平今
// TraderAdapter.cpp:652-701 buy AT_Close 区分平昨平今(先昨后今)
// TraderAdapter.cpp:709-713 剩余量未触发的告警
TEST(R54TraderCallback, buy_close_rule_matrix)
{
	r54::Env env("r54buyclose", r54::policyMatrixJson());
	env.add("SHFE", "ni", "ni2406", 0, 0, CM_OpenCover);
	env.add("SHFE", "ni2", "ni2506", 0, 0, CM_OpenCover);
	env.add("SHFE", "cu", "cu2406", 0, 0, CM_CoverToday);

	// ni: 平昨 3 + 平今(非 CoverToday 取全部可平 5)
	env.loadPosition("SHFE", "ni2406", 0, 0, 0, 0, 3, 3, 2, 2);
	OrderIDs ids = env.adapter.buy("SHFE.ni2406", 5000.0, 10.0, 0, false);
	ASSERT_EQ(2u, ids.size());
	ASSERT_EQ(2u, env.api._entrusts.size());
	EXPECT_DOUBLE_EQ(3.0, env.api._entrusts[0]._vol);
	EXPECT_EQ((int)WOT_CLOSE, env.api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(5.0, env.api._entrusts[1]._vol);
	EXPECT_EQ((int)WOT_CLOSE, env.api._entrusts[1]._offset);

	// ni2: AT_Close 非 CoverToday -> 全部可平 5
	env.api._entrusts.clear();
	env.loadPosition("SHFE", "ni2506", 0, 0, 0, 0, 3, 3, 2, 2);
	OrderIDs ids2 = env.adapter.buy("SHFE.ni2506", 5000.0, 10.0, 0, false);
	ASSERT_EQ(1u, ids2.size());
	ASSERT_EQ(1u, env.api._entrusts.size());
	EXPECT_DOUBLE_EQ(5.0, env.api._entrusts[0]._vol);

	// cu: AT_Close 且 CoverToday -> 先平昨 2 再平今 2
	env.api._entrusts.clear();
	env.loadPosition("SHFE", "cu2406", 0, 0, 0, 0, 3, 2, 2, 2);
	OrderIDs ids3 = env.adapter.buy("SHFE.cu2406", 5100.0, 10.0, 0, false);
	ASSERT_EQ(2u, ids3.size());
	ASSERT_EQ(2u, env.api._entrusts.size());
	EXPECT_DOUBLE_EQ(2.0, env.api._entrusts[0]._vol);
	EXPECT_EQ((int)WOT_CLOSE, env.api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(2.0, env.api._entrusts[1]._vol);
	EXPECT_EQ((int)WOT_CLOSETODAY, env.api._entrusts[1]._offset);
}

// TraderAdapter.cpp:804-849 sell AT_CloseToday(非 CoverToday + else 日志)
// TraderAdapter.cpp:852-891 sell AT_CloseYestoday(非 CoverToday else 日志)
// TraderAdapter.cpp:923-977 sell AT_Close 且 CoverToday(先昨后今)
TEST(R54TraderCallback, sell_close_rule_matrix)
{
	r54::Env env("r54sellclose", r54::policyMatrixJson());
	env.add("SHFE", "ao", "ao2406", 0, 0, CM_OpenCover);
	env.add("SHFE", "ao2", "ao2506", 0, 0, CM_OpenCover);
	env.add("SHFE", "cu2", "cu2506", 0, 0, CM_CoverToday);
	env.add("SHFE", "ap", "ap2506", 0, 0, CM_OpenCover);

	// ao: 平昨 3 + 平今(非 CoverToday 取全部可平 5)
	env.loadPosition("SHFE", "ao2406", 3, 3, 2, 2, 0, 0, 0, 0);
	OrderIDs ids = env.adapter.sell("SHFE.ao2406", 3000.0, 10.0, 0, false);
	ASSERT_EQ(2u, ids.size());
	ASSERT_EQ(2u, env.api._entrusts.size());
	EXPECT_DOUBLE_EQ(3.0, env.api._entrusts[0]._vol);
	EXPECT_DOUBLE_EQ(5.0, env.api._entrusts[1]._vol);
	for (auto& snap : env.api._entrusts)
		EXPECT_EQ((int)WOT_CLOSE, snap._offset);

	// ao2: AT_Close 非 CoverToday -> 全部可平 5
	env.api._entrusts.clear();
	env.loadPosition("SHFE", "ao2506", 3, 3, 2, 2, 0, 0, 0, 0);
	OrderIDs ids2 = env.adapter.sell("SHFE.ao2506", 3000.0, 10.0, 0, false);
	ASSERT_EQ(1u, ids2.size());
	EXPECT_DOUBLE_EQ(5.0, env.api._entrusts[0]._vol);

	// cu2: AT_Close 且 CoverToday -> 平昨 2 + 平今 2
	env.api._entrusts.clear();
	env.loadPosition("SHFE", "cu2506", 3, 2, 2, 2, 0, 0, 0, 0);
	OrderIDs ids3 = env.adapter.sell("SHFE.cu2506", 70000.0, 10.0, 0, false);
	ASSERT_EQ(2u, ids3.size());
	ASSERT_EQ(2u, env.api._entrusts.size());
	EXPECT_EQ((int)WOT_CLOSE, env.api._entrusts[0]._offset);
	EXPECT_DOUBLE_EQ(2.0, env.api._entrusts[0]._vol);
	EXPECT_EQ((int)WOT_CLOSETODAY, env.api._entrusts[1]._offset);
	EXPECT_DOUBLE_EQ(2.0, env.api._entrusts[1]._vol);

	// ap: pure 规则在反向仓未清时跳过(平今看昨仓、平昨看今仓)
	env.api._entrusts.clear();
	env.loadPosition("SHFE", "ap2506", 3, 3, 2, 2, 0, 0, 0, 0);
	OrderIDs ids4 = env.adapter.sell("SHFE.ap2506", 10000.0, 5.0, 0, false);
	EXPECT_TRUE(ids4.empty());
	EXPECT_TRUE(env.api._entrusts.empty());
}

// TraderAdapter.cpp:480-499 buy AT_Open 的 limit_l / limit 两条 continue
// TraderAdapter.cpp:759-778 sell AT_Open 的 limit_s / limit 两条 continue
TEST(R54TraderCallback, order_limit_exhaustion)
{
	r54::Env env("r54limits", r54::policyMatrixJson());
	env.add("SHFE", "zn", "zn2506", 0, 0, CM_OpenCover);
	env.add("SHFE", "pb", "pb2506", 0, 0, CM_OpenCover);
	env.add("SHFE", "sn", "sn2506", 0, 0, CM_OpenCover);

	// limit_l=2 用尽
	env.feedTrade("SHFE", "zn2506", WDT_LONG, WOT_OPEN, 2);
	EXPECT_TRUE(env.adapter.buy("SHFE.zn2506", 20000.0, 1.0, 0, false).empty());

	// limit=3 由多空合计用尽
	env.feedTrade("SHFE", "pb2506", WDT_LONG, WOT_OPEN, 2);
	env.feedTrade("SHFE", "pb2506", WDT_SHORT, WOT_OPEN, 1);
	EXPECT_TRUE(env.adapter.buy("SHFE.pb2506", 15000.0, 1.0, 0, false).empty());
	EXPECT_TRUE(env.adapter.sell("SHFE.pb2506", 15000.0, 1.0, 0, false).empty());

	// limit_s=2 用尽
	env.feedTrade("SHFE", "sn2506", WDT_SHORT, WOT_OPEN, 2);
	EXPECT_TRUE(env.adapter.sell("SHFE.sn2506", 200000.0, 1.0, 0, false).empty());

	EXPECT_EQ(0u, env.api._entrusts.size());
}

// TraderAdapter.cpp:1091-1093 closeShort 市价单价格类型
// TraderAdapter.cpp:1492-1564 onPushOrder 卖方错单统计 + 状态文本
// TraderAdapter.cpp:1609-1611 平今(多头)首推扣可平今
// TraderAdapter.cpp:1659-1662 平今(多头)撤单回补可平今
// TraderAdapter.cpp:1679-1687 平仓(空头)撤单回补并夹回昨仓
// TraderAdapter.cpp:1737-1836 onPushTrade 空头平今扣今仓
TEST(R54TraderCallback, push_order_and_trade_edges)
{
	r54::Env env("r54push", q04::policyJsonOpenCloseLimited());
	env.add("SHFE", "ag", "ag2406");
	// 多: 昨10/可6, 今4/可4; 空: 昨3/可1, 今4/可4
	env.loadPosition("SHFE", "ag2406", 10, 6, 4, 4, 3, 1, 4, 4);

	// 平多多头今仓: 首推 4->2, 撤单回补 2->4
	uint32_t id1 = env.place(5000, 2, true, false, true);
	WTSOrderInfo* o1 = env.pushLast("R54-P1", WOS_NotTraded_Queuing, WOT_CLOSETODAY);
	EXPECT_DOUBLE_EQ(8.0, env.adapter.getPosition(kAg, true, 1));
	o1->setOrderState(WOS_Canceled);
	env.adapter.onPushOrder(o1);
	EXPECT_DOUBLE_EQ(10.0, env.adapter.getPosition(kAg, true, 1));
	o1->release();
	(void)id1;

	// 平空空头今仓: 首推 4->2, 撤单回补 2->4
	env.api._entrusts.clear();
	uint32_t id2 = env.place(5000, 2, false, false, true);
	WTSOrderInfo* o2 = env.pushLast("R54-P2", WOS_NotTraded_Queuing, WOT_CLOSETODAY);
	EXPECT_DOUBLE_EQ(-3.0, env.adapter.getPosition(kAg, true, 2));
	o2->setOrderState(WOS_Canceled);
	env.adapter.onPushOrder(o2);
	EXPECT_DOUBLE_EQ(-5.0, env.adapter.getPosition(kAg, true, 2));
	o2->release();
	(void)id2;

	// 平空空头昨仓: 可平昨 1 先扣完, 余 4 扣今可平; 撤单 5 手回补后
	// 昨可平 5 > 昨持仓 3, 超出的 2 夹回今仓
	env.api._entrusts.clear();
	uint32_t id3 = env.place(5000, 5, false, false, false);
	WTSOrderInfo* o3 = env.pushLast("R54-P3", WOS_NotTraded_Queuing, WOT_CLOSE);
	EXPECT_DOUBLE_EQ(0.0, env.adapter.getPosition(kAg, true, 2));
	o3->setOrderState(WOS_Canceled);
	env.adapter.onPushOrder(o3);
	EXPECT_DOUBLE_EQ(-5.0, env.adapter.getPosition(kAg, true, 2));
	o3->release();
	(void)id3;

	// 卖方错单 + 已撤: 统计旁支
	WTSOrderInfo* wrong = makeOrder("ag2406", "SHFE", env.bd.getContract("ag2406", "SHFE"),
		WDT_SHORT, WOT_OPEN, 2, WOS_Canceled, "R54-P4", "other");
	wrong->setError(WOEF_Normal);
	env.adapter.onPushOrder(wrong);
	wrong->release();

	// stateToName 的 AllTrd/Smtting/UnSmt/Error 四支
	const WTSOrderState states[4] = { WOS_AllTraded, WOS_Submitting, WOS_Nottouched,
		(WTSOrderState)1234 };
	for (int i = 0; i < 4; i++)
	{
		WTSOrderInfo* o = makeOrder("ag2406", "SHFE", env.bd.getContract("ag2406", "SHFE"),
			WDT_LONG, WOT_OPEN, 1, states[i], ("R54-S" + std::to_string(i)).c_str(), "other");
		o->setVolLeft(1);
		env.adapter.onPushOrder(o);
		o->release();
	}

	// 空头平今成交: s_newvol 4 -> 3
	WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
	t->setDirection(WDT_SHORT);
	t->setOffsetType(WOT_CLOSETODAY);
	t->setVolume(1);
	t->setPrice(4900.0);
	env.adapter.onPushTrade(t);
	t->release();
	EXPECT_DOUBLE_EQ(-6.0, env.adapter.getPosition(kAg, false, 2));

	// 市价平空: WPT_ANYPRICE
	env.api._entrusts.clear();
	uint32_t mid = env.adapter.closeShort(kAg, 0.0, 1.0, false, 0);
	ASSERT_NE(UINT_MAX, mid);
	ASSERT_EQ(1u, env.api._entrusts.size());
	EXPECT_EQ((int)WPT_ANYPRICE, env.api._entrusts[0]._ptype);
}

// TraderAdapter.cpp:241-244 enumPosition 的 stdCode 过滤 continue
// TraderAdapter.cpp:354-357 doCancel 对死单返回 false
// TraderAdapter.cpp:399-411 cancelAll 跳过非活单
TEST(R54TraderCallback, query_filter_and_dead_orders)
{
	r54::Env env("r54query", q04::policyJsonOpenCloseLimited());
	env.add("SHFE", "ag", "ag2406");
	env.add("SHFE", "au", "au2406");
	env.loadPosition("SHFE", "ag2406", 2, 2, 1, 1, 0, 0, 0, 0);
	env.loadPosition("SHFE", "au2406", 7, 7, 0, 0, 0, 0, 0, 0);
	// onRspPosition 已经发过 4 条持仓通知, 这里只看 enumPosition 的增量
	env.sink._positions.clear();

	// 带标准代码过滤: au 走 continue, 只汇总 ag
	EXPECT_DOUBLE_EQ(3.0, env.adapter.enumPosition(kAg));
	EXPECT_EQ(2u, env.sink._positions.size());
	// 空代码汇总全部
	env.sink._positions.clear();
	EXPECT_DOUBLE_EQ(10.0, env.adapter.enumPosition(""));
	EXPECT_EQ(4u, env.sink._positions.size());

	// 先推一张活单进 _orders, 再把它改成死单(不经过 onPushOrder)
	uint32_t id = env.place(5000, 1, true, true);
	WTSOrderInfo* ord = env.pushLast("R54-Q1", WOS_NotTraded_Queuing, WOT_OPEN);
	ord->setOrderState(WOS_Canceled);

	EXPECT_FALSE(env.adapter.cancel(id));
	EXPECT_TRUE(env.adapter.cancelAll("").empty());
	EXPECT_TRUE(env.api._actions.empty());
	// 死单仍在 _orders 里, 供后续回报链复用
	OrderMap* orders = env.adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
	ord->release();
}

// TraderAdapter.cpp:1414-1418 onRspTrades 只认 tInfo->getContractInfo(),
// 不像 onRspOrders(1286-1288)/onPushTrade(1739-1740) 会回落 _bd_mgr;
// 柜台查询回报不带 ContractInfo 时整条成交被丢弃, 风控统计永远不会推进。
// BUG-R54-001, 启用会稳定失败, 故整条 DISABLED。
TEST(DISABLED_R54TraderCallback, onRspTrades_without_contract_info_drops_stats)
{
	const char* kLimited = R"JSON({
	"default": {
		"order": [ {"action":"open","limit":0,"limit_s":0,"limit_l":1,"pure":false} ],
		"filters": []
	}
})JSON";
	r54::Env env("r54notradeci", kLimited);
	env.add("SHFE", "ag", "ag2406");

	WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
	t->setDirection(WDT_LONG);
	t->setOffsetType(WOT_OPEN);
	t->setVolume(2);
	t->setPrice(5000.0);
	WTSArray* ay = WTSArray::create();
	ay->append(t);
	env.adapter.onRspTrades(ay);
	ay->release();
	t->release();

	// 已开 2 手, limit_l=1 应把后续 1 手全部拦下; 实际统计为 0, 仍会发单
	EXPECT_TRUE(env.adapter.buy(kAg, 5000.0, 1.0, 0, false).empty());
	EXPECT_EQ(0u, env.api._entrusts.size());
}
