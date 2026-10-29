// R7 lane：WtTWapExeUnit（TWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtTWapExeUnit.cpp / .h
// 覆盖面: init 参数与派生量（calTmSecs/_fire_span 走日志断言）、do_calc 的均分/尾桶打板/
//         minopenlots 兜底/价格模式/价格偏移/零价回退/涨跌停收敛、每条早退分支、
//         on_tick 首笔与定时分支、on_order 撤单计数与重发、on_channel_ready 三分支、
//         on_entrust、fire_at_once 的价格模式与涨停收敛。
//
// 分片公式（L705）：curQty = max(minopenlots, round(|diffQty| / leftTimes)) * |diffQty|/diffQty
//   leftTimes = total_times - _fired_times（L675）；leftTimes==0 且 diffQty!=0 时走打板：
//   curQty = max(minopenlots, diffQty)（L700），价格另加 5 跳（L734）。
// 时序：on_tick 非首笔分支用产品侧挂钟比对 _fire_span*1000。本文件把 total_secs/tail_secs
//   配成 (19800-18000)/10 = 180 秒，或 tail==total 使 _fire_span=0，两条都能判定。
// 引用计数：do_calc L769 / fire_at_once L535 各有一处未配对 release（与 WtVWap 同型缺陷），
//   所以喂给 on_tick 且发过单的 tick 由单元析构收口，用例侧不再 release。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtTWapExeUnit.h"
#include "r7_init_probe.h"

using namespace wtp;

namespace
{
const char* const kTCode = "tw2403";

class TFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return pos; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char* c, double p, double q, bool) override
	{
		buys.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool) override
	{
		sells.push_back({ c, p, q });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t id) override { byidCancels.push_back(id); return true; }
	OrderIDs cancel(const char* c, bool isBuy, double = 0) override
	{
		bydirCancels.push_back({ c, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}
	void writeLog(const char* l) override { logs.push_back(l ? l : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return sess; }
	uint64_t getCurTime() override { return 20240101093000000ull; }

	double pos = 0;
	double undone = 0;
	uint32_t nextId = 8000;
	uint32_t cancelBydirRetCnt = 2;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<std::string> logs;

	bool hasLog(const std::string& sub) const { return countLog(sub) > 0; }
	uint32_t countLog(const std::string& sub) const
	{
		uint32_t n = 0;
		for (const std::string& l : logs)
			if (l.find(sub) != std::string::npos)
				n++;
		return n;
	}
};

WTSCommodityInfo* mkTComm()
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("tw", "R7TWAP", "CFFEX", "NIGHT_DAY", "CHN");
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	return c;
}

WTSTickData* mkTTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 80;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// L147-159 实际读取的键；默认 tail_secs=total_secs(19800) 使 _fire_span=0
WTSVariant* mkTCfg(uint32_t totalTimes = 4, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 19800, uint32_t beginTime = 930, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("total_secs", (uint32_t)3600);          // L150 读到的值会被 L162 覆盖
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("price_offset", priceOffset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

void bootT(WtTWapExeUnit& unit, TFakeCtx& ctx, double target, WTSVariant* cfg)
{
	unit.init(&ctx, kTCode, cfg);
	cfg->release();
	unit.set_position(kTCode, target);
	unit.on_channel_ready();
}


int run_twap_init_probe(uint32_t totalTimes, int markerFd)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	int exitCode = 0;
	{
		WtTWapExeUnit unit;
		WTSVariant* cfg = mkTCfg(totalTimes, 0, 0, 1.0, 10.0, 0, 0);
		if (!r7_test::write_marker(markerFd))
			exitCode = 4;
		else
			unit.init(&ctx, kTCode, cfg);
		cfg->release();
	}
	ctx.comm->release();
	return exitCode;
}
}

//////////////////////////////////////////////////////////////////////////
// 名字契约 / init 派生量（calTmSecs 与 _fire_span 从 L169 日志里读出来断言）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, names_and_init_derived_span)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtTWapExeUnit");      // L120
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");      // L110
	// calTmSecs(930,1500) = 54000-34200 = 19800；(19800-18000)/10 = 180
	unit.init(&ctx, kTCode, mkTCfg(10, 0, 0, 1.0, 10.0, 0, 18000));
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("执行时限 19800 秒"));
	EXPECT_TRUE(ctx.hasLog("间隔时间 180 秒"));
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 均分切片：diffQty=100 / leftTimes=4 -> 25 手（L705）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, splits_remaining_qty_evenly_across_left_times)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);            // max(1, round(100/4)) = 25
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);         // price_mode=0 -> 最新价（L720）
	EXPECT_EQ(ctx.buys[0].code, kTCode);
	EXPECT_TRUE(ctx.hasLog("第 1 次发单"));              // L680
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 尾差归整：round(|−10|/4)=round(2.5)=3（std::round 远离零）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, rounds_half_away_from_zero_when_remainder_not_even)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 10, mkTCfg(4));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 3.0);             // max(1, round(2.5)) = 3
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 0 量兜底：diffQty 小于 leftTimes 时均分结果为 0 -> 按 minopenlots 发
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, tiny_diffqty_floors_to_min_open_lots)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 3, mkTCfg(10, 0, 0, 1.0));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// round(3/10)=0 -> max(minopenlots=1, 0) = 1
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 1.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 卖出方向：均分后保持符号（*|diff|/diff = -1），走 sell 分支
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, sell_side_keeps_sign_and_splits_evenly)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.pos = 100;
	WtTWapExeUnit unit;
	bootT(unit, ctx, 20, mkTCfg(4, 2));                 // diffQty = -80, mode2 -> bid
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// max(1, round(80/4)) = 20 -> *(-1) = -20 -> sell 20
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 20.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.49);        // L728: isBuy=false -> bidprice(0)
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 尾桶打板：同一目标下首笔常规单部分成交，余单终结后新 tick 触发 leftTimes==0，
// 买入剩余 diffQty 并在最新价上加 5 跳（L675、L694-700、L732-735）。
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, last_slice_showhand_buys_remaining_with_5_tick_premium)
{
	TFakeCtx ctx;
	auto releaseComm = [](WTSCommodityInfo* comm) { if (comm) comm->release(); };
	std::unique_ptr<WTSCommodityInfo, decltype(releaseComm)> comm(mkTComm(), releaseComm);
	ctx.comm = comm.get();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(1));
	auto sendTick = [&unit](WTSTickData* rawTick) {
		auto releaseTick = [](WTSTickData* tick) { if (tick) tick->release(); };
		std::unique_ptr<WTSTickData, decltype(releaseTick)> tick(rawTick, releaseTick);
		unit.on_tick(tick.get());
		// 临时规避 BUG-R7-015：do_calc() 错误地额外 release 借用的 _last_tick，
		// 因此这里按回调后的引用数交接，避免这个既有缺陷干扰本用例的价格/数量断言。
		// 这不是标准 tick 所有权契约，也不证明生命周期安全；BUG-R7-015 修复后，
		// 应移除此规避并按正常 retain/release 契约统一回归。
		if (tick->retainCount() == 1)
			(void)tick.release();
	};
	sendTick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 首次发单：_fired_times=0、total_times=1，leftTimes=1，常规切片为 100 手。
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);
	EXPECT_TRUE(ctx.hasLog("第 1 次发单"));

	// 模拟该单成交 20 手、余下挂单已终结：仓位更新为 20，当前无未完成委托。
	// 通道状态同步会清理本地的旧订单记录，但不会重置本轮 _fired_times。
	ctx.pos = 20;
	ctx.undone = 0;
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);

	// 新 tick：_fired_times 仍为 1，leftTimes=1-1=0，剩余 diffQty=80 走 showhand。
	sendTick(mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 80.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.65);         // 最新价 10.60 + 5 * priceTick 0.01
	EXPECT_TRUE(ctx.hasLog("第 2 次发单"));
}

//////////////////////////////////////////////////////////////////////////
// _fire_span > 0 时，非首笔 tick 不重算（L415 的间隔判定）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_span_blocks_second_tick_recalc)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	// (19800-18000)/10 = 180 秒 -> 两条连续 tick 之间远小于 180000ms
	bootT(unit, ctx, 100, mkTCfg(10, 0, 0, 1.0, 10.0, 0, 18000));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 首笔 tick 触发 do_calc：leftTimes=10 -> round(100/10)=10
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 10.0);
	unit.on_tick(mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.buys.size(), 1u);                     // 间隔未到 -> 不再发单
	unit.on_tick(mkTTick(kTCode, 100200000, 10.70, 10.69, 10.71));
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_mode=1：买入取买一；price_mode=2：买入取卖一
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, price_mode_1_buy_uses_bid_and_mode_2_uses_ask)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1, u2;
	bootT(u1, ctx, 40, mkTCfg(2, 1));
	u1.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	bootT(u2, ctx, 40, mkTCfg(2, 2));
	u2.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// 两个单元共享 ctx，各自 1 笔：buys[0] 属于 mode1，buys[1] 属于 mode2
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);         // L724: isBuy -> bidprice(0)
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);         // L728: isBuy -> askprice(0)
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// price_offset：非打板时按 offset 跳加价（L736-738）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, price_offset_shifts_buy_up_and_sell_down)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1;
	bootT(u1, ctx, 100, mkTCfg(4, 0, 3));               // offset=3
	u1.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.53);         // 10.50 + 0.01*3
	ctx.pos = 100;
	WtTWapExeUnit u2;
	bootT(u2, ctx, 20, mkTCfg(4, 0, 3));                // diffQty=-80 -> isBuy=false
	u2.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.47);        // 10.50 - 0.01*3
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 零价回退：最新价为 0 时改用 preclose（L741-742）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, zero_last_price_falls_back_to_preclose)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0, 0, 1.0, 10.0, 0, 19800, 930, 1500));
	unit.on_tick(mkTTick(kTCode, 100000000, 0.0, 0.0, 0.0));
	// price_mode=0 -> targetPx = price()=0 -> L741 命中 -> price()==0 -> preclose()=10.0
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨停收敛（L746-751）与跌停收敛（L752-757）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, limit_prices_cap_the_order_price)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1;
	bootT(u1, ctx, 100, mkTCfg(4, 0, 300));
	u1.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);         // 10.50+3.00 -> 涨停 10.60
	EXPECT_TRUE(ctx.hasLog("modified to upper limit price"));
	ctx.pos = 100;
	WtTWapExeUnit u2;
	bootT(u2, ctx, 20, mkTCfg(4, 0, 300));
	u2.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.20);        // 10.50-3.00 -> 跌停 10.20
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 通道未就绪：一切计算作废（L552-553），就绪后同一笔 tick 立刻补发
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, calc_before_channel_ready_does_nothing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());                     // !_channel_ready -> L552 return
	unit.on_channel_ready();                           // undone=0、OMS 空 -> L303 else 分支
	// 就绪后 on_channel_ready 末尾的 do_calc 已经有 tick 了 -> 正常发单
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 无 tick / 同向挂单 两条早退（L627-631、L616-621）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, calc_exits_without_tick_then_blocks_on_live_order)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("没有最新tick数据"));           // L630
	EXPECT_TRUE(ctx.buys.empty());
	ctx.undone = 3.0;
	unit.set_position(kTCode, 200);
	EXPECT_TRUE(ctx.hasLog("上一轮有挂单未完成"));          // L619
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单在途：_cancel_cnt != 0 -> 本轮退出（L578-583）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, pending_cancel_blocks_calc)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	// L262: undone!=0 且 OMS 空 -> 撤外部单，isBuy=(undone>0)=true，_cancel_cnt=2
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("Unmanaged order updated cancelcnt to 2"));   // L282
	EXPECT_TRUE(ctx.hasLog("尚有未完成撤单指令"));                          // L581（随后的 do_calc）
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 反向未完成单先撤（L591-614）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, opposite_live_order_canceled_first_without_new_order)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.cancelBydirRetCnt = 1;
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));                   // boot 时 undone=0 -> else 分支
	ctx.undone = -3.0;
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// diffQty=100, undone=-3 -> L594 isBuy=(undone>0)=false
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_FALSE(ctx.bydirCancels[0].second);
	EXPECT_TRUE(ctx.hasLog("live opposite order of"));   // L609
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready：OMS 有单但柜台未完成单为 0 -> 清本地错单（L286-298）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, channel_ready_drops_unconfirmed_local_orders)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // OMS 里有 8000
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.hasLog("Local orders of"));
	EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));   // L296
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同一笔 tick 不重复下单（L657-665），拒单重算走同一条闸门
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, stale_tick_gate_and_entrust_reject)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_entrust(8000, kTCode, false, "rejected");    // erase -> do_calc -> 同一 tick 被拦
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("No tick of"));
	unit.on_entrust(8000, kTCode, true, "ok");           // 成功回报不进这条链
	EXPECT_EQ(ctx.buys.size(), 1u);
	unit.on_entrust(60606, kTCode, false, "not mine");   // L451 非本单元单
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_order：未知 id 忽略；撤单回报递减 _cancel_cnt（L186-201）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, on_order_drains_cancel_cnt)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.on_channel_ready();                             // 撤外部单 -> 8001/8002，_cancel_cnt=2
	unit.on_order(4242, kTCode, true, 0, 10.50, true);   // 未知 id -> L186 return
	EXPECT_FALSE(ctx.hasLog("updated cancelcnt"));
	unit.on_order(8001, kTCode, true, 0, 10.50, true);
	EXPECT_TRUE(ctx.hasLog("Order 8001 updated cancelcnt -> 1"));   // L200
	unit.on_order(8001, kTCode, true, 0, 10.50, true);              // 已摘除 -> 再次忽略
	EXPECT_EQ(ctx.countLog("updated cancelcnt"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单回报 + 本轮目标未达成 -> fire_at_once 重发并累计加价（L213-232、L468-536）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, cancel_report_refires_with_cancel_times_price_step)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // _this_target = 0 + 25 = 25
	unit.on_order(8000, kTCode, true, 5, 10.50, true);   // leftover!=0 且已撤 -> L213 命中
	// _cancel_times=1 -> fire_at_once(max(1, 25-0)=25)
	// price_mode=0 -> 10.50；L508 再 + 0.01*1 = 10.51
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));     // L223
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// fire_at_once 的价格模式：mode1 取买一、mode2 取卖一，均再按 _cancel_times 加价
// （与 do_calc 的 mode1/2 用同一套基准，见 L495-504 vs L722-729）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_at_once_mode_1_and_2_pricing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit u1, u2;
	bootT(u1, ctx, 100, mkTCfg(4, 1));
	u1.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	bootT(u2, ctx, 100, mkTCfg(4, 2));
	u2.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.49);         // u1 do_calc mode1 买入 -> bid
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);         // u2 do_calc mode2 买入 -> ask
	u1.on_order(8000, kTCode, true, 5, 10.49, true);    // u1 的首单 -> fire_at_once(mode1, bid)
	u2.on_order(8001, kTCode, true, 5, 10.51, true);    // u2 的首单 -> fire_at_once(mode2, ask)
	ASSERT_EQ(ctx.buys.size(), 4u);
	// L508: + priceTick*_cancel_times(1) = +0.01
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.50);         // 10.49 + 0.01
	EXPECT_DOUBLE_EQ(ctx.buys[3].price, 10.52);         // 10.51 + 0.01
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 25.0);            // _this_target = 0 + round(100/4) = 25
	EXPECT_DOUBLE_EQ(ctx.buys[3].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非法 price_mode 在 fire_at_once 里没有零价兜底（do_calc 有，L741-742 缺在此处）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, fire_at_once_unknown_price_mode_has_no_preclose_fallback)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 3));                // price_mode=3
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// do_calc 的 else 分支（L726-729）仍取对手价 10.51
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);
	unit.on_order(8000, kTCode, true, 5, 10.51, true);  // -> fire_at_once
	// fire_at_once 的 if/elif 只覆盖 0/1/2（L490-504）-> targetPx 停在 0
	// 再按 L508 加 0.01*_cancel_times(1) -> 报出 0.01 的价
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 0.01);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非法 price_mode 在 do_calc 落到 else 分支（与 mode 2 同价）（L726-729）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, unknown_price_mode_falls_into_counter_price)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 9));                 // price_mode=9 -> else
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);          // 买入 -> askprice(0)
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_tick 入参防御 + on_trade/on_channel_lost 空实现（L332、L432-435、L316-320）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, tick_guards_and_noop_callbacks)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	unit.on_tick(NULL);
	unit.on_trade(8000, kTCode, true, 10, 10.50);
	unit.on_channel_lost();
	WTSTickData* foreign = mkTTick("xx2403", 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(foreign);
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_FALSE(ctx.hasLog("次发单"));
	foreign->release();
	unit.set_position("other", 100);                     // L782 代码不符 -> 丢弃
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 重复目标仓位被丢弃（L787-788），因此不会重算
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, repeated_target_is_ignored)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.set_position(kTCode, 100);                      // 与 _target_pos 相等 -> 直接 return
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);
	unit.on_tick(mkTTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	// _fire_span=0 -> 会重算；此时 diffQty 仍是 100（realPos 未变），_fired_times=1,
	// total_times=4 -> leftTimes=3 -> round(100/3)=33
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 33.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 清仓（DBL_MAX）与 0 实仓：diffQty==0 早退（L586-587）
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, clear_target_with_flat_position_does_nothing)
{
	TFakeCtx ctx;
	ctx.comm = mkTComm();
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4));
	unit.on_channel_ready();
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	unit.set_position(kTCode, DBL_MAX);                  // get_real_target -> 0 == realPos
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// bounded child probe only: no fallback/default is asserted, and a normal ARM64
// return does not rule out C++ undefined behavior in the division.
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, init_zero_total_times_bounded_child_probe)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_TWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 0, run_twap_init_probe);
	ASSERT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
	if (!r7_test::expect_normal_return(result))
		return;
	GTEST_SKIP() << "The child returned normally on this build; without UBSan or an explicit invalid-config failure contract, this cannot prove safe rejection or absence of undefined behavior.";
}

TEST(r7_twap, init_positive_total_times_control_returns)
{
	r7_test::ScopedTempDir probeDir("WT_R7_002_TWAP");
	ASSERT_TRUE(probeDir.ready());
	const r7_test::ProbeResult result = r7_test::run_bounded_child(probeDir.path(), 5, run_twap_init_probe);
	r7_test::expect_normal_return(result);
	EXPECT_TRUE(probeDir.cleanup()) << "failed to remove private init-probe directory";
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（空指针解引用，不能启用）：ctx 不提供品种信息时 L738 直接踩 NULL
// init 在 L136-139 允许 _comm_info 为 NULL，do_calc 却无条件 _comm_info->getPriceTick()
//////////////////////////////////////////////////////////////////////////
TEST(r7_twap, DISABLED_null_commodity_info_crashes_price_offset_branch)
{
	TFakeCtx ctx;
	ctx.comm = NULL;
	WtTWapExeUnit unit;
	unit.init(&ctx, kTCode, mkTCfg(4, 0, 3));
	unit.set_position(kTCode, 100);
	unit.on_channel_ready();
	unit.on_tick(mkTTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_TRUE(ctx.buys.empty());                       // 期望：崩溃（BUG-R7-006）
}
