// BUG-R7-012（迭代03 卡E，2026-11-04，exec-units 包）：TWAP 按交易所最小合法单位发单。
//
// 合同（决策积压清单 §3 E 类，计划 §2“股票 VWAP”同口径）：卖出按标的最小合法交易单位
// （商品的 lotstick/minlots）取整；不足一个合法单位的余量保留为未完成，不发送不合法数量，
// 不假设允许零股。买入同样只发合法数量（卡8：整手 provider 不接受小数，框架不替策略取整）。
// 覆盖买入、卖出、T+1、目标变化、重复行情、元数据不可用。只使用本地 fake ExecuteContext。
#include <gtest/gtest.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtOrdMon.h"
#define private public
#include "WtExeFact/WtTWapExeUnit.h"
#undef private

using namespace wtp;

namespace
{
const char* const kLotCode = "SSE.STK.600000";

/// 持仓按方向/可用分别给出，满足 TWAP 的一致性校验（总量=多+空，可用不超过总量）。
class LotCtx : public ExecuteContext
{
public:
	LotCtx()
	{
		sess = WTSSessionInfo::create("R7_LOT", "R7_LOT", 0);
		sess->addTradingSection(930, 1500);
	}
	~LotCtx() override
	{
		sess->release();
		if (comm)
			comm->release();
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool validOnly = true, int32_t flag = 3) override
	{
		const double lng = validOnly ? longAvail : longTotal;
		const double sht = validOnly ? shortAvail : shortTotal;
		if (flag == 1)
			return lng;
		if (flag == 2)
			return sht;
		return lng + sht;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char*, double, double qty, bool forceClose) override
	{
		buys.push_back(qty);
		buyForce.push_back(forceClose);
		return OrderIDs{ nextId++ };
	}
	OrderIDs sell(const char*, double, double qty, bool forceClose) override
	{
		sells.push_back(qty);
		sellForce.push_back(forceClose);
		return OrderIDs{ nextId++ };
	}
	bool cancel(uint32_t) override { return true; }
	OrderIDs cancel(const char*, bool, double = 0) override { return OrderIDs(); }
	void writeLog(const char* message) override { logs.push_back(message ? message : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

	uint32_t countLog(const std::string& needle) const
	{
		uint32_t n = 0;
		for (const std::string& message : logs)
			if (message.find(needle) != std::string::npos)
				++n;
		return n;
	}

	/// 模拟已发订单全部成交：多头总量与可用量按 T+1 规则变化
	void fillLastSell(bool availableToo = true)
	{
		longTotal -= sells.back();
		if (availableToo)
			longAvail -= sells.back();
	}
	void fillLastBuyT1()
	{
		longTotal += buys.back(); // T+1: bought volume is not available today
	}

	double longTotal = 0.0;
	double longAvail = 0.0;
	double shortTotal = 0.0;
	double shortAvail = 0.0;
	double undone = 0.0;
	uint64_t curTime = 20240102100000000ull;
	uint32_t nextId = 6100;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	std::vector<double> buys;
	std::vector<double> sells;
	std::vector<bool> buyForce;
	std::vector<bool> sellForce;
	std::vector<std::string> logs;
};

WTSCommodityInfo* mkLotComm(double lotsTick, double minLots, TradingMode mode)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("STK", "R7LOT", "SSE", "R7_LOT", "CNY");
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(lotsTick);
	c->setMinLots(minLots);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(mode);
	return c;
}

WTSVariant* mkLotCfg(uint32_t totalTimes)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", (uint32_t)930);
	cfg->append("end_time", (uint32_t)1500);
	cfg->append("ord_sticky", (uint32_t)0);
	cfg->append("tail_secs", (uint32_t)19800); // _fire_span = 0: every fresh tick may slice
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", (uint32_t)0);
	cfg->append("price_offset", (uint32_t)0);
	cfg->append("lots", 1000.0);
	cfg->append("minopenlots", 0.0);
	return cfg;
}

void feed(WtTWapExeUnit& unit, uint32_t actTime)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", kLotCode);
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	ts.action_time = actTime;
	ts.price = 10.0;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = 9.99;
	ts.bid_qty[0] = 10000;
	ts.ask_prices[0] = 10.01;
	ts.ask_qty[0] = 10000;
	WTSTickData* tick = WTSTickData::create(ts);
	unit.on_tick(tick);
	tick->release();
}

void boot(WtTWapExeUnit& unit, LotCtx& ctx, uint32_t totalTimes)
{
	WTSVariant* cfg = mkLotCfg(totalTimes);
	unit.init(&ctx, kLotCode, cfg);
	cfg->release();
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.on_channel_ready();
}
}

// 卖出：250 股按 100 股合法单位只发 200；剩余 50 股保留未完成，重复行情不发零股。
TEST(r7_twap_legal_lot, sell_rounds_down_to_legal_unit_and_keeps_odd_remainder)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(100.0, 100.0, TM_LongT1);
	ctx.longTotal = ctx.longAvail = 250.0;
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, 0.0);
	feed(unit, 100000000);
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0], 200.0);
	EXPECT_TRUE(ctx.buys.empty());

	ctx.fillLastSell();
	feed(unit, 100001000);
	feed(unit, 100002000);
	feed(unit, 100002000); // repeated tick time
	EXPECT_EQ(ctx.sells.size(), 1u) << "a 50-share remainder is below the legal unit and is never sent";
	EXPECT_GE(ctx.countLog("below the minimum legal order unit"), 1u);
	EXPECT_DOUBLE_EQ(ctx.longTotal, 50.0) << "the remainder stays outstanding";
}

// T+1：当日买入不可卖，卖出先受可用量限制，再按合法单位取整；可用不足一个单位时不发单。
TEST(r7_twap_legal_lot, t1_sell_is_capped_by_available_then_legal_unit)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(100.0, 100.0, TM_LongT1);
	ctx.longTotal = 330.0;
	ctx.longAvail = 130.0; // 200 bought today
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, 0.0);
	feed(unit, 100000000);
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0], 100.0) << "130 available -> one legal 100-share lot";

	ctx.fillLastSell();
	feed(unit, 100001000);
	feed(unit, 100002000);
	EXPECT_EQ(ctx.sells.size(), 1u) << "30 available shares are not a legal lot; today's 200 are not sellable";
	EXPECT_DOUBLE_EQ(ctx.longTotal, 230.0);
}

// 买入与目标变化：买入同样只发合法数量；目标变化后按新差额重新切片，仍只发合法单位。
TEST(r7_twap_legal_lot, buy_and_target_change_use_legal_units_only)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(100.0, 100.0, TM_LongT1);
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, 250.0);
	feed(unit, 100000000);
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0], 200.0);
	ctx.fillLastBuyT1();
	feed(unit, 100001000);
	EXPECT_EQ(ctx.buys.size(), 1u) << "the 50-share buy remainder is held";

	ctx.longAvail = ctx.longTotal; // next trading day: all 200 sellable
	unit.set_position(kLotCode, 50.0); // target change
	feed(unit, 100002000);
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0], 100.0) << "150 to sell -> one legal lot; 50 remains outstanding";
	ctx.fillLastSell();
	feed(unit, 100003000);
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);
}

// 期货整手（lotstick=minlots=1）对照：整数差额照常发出；小数目标由策略控制（卡8），
// 框架只发整手部分，0.5 手余量保留，不发送小数手。
TEST(r7_twap_legal_lot, whole_lot_future_control_and_fractional_target_remainder)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(1.0, 1.0, TM_Both);
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, 2.5);
	feed(unit, 100000000);
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0], 2.0);
	ctx.longTotal = ctx.longAvail = 2.0;
	feed(unit, 100001000);
	EXPECT_EQ(ctx.buys.size(), 1u);

	unit.set_position(kLotCode, 0.0);
	feed(unit, 100002000);
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0], 2.0) << "integer control is unchanged";
}

// 合法单位无法确定（lotstick 非正）时不发任何单，余量保留并给出诊断。
TEST(r7_twap_legal_lot, unavailable_lot_rules_hold_every_order)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(0.0, 100.0, TM_LongT1);
	ctx.longTotal = ctx.longAvail = 300.0;
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, 0.0);
	feed(unit, 100000000);
	feed(unit, 100001000);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_GE(ctx.countLog("lot rules are unavailable"), 1u);
}

// BUG-R7-004（卡15最终复核发现的明确缺陷）：清仓指令（DBL_MAX）在净持仓为零但有锁仓
// （多空毛仓并存）时，原实现因“差额为零即返回”使清仓分支不可达，毛仓永不平掉。
// 修复后先强平可用多头，再经普通路径买回空头；平仓单均带 forceClose。
TEST(r7_twap_clear, net_zero_locked_position_is_flattened_with_close_orders)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(1.0, 1.0, TM_Both);
	ctx.longTotal = ctx.longAvail = 2.0;
	ctx.shortTotal = ctx.shortAvail = -2.0; // net 0
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, DBL_MAX);
	feed(unit, 100000000);
	ASSERT_EQ(ctx.sells.size(), 1u) << "net zero must not hide the gross long under a clear command";
	EXPECT_DOUBLE_EQ(ctx.sells[0], 2.0);
	EXPECT_TRUE(ctx.sellForce[0]);
	EXPECT_TRUE(ctx.buys.empty());

	ctx.longTotal = ctx.longAvail = 0.0; // long closed, net -2
	feed(unit, 100001000);
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0], 2.0);
	EXPECT_TRUE(ctx.buyForce[0]);

	ctx.shortTotal = ctx.shortAvail = 0.0; // flat
	feed(unit, 100002000);
	EXPECT_EQ(ctx.sells.size(), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);
}

// 对照：清仓时已无持仓不发单；普通（非清仓）零差额同样不发单。
TEST(r7_twap_clear, flat_clear_and_ordinary_zero_difference_send_nothing)
{
	LotCtx ctx;
	ctx.comm = mkLotComm(1.0, 1.0, TM_Both);
	WtTWapExeUnit unit;
	boot(unit, ctx, 1);
	unit.set_position(kLotCode, DBL_MAX);
	feed(unit, 100000000);
	feed(unit, 100001000);
	ctx.longTotal = ctx.longAvail = 1.0;
	ctx.shortTotal = ctx.shortAvail = -1.0;
	unit.set_position(kLotCode, 0.0); // ordinary zero target with a locked position
	feed(unit, 100002000);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.buys.empty());
}
