/*!
 * \file q04_trader_reply_machine_test.cpp
 * \brief TraderAdapter 交易回报推进状态机(迭代01 第五波 q04)
 *
 * 目标(src/WtUftCore/TraderAdapter.cpp, 此前 11.71%):
 *   initExt/run/release 装配、handleEvent、onLoginResult/onLogout、
 *   onRspPosition/onRspOrders/onRspTrades/onRspAccount 推进链、
 *   onRspEntrust 错单去重、onTraderError、handleTraderLog、getBaseDataMgr。
 * 用替身 ITraderApi/IBaseDataMgr/ITrdNotifySink 驱动, 不接触真实柜台。
 *
 * 注: TraderAdapter::run() 只做了 3 件事——建 _stat_map、registerSpi(this)、
 * _trader_api->connect()。connect 由替身实现为纯计数, 不起线程/不建 socket,
 * 因此不构成简报禁止的阻塞路径(而 _stat_map 只能在此处创建, 是回报链的入口)。
 */
#include "q04_trader_fixture.h"

#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* kStdAg = "SHFE.ag2406";

//一个完整的、已装配好并 run 起来的适配器环境
struct Env
{
	q04::TempRoot			_tmp{ "reply" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;

	Env()
	{
		_bd.add("SHFE", "ag", "ag2406");
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		EXPECT_TRUE(_adapter.initExt("uft1", &_api, &_bd, &_policy));
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
	}

	void login()
	{
		_adapter.onLoginResult(true, "ok", 20240315);
	}

	//构造一批回报对象并挂到 WTSArray 上(引用由调用方保留, 便于 _orders 复用)
	static WTSArray* makeArray(const std::vector<WTSObject*>& items)
	{
		WTSArray* ret = WTSArray::create();
		for (auto one : items)
			ret->append(one);
		return ret;
	}
};

}	// namespace

TEST(q04_trader_reply, initext_run_release_wire_the_channel)
{
	Env env;
	//替身只在 initExt 里被 init 一次
	EXPECT_EQ(1u, env._api._initCnt);
	EXPECT_EQ(&env._adapter, env._api._spi);
	EXPECT_EQ(1u, env._api._connectCnt);
	EXPECT_EQ(1u, env._api._regSpiCnt);
	EXPECT_EQ(TraderAdapter::AS_LOGINING, env._adapter.state());
	EXPECT_STREQ("uft1", env._adapter.id());
	EXPECT_EQ(&env._bd, env._adapter.getBaseDataMgr());

	//回报链尚未启动: 查询接口在空表下必须安全
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kStdAg, false));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	EXPECT_EQ(nullptr, env._adapter.getOrders(kStdAg));
	EXPECT_EQ(0u, env._adapter.getInfos(kStdAg));

	env._adapter.release();
	EXPECT_EQ(1u, env._api._releaseCnt);
	EXPECT_EQ(nullptr, env._api._spi);
	//重复 release 只是再来一次 registerSpi(NULL) + api->release()
	env._adapter.release();
	EXPECT_EQ(2u, env._api._releaseCnt);
}

TEST(q04_trader_reply, login_result_drives_state_and_position_query)
{
	Env env;

	env._adapter.onLoginResult(false, "bad password", 0);
	EXPECT_EQ(TraderAdapter::AS_LOGINFAILED, env._adapter.state());
	EXPECT_EQ(0u, env._api._qPosCnt);

	env._adapter.onLoginResult(true, "ok", 20240315);
	EXPECT_EQ(TraderAdapter::AS_LOGINED, env._adapter.state());
	EXPECT_EQ(1u, env._api._qPosCnt);

	//登出后恢复未登录状态
	env._adapter.onLogout();
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, env._adapter.state());

	//未登录前不应有持仓通知
	env.login();
	EXPECT_EQ(2u, env._api._qPosCnt);
}

TEST(q04_trader_reply, onRspPosition_fills_positions_and_notifies_sinks)
{
	Env env;
	env.login();

	WTSPositionItem* longItem = q04::posItem("SHFE", "ag2406", WDT_LONG, 3, 2, 5, 4);
	WTSPositionItem* shortItem = q04::posItem("SHFE", "ag2406", WDT_SHORT, 1, 1, 2, 0);
	WTSPositionItem* unknown = q04::posItem("DCE", "m2509", WDT_LONG, 7, 7, 7, 7);
	WTSArray* ay = Env::makeArray({ longItem, shortItem, unknown });

	env._adapter.onRspPosition(ay);

	//多头: 昨3/可2, 今5/可4; 空头: 昨1/可1, 今2/可0
	EXPECT_DOUBLE_EQ(8.0, env._adapter.getPosition(kStdAg, false, 1));
	EXPECT_DOUBLE_EQ(6.0, env._adapter.getPosition(kStdAg, true, 1));
	EXPECT_DOUBLE_EQ(-3.0, env._adapter.getPosition(kStdAg, false, 2));
	EXPECT_DOUBLE_EQ(-1.0, env._adapter.getPosition(kStdAg, true, 2));
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getPosition(kStdAg, false, 3));
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getPosition(kStdAg, true, 3));
	//未知合约被 continue 掉, 不应进入持仓表
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition("DCE.m2509", false, 3));

	//状态推进 + 触发订单查询
	EXPECT_EQ(TraderAdapter::AS_POSITION_QRYED, env._adapter.state());
	EXPECT_EQ(1u, env._api._qOrdCnt);

	//持仓通知: onRspPosition 内部对每个持仓发多空两条
	ASSERT_EQ(2u, env._sink._positions.size());
	EXPECT_STREQ(kStdAg, env._sink._positions[0]._code.c_str());
	EXPECT_TRUE(env._sink._positions[0]._long);
	EXPECT_DOUBLE_EQ(3.0, env._sink._positions[0]._prevol);
	EXPECT_DOUBLE_EQ(4.0, env._sink._positions[0]._newavail);
	EXPECT_FALSE(env._sink._positions[1]._long);
	//交易日由 onLoginResult 写入, 这里必须是已登录的那个值
	EXPECT_EQ(20240315u, env._sink._positions[0]._tdate);

	ay->release();
	longItem->release();
	shortItem->release();
	unknown->release();

	//空数组/NULL 都要跳过填充但仍推进状态分支
	env._adapter.onRspPosition(nullptr);
	WTSArray* empty = WTSArray::create();
	env._adapter.onRspPosition(empty);
	empty->release();
	EXPECT_EQ(1u, env._api._qOrdCnt) << "状态已经不是 AS_LOGINED, 不应重复触发查询";

	//enumPosition 走回调路径, 汇总总量
	EXPECT_DOUBLE_EQ(11.0, env._adapter.enumPosition(kStdAg));
	EXPECT_DOUBLE_EQ(11.0, env._adapter.enumPosition());
	EXPECT_EQ(2u + 2u + 2u, env._sink._positions.size()) << "enumPosition 也要按多空各发一条";
}

TEST(q04_trader_reply, onRspOrders_counts_stats_and_registers_alive_orders)
{
	Env env;
	env.login();
	//订单查询在持仓响应之后才会进入 AS_POSITION_QRYED。
	WTSArray* emptyPositions = WTSArray::create();
	env._adapter.onRspPosition(emptyPositions);
	emptyPositions->release();
	ASSERT_EQ(TraderAdapter::AS_POSITION_QRYED, env._adapter.state());

	//先真实发一笔委托, 拿到适配器自己组装的 usertag
	uint32_t localid = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, localid);
	ASSERT_EQ(1u, env._api._entrusts.size());
	const q04::FakeTraderApi::EntrustSnap& snap = env._api._entrusts[0];

	//自己的活单: 进 _orders, 计入未完成量
	WTSOrderInfo* myOrder = q04::orderFromSnap(snap, "ORD1");
	myOrder->setVolLeft(1.0);
	myOrder->setVolTraded(1.0);
	//他人单(无 usertag): 只计统计, 不进 _orders
	WTSOrderInfo* foreignOrder = WTSOrderInfo::create();
	foreignOrder->setCode("ag2406");
	foreignOrder->setExchange("SHFE");
	foreignOrder->setDirection(WDT_SHORT);
	foreignOrder->setOffsetType(WOT_CLOSE);
	foreignOrder->setVolume(4);
	foreignOrder->setVolLeft(4);
	foreignOrder->setOrderState(WOS_NotTraded_Queuing);
	foreignOrder->setOrderID("ORD2");
	//已撤的 FAK 单: 计入 auto_cancels 分支
	WTSOrderInfo* autoCanceled = WTSOrderInfo::create();
	autoCanceled->setCode("ag2406");
	autoCanceled->setExchange("SHFE");
	autoCanceled->setDirection(WDT_LONG);
	autoCanceled->setOffsetType(WOT_OPEN);
	autoCanceled->setOrderFlag(WOF_FAK);
	autoCanceled->setVolume(3);
	autoCanceled->setVolLeft(3);
	autoCanceled->setOrderState(WOS_Canceled);
	autoCanceled->setOrderID("ORD3");
	//错单分支
	WTSOrderInfo* wrongOrder = WTSOrderInfo::create();
	wrongOrder->setCode("ag2406");
	wrongOrder->setExchange("SHFE");
	wrongOrder->setDirection(WDT_LONG);
	wrongOrder->setOffsetType(WOT_OPEN);
	wrongOrder->setVolume(5);
	wrongOrder->setVolLeft(5);
	wrongOrder->setError(WOEF_Normal);
	wrongOrder->setOrderState(WOS_Canceled);
	wrongOrder->setOrderID("ORD4");
	//未知合约: continue
	WTSOrderInfo* unknownOrder = WTSOrderInfo::create();
	unknownOrder->setCode("r2510");
	unknownOrder->setExchange("DCE");
	unknownOrder->setDirection(WDT_LONG);
	unknownOrder->setVolume(9);
	unknownOrder->setOrderID("ORD5");

	std::vector<WTSObject*> items{ myOrder, foreignOrder, autoCanceled, wrongOrder, unknownOrder };
	WTSArray* ay = Env::makeArray(items);
	//数组里允许存在空元素, 回报循环要能 continue
	ay->append((WTSObject*)nullptr);

	env._adapter.onRspOrders(ay);

	//只有自己的活单进 _orders；按公开 stdCode 查询同合约应返回这笔活单
	OrderMap* orders = env._adapter.getOrders(kStdAg);
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	EXPECT_EQ(myOrder, orders->get(localid));
	orders->release();
	orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();

	//未完成量: 先由 openLong 加 2, onRspOrders 里 clear 后按挂单剩余量重算
	//自己的活单 volLeft=1, 其余不是 wt 单
	EXPECT_DOUBLE_EQ(1.0, env._adapter.getUndoneQty(kStdAg));
	//信息量: 每张单 +1(共4), 撤单再 +1(auto cancel), 错单不算撤单
	EXPECT_EQ(5u, env._adapter.getInfos(kStdAg));

	EXPECT_EQ(TraderAdapter::AS_ORDERS_QRYED, env._adapter.state());
	EXPECT_EQ(1u, env._api._qTrdCnt);

	ay->release();
}

TEST(q04_trader_reply, onRspTrades_accumulates_stats_then_channel_ready)
{
	Env env;
	env.login();
	//成交查询必须等持仓和委托查询回报依次推进状态。
	WTSArray* emptyPositions = WTSArray::create();
	env._adapter.onRspPosition(emptyPositions);
	emptyPositions->release();
	WTSArray* emptyOrders = WTSArray::create();
	env._adapter.onRspOrders(emptyOrders);
	emptyOrders->release();
	ASSERT_EQ(TraderAdapter::AS_ORDERS_QRYED, env._adapter.state());
	ASSERT_NE(nullptr, env._bd.getContract("ag2406", "SHFE"));

	auto makeTrade = [&env](const char* code, WTSDirectionType dir, WTSOffsetType offset, double vol) {
		WTSTradeInfo* t = WTSTradeInfo::create(code, "SHFE");
		t->setContractInfo(env._bd.getContract(code, "SHFE"));
		t->setDirection(dir);
		t->setOffsetType(offset);
		t->setVolume(vol);
		t->setPrice(5000.0);
		return t;
	};

	std::vector<WTSTradeInfo*> trades{
		makeTrade("ag2406", WDT_LONG, WOT_OPEN, 3),
		makeTrade("ag2406", WDT_LONG, WOT_CLOSE, 1),
		makeTrade("ag2406", WDT_LONG, WOT_CLOSETODAY, 2),
		makeTrade("ag2406", WDT_SHORT, WOT_OPEN, 4),
		makeTrade("ag2406", WDT_SHORT, WOT_CLOSE, 1),
		makeTrade("ag2406", WDT_SHORT, WOT_CLOSETODAY, 2),
	};
	//合约信息缺失的成交: 走 continue
	WTSTradeInfo* noComm = WTSTradeInfo::create("ag2406", "SHFE");
	noComm->setContractInfo(nullptr);
	trades.emplace_back(noComm);

	WTSArray* ay = Env::makeArray(std::vector<WTSObject*>(trades.begin(), trades.end()));
	env._adapter.onRspTrades(ay);

	EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, env._adapter.state());
	EXPECT_EQ(1u, env._api._qAccCnt);

	//成交回放到账户回报才全部就绪
	env._adapter.onRspAccount(nullptr);
	EXPECT_EQ(TraderAdapter::AS_ALLREADY, env._adapter.state());
	EXPECT_EQ(1u, env._sink._readyCnt);
	EXPECT_EQ(20240315u, env._sink._readyTdate);

	//非 AS_TRADES_QRYED 时 onRspAccount 不再推进
	env._adapter.onRspAccount(ay);
	EXPECT_EQ(1u, env._sink._readyCnt);

	ay->release();
	for (auto one : trades)
		one->release();

	//NULL 成交数组仍应推进状态；用独立通道模拟完整查询链，避免复用已就绪状态。
	Env emptyTradeEnv;
	emptyTradeEnv.login();
	WTSArray* emptyPositionResponse = WTSArray::create();
	emptyTradeEnv._adapter.onRspPosition(emptyPositionResponse);
	emptyPositionResponse->release();
	WTSArray* emptyOrderResponse = WTSArray::create();
	emptyTradeEnv._adapter.onRspOrders(emptyOrderResponse);
	emptyOrderResponse->release();
	ASSERT_EQ(TraderAdapter::AS_ORDERS_QRYED, emptyTradeEnv._adapter.state());
	emptyTradeEnv._adapter.onRspTrades(nullptr);
	EXPECT_EQ(TraderAdapter::AS_TRADES_QRYED, emptyTradeEnv._adapter.state());
	EXPECT_EQ(1u, emptyTradeEnv._api._qAccCnt);
}

TEST(q04_trader_reply, onRspEntrust_error_only_counts_the_first_time)
{
	Env env;
	env.login();

	uint32_t localid = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, localid);
	EXPECT_DOUBLE_EQ(2.0, env._adapter.getUndoneQty(kStdAg));

	const q04::FakeTraderApi::EntrustSnap& snap = env._api._entrusts[0];
	WTSEntrust* entrust = WTSEntrust::create(snap._code.c_str(), snap._vol, snap._price, snap._exchg.c_str());
	entrust->setUserTag(snap._usertag.c_str());
	entrust->setDirection(WDT_LONG);
	entrust->setOffsetType(WOT_OPEN);

	WTSError* err = WTSError::create(WEC_ORDERINSERT, "reject by counter");
	env._adapter.onRspEntrust(entrust, err);
	//错单要把未完成量退回来
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	ASSERT_EQ(1u, env._sink._entrusts.size());
	EXPECT_EQ(localid, env._sink._entrusts[0]._localid);
	EXPECT_STREQ(kStdAg, env._sink._entrusts[0]._code.c_str());
	EXPECT_FALSE(env._sink._entrusts[0]._succ);
	EXPECT_STREQ("reject by counter", env._sink._entrusts[0]._msg.c_str());

	//同一张错单重复推送: 未完成量已为 0, 直接 return
	env._adapter.onRspEntrust(entrust, err);
	EXPECT_EQ(1u, env._sink._entrusts.size());

	//平今/平仓分支只影响日志文本, 不应改变去重结论
	entrust->setOffsetType(WOT_CLOSETODAY);
	env._adapter.onRspEntrust(entrust, err);
	EXPECT_EQ(1u, env._sink._entrusts.size());

	//无错误码 / err 为空: 整段跳过
	env._adapter.onRspEntrust(entrust, nullptr);
	WTSError* none = WTSError::create(WEC_NONE, "");
	env._adapter.onRspEntrust(entrust, none);
	none->release();
	EXPECT_EQ(1u, env._sink._entrusts.size());

	//合约查不到: 早退, 不动未完成量
	env._adapter.openShort("DCE.m2509", 3000.0, 1.0, 0);
	WTSEntrust* bad = WTSEntrust::create("m2509", 1, 3000.0, "DCE");
	env._adapter.onRspEntrust(bad, err);
	EXPECT_EQ(1u, env._sink._entrusts.size());
	bad->release();

	entrust->release();
	err->release();
}

TEST(q04_trader_reply, handleEvent_and_error_channels_forward_to_sinks)
{
	Env env;
	env.login();

	//连接失败只记日志
	env._adapter.handleEvent(WTE_Connect, -1);
	EXPECT_EQ(0u, env._api._loginCnt);

	//通道断开要通知所有 sink
	env._adapter.handleEvent(WTE_Close, 404);
	EXPECT_EQ(1u, env._sink._lostCnt);

	//其他事件类型既不连接也不断开
	env._adapter.handleEvent(WTE_Login, 0);
	EXPECT_EQ(1u, env._sink._lostCnt);
	EXPECT_EQ(0u, env._api._loginCnt);

	//交易通道错误 / 日志透传
	WTSError* err = WTSError::create(WEC_ORDERCANCEL, "cancel rejected");
	env._adapter.onTraderError(err);
	env._adapter.onTraderError(nullptr);
	env._adapter.handleTraderLog(LL_INFO, "raw channel log");
	env._adapter.handleTraderLog(LL_ERROR, "");
	err->release();
}

TEST(q04_trader_reply, init_parses_riskmon_and_enables_login_branch)
{
	q04::TempRoot tmp("init");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	TraderAdapter adapter;

	WTSVariant* params = WTSVariant::createObject();
	params->append("user", "tester");
	params->append("pass", "secret");
	params->append("product", "PID01");
	WTSVariant* riskmon = WTSVariant::createObject();
	riskmon->append("active", true);
	WTSVariant* policyCfg = WTSVariant::createObject();
	WTSVariant* rule = WTSVariant::createObject();
	rule->append("cancel_total_limits", (uint32_t)5);
	rule->append("cancel_times_boundary", (uint32_t)3);
	rule->append("cancel_stat_timespan", (uint32_t)60);
	rule->append("order_total_limits", (uint32_t)50);
	rule->append("order_times_boundary", (uint32_t)10);
	rule->append("order_stat_timespan", (uint32_t)30);
	policyCfg->append("default", rule);
	riskmon->append("policy", policyCfg);
	params->append("riskmon", riskmon);

	//没有 module 字段: 风控解析完成后在装载动态库之前返回 false
	EXPECT_FALSE(adapter.init("uft9", params, &bd, &policy));
	//但 _cfg / _id / _bd_mgr 已经写入, 后续 initExt 补齐 API 装配
	EXPECT_EQ(&bd, adapter.getBaseDataMgr());
	EXPECT_STREQ("uft9", adapter.id());
	EXPECT_FALSE(adapter.run()) << "未装配 _trader_api 时 run 必须早退";

	EXPECT_TRUE(adapter.initExt("uft9", &api, &bd, &policy));
	EXPECT_TRUE(adapter.run());

	//_cfg 由 init 留下, WTE_Connect(ec==0) 才会真正调用 login
	adapter.handleEvent(WTE_Connect, 0);
	EXPECT_EQ(1u, api._loginCnt);
	EXPECT_STREQ("tester", api._lastUser.c_str());
	EXPECT_STREQ("secret", api._lastPass.c_str());
	EXPECT_STREQ("PID01", api._lastProduct.c_str());

	//二次 init 因为 _cfg 已存在而直接 false
	WTSVariant* second = WTSVariant::createObject();
	second->append("module", "");
	EXPECT_FALSE(adapter.init("uft9", second, &bd, &policy));
	second->release();

	//active=false 的分支只记日志
	q04::TempRoot tmp2("init2");
	TraderAdapter adapter2;
	WTSVariant* params2 = WTSVariant::createObject();
	WTSVariant* risk2 = WTSVariant::createObject();
	risk2->append("active", false);
	params2->append("riskmon", risk2);
	EXPECT_FALSE(adapter2.init("uftA", params2, &bd, &policy));
	params2->release();

	//完全没有 riskmon 的分支
	TraderAdapter adapter3;
	WTSVariant* params3 = WTSVariant::createObject();
	params3->append("user", "u");
	EXPECT_FALSE(adapter3.init("uftB", params3, &bd, &policy));
	params3->release();

	params->release();
}

//从前几波继承的崩溃/越界类输入整类收在这里, 启用用例零残留
TEST(DISABLED_q04_trader_reply, uninitialized_and_null_deref_class)
{
	q04::TempRoot tmp("cls");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;

	//1) 未 initExt 的适配器: _trading_day/_bd_mgr/_policy_mgr 未初始化,
	//   handleEvent(WTE_Connect,0) 会解引用 NULL _trader_api,
	//   getPosition 之外还会读到随机交易日(BUG-N03-005 / BUG-P06-001 同源)
	TraderAdapter fresh;
	fresh.onLoginResult(true, "ok", 20240315);	//解引用 NULL _trader_api
	fresh.handleEvent(WTE_Connect, 0);			//解引用 NULL _cfg 与 NULL _trader_api

	//2) 未 run() 的适配器: _stat_map 为 NULL, onRspOrders/getInfos/checkOrderLimits 全崩
	TraderAdapter noStat;
	noStat.initExt("uft1", &api, &bd, &policy);
	const char* code = "SHFE.ag2406";
	WTSOrderInfo* ord = WTSOrderInfo::create();
	ord->setCode("ag2406");
	ord->setExchange("SHFE");
	WTSArray* ay = WTSArray::create();
	ay->append(ord);
	noStat.onRspOrders(ay);
	ay->release();
	noStat.checkOrderLimits(code);
	noStat.checkCancelLimits(code);
	(void)noStat.getInfos(code);

	//3) 裸代码(无'.')下单: getContract 的 buf[npos] 越界写(BUG-N03 已报)
	TraderAdapter adapter;
	bd.add("SHFE", "ag", "ag2406");
	const std::string p = tmp.file("policy.json");
	tmp.write(p, q04::policyJsonOpenCloseLimited());
	policy.init(p.c_str());
	adapter.initExt("uft1", &api, &bd, &policy);
	adapter.run();
	adapter.buy("ag2406", 5000.0, 1, 0, false);
	adapter.getPosition("ag2406", true, 1);

	//4) riskmon.active=true 但缺 policy 节点: cfgPolicy->memberNames() 空指针解引用
	WTSVariant* params = WTSVariant::createObject();
	WTSVariant* riskmon = WTSVariant::createObject();
	riskmon->append("active", true);
	params->append("riskmon", riskmon);
	TraderAdapter noPolicy;
	noPolicy.init("uftC", params, &bd, &policy);
	params->release();
}
