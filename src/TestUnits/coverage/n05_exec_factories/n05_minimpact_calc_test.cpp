// N05 lane：WtMinImpactExeUnit 最小冲击执行单元算法核心测试
// 目标文件: src/WtExeFact/WtMinImpactExeUnit.cpp / .h
// 覆盖符号: init / getName / getFactName / set_position / clear_all_position /
//           on_tick / on_order / on_trade / on_entrust / on_channel_lost / do_calc(私有,经上述入口驱动)
// 说明: 用 FakeExecCtx 替代真实交易通道，只验证撤单/报单的方向、数量、价格类型；
//       需要真实柜台回报的分支（on_channel_ready 的未完成单对账）单独用 fake 数据覆盖。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtMinImpactExeUnit.h"

using namespace wtp;

namespace
{
class FakeExecCtx : public ExecuteContext
{
public:
	struct Rec
	{
		std::string code;
		double price;
		double qty;
		bool force;
	};

	// ExecuteContext 接口
	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return pos; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		buys.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastIds = ids;
		return ids;
	}

	OrderIDs sell(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		sells.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastIds = ids;
		return ids;
	}

	bool cancel(uint32_t localid) override
	{
		byidCancels.push_back(localid);
		return cancelByIdRet;
	}

	OrderIDs cancel(const char* stdCode, bool isBuy, double /*qty*/ = 0) override
	{
		bydirCancels.push_back({ stdCode, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		lastIds = ids;
		return ids;
	}

	void writeLog(const char*) override {}
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return curTime; }

public:
	double pos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 9000;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	bool cancelByIdRet = true;
	uint32_t cancelBydirRetCnt = 1;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<uint32_t> byidCancels;
	OrderIDs lastIds;
};

WTSCommodityInfo* mkComm()
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "NIGHT_DAY", "CHN");
	c->setVolScale(10);
	c->setPriceTick(1.0);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	return c;
}

// 直接构造已填满字段的 tick（tick->code() 与单元 _code 一致）
WTSTickData* mkTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double bidQty, double askPx, double askQty,
	double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 3400.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = bidQty;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = askQty;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

WTSVariant* mkCfg(int32_t offset, uint32_t expire, int32_t priceMode,
	uint32_t span, bool byRate, double lots, double rate,
	double minOpenLots = 1.0)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", offset);
	cfg->append("expire", expire);
	cfg->append("pricemode", priceMode);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

// 单个测试的装配体：保证 单元析构(释放retain) 先于 comm/tick 的收尾释放
struct Harness
{
	FakeExecCtx ctx;
	WTSCommodityInfo* comm = NULL;

	Harness()
	{
		comm = mkComm();
		ctx.comm = comm;
	}
};
}

//////////////////////////////////////////////////////////////////////////
// 初始化与命名契约
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, names_and_init_params)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		EXPECT_STREQ(unit.getFactName(), "WtExeFact");
		EXPECT_STREQ(unit.getName(), "WtMinImpactExeUnit");
		WTSVariant* cfg = mkCfg(2, 3, 1, 500, false, 3.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		// init 后立刻按错误代码设置目标仓位：应被忽略、不发单
		unit.set_position("other2401", 5);
		EXPECT_TRUE(h.ctx.buys.empty());
		EXPECT_TRUE(h.ctx.sells.empty());
		cfg->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 开仓买入：数量=min(lots,|target-pos|)，对手价+offset*tick
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, open_buy_by_lots_counterprice)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(2, 0, 1, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		// 无 tick 时设置目标仓位：不发单（No lastest tick 分支）
		unit.set_position("rb2401", 5);
		EXPECT_TRUE(h.ctx.buys.empty());

		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(tick);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		// pricemode=1(对手价): buyPx=askprice(0)+priceTick*offset=3510+1*2
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3512.0);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 3.0);   // min(lots=3, 5-0)
		EXPECT_FALSE(h.ctx.buys[0].force);
		tick->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// byrate 比例下单 + 目标仓位修正
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, open_buy_by_rate)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, true, 100.0, 0.5);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 80, 3510, 100);
		unit.set_position("rb2401", 3);  // 无tick，先记录目标
		unit.on_tick(tick);              // askqty0=100 -> round(100*0.5)=50 -> min(50,3)
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 3.0);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3505.0);  // pricemode=0 最新价
		tick->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 自动价格模式(pricemode=2)：买量失衡时取对手侧一档价
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, auto_price_mode_uses_ask_when_bid_heavier)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 2, 0, false, 1.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		// mp=(100-50)/(100+50)>0 -> isUp -> 买卖基准价均取 askprice(0)=3510
		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.set_position("rb2401", 1);
		unit.on_tick(tick);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3510.0);
		tick->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 清仓：clear_all_position 触发强制平仓卖单
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, clear_all_position_forced_sell)
{
	Harness h;
	{
		h.ctx.pos = 10;
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(tick);
		unit.clear_all_position("rb2401");
		// target=DBL_MAX -> newVol=0, realPos=10 -> 卖出 min(lots=2,10)=2, 强制平仓
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].qty, 2.0);
		EXPECT_TRUE(h.ctx.sells[0].force);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3505.0);
		tick->release();

		// 已处于清仓流程时再设 0 目标：被拒绝，不再触发新单
		unit.set_position("rb2401", 0);
		EXPECT_EQ(h.ctx.sells.size(), 1u);

		// 非本合约的 clear 被忽略
		unit.clear_all_position("other");
		EXPECT_EQ(h.ctx.sells.size(), 1u);
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 反向未完成单：先撤方向相反的挂单，撤单回报齐全后才发新单
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, cancel_opposite_live_orders_then_reorder)
{
	Harness h;
	{
		h.ctx.undone = -3;  // 有 3 手卖单在场
		h.ctx.cancelBydirRetCnt = 2;
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(tick);
		unit.set_position("rb2401", 5);  // diff=5, undone=-3, 乘积<0 -> 撤卖单
		ASSERT_EQ(h.ctx.bydirCancels.size(), 1u);
		EXPECT_FALSE(h.ctx.bydirCancels[0].second);  // 撤的是卖单
		EXPECT_TRUE(h.ctx.buys.empty());
		const OrderIDs cancelIds = h.ctx.lastIds;
		ASSERT_EQ(cancelIds.size(), 2u);

		// 第一笔撤单回报：cancel_cnt 还有 1，不应发新单
		unit.on_order(cancelIds[0], "rb2401", false, 3, 3500, true);
		EXPECT_TRUE(h.ctx.buys.empty());

		// 柜台已无未完成单，第二笔回报后触发重算并发出买单
		h.ctx.undone = 0;
		unit.on_order(cancelIds[1], "rb2401", false, 3, 3500, true);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 3.0);
		// 不属于监控器的订单回报应被忽略
		size_t buysBefore = h.ctx.buys.size();
		unit.on_order(12345, "rb2401", true, 1, 3500, true);
		unit.on_order(12345, "rb2401", true, 0, 3500, false);
		EXPECT_EQ(h.ctx.buys.size(), buysBefore);
		tick->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 超时撤单：on_tick 内按 expire_secs 扫描并撤单
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, expired_order_canceled_on_next_tick)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 0, 0, false, 3.0, 0.0);  // expire=1s, span=0
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* t1 = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.set_position("rb2401", 5);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		uint32_t liveId = h.ctx.lastIds[0];

		// 时间前进 2s，新 tick 进来 -> 过期订单被按 id 撤销
		h.ctx.curTime += 2000;
		WTSTickData* t2 = mkTick("rb2401", 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		ASSERT_EQ(h.ctx.byidCancels.size(), 1u);
		EXPECT_EQ(h.ctx.byidCancels[0], liveId);

		// 撤单回报后 cancel_cnt 归零，重算发新单（同一目标仓位未达成）
		unit.on_order(liveId, "rb2401", true, 3, 3505, true);
		EXPECT_EQ(h.ctx.buys.size(), 2u);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨跌停修正：价格被修正为停板价后订单不可撤
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, buy_price_capped_at_upper_limit)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(5, 0, 1, 0, false, 2.0, 0.0);  // 对手价+5跳会越过涨停
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* tick = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50,
			3512 /*涨停*/, 3400 /*跌停*/);
		unit.set_position("rb2401", 4);
		unit.on_tick(tick);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		// 3510+5=3515 > 涨停 3512 -> 修正为涨停价
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3512.0);
		tick->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 委托失败回报 / 成交回报 / 通道丢失：不应崩溃且不产生多余动作
//////////////////////////////////////////////////////////////////////////
TEST(n05_minimpact, entrust_fail_and_misc_callbacks)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* t1 = mkTick("rb2401", 93000000, 3505, 3500, 100, 3510, 50);
		unit.set_position("rb2401", 4);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);

		unit.on_trade(h.ctx.lastIds[0], "rb2401", true, 2, 3505);  // 空实现
		unit.on_channel_lost();                                    // 空实现

		// 委托失败 -> 监控器剔除该单；行情更新后可再次发单
		unit.on_entrust(h.ctx.lastIds[0], "rb2401", false, "reject");
		WTSTickData* t2 = mkTick("rb2401", 93001000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t2);
		EXPECT_EQ(h.ctx.buys.size(), 2u);
		// 成功回报不做处理
		unit.on_entrust(h.ctx.lastIds[0], "rb2401", true, "");
		t1->release();
		t2->release();
	}
	h.comm->release();
}
