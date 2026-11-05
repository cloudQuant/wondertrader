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

struct DetachedOrderCancelEnv
{
	q04::TempRoot			_tmp{ "cancel_missing_contract" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr		_policy;
	q04::FakeSink			_sink;
	TraderAdapter			_adapter;
	std::string			_id;

	bool init(const char* id, IBaseDataMgr* bdMgr)
	{
		_id = id;
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		if (!_policy.init(path.c_str()) || !_adapter.initExt(_id.c_str(), &_api, bdMgr, &_policy))
			return false;
		_adapter.addSink(&_sink);
		if (!_adapter.run())
			return false;
		_adapter.onLoginResult(true, "ok", 20240315);
		return true;
	}

	void registerOrderWithoutRetainingContract(uint32_t localid, WTSContractInfo* cInfo)
	{
		WTSOrderInfo* order = WTSOrderInfo::create();
		order->setCode(cInfo->getCode());
		order->setExchange(cInfo->getExchg());
		order->setDirection(WDT_LONG);
		order->setOffsetType(WOT_OPEN);
		order->setOrderFlag(WOF_NOR);
		order->setVolume(1);
		order->setVolLeft(1);
		order->setOrderState(WOS_NotTraded_Queuing);
		order->setOrderID("ORD-MISSING-CONTRACT");
		const std::string userTag = std::string("wtp.") + _id + "." + std::to_string(localid);
		order->setUserTag(userTag.c_str());
		order->setContractInfo(cInfo);
		WTSArray* orders = WTSArray::create();
		orders->append(order);
		_adapter.onRspOrders(orders);
		orders->release();
		// onRspOrders uses this metadata to register the live order; clear it so
		// cancel() must resolve the contract through the configured data manager.
		order->setContractInfo(nullptr);
		order->release();
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

TEST(q04_trader_entrust, malformed_standard_codes_fail_before_vendor_calls)
{
	OrdEnv env;
	const std::string tooLongExchange = std::string(16, 'X') + ".ag2406";
	const std::string tooLongFullCode = std::string("SHFE.") + std::string(MAX_INSTRUMENT_LENGTH - 5, 'x');
	const char* invalidCodes[] = {
		"ag2406", ".ag2406", "SHFE.", "SHFE..ag2406", "SHFE.ag.2406.10.C",
		tooLongExchange.c_str(), tooLongFullCode.c_str()
	};

	for (const char* code : invalidCodes)
	{
		const uint32_t makeEntrustIdCalls = env._api._mkEidCnt;
		const std::size_t orderInsertCalls = env._api._entrusts.size();
		EXPECT_EQ(UINT_MAX, env._adapter.openLong(code, 5000.0, 1.0, 0)) << code;
		EXPECT_EQ(UINT_MAX, env._adapter.openShort(code, 5000.0, 1.0, 0)) << code;
		EXPECT_EQ(UINT_MAX, env._adapter.closeLong(code, 5000.0, 1.0, false, 0)) << code;
		EXPECT_EQ(UINT_MAX, env._adapter.closeShort(code, 5000.0, 1.0, false, 0)) << code;
		EXPECT_EQ(makeEntrustIdCalls, env._api._mkEidCnt) << code;
		EXPECT_EQ(orderInsertCalls, env._api._entrusts.size()) << code;
		EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(code)) << code;
	}

	const uint32_t normalId = env._adapter.openLong(kStdAg, 5000.0, 1.0, 0);
	const uint32_t dottedInstrumentId = env._adapter.openLong("SHFE.rb.2406", 5000.0, 1.0, 0);
	const uint32_t optionId = env._adapter.openLong("CFFEX.IO2007.C.4000", 5000.0, 1.0, 0);
	ASSERT_NE(UINT_MAX, normalId);
	ASSERT_NE(UINT_MAX, dottedInstrumentId);
	ASSERT_NE(UINT_MAX, optionId);
	ASSERT_EQ(3u, env._api._mkEidCnt);
	ASSERT_EQ(3u, env._api._entrusts.size());
	EXPECT_EQ("SHFE", env._api._entrusts[0]._exchg);
	EXPECT_EQ("ag2406", env._api._entrusts[0]._code);
	EXPECT_EQ("SHFE", env._api._entrusts[1]._exchg);
	EXPECT_EQ("rb.2406", env._api._entrusts[1]._code);
	EXPECT_EQ("CFFEX", env._api._entrusts[2]._exchg);
	EXPECT_EQ("IO2007.C.4000", env._api._entrusts[2]._code);
}

TEST(q04_trader_entrust, insert_failure_returns_uint_max_without_counting_undone)
{
	OrdEnv env;

	env._api._insertRet = -1;
	uint32_t bad = env._adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	EXPECT_EQ(UINT_MAX, bad);
	//API 同步拒绝本笔委托, 未完成量不应计入未被接受的数量
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	EXPECT_EQ(UINT_MAX, env._adapter.openShort(kStdAg, 5000.0, 1.0, 0));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	EXPECT_EQ(UINT_MAX, env._adapter.closeLong(kStdAg, 5000.0, 0.5, false, 0));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	EXPECT_EQ(UINT_MAX, env._adapter.closeShort(kStdAg, 5000.0, 0.75, true, 0));
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	//失败单不会被登记进 _orders
	EXPECT_EQ(nullptr, env._adapter.getOrders(""));

	//后续成功单只贡献自己的未完成量
	env._api._insertRet = 0;
	uint32_t good = env._adapter.openLong(kStdAg, 5000.0, 3.0, 0);
	EXPECT_NE(UINT_MAX, good);
	EXPECT_NE(bad, good);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
}

TEST(q04_trader_entrust, rejection_without_this_adapters_usertag_does_not_change_undone)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	ASSERT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	const q04::FakeTraderApi::EntrustSnap snap = env._api._entrusts.back();
	WTSError* err = WTSError::create(WEC_ORDERINSERT, "unmatched rejection");

	for (const char* userTag : { "", "wtp.other.123" })
	{
		WTSEntrust* entrust = WTSEntrust::create(snap._code.c_str(), snap._vol, snap._price, snap._exchg.c_str());
		entrust->setUserTag(userTag);
		entrust->setDirection((WTSDirectionType)snap._dir);
		entrust->setOffsetType((WTSOffsetType)snap._offset);
		env._adapter.onRspEntrust(entrust, err);
		EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
		EXPECT_TRUE(env._sink._entrusts.empty());
		entrust->release();
	}

	err->release();
}

// Regression for Q04-002: the aggregate for a contract cannot be used to
// recognize a duplicate response while another order on that contract is live.
TEST(q04_trader_entrust, duplicate_rejection_does_not_consume_another_live_order)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 2.0, 0));
	const auto first = env._api._entrusts.back();
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	ASSERT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kStdAg));

	WTSEntrust* rejected = WTSEntrust::create(first._code.c_str(), first._vol,
		first._price, first._exchg.c_str());
	rejected->setUserTag(first._usertag.c_str());
	rejected->setDirection((WTSDirectionType)first._dir);
	rejected->setOffsetType((WTSOffsetType)first._offset);
	WTSError* err = WTSError::create(WEC_ORDERINSERT, "duplicate rejection");
	env._adapter.onRspEntrust(rejected, err);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	env._adapter.onRspEntrust(rejected, err);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	err->release();
	rejected->release();
}

namespace {
class ReentrantRejectApi : public q04::FakeTraderApi
{
public:
	bool rejectDuringInsert = false;
	int orderInsert(WTSEntrust* entrust) override
	{
		const int ret = q04::FakeTraderApi::orderInsert(entrust);
		if (rejectDuringInsert)
		{
			WTSError* err = WTSError::create(WEC_ORDERINSERT, "synchronous rejection callback");
			_spi->onRspEntrust(entrust, err);
			err->release();
			return -1;
		}
		return ret;
	}
};
}

// Control for a tempting but unsafe fix: a provider may call the SPI before
// returning a negative value, so blindly subtracting at both sites is wrong.
TEST(q04_trader_entrust, reentrant_rejection_and_negative_return_leave_other_order_unchanged)
{
	q04::TempRoot tmp("reentrant_rejection");
	ReentrantRejectApi api;
	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	const std::string path = tmp.file("policy.json");
	tmp.write(path, q04::policyJsonOpenCloseLimited());
	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init(path.c_str()));
	q04::FakeSink sink;
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("uft_reentrant", &api, &bd, &policy));
	adapter.addSink(&sink);
	ASSERT_TRUE(adapter.run());
	adapter.onLoginResult(true, "ok", 20240315);
	ASSERT_NE(UINT_MAX, adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	ASSERT_DOUBLE_EQ(3.0, adapter.getUndoneQty(kStdAg));
	api.rejectDuringInsert = true;
	EXPECT_EQ(UINT_MAX, adapter.openLong(kStdAg, 5000.0, 2.0, 0));
	EXPECT_DOUBLE_EQ(3.0, adapter.getUndoneQty(kStdAg));
}

TEST(q04_trader_entrust, duplicate_trade_does_not_reduce_undone_twice)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto snap = env._api._entrusts.back();
	WTSTradeInfo* trade = WTSTradeInfo::create(snap._code.c_str(), snap._exchg.c_str());
	trade->setTradeID("duplicate-trade-1");
	trade->setRefOrder("order-1");
	trade->setTradeDate(20240315);
	trade->setUserTag(snap._usertag.c_str());
	trade->setDirection(WDT_LONG);
	trade->setOffsetType(WOT_OPEN);
	trade->setVolume(2.0);
	trade->setPrice(5000.0);
	env._adapter.onPushTrade(trade);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	env._adapter.onPushTrade(trade);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	trade->release();
}

TEST(q04_trader_entrust, duplicate_cancellation_does_not_reduce_undone_twice)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto first = env._api._entrusts.back();
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	WTSOrderInfo* order = q04::orderFromSnap(first, "cancel-order-1");
	order->setOrderState(WOS_Canceled);
	env._adapter.onPushOrder(order);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	env._adapter.onPushOrder(order);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	order->release();
}

// A query result captured before a live trade must not restore its already
// traded quantity when the query response arrives later.
TEST(q04_trader_entrust, older_order_snapshot_does_not_undo_live_trade)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto snap = env._api._entrusts.back();
	WTSOrderInfo* olderOrder = q04::orderFromSnap(snap, "snapshot-order-1");
	WTSArray* olderSnapshot = WTSArray::create();
	olderSnapshot->append(olderOrder);
	WTSTradeInfo* trade = WTSTradeInfo::create(snap._code.c_str(), snap._exchg.c_str());
	trade->setTradeID("snapshot-trade-1");
	trade->setRefOrder("snapshot-order-1");
	trade->setUserTag(snap._usertag.c_str());
	trade->setDirection(WDT_LONG);
	trade->setOffsetType(WOT_OPEN);
	trade->setVolume(2.0);
	trade->setPrice(5000.0);
	env._adapter.onPushTrade(trade);
	ASSERT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	env._adapter.onRspOrders(olderSnapshot);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	trade->release();
	olderSnapshot->release();
	olderOrder->release();
}

//////////////////////////////////////////////////////////////////////////
// BUG-Q04-002 (card 5): per-order ledger regressions added 2026-11-04.
namespace {
WTSTradeInfo* q04Trade(const q04::FakeTraderApi::EntrustSnap& snap, const char* tradeId, double vol,
	const char* tag = nullptr)
{
	WTSTradeInfo* trade = WTSTradeInfo::create(snap._code.c_str(), snap._exchg.c_str());
	trade->setTradeID(tradeId);
	trade->setUserTag(tag == nullptr ? snap._usertag.c_str() : tag);
	trade->setDirection((WTSDirectionType)snap._dir);
	trade->setOffsetType((WTSOffsetType)snap._offset);
	trade->setVolume(vol);
	trade->setPrice(5000.0);
	return trade;
}

uint32_t ownLocalId(const q04::FakeTraderApi::EntrustSnap& snap)
{
	const std::size_t dot = snap._usertag.rfind('.');
	return (uint32_t)std::stoul(snap._usertag.substr(dot + 1));
}
}

// One fill is reported by both the order channel (volLeft) and the trade
// channel; whichever arrives first, it is subtracted exactly once.
TEST(q04_trader_entrust, fill_reported_by_order_and_trade_is_counted_once)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto first = env._api._entrusts.back();
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto second = env._api._entrusts.back();
	ASSERT_DOUBLE_EQ(10.0, env._adapter.getUndoneQty(kStdAg));

	// order report first, then the trade of the same 2 lots
	WTSOrderInfo* partial = q04::orderFromSnap(first, "fill-order-1");
	partial->setOrderState(WOS_PartTraded_Queuing);
	partial->setVolLeft(3.0);
	partial->setVolTraded(2.0);
	env._adapter.onPushOrder(partial);
	EXPECT_DOUBLE_EQ(8.0, env._adapter.getUndoneQty(kStdAg));
	WTSTradeInfo* trade1 = q04Trade(first, "fill-trade-1", 2.0);
	env._adapter.onPushTrade(trade1);
	EXPECT_DOUBLE_EQ(8.0, env._adapter.getUndoneQty(kStdAg));

	// trade first, then the order report of the same 2 lots
	WTSTradeInfo* trade2 = q04Trade(second, "fill-trade-2", 2.0);
	env._adapter.onPushTrade(trade2);
	EXPECT_DOUBLE_EQ(6.0, env._adapter.getUndoneQty(kStdAg));
	WTSOrderInfo* partial2 = q04::orderFromSnap(second, "fill-order-2");
	partial2->setOrderState(WOS_PartTraded_Queuing);
	partial2->setVolLeft(3.0);
	partial2->setVolTraded(2.0);
	env._adapter.onPushOrder(partial2);
	EXPECT_DOUBLE_EQ(6.0, env._adapter.getUndoneQty(kStdAg));

#ifdef WT_UFT_UNDONE_LEDGER	// red-control builds against the pre-change header omit ledger-view checks
	const TraderAdapter::UndoneLedgerView view = env._adapter.undoneLedger(ownLocalId(second));
	ASSERT_TRUE(view.found);
	EXPECT_DOUBLE_EQ(3.0, view.accounted);
	EXPECT_DOUBLE_EQ(2.0, view.trade_sum);
	EXPECT_DOUBLE_EQ(2.0, view.order_traded);
#endif

	trade1->release();
	trade2->release();
	partial->release();
	partial2->release();
}

// A terminal order is a tombstone: a late live report neither reopens its
// quantity nor re-registers it as a cancellable order.
TEST(q04_trader_entrust, late_live_report_after_cancel_does_not_reopen_order)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto snap = env._api._entrusts.back();
	WTSOrderInfo* canceled = q04::orderFromSnap(snap, "late-order-1");
	canceled->setOrderState(WOS_Canceled);
	env._adapter.onPushOrder(canceled);
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));

	WTSOrderInfo* stale = q04::orderFromSnap(snap, "late-order-1");
	env._adapter.onPushOrder(stale);
	EXPECT_DOUBLE_EQ(0.0, env._adapter.getUndoneQty(kStdAg));
	OrderMap* orders = env._adapter.getOrders("");
	if (orders != nullptr)
	{
		EXPECT_EQ(0u, orders->size());
		orders->release();
	}
	EXPECT_FALSE(env._adapter.cancel(ownLocalId(snap)));
#ifdef WT_UFT_UNDONE_LEDGER	// red-control builds against the pre-change header omit ledger-view checks
	const TraderAdapter::UndoneLedgerView view = env._adapter.undoneLedger(ownLocalId(snap));
	ASSERT_TRUE(view.found);
	EXPECT_EQ(TraderAdapter::UndoneLedgerState::Terminal, view.state);
#endif

	canceled->release();
	stale->release();
}

// Reports that cannot be attributed to a live order of this adapter (unknown
// local id, another adapter whose id has this adapter's id as prefix) never
// change undone quantities.
TEST(q04_trader_entrust, unattributable_trades_do_not_change_undone)
{
	OrdEnv env("uft1");
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	const auto snap = env._api._entrusts.back();
	ASSERT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));

	const std::string unknownTag = "wtp.uft1." + std::to_string(ownLocalId(snap) + 1000);
	WTSTradeInfo* unknown = q04Trade(snap, "unattributed-1", 2.0, unknownTag.c_str());
	env._adapter.onPushTrade(unknown);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));

	const std::string otherAdapterTag = "wtp.uft10." + std::to_string(ownLocalId(snap));
	WTSTradeInfo* foreign = q04Trade(snap, "unattributed-2", 1.0, otherAdapterTag.c_str());
	env._adapter.onPushTrade(foreign);
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	ASSERT_EQ(2u, env._sink._trades.size());
	EXPECT_EQ(0u, env._sink._trades[1]._localid) << "a uft10 tag must not be claimed by uft1";

	unknown->release();
	foreign->release();
}

// A duplicate trade report (same trade id of the same order) is applied once
// to positions and strategy notifications as well, not only to undone.
TEST(q04_trader_entrust, duplicate_trade_is_notified_and_positioned_once)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 5.0, 0));
	const auto snap = env._api._entrusts.back();
	WTSTradeInfo* trade = q04Trade(snap, "dup-pos-1", 2.0);
	env._adapter.onPushTrade(trade);
	env._adapter.onPushTrade(trade);
	EXPECT_EQ(1u, env._sink._trades.size());
	EXPECT_DOUBLE_EQ(2.0, env._adapter.getPosition(kStdAg, false, 1));
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
	trade->release();
}

namespace {
class AcceptThenFailApi : public q04::FakeTraderApi
{
public:
	int orderInsert(WTSEntrust* entrust) override
	{
		q04::FakeTraderApi::orderInsert(entrust);
		WTSOrderInfo* live = WTSOrderInfo::create(entrust);
		live->setOrderID("accepted-in-insert");
		live->setVolLeft(entrust->getVolume());
		live->setOrderState(WOS_NotTraded_Queuing);
		_spi->onPushOrder(live);
		live->release();
		return -1;
	}
};
}

// A provider that reports the order live before returning a negative value
// has accepted it: the order and its reservation are kept, exactly once.
TEST(q04_trader_entrust, accepted_report_during_insert_keeps_order_despite_negative_return)
{
	q04::TempRoot tmp("accept_then_fail");
	AcceptThenFailApi api;
	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	const std::string path = tmp.file("policy.json");
	tmp.write(path, q04::policyJsonOpenCloseLimited());
	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init(path.c_str()));
	TraderAdapter adapter;
	ASSERT_TRUE(adapter.initExt("uft_accept", &api, &bd, &policy));
	ASSERT_TRUE(adapter.run());
	adapter.onLoginResult(true, "ok", 20240315);

	const uint32_t localid = adapter.openLong(kStdAg, 5000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, localid);
	EXPECT_DOUBLE_EQ(2.0, adapter.getUndoneQty(kStdAg));
#ifdef WT_UFT_UNDONE_LEDGER	// red-control builds against the pre-change header omit ledger-view checks
	const TraderAdapter::UndoneLedgerView view = adapter.undoneLedger(localid);
	ASSERT_TRUE(view.found);
	EXPECT_EQ(TraderAdapter::UndoneLedgerState::Live, view.state);
#endif
}

// Card 5d: entries older than the order query and missing from its (complete)
// result become Absent tombstones; entries created after the query are newer
// than the snapshot and are kept. A failed query (NULL) changes nothing, and a
// later realtime live report revives an Absent order.
TEST(q04_trader_entrust, snapshot_absent_orders_use_the_query_epoch)
{
	OrdEnv env;
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 2.0, 0));
	const auto before = env._api._entrusts.back();
	WTSArray* noPositions = WTSArray::create();
	env._adapter.onRspPosition(noPositions);	// issues queryOrders: snapshot epoch
	noPositions->release();
	ASSERT_EQ(1u, env._api._qOrdCnt);
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 3.0, 0));
	ASSERT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kStdAg));

	env._adapter.onRspOrders(nullptr);
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kStdAg));

	WTSArray* empty = WTSArray::create();
	env._adapter.onRspOrders(empty);
	empty->release();
	EXPECT_DOUBLE_EQ(3.0, env._adapter.getUndoneQty(kStdAg));
#ifdef WT_UFT_UNDONE_LEDGER	// red-control builds against the pre-change header omit ledger-view checks
	EXPECT_EQ(TraderAdapter::UndoneLedgerState::Absent, env._adapter.undoneLedger(ownLocalId(before)).state);
#endif

	WTSOrderInfo* revived = q04::orderFromSnap(before, "absent-order-1");
	env._adapter.onPushOrder(revived);
	EXPECT_DOUBLE_EQ(5.0, env._adapter.getUndoneQty(kStdAg));
#ifdef WT_UFT_UNDONE_LEDGER	// red-control builds against the pre-change header omit ledger-view checks
	EXPECT_EQ(TraderAdapter::UndoneLedgerState::Live, env._adapter.undoneLedger(ownLocalId(before)).state);
#endif
	revived->release();
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
	//卡5d(Q04-002): 快照不再整体清空未完成量; A 自己的单在查询纪元之后创建,
	//快照不能证明其缺席, 其 1 手预占保持不变(旧断言期望清空为 0)
	EXPECT_DOUBLE_EQ(1.0, adapterA.getUndoneQty(kStdAg));
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

TEST(q04_trader_entrust, cancel_single_order_fails_closed_without_manager_or_contract)
{
	DetachedOrderCancelEnv noManager;
	ASSERT_TRUE(noManager.init("uft_no_bd", nullptr));
	WTSContractInfo* managerOnlyContract = WTSContractInfo::create("ag2406", "ag2406", "SHFE", "ag");
	noManager.registerOrderWithoutRetainingContract(7101, managerOnlyContract);
	managerOnlyContract->release();
	EXPECT_FALSE(noManager._adapter.cancel(7101));
	EXPECT_TRUE(noManager._api._actions.empty());

	DetachedOrderCancelEnv unknownContract;
	ASSERT_TRUE(unknownContract.init("uft_unknown_contract", &unknownContract._bd));
	WTSContractInfo* unknown = WTSContractInfo::create("zz9999", "zz9999", "SHFE", "zz");
	unknownContract.registerOrderWithoutRetainingContract(7102, unknown);
	unknown->release();
	EXPECT_FALSE(unknownContract._adapter.cancel(7102));
	EXPECT_TRUE(unknownContract._api._actions.empty());
}

TEST(q04_trader_entrust, cancel_all_filters_alive_orders_by_full_code)
{
	OrdEnv env;

	uint32_t id1 = env._adapter.openLong(kStdAg, 5000.0, 1.0, 0);
	uint32_t id2 = env._adapter.openShort(kStdCu, 70000.0, 2.0, 0);
	ASSERT_NE(UINT_MAX, id1);
	ASSERT_NE(UINT_MAX, id2);
	//卡5(Q04-002): 死单须是另一笔订单; 旧夹具复用 id1 的 usertag, 同一订单身份
	//先活后死在逐单账本中是终态墓碑, 会把 id1 一并判为已撤
	ASSERT_NE(UINT_MAX, env._adapter.openLong(kStdAg, 5000.0, 1.0, 0));

	//cancelAll 走 cInfo->getFullCode(), 所以必须带 ContractInfo
	WTSOrderInfo* ag = q04::orderFromSnap(env._api._entrusts[0], "ORD-AG");
	ag->setContractInfo(env._bd.getContract("ag2406", "SHFE"));
	WTSOrderInfo* cu = q04::orderFromSnap(env._api._entrusts[1], "ORD-CU");
	cu->setContractInfo(env._bd.getContract("cu2406", "SHFE"));
	WTSOrderInfo* deadAg = q04::orderFromSnap(env._api._entrusts[2], "ORD-DEAD");
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

//onRspOrders 会用基础数据回查合约以登记订单，但不会把结果回填到 OrderInfo。
//cancelAll 必须在非空代码过滤时再次回查该订单的合约。
TEST(q04_trader_entrust, cancel_all_with_lookup_only_contract_info)
{
	OrdEnv env;
	uint32_t localid = env._adapter.openLong(kStdAg, 5000.0, 1.0, 0);
	ASSERT_NE(UINT_MAX, localid);
	ASSERT_EQ(1u, env._api._entrusts.size());

	//Entrust 快照提供由适配器生成的有效 usertag；FakeBaseDataMgr 已登记
	//SHFE.ag2406，因此 onRspOrders 可以成功回查合约。orderFromSnap 不附加
	//ContractInfo，正好隔离“回查结果没有回填订单”这一条路径。
	WTSOrderInfo* noComm = q04::orderFromSnap(env._api._entrusts[0], "ORD-LOOKUP");
	ASSERT_EQ(nullptr, noComm->getContractInfo());
	WTSArray* ay = WTSArray::create();
	ay->append(noComm);
	env._adapter.onRspOrders(ay);
	ay->release();

	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	ASSERT_EQ(1u, orders->size());
	EXPECT_EQ(noComm, orders->get(localid));
	EXPECT_EQ(nullptr, noComm->getContractInfo());
	orders->release();

	//契约要求按 SHFE.ag2406 撤销该活单并返回 localid；不能用空串全撤，
	//因为空串会短路 getFullCode()，掩盖缺失的 ContractInfo。
	OrderIDs canceled = env._adapter.cancelAll(kStdAg);
	ASSERT_EQ(1u, canceled.size());
	EXPECT_EQ(localid, canceled[0]);
	ASSERT_EQ(1u, env._api._actions.size());
	EXPECT_EQ("ORD-LOOKUP", env._api._actions[0]._code);
	noComm->release();
}

TEST(q04_trader_entrust, cancel_all_skips_unknown_contract_and_cancels_known_orders)
{
	OrdEnv env;
	const uint32_t unknownId = env._adapter.openLong(kStdAg, 5000.0, 1.0, 0);
	const uint32_t knownId = env._adapter.openShort(kStdCu, 70000.0, 1.0, 0);
	ASSERT_NE(UINT_MAX, unknownId);
	ASSERT_NE(UINT_MAX, knownId);

	WTSOrderInfo* unknown = q04::orderFromSnap(env._api._entrusts[0], "ORD-UNKNOWN");
	WTSOrderInfo* known = q04::orderFromSnap(env._api._entrusts[1], "ORD-KNOWN");
	ASSERT_EQ(nullptr, unknown->getContractInfo());
	ASSERT_EQ(nullptr, known->getContractInfo());
	WTSArray* ay = WTSArray::create();
	ay->append(unknown);
	ay->append(known);
	env._adapter.onRspOrders(ay);
	ay->release();

	OrderMap* orders = env._adapter.getOrders("");
	ASSERT_NE(nullptr, orders);
	ASSERT_EQ(2u, orders->size());
	orders->release();

	//订单能在基础数据存在时进入 _orders，但后续回查可能失败。空代码的全撤
	//也必须先校验合约，跳过未知单，同时撤销仍可回查的已知单。
	ASSERT_TRUE(env._bd.removeContract("SHFE", "ag2406"));
	OrderIDs ids = env._adapter.cancelAll("");
	ASSERT_EQ(1u, ids.size());
	EXPECT_EQ(knownId, ids[0]);
	ASSERT_EQ(1u, env._api._actions.size());
	EXPECT_EQ("ORD-KNOWN", env._api._actions[0]._code);
	EXPECT_EQ("EU000002", env._api._actions[0]._exchg);

	unknown->release();
	known->release();
}

// 裸代码输入保留为单独禁用探针。initExt 所需 ITraderApi 是必需依赖，nullptr
// 没有接口合同支持；相关空依赖主张已从本用例移除。
TEST(DISABLED_q04_trader_entrust, bare_code_with_initialized_api_probe)
{
	q04::TempRoot tmp("cls2");
	q04::FakeTraderApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	TraderAdapter adapter;
	adapter.initExt("uft1", &api, &bd, &policy);
	adapter.run();

	//裸代码会让 doEntrust 的代码拆分路径收到缺少交易所的输入。
	adapter.openLong("ag2406", 5000.0, 1.0, 0);
}
