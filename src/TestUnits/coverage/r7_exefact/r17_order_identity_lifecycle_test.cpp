// BUG-R17-001（迭代03，2026-11-04，exec-units 包）：WtDiffMinImpactExeUnit / WtOrdMon
// 订单身份生命周期补测——重复 ACK、多订单、localid 复用与跨重连回收。
//
// 合同依据：R17-001 原修复方向“终态订单退出超时扫描、保留迟到成交关联、监控与关联记录
// 最终清理”；卡5（5d=A）要求回报幂等。执行单元回调不携带成交编号与订单令牌，重复成交
// 回报的去重属于宿主 TraderAdapter（WP1 逐单账本）职责，本文件不在单元层声称覆盖。
// 全部用例只用本地 fake ExecuteContext，不连接任何柜台。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtDiffMinImpactExeUnit.h"
#undef private

using namespace wtp;

namespace
{
class R17IdCtx : public ExecuteContext
{
public:
	R17IdCtx()
	{
		sess = WTSSessionInfo::create("R17_ID", "R17 identity session");
		sess->addTradingSection(900, 1500);
		comm = WTSCommodityInfo::create("rb", "rb", "SHFE", "R17_ID", "CHN");
		comm->setVolScale(10);
		comm->setPriceTick(1.0);
		comm->setLotsTick(1.0);
		comm->setMinLots(1.0);
		comm->setCategory(CC_Future);
		comm->setCoverMode(CM_OpenCover);
		comm->setPriceMode(PM_Both);
		comm->setTradingMode(TM_Both);
	}
	~R17IdCtx() override
	{
		sess->release();
		comm->release();
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return 0.0; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char*, double, double qty, bool) override
	{
		buys.push_back(qty);
		return OrderIDs{ nextId++ };
	}
	OrderIDs sell(const char*, double, double qty, bool) override
	{
		sells.push_back(qty);
		return OrderIDs{ nextId++ };
	}
	bool cancel(uint32_t localid) override
	{
		byid.push_back(localid);
		return true;
	}
	OrderIDs cancel(const char*, bool, double = 0) override
	{
		OrderIDs ids;
		for (uint32_t id : bydirIds)
			ids.push_back(id);
		return ids;
	}
	void writeLog(const char* message) override { logs.push_back(message ? message : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	bool hasLog(const std::string& needle) const
	{
		for (const std::string& message : logs)
			if (message.find(needle) != std::string::npos)
				return true;
		return false;
	}

	double undone = 0.0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 9100;
	std::vector<uint32_t> bydirIds;
	std::vector<double> buys;
	std::vector<double> sells;
	std::vector<uint32_t> byid;
	std::vector<std::string> logs;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
};

const char* const kR17Code = "SHFE.rb.2405";

WTSTickData* r17Tick(uint32_t actTime)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", kR17Code);
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = actTime;
	ts.price = 3505;
	ts.pre_close = 3400;
	ts.bid_prices[0] = 3500;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = 3510;
	ts.ask_qty[0] = 50;
	return WTSTickData::create(ts);
}

void r17Init(WtDiffMinImpactExeUnit& unit, R17IdCtx& ctx, uint32_t expire)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", (int32_t)0);
	cfg->append("expire", expire);
	cfg->append("pricemode", (int32_t)0);
	cfg->append("span", (uint32_t)0);
	cfg->append("byrate", false);
	cfg->append("lots", 1.0);
	cfg->append("rate", 0.0);
	unit.init(&ctx, kR17Code, cfg);
	cfg->release();
}

void r17FeedTick(WtDiffMinImpactExeUnit& unit, uint32_t actTime)
{
	WTSTickData* tick = r17Tick(actTime);
	unit.on_tick(tick);
	tick->release();
}
}

// 重复的撤单 ACK 只消费一次：两笔待撤订单中第一笔的撤单回报重复到达时，撤单计数不能
// 被减到 0 而提前放开下单，也不能重复累加撤单价格惩罚。
TEST(r17_order_identity, duplicate_cancel_ack_with_two_pending_cancels_is_applied_once)
{
	R17IdCtx ctx;
	WtDiffMinImpactExeUnit unit;
	r17Init(unit, ctx, 0);
	ctx.undone = 2.0;
	ctx.bydirIds = { 501u, 502u };
	unit.on_channel_ready(); // unmanaged live quantity: cancel both by direction
	ASSERT_EQ(unit._cancel_cnt, 2u);
	ASSERT_TRUE(unit._orders_mon.has_order(501u));
	ASSERT_TRUE(unit._orders_mon.has_order(502u));

	unit.set_position(kR17Code, 3.0);
	unit.on_order(501u, kR17Code, true, 1.0, 3500, true);
	EXPECT_EQ(unit._cancel_cnt, 1u);
	EXPECT_EQ(unit._cancel_times, 1u);
	unit.on_order(501u, kR17Code, true, 1.0, 3500, true); // duplicate ACK
	EXPECT_EQ(unit._cancel_cnt, 1u) << "a repeated terminal report must not release order 502's pending cancel";
	EXPECT_EQ(unit._cancel_times, 1u) << "a repeated terminal report must not re-price twice";
	EXPECT_TRUE(ctx.hasLog("Duplicate terminal report of order 501"));

	ctx.undone = 0.0; // the adapter no longer reports live quantity, but 502's cancel is unconfirmed
	r17FeedTick(unit, 93001000);
	EXPECT_TRUE(ctx.buys.empty()) << "no new order while a cancellation is still pending";

	unit.on_order(502u, kR17Code, true, 1.0, 3500, true); // the other order: a distinct ACK
	EXPECT_EQ(unit._cancel_cnt, 0u);
	EXPECT_EQ(unit._cancel_times, 2u);
	// The confirmed cancellation recalculates once on the current tick.
	ASSERT_EQ(ctx.buys.size(), 1u) << "both cancellations confirmed: the diff is executed";
	EXPECT_DOUBLE_EQ(ctx.buys[0], 1.0);
	ctx.undone = 1.0; // that order is now live: a later tick must not overlap it
	r17FeedTick(unit, 93002000);
	EXPECT_EQ(ctx.buys.size(), 1u);
}

// 重复的全成 ACK 不重复处理；多订单各自终态互不影响，终态记录继续承接迟到成交。
TEST(r17_order_identity, duplicate_fill_ack_and_multiple_orders_keep_independent_state)
{
	R17IdCtx ctx;
	WtDiffMinImpactExeUnit unit;
	r17Init(unit, ctx, 0);
	unit.on_channel_ready();
	unit.set_position(kR17Code, 2.0);
	r17FeedTick(unit, 93000000);
	ASSERT_EQ(ctx.buys.size(), 1u);
	const uint32_t first = ctx.nextId - 1;

	unit.on_order(first, kR17Code, true, 0.0, 3510, false); // fully filled
	unit.on_order(first, kR17Code, true, 0.0, 3510, false); // duplicate fill ACK
	EXPECT_TRUE(ctx.hasLog("Duplicate terminal report of order " + std::to_string(first)));
	EXPECT_EQ(unit._cancel_times, 0u);
	unit.on_trade(first, kR17Code, true, 1.0, 3510); // late trade still associated
	EXPECT_DOUBLE_EQ(unit._left_diff, 1.0);

	r17FeedTick(unit, 93001000);
	ASSERT_EQ(ctx.buys.size(), 2u);
	const uint32_t second = ctx.nextId - 1;
	ASSERT_NE(first, second);
	unit.on_order(second, kR17Code, true, 1.0, 3510, false); // live, partial state
	EXPECT_TRUE(unit._orders_mon.has_order(first));
	EXPECT_TRUE(unit._orders_mon.has_order(second));
	unit.on_trade(second, kR17Code, true, 1.0, 3510);
	EXPECT_DOUBLE_EQ(unit._left_diff, 0.0);
	unit.on_order(second, kR17Code, true, 0.0, 3510, false);
	EXPECT_FALSE(ctx.hasLog("Duplicate terminal report of order " + std::to_string(second)));
}

// localid 复用：被保留的终态记录在同号新订单登记时被替换，新订单重新成为可超时、
// 可再次终态化的存活订单；不会因旧终态而吞掉新订单的终态回报。
TEST(r17_order_identity, reused_localid_starts_a_new_live_order)
{
	WtOrdMon monitor;
	const uint32_t id = 77u;
	monitor.push_order(&id, 1, 1000);
	EXPECT_TRUE(monitor.mark_order_terminal(id));
	EXPECT_FALSE(monitor.mark_order_terminal(id)) << "second terminal report of the same order";
	std::vector<uint32_t> expired;
	monitor.check_orders(1, 1000 + 5000, [&expired](uint32_t localid) { expired.push_back(localid); });
	EXPECT_TRUE(expired.empty()) << "a terminal order is never an expiry candidate";

	WtOrdMonOrderIdentity before;
	ASSERT_TRUE(monitor.get_order_identity(id, before));
	monitor.push_order(&id, 1, 9000); // host reused the localid for a new order
	WtOrdMonOrderIdentity after;
	ASSERT_TRUE(monitor.get_order_identity(id, after));
	EXPECT_FALSE(monitor.erase_order(before)) << "the old identity no longer matches the reused order";
	EXPECT_TRUE(monitor.has_order(id));
	monitor.check_orders(1, 9000 + 5000, [&expired](uint32_t localid) { expired.push_back(localid); });
	ASSERT_EQ(expired.size(), 1u) << "the reused order is live and eligible for expiry";
	EXPECT_EQ(expired[0], id);
	EXPECT_TRUE(monitor.mark_order_terminal(id)) << "the new order's first terminal report is applied";
	EXPECT_TRUE(monitor.erase_order(after));
}

// 跨重连回收：断线期间保留的终态关联在重连确认无挂单后被清理，之后迟到成交不再改动
// 单元差量（差量由宿主 Diff 账本在 ready 时重新下发）；仍有挂单时不清理、不误撤。
TEST(r17_order_identity, reconnect_reclaims_terminal_records_only_without_live_orders)
{
	R17IdCtx ctx;
	{
		WtDiffMinImpactExeUnit unit;
		r17Init(unit, ctx, 0);
		unit.on_channel_ready();
		unit.set_position(kR17Code, 2.0);
		r17FeedTick(unit, 93000000);
		ASSERT_EQ(ctx.buys.size(), 1u);
		const uint32_t first = ctx.nextId - 1;
		unit.on_order(first, kR17Code, true, 1.0, 3510, true); // canceled, retained for late fills

		unit.on_channel_lost();
		ctx.undone = 1.0; // another live order exists on the account after reconnect
		unit.on_channel_ready();
		EXPECT_TRUE(unit._orders_mon.has_order(first)) << "records are not reclaimed while live quantity exists";
		EXPECT_TRUE(ctx.byid.empty());

		unit.on_channel_lost();
		ctx.undone = 0.0;
		unit.on_channel_ready();
		EXPECT_FALSE(unit._orders_mon.has_order(first)) << "no live orders after reconnect: reclaim";
		const double leftBefore = unit._left_diff;
		unit.on_trade(first, kR17Code, true, 1.0, 3510);
		EXPECT_DOUBLE_EQ(unit._left_diff, leftBefore) << "a reclaimed id no longer adjusts the unit diff";
	}
}
