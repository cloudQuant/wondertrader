/*!
 * \file q04_trader_entrust_paths_test.cpp
 * \brief TraderAdapter 委托组装/代码改写/usertag/撤单路径(迭代01 第五波 q04)
 *
 * 覆盖: openLong/openShort/closeLong/closeShort、doEntrust(代码改写 + usertag 组装 +
 * 委托失败返回 UINT_MAX)、updateUndone/getUndoneQty、doCancel/cancel/cancelAll、
 * onPushOrder 的本通道认领与撤销通知。
 * 全部通过替身 ITraderApi 抓到的委托快照回读, 不接触柜台。
 */
#include "q04_trader_fixture.h"

#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* kStdAg = "SHFE.ag2406";
const char* kStdCu = "SHFE.cu2406";

struct OrdEnv
{
	q04::TempRoot			_tmp{ "entrust" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;

	OrdEnv(const char* id = "uft1")
	{
		_bd.add("SHFE", "ag", "ag2406");
		_bd.add("SHFE", "cu", "cu2406");
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		EXPECT_TRUE(_adapter.initExt(id, &_api, &_bd, &_policy));
		_adapter.addSink(&_sink);
		EXPECT_TRUE(_adapter.run());
		_adapter.onLoginResult(true, "ok", 20240315);
	}

	//把某笔委托按柜台回报登记进 _orders
	void registerOrder(const q04::FakeTraderApi::EntrustSnap& snap, const char* orderid,
		WTSOrderState st = WOS_NotTraded_Queuing, WTSContractInfo* cInfo = nullptr)
	{
		WTSOrderInfo* ord = q04::orderFromSnap(snap, orderid);
		ord->setOrderState(st);
		if (cInfo != nullptr)
			ord->setContractInfo(cInfo);
		WTSArray* ay = WTSArray::create();
		ay->append(ord);
		_adapter.onRspOrders(ay);
		ay->release();
		ord->release();
	}
};

}	// namespace

TEST(q04_trader_entrust, four_direction_actions_are_assembled_correctly)
{
	OrdEnv env;

	uint32_t idOl = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	uint32_t idOs = env._adapter.openShort(kStdAg, 5010.0, 3.0, 0);
	uint32_t idCl = env._adapter.closeLong(kStdAg, 5020.0, 1.0, false, 0);
	uint32_t idClT = env._adapter.closeLong(kStdAg, 5030.0, 1.0, true, 0);
	uint32_t idCs = env._adapter.closeShort(kStdAg, 5040.0, 4.0, false, 0);
	uint32_t idCsT = env._adapter.closeShort(kStdAg, 5050.0, 4.0, true, 0);

	for (uint32_t id : { idOl, idOs, idCl, idClT, idCs, idCsT })
		EXPECT_NE(UINT_MAX, id);

	ASSERT_EQ(6u, env._api._entrusts.size());
	//代码改写: 标准代码 "SHFE.ag2406" 必须拆成 裸代码 + 市场
	for (auto& snap : env._api._entrusts)
	{
		EXPECT_EQ("ag2406", snap._code);
		EXPECT_EQ("SHFE", snap._exchg);
	}
	EXPECT_EQ(6u, env._api._mkEidCnt);

	const int dirLong = (int)WDT_LONG;
	const int dirShort = (int)WDT_SHORT;
	const int offOpen = (int)WOT_OPEN;
	const int offClose = (int)WOT_CLOSE;
	const int offCloseT = (int)WOT_CLOSETODAY;

	EXPECT_EQ(dirLong, env._api._entrusts[0]._dir);
	EXPECT_EQ(offOpen, env._api._entrusts[0]._offset);
	EXPECT_EQ(dirShort, env._api._entrusts[1]._dir);
	EXPECT_EQ(offOpen, env._api._entrusts[1]._offset);
	EXPECT_EQ(dirLong, env._api._entrusts[2]._dir);
	EXPECT_EQ(offClose, env._api._entrusts[2]._offset);
	EXPECT_EQ(dirLong, env._api._entrusts[3]._dir);
	EXPECT_EQ(offCloseT, env._api._entrusts[3]._offset);
	EXPECT_EQ(dirShort, env._api._entrusts[4]._dir);
	EXPECT_EQ(offClose, env._api._entrusts[4]._offset);
	EXPECT_EQ(dirShort, env._api._entrusts[5]._dir);
	EXPECT_EQ(offCloseT, env._api._entrusts[5]._offset);

	//数量/价格原样透传
	EXPECT_DOUBLE_EQ(2.0, env._api._entrusts[0]._vol);
	EXPECT_DOUBLE_EQ(5050.0, env._api._entrusts[5]._price);

	//usertag 组装: wtp.<id>.<localid>
	EXPECT_EQ(std::string("wtp.uft1.") + std::to_string(idOl), env._api._entrusts[0]._usertag);
	EXPECT_EQ(std::string("wtp.uft1.") + std::to_string(idCsT), env._api._entrusts[5]._usertag);
	//委托号由 API 侧 makeEntrustID 生成, 适配器只负责回填
	EXPECT_EQ("EU000001", env._api._entrusts[0]._entrustid);
	EXPECT_EQ("EU000006", env._api._entrusts[5]._entrustid);

	//未完成量按合约累加: 2+3+1+1+4+4
	EXPECT_DOUBLE_EQ(15.0, env._adapter.getUndoneQty(kStdAg));
	//不同合约各自独立
	env._adapter.openLong(kStdCu, 70000.0, 1.0, 0);
	EXPECT_DOUBLE_EQ(1.0, env._adapter.getUndoneQty(kStdCu));
	EXPECT_DOUBLE_EQ(15.0, env._adapter.getUndoneQty(kStdAg));
	//未知合约也能正常下单(只影响代码改写, 不查基础数据)
	env._adapter.openLong("DCE.m2509", 3000.0, 2.0, 0);
	EXPECT_DOUBLE_EQ(2.0, env._adapter.getUndoneQty("DCE.m2509"));
	ASSERT_EQ(8u, env._api._entrusts.size());
	EXPECT_EQ("m2509", env._api._entrusts[7]._code);
	EXPECT_EQ("DCE", env._api._entrusts[7]._exchg);
}

TEST(q04_trader_entrust, price_type_and_order_flag_mapping)
{
	OrdEnv env;

	env._adapter.openLong(kStdAg, 0.0, 1.0, 0);
	env._adapter.openLong(kStdAg, 100.0, 1.0, 1);
	env._adapter.openLong(kStdAg, 100.0, 1.0, 2);
	env._adapter.openShort(kStdAg, 0.0, 1.0, 1);
	env._adapter.closeLong(kStdAg, 0.0, 1.0, true, 2);

	ASSERT_EQ(5u, env._api._entrusts.size());
	//price==0 走市价, 否则限价
	EXPECT_EQ((int)WPT_ANYPRICE, env._api._entrusts[0]._ptype);
	EXPECT_EQ((int)WPT_LIMITPRICE, env._api._entrusts[1]._ptype);
	//flag 直接叠加在 WOF_NOR('0') 上
	EXPECT_EQ((int)WOF_NOR, env._api._entrusts[0]._oflag);
	EXPECT_EQ((int)WOF_NOR + 1, env._api._entrusts[1]._oflag);
	EXPECT_EQ((int)WOF_FAK, env._api._entrusts[1]._oflag);
	EXPECT_EQ((int)WOF_FOK, env._api._entrusts[2]._oflag);
	EXPECT_EQ((int)WPT_ANYPRICE, env._api._entrusts[3]._ptype);
	EXPECT_EQ((int)WOF_FAK, env._api._entrusts[3]._oflag);
	EXPECT_EQ((int)WPT_ANYPRICE, env._api._entrusts[4]._ptype);
	EXPECT_EQ((int)WOT_CLOSETODAY, env._api._entrusts[4]._offset);
}

TEST(q04_trader_entrust, insert_failure_returns_uint_max_but_keeps_undone)
{
	OrdEnv env;

	env._api._insertRet = -1;
	uint32_t bad = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	EXPECT_EQ(UINT_MAX, bad);
	//委托失败时 updateUndone 已经加过, doEntrust 不回滚
	//(实现如此, 是否合理见 q04_uftcore3.md BUG-Q04-002, 状态: 待裁定)
	EXPECT_DOUBLE_EQ(2.0, env._adapter.getUndoneQty(kStdAg));
	//失败单不会被登记进 _orders
	EXPECT_EQ(nullptr, env._adapter.getOrders(""));

	//恢复成功后, 未完成量继续累加
	env._api._insertRet = 0;
	uint32_t good = env._adapter.openShort(kStdAg, 5000.0, 3.0, 0);
	EXPECT_NE(UINT_MAX, good);
	EXPECT_NE(bad, good);
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kStdAg));
}

TEST(q04_trader_entrust, usertag_pattern_is_scoped_per_channel)
{
	q04::TempRoot tmp("scope");
	q04::FakeTraderApi apiA;
	q04::FakeTraderApi apiB;
	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	const std::string path = tmp.file("policy.json");
	tmp.write(path, q04::policyJsonOpenCloseLimited());
	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init(path.c_str()));

	q04::FakeSink sinkA;
	q04::FakeSink sinkB;
	TraderAdapter adapterA;
	TraderAdapter adapterB;
	ASSERT_TRUE(adapterA.initExt("uftA", &apiA, &bd, &policy));
	ASSERT_TRUE(adapterB.initExt("uftB", &apiB, &bd, &policy));
	adapterA.addSink(&sinkA);
	adapterB.addSink(&sinkB);
	ASSERT_TRUE(adapterA.run());
	ASSERT_TRUE(adapterB.run());
	adapterA.onLoginResult(true, "ok", 20240315);
	adapterB.onLoginResult(true, "ok", 20240315);

	uint32_t idA = adapterA.openLong(kStdAg, 5000.0, 1.0, 0);
	uint32_t idB = adapterB.openLong(kStdAg, 5000.0, 1.0, 0);
	EXPECT_EQ(std::string("wtp.uftA.") + std::to_string(idA), apiA._entrusts[0]._usertag);
	EXPECT_EQ(std::string("wtp.uftB.") + std::to_string(idB), apiB._entrusts[0]._usertag);

	//A 的回报里带 B 前缀的 usertag 不应被 A 认领
	WTSOrderInfo* foreign = WTSOrderInfo::create();
	foreign->setCode("ag2406");
	foreign->setExchange("SHFE");
	foreign->setDirection(WDT_LONG);
	foreign->setOffsetType(WOT_OPEN);
	foreign->setVolume(1);
	foreign->setVolLeft(1);
	foreign->setOrderState(WOS_NotTraded_Queuing);
	foreign->setOrderID("F-1");
	foreign->setUserTag(apiB._entrusts[0]._usertag.c_str());
	foreign->setContractInfo(bd.getContract("ag2406", "SHFE"));

	WTSArray* ay = WTSArray::create();
	ay->append(foreign);
	adapterA.onRspOrders(ay);
	ay->release();

	OrderMap* orders = adapterA.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size()) << "非本通道 usertag 的委托不得登记";
	orders->release();
	//onRspOrders 开头会清空未完成量
	EXPECT_DOUBLE_EQ(0.0, adapterA.getUndoneQty(kStdAg));
	//A 的 _orders 是空表, 撤自己的单也只能返回 false
	EXPECT_FALSE(adapterA.cancel(idA));

	//B 收到同一张单子后才会登记进自己的 _orders
	WTSArray* ay2 = WTSArray::create();
	ay2->append(foreign);
	adapterB.onRspOrders(ay2);
	ay2->release();
	OrderMap* orders2 = adapterB.getOrders("");
	ASSERT_NE(nullptr, orders2);
	EXPECT_EQ(1u, orders2->size());
	orders2->release();
	EXPECT_DOUBLE_EQ(1.0, adapterB.getUndoneQty(kStdAg));

	//撤销推送: 本通道 usertag 要退回未完成量并通知 sink
	WTSOrderInfo* pushed = WTSOrderInfo::create();
	pushed->setCode("ag2406");
	pushed->setExchange("SHFE");
	pushed->setDirection(WDT_LONG);
	pushed->setOffsetType(WOT_OPEN);
	pushed->setVolume(1);
	pushed->setVolLeft(1);
	pushed->setOrderState(WOS_Canceled);
	pushed->setOrderID("P-1");
	pushed->setUserTag(apiB._entrusts[0]._usertag.c_str());
	pushed->setContractInfo(bd.getContract("ag2406", "SHFE"));
	adapterB.onPushOrder(pushed);
	ASSERT_EQ(1u, sinkB._orders.size());
	EXPECT_EQ(idB, sinkB._orders[0]._localid);
	EXPECT_TRUE(sinkB._orders[0]._canceled);
	EXPECT_EQ(0u, sinkA._orders.size());
	//死单要从 _orders 里摘掉
	OrderMap* orders3 = adapterB.getOrders("");
	ASSERT_NE(nullptr, orders3);
	EXPECT_EQ(0u, orders3->size());
	orders3->release();
	pushed->release();
	foreign->release();
}

TEST(q04_trader_entrust, cancel_single_order_goes_through_doCancel)
{
	OrdEnv env;

	uint32_t localid = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, localid);

	//_orders 尚未建立: 直接 false, 不发撤销指令
	EXPECT_FALSE(env._adapter.cancel(localid));
	EXPECT_EQ(0u, env._api._actions.size());

	//按回报登记活单后, 撤单要带上柜台订单号与委托号
	env.registerOrder(env._api._entrusts.back(), "ORD-A");
	EXPECT_TRUE(env._adapter.cancel(localid));
	ASSERT_EQ(1u, env._api._actions.size());
	EXPECT_EQ("ORD-A", env._api._actions[0]._code);
	EXPECT_EQ("EU000001", env._api._actions[0]._exchg);

	//未知 localid: 表非空但 grab 不到
	EXPECT_FALSE(env._adapter.cancel(localid + 9999));
	EXPECT_EQ(1u, env._api._actions.size());

	//柜台撤销失败(ret<0)时 doCancel 返回 false
	env._api._actionRet = -1;
	env.registerOrder(env._api._entrusts.back(), "ORD-B");
	EXPECT_FALSE(env._adapter.cancel(localid));
	EXPECT_EQ(2u, env._api._actions.size());
	//同一 localid 的后续回报会覆盖 _orders 里的旧对象
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(1u, orders->size());
	orders->release();
}

TEST(q04_trader_entrust, cancel_all_filters_alive_orders_by_full_code)
{
	OrdEnv env;

	uint32_t id1 = env._adapter.openLong(kStdAg, 5000.0, 1.0, 0);
	uint32_t id2 = env._adapter.openShort(kStdCu, 70000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, id1);
	ASSERT_NE(UINT_MAX, id2);

	//cancelAll 走 cInfo->getFullCode(), 所以必须带 ContractInfo
	WTSOrderInfo* ag = q04::orderFromSnap(env._api._entrusts[0], "ORD-AG");
	ag->setContractInfo(env._bd.getContract("ag2406", "SHFE"));
	WTSOrderInfo* cu = q04::orderFromSnap(env._api._entrusts[1], "ORD-CU");
	cu->setContractInfo(env._bd.getContract("cu2406", "SHFE"));
	WTSOrderInfo* deadAg = q04::orderFromSnap(env._api._entrusts[0], "ORD-DEAD");
	deadAg->setOrderState(WOS_Canceled);
	deadAg->setContractInfo(env._bd.getContract("ag2406", "SHFE"));

	WTSArray* ay = WTSArray::create();
	ay->append(ag);
	ay->append(cu);
	ay->append(deadAg);
	env._adapter.onRspOrders(ay);
	ay->release();

	//_orders 里只有两张活单(死单被 continue)
	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(2u, orders->size());
	orders->release();

	//只撤 ag2406
	OrderIDs ids = env._adapter.cancelAll(kStdAg);
	ASSERT_EQ(1u, ids.size());
	EXPECT_EQ(id1, ids[0]);

	//空代码撤全部(_orders 不会因为撤单而清理)
	OrderIDs all = env._adapter.cancelAll("");
	EXPECT_EQ(2u, all.size());

	//不存在的合约一张都撤不到
	OrderIDs none = env._adapter.cancelAll("DCE.m2509");
	EXPECT_EQ(0u, none.size());

	//柜台拒绝撤销时返回空列表
	env._api._actionRet = -1;
	OrderIDs failed = env._adapter.cancelAll("");
	EXPECT_EQ(0u, failed.size());
	env._api._actionRet = 0;

	ag->release();
	cu->release();
	deadAg->release();
}

//崩溃/未初始化类输入整类收在这里(与 BUG-N03-005 / BUG-P06-004 同源)
TEST(DISABLED_q04_trader_entrust, null_contract_and_bare_code_class)
{
	q04::TempRoot tmp("cls2");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	TraderAdapter adapter;
	adapter.initExt("uft1", &api, &bd, &policy);
	adapter.run();

	//1) 裸代码: doEntrust 里 findFirst 返回 npos, setExchange(…, npos) 越界写
	adapter.openLong("ag2406", 5000.0, 1.0, 0);

	//2) cancelAll: orderInfo->getContractInfo() 为 NULL 时直接 cInfo->getFullCode()
	WTSOrderInfo* noComm = WTSOrderInfo::create();
	noComm->setCode("ag2406");
	noComm->setExchange("SHFE");
	noComm->setOrderState(WOS_NotTraded_Queuing);
	noComm->setUserTag("wtp.uft1.123");
	WTSArray* ay = WTSArray::create();
	ay->append(noComm);
	adapter.onRspOrders(ay);
	ay->release();
	adapter.cancelAll("");
	noComm->release();

	//3) initExt 传 NULL api: api->init(NULL) 空指针调用
	TraderAdapter adapter2;
	adapter2.initExt("x", nullptr, &bd, &policy);
}
