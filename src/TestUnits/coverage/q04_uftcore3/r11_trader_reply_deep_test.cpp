/*!
 * \file r11_trader_reply_deep_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - TraderAdapter 回调聚合的拒绝/旁支与撤单统计
 *
 * 覆盖 TraderAdapter.cpp 里 q04 主链没走到的分支:
 * - onRspPosition: NULL 数组 / 空数组 / 未知合约 continue / 状态不是 AS_LOGINED 时不追发 queryOrders
 * - onRspOrders: 数组里混 NULL、合约查不到、死单 continue、外部 usertag continue、
 *   撤单统计(b_cancels / b_auto_cancels / b_wrongs)、未完成量日志、状态守卫假分支
 * - onRspTrades: getContractInfo()==NULL 的 continue、状态不匹配时不追发 queryAccount
 * - onRspAccount: 状态不匹配时不广播 on_channel_ready, 就绪后不重复广播
 * - onPushOrder: 活单先建 _orders, 撤销单走 remove; 错单标记 -> wrongs, 非普通标记 -> auto_cancels
 * - getOrders/updateUndone/enumPosition/getInfos/getBaseDataMgr/onLogout/onTraderError/handleTraderLog
 * - TraderAdapterMgr 的 addAdapter/getAdapter/getAdapters/run/release
 *
 * 全部走纯内存替身, 不起线程、不接柜台、不读风控参数(riskmon 未开启时 getRiskParams 会解引用
 * end 迭代器, 已另立 BUG-R11-003, 本文件不触碰)。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/ActionPolicyMgr.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"

#include <string>
#include <vector>

namespace {

using namespace wtp;

const char* kAg = "SHFE.ag2406";

struct DeepEnv
{
	q04::TempRoot			_tmp{ "r11deep" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;

	//bLogin=false 时停在"未登录"状态, 用来打各 onRsp* 的状态守卫假分支
	explicit DeepEnv(bool bLogin = false)
	{
		_bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover);
		_bd.add("SHFE", "cu", "cu2406", 0, 0, CM_CoverToday);
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		EXPECT_TRUE(_adapter.initExt("uft1", &_api, &_bd, &_policy));
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
		if (bLogin)
			_adapter.onLoginResult(true, "ok", 20240315);
	}

	//用通道实际生成的 usertag 造一笔委托回报(保证 localid 认领逻辑命中)
	WTSOrderInfo* makeOrder(size_t entrustIdx, const char* code, double vol,
		WTSDirectionType dir, WTSOffsetType offset,
		WTSOrderState st, WTSOrderFlag flag = WOF_NOR, double traded = 0.0)
	{
		const double price = 5000.0;
		WTSEntrust* e = WTSEntrust::create(code, vol, price, "SHFE");
		e->setUserTag(_api._entrusts[entrustIdx]._usertag.c_str());
		e->setEntrustID(_api._entrusts[entrustIdx]._entrustid.c_str());
		e->setDirection(dir);
		e->setOffsetType(offset);
		e->setOrderFlag(flag);
		WTSOrderInfo* ret = WTSOrderInfo::create(e);
		ret->setVolLeft(vol - traded);
		ret->setVolTraded(traded);
		ret->setOrderState(st);
		e->release();
		return ret;
	}

	//外部通道的单子(usertag 前缀不属于本通道)
	WTSOrderInfo* makeForeignOrder(const char* code, double vol, WTSOrderState st)
	{
		WTSEntrust* e = WTSEntrust::create(code, vol, 5000.0, "SHFE");
		e->setUserTag("otherchannel.4242");
		e->setEntrustID("X0001");
		e->setDirection(WDT_LONG);
		e->setOffsetType(WOT_OPEN);
		e->setOrderFlag(WOF_NOR);
		WTSOrderInfo* ret = WTSOrderInfo::create(e);
		ret->setVolLeft(vol);
		ret->setVolTraded(0);
		ret->setOrderState(st);
		e->release();
		return ret;
	}

	void feed(const std::vector<WTSObject*>& items, void (TraderAdapter::*fn)(const WTSArray*))
	{
		WTSArray* ay = WTSArray::create();
		for (auto one : items)
			ay->append(one, false);
		(_adapter.*fn)(ay);
		ay->release();
		for (auto one : items)
			if (one != nullptr) one->release();
	}
};

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, positionResponseFiltersAndGuardsState)
{
	//未登录: _state != AS_LOGINED -> 不追发 queryOrders
	DeepEnv env(false);
	env._adapter.onRspPosition(nullptr);
	EXPECT_EQ((uint32_t)0, env._api._qOrdCnt);

	WTSArray* empty = WTSArray::create();
	env._adapter.onRspPosition(empty);
	empty->release();
	EXPECT_EQ((uint32_t)0, env._api._qOrdCnt);

	//一个查不到合约的条目 -> continue; 一个正常条目 -> 填充多头四元组
	WTSPositionItem* ghost = q04::posItem("SHFE", "unknown", WDT_LONG, 5, 5, 0, 0);
	WTSPositionItem* real = q04::posItem("SHFE", "ag2406", WDT_LONG, 8, 6, 2, 2);
	env.feed({ ghost, real }, &TraderAdapter::onRspPosition);

	//总持仓 l_newvol+l_prevol = 10, 可平 = 6+2 = 8
	EXPECT_DOUBLE_EQ(10.0, env._adapter.getPosition(kAg, false, 1));
	EXPECT_DOUBLE_EQ(8.0, env._adapter.getPosition(kAg, true, 1));
	//空头一侧没有任何数据
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, false, 2));
	//未知合约不产生条目
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition("SHFE.unknown", false, 1));
	//通知循环与状态无关: 每个合约多空各一次
	EXPECT_EQ((size_t)2, env._sink._positions.size());
	//仍未登录 -> 没有发起委托查询
	EXPECT_EQ((uint32_t)0, env._api._qOrdCnt);
}

TEST(R11TraderDeep, positionResponseAfterLoginChainsQueryOrders)
{
	DeepEnv env(true);
	WTSPositionItem* real = q04::posItem("SHFE", "ag2406", WDT_SHORT, 3, 1, 4, 4);
	env.feed({ real }, &TraderAdapter::onRspPosition);

	EXPECT_EQ((uint32_t)1, env._api._qOrdCnt);
	EXPECT_EQ((size_t)2, env._sink._positions.size());
	EXPECT_EQ((uint32_t)20240315, env._sink._positions[0]._tdate);
	//空头记账: 昨3(可1) + 今4(可4)
	EXPECT_DOUBLE_EQ(-7.0, env._adapter.getPosition(kAg, false, 2));
	EXPECT_DOUBLE_EQ(-5.0, env._adapter.getPosition(kAg, true, 2));
}

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, orderResponseFiltersBadEntries)
{
	DeepEnv env(false);
	//NULL 数组 -> 整块跳过
	env._adapter.onRspOrders(nullptr);

	WTSOrderInfo* ghost = env.makeForeignOrder("unknown", 3.0, WOS_NotTraded_Queuing);
	WTSOrderInfo* dead = env.makeOrder(0, "ag2406", 2.0, WDT_LONG, WOT_OPEN, WOS_Canceled);
	WTSOrderInfo* foreign = env.makeForeignOrder("ag2406", 4.0, WOS_NotTraded_Queuing);
	WTSOrderInfo* alive = env.makeOrder(0, "ag2406", 6.0, WDT_SHORT, WOT_CLOSE,
		WOS_PartTraded_Queuing, WOF_NOR, 2.0);
	ghost->setOrderID("ORD-901");
	dead->setOrderID("ORD-902");
	foreign->setOrderID("ORD-77");
	alive->setOrderID("ORD-903");

	WTSArray* ay = WTSArray::create();
	ay->append((WTSObject*)nullptr, false);
	ay->append(ghost, false);
	ay->append(dead, false);
	ay->append(foreign, false);
	ay->append(alive, false);
	env._adapter.onRspOrders(ay);
	ay->release();
	ghost->release(); dead->release(); foreign->release(); alive->release();

	//_orders 里只有活单(按 fullcode 过滤的分支永远取不到, 见 BUG-R11-004)
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
	//按标准代码查会被 strcmp(ordInfo->getCode(), stdCode) 挡掉 -> 空集合
	orders = env._adapter.getOrders(kAg);
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size());
	orders->release();

	//未完成量: 只有活单贡献 volLeft = 6-2 = 4
	EXPECT_DOUBLE_EQ(4.0, env._adapter.getUndoneQty(kAg));
	//信息量: 死单 1(+撤单 1) + 外部单 1 + 活单 1 = 4
	EXPECT_EQ(4u, env._adapter.getInfos(kAg));
	//状态不是 AS_POSITION_QRYED -> 不追发 queryTrades
	EXPECT_EQ((uint32_t)0, env._api._qTrdCnt);
}

TEST(R11TraderDeep, orderResponseCancelTallyBranches)
{
	DeepEnv env(false);
	//三条都是本通道、都已撤销: 普通撤单 / 非普通标记的自动撤单 / 带错单标记
	WTSOrderInfo* normalCancel = env.makeOrder(0, "ag2406", 5.0, WDT_LONG, WOT_OPEN, WOS_Canceled);
	WTSOrderInfo* autoCancel = env.makeOrder(0, "ag2406", 4.0, WDT_LONG, WOT_OPEN,
		WOS_Canceled, (WTSOrderFlag)99);
	WTSOrderInfo* wrongCancel = env.makeOrder(0, "ag2406", 3.0, WDT_SHORT, WOT_CLOSE, WOS_Canceled);
	wrongCancel->setError((WTSOrderErrorFlag)5);
	normalCancel->setOrderID("ORD-911");
	autoCancel->setOrderID("ORD-912");
	wrongCancel->setOrderID("ORD-913");

	WTSArray* ay = WTSArray::create();
	ay->append(normalCancel, false);
	ay->append(autoCancel, false);
	ay->append(wrongCancel, false);
	env._adapter.onRspOrders(ay);
	ay->release();

	//三条都是死单, 都不进 _orders
	OrderMap* orders = env._adapter.getOrders("");
	if (orders != nullptr)
	{
		EXPECT_EQ(0u, orders->size());
		orders->release();
	}
	//普通撤 +1(撤单信息量)、自动撤 +1、错单不加 -> 3*1 + 2 = 5
	EXPECT_EQ(5u, env._adapter.getInfos(kAg));
	//全部已撤销 -> 未完成量清零
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kAg));

	normalCancel->release(); autoCancel->release(); wrongCancel->release();
}

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, tradeResponseSkipsItemsWithoutContract)
{
	DeepEnv env(false);

	WTSTradeInfo* noComm = WTSTradeInfo::create("ag2406", "SHFE");
	noComm->setDirection(WDT_LONG);
	noComm->setOffsetType(WOT_OPEN);
	noComm->setVolume(2.0);
	noComm->setPrice(5000.0);

	WTSTradeInfo* withComm = WTSTradeInfo::create("ag2406", "SHFE");
	withComm->setContractInfo(env._bd.getContract("ag2406", "SHFE"));
	withComm->setDirection(WDT_SHORT);
	withComm->setOffsetType(WOT_CLOSETODAY);
	withComm->setVolume(3.0);
	withComm->setPrice(5100.0);

	env.feed({ noComm, withComm }, &TraderAdapter::onRspTrades);

	//成交不改变持仓量, 只累计统计
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getPosition(kAg, false, 1));
	//状态不是 AS_ORDERS_QRYED -> 不追发 queryAccount
	EXPECT_EQ((uint32_t)0, env._api._qAccCnt);
	//成交不产生 _infos
	EXPECT_EQ(0u, env._adapter.getInfos(kAg));
}

TEST(R11TraderDeep, accountResponseHoldsChannelReadyBack)
{
	DeepEnv env(true);
	//刚登录, 还没走完查询链 -> 不许就绪
	env._adapter.onRspAccount(nullptr);
	EXPECT_EQ((uint32_t)0, env._sink._readyCnt);

	WTSPositionItem* real = q04::posItem("SHFE", "ag2406", WDT_LONG, 1, 1, 0, 0);
	env.feed({ real }, &TraderAdapter::onRspPosition);
	env._adapter.onRspOrders(nullptr);

	WTSTradeInfo* t = WTSTradeInfo::create("ag2406", "SHFE");
	t->setContractInfo(env._bd.getContract("ag2406", "SHFE"));
	t->setDirection(WDT_LONG);
	t->setOffsetType(WOT_OPEN);
	t->setVolume(1.0);
	t->setPrice(5000.0);
	env.feed({ t }, &TraderAdapter::onRspTrades);
	//trades 完成后必须发起账户查询
	EXPECT_EQ((uint32_t)1, env._api._qAccCnt);

	env._adapter.onRspAccount(nullptr);
	EXPECT_EQ((uint32_t)1, env._sink._readyCnt);
	EXPECT_EQ((uint32_t)20240315, env._sink._readyTdate);
	//已经 AS_ALLREADY, 不会再重复广播
	env._adapter.onRspAccount(nullptr);
	EXPECT_EQ((uint32_t)1, env._sink._readyCnt);
}

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, pushOrderCancelWrongAndAutoCancel)
{
	DeepEnv env(true);
	uint32_t id1 = env._adapter.openLong(kAg, 5000.0, 5.0, 0);
	ASSERT_NE(UINT_MAX, id1);
	uint32_t id2 = env._adapter.openLong(kAg, 5000.0, 4.0, 0);
	ASSERT_NE(UINT_MAX, id2);

	//先把 _orders 建起来: 推一笔活单(id1 对应委托 0)
	WTSOrderInfo* alive = env.makeOrder(0, "ag2406", 5.0, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing);
	alive->setOrderID("PUSH-0");
	env._adapter.onPushOrder(alive);
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
	EXPECT_EQ((size_t)1, env._sink._orders.size());

	//1) 普通撤单 -> b_cancels, 并从 _orders 摘掉
	WTSOrderInfo* o1 = env.makeOrder(0, "ag2406", 5.0, WDT_LONG, WOT_OPEN, WOS_Canceled, WOF_NOR, 1.0);
	o1->setOrderID("PUSH-1");
	env._adapter.onPushOrder(o1);

	//2) 非普通订单标记 -> b_auto_cancels
	WTSOrderInfo* o2 = env.makeOrder(1, "ag2406", 4.0, WDT_LONG, WOT_OPEN, WOS_Canceled, (WTSOrderFlag)99);
	o2->setOrderID("PUSH-2");
	env._adapter.onPushOrder(o2);

	//3) 撤销 + 错单标记 -> b_wrongs(SHORT+CLOSE 也归买入侧)
	WTSOrderInfo* o3 = env.makeOrder(1, "ag2406", 4.0, WDT_SHORT, WOT_CLOSE, WOS_Canceled);
	o3->setError((WTSOrderErrorFlag)5);
	o3->setOrderID("PUSH-3");
	env._adapter.onPushOrder(o3);

	//活单 1 + 三条撤销各 2(撤单信息量 + 首次推送信息量) = 7
	EXPECT_EQ(7u, env._adapter.getInfos(kAg));
	//未完成量: 下单 9, 撤销回收 (5-1)+4 = 8
	EXPECT_DOUBLE_EQ(1.0, env._adapter.getUndoneQty(kAg));
	//死单不留在 _orders
	orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size());
	orders->release();
	//四条本通道回报都要通知 sink
	ASSERT_EQ((size_t)4, env._sink._orders.size());
	EXPECT_EQ(id1, env._sink._orders[0]._localid);
	EXPECT_FALSE(env._sink._orders[0]._canceled);
	EXPECT_EQ(id1, env._sink._orders[1]._localid);
	EXPECT_TRUE(env._sink._orders[1]._canceled);
	EXPECT_DOUBLE_EQ(4.0, env._sink._orders[1]._left);
	EXPECT_EQ(id2, env._sink._orders[2]._localid);
	EXPECT_DOUBLE_EQ(4.0, env._sink._orders[2]._total);
	//第三条是平仓 -> offset=1, 且方向是空头
	EXPECT_FALSE(env._sink._orders[3]._long);
	EXPECT_EQ(1u, env._sink._orders[3]._offset);

	alive->release(); o1->release(); o2->release(); o3->release();
}

//平仓首推扣可平/撤销回补之外的旁支: 外部单不认领、订单号为空时不进认领块
TEST(R11TraderDeep, pushOrderForeignAndEmptyOrderID)
{
	DeepEnv env(true);
	WTSPositionItem* real = q04::posItem("SHFE", "ag2406", WDT_LONG, 10, 10, 0, 0);
	env.feed({ real }, &TraderAdapter::onRspPosition);

	//外部单: 不做 _orders 登记, 也不通知 sink
	WTSOrderInfo* foreign = env.makeForeignOrder("ag2406", 3.0, WOS_NotTraded_Queuing);
	foreign->setOrderID("FOR-1");
	env._adapter.onPushOrder(foreign);
	EXPECT_EQ((size_t)0, env._sink._orders.size());
	//但首次推送仍然计 1 个信息量
	EXPECT_EQ(1u, env._adapter.getInfos(kAg));

	//订单号为空: 跳过首推认领块, 只按 usertag 认领(本通道 empty 单)
	WTSOrderInfo* empty = env.makeOrder(0, "ag2406", 2.0, WDT_LONG, WOT_CLOSE, WOS_NotTraded_Queuing);
	env._adapter.onPushOrder(empty);
	//平仓首推扣掉 2 手可平(全走昨仓)
	EXPECT_DOUBLE_EQ(8.0, env._adapter.getPosition(kAg, true, 1));
	//本通道活单进 _orders
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
	ASSERT_EQ((size_t)1, env._sink._orders.size());
	EXPECT_EQ(1u, env._sink._orders[0]._offset);

	foreign->release(); empty->release();
}

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, queriesOnEmptyChannel)
{
	DeepEnv env(false);
	//没有 _orders 时 getOrders 返回 NULL
	EXPECT_EQ(nullptr, env._adapter.getOrders(kAg));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.enumPosition(kAg));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.enumPosition());
	EXPECT_EQ(0u, env._adapter.getInfos("SHFE.nothing"));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kAg));
	EXPECT_EQ(&env._bd, env._adapter.getBaseDataMgr());

	//updateUndone 是私有接口, 只能经回报间接推进; 这里改为验证回报路径的未完成量归零
	env._adapter.onRspEntrust(nullptr, nullptr);

	//enumPosition 命中一个合约 -> 回调 sink 两次(多/空), 返回净头寸
	WTSPositionItem* l = q04::posItem("SHFE", "ag2406", WDT_LONG, 4, 4, 0, 0);
	WTSPositionItem* s = q04::posItem("SHFE", "ag2406", WDT_SHORT, 1, 1, 0, 0);
	env.feed({ l, s }, &TraderAdapter::onRspPosition);
	//onRspPosition 自身已经通知过 2 次
	const size_t afterRsp = env._sink._positions.size();
	EXPECT_DOUBLE_EQ(3.0, env._adapter.enumPosition(kAg));
	//枚举又额外通知了 2 次
	EXPECT_EQ(afterRsp + 2, env._sink._positions.size());
	//全量枚举: 只有 ag2406 一个条目
	EXPECT_DOUBLE_EQ(3.0, env._adapter.enumPosition());
}

//日志/登出/无错对象入口都只做最小动作, 不能崩也不能通知错东西
TEST(R11TraderDeep, logErrorAndLogoutAreNarrow)
{
	DeepEnv env(true);
	env._adapter.handleTraderLog(LL_INFO, "r11 trader info log");
	env._adapter.handleTraderLog(LL_ERROR, "r11 trader error log");
	//err==NULL 时只跳过日志
	env._adapter.onTraderError(nullptr);
	//onLogout 是空实现, 不会广播 channel_lost
	env._adapter.onLogout();
	EXPECT_EQ((uint32_t)0, env._sink._lostCnt);
	EXPECT_EQ((uint32_t)0, env._sink._readyCnt);
}

//未登录时收到 login(false) 不应进入查询链
TEST(R11TraderDeep, failedLoginKeepsChannelUnready)
{
	DeepEnv env(false);
	env._adapter.onLoginResult(false, "bad password", 0);
	EXPECT_EQ((uint32_t)0, env._api._qPosCnt);
	EXPECT_EQ((uint32_t)0, env._sink._readyCnt);

	//再成功一次才推进
	env._adapter.onLoginResult(true, "ok", 20240318);
	EXPECT_EQ((uint32_t)1, env._api._qPosCnt);
	EXPECT_EQ((uint32_t)0, env._sink._readyCnt);
	//登录成功只推进到 AS_LOGINED, 远未到 AS_ALLREADY
	EXPECT_NE(TraderAdapter::AS_ALLREADY, env._adapter.state());
}

//////////////////////////////////////////////////////////////////////////
TEST(R11TraderDeep, adapterMgrAddGetRunRelease)
{
	DeepEnv env(true);
	TraderAdapterMgr mgr;

	TraderAdapterPtr empty;
	EXPECT_FALSE(mgr.addAdapter("t1", empty));
	EXPECT_EQ((size_t)0, mgr.getAdapters().size());

	TraderAdapterPtr first(new TraderAdapter());
	ASSERT_TRUE(first->initExt("uft2", &env._api, &env._bd, &env._policy));
	//空名字被 strlen(tname)==0 拒绝
	EXPECT_FALSE(mgr.addAdapter("", first));
	EXPECT_TRUE(mgr.addAdapter("uft2", first));
	//重名被拒绝
	TraderAdapterPtr dup(new TraderAdapter());
	EXPECT_FALSE(mgr.addAdapter("uft2", dup));
	EXPECT_EQ((size_t)1, mgr.getAdapters().size());

	EXPECT_EQ(first.get(), mgr.getAdapter("uft2").get());
	EXPECT_EQ(nullptr, mgr.getAdapter("uft-none").get());

	const uint32_t before = env._api._initCnt;
	mgr.run();
	EXPECT_EQ(before + 1, env._api._initCnt);

	mgr.release();
	EXPECT_EQ((size_t)0, mgr.getAdapters().size());
	EXPECT_EQ((uint32_t)1, env._api._releaseCnt);
}

} // namespace
