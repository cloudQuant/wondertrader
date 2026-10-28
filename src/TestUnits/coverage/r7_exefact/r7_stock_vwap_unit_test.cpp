// R7 lane：WtStockVWapExeUnit（股票/科创板 VWAP 分片执行单元）
// 目标文件: src/WtExeFact/WtStockVWapExeUnit.cpp / WtStockVWapExeUnit.h
// 覆盖面: init（产品最小手数、科创板 max/普通 min 的钳位、可选键 "offset"、分布文件解析）、
//         do_calc 的持仓不足目标修正、碎股完成判定、成交量分片被丢弃的路径、价格模式与涨跌停、
//         on_tick/on_order/on_channel_ready/on_entrust/on_trade/clear_all_position/set_position 各分支。
//
// 关键量（L601-609）：isBuy 在 L530 由 **undone** 决定，而能走到这里说明 undone==0（L542 已拦），
// 所以恒为 false -> 永远走「卖」分支，且 L609 无条件 `curQty = min(vailyPos, _min_open_lots)`
// 覆盖了 L601/L607 的结果。因此 VWAP 分桶值与分片量无关（下面两个用例用不同的分布文件断言
// 出同样的成交量），并且减仓目标会发出买单。两条都是缺陷，见 bugs/R7_exefact.md。
//
// 时序：tail_secs 一律取 0 —— init 在 L192 计算 _fire_span 时 _total_secs 仍是 0（L198 才赋值），
// tail_secs=0 时 _fire_span=0，on_tick 非首笔分支的间隔判定恒真（同 BUG-R7-001）。
// 驱动顺序：init -> set_position -> on_tick -> on_channel_ready（do_calc L517 在 L547 的
// 空指针检查之前就解引用 _last_tick，没有 tick 会崩，见 BUG-R7-005）。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtStockVWapExeUnit.h"

using namespace wtp;

namespace
{
// stdCode 需要 3 段：init L201 与 get_minOrderQty 都用 split(stdCode,".")[2] 取代码号
const char* const kStk = "SHSE.STK.600000";      // 普通股（< 688000）
const char* const kKcb = "SHSE.STK.688001";      // 科创板（>= 688000）
const char* const kBond = "SHSE.CBOND.113001";   // 可转债

class SFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	// getPosition(code) -> 总仓；getPosition(code,false) -> 可用（昨仓）；
	// getPosition(code,true,1) -> 今仓多头（清仓对比用）
	double getPosition(const char*, bool bOnlyValid = true, int32_t strategy = 3) override
	{
		if (!bOnlyValid)
			return avail;
		if (strategy == 1)
			return todayPos;
		return pos;
	}
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
	uint64_t getCurTime() override { return 20240103093000000ull; }

	double pos = 0;
	double avail = 0;
	double todayPos = 0;
	double undone = 0;
	uint32_t nextId = 7000;
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

inline std::string sCommName()
{
	return "R7SVWAP" + std::to_string((long long)getpid());
}

class SVwapFile
{
public:
	explicit SVwapFile(double bucketVal, uint32_t rows = 241)
	{
		_path = std::string("Vwap_") + sCommName() + ".txt";
		std::ofstream f(_path.c_str());
		for (uint32_t i = 0; i < rows; i++)
		{
			if (i)
				f << ",";
			f << bucketVal;
		}
		f.close();
	}
	~SVwapFile() { std::remove(_path.c_str()); }

private:
	std::string _path;
};

// 第 5 个参数就是 product（get_minOrderQty 拿它和 "STK"/"CBOND" 比对）。
// 额外 retain() 一次是为了抵消 get_minOrderQty（WtStockVWapExeUnit.h:386）对
// ctx->getCommodityInfo() 借来指针的那次 release —— 否则引用计数净少 1，
// 单元析构就会把对象打回 0，用例收尾的 release 会踩已销毁对象。
WTSCommodityInfo* mkSComm(const char* product)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("sh6", sCommName().c_str(), "SHSE", "NIGHT_DAY", product);
	c->retain();
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Long);          // 股票只可做多 -> init L221 置 _is_t0
	return c;
}

WTSTickData* mkSTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240103;
	ts.trading_date = 20240103;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 1000;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 900;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// L178-189 读取的键（注意价格偏移的键名是 "offset"）
WTSVariant* mkSCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t offset = 0,
	double minOpenLots = 100.0, double lots = 1000.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 0)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", (uint32_t)930);
	cfg->append("end_time", (uint32_t)1500);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("offset", offset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

// 唯一安全顺序：init -> 落目标 -> 先给 tick -> 再就绪
void bootS(WtStockVWapExeUnit& unit, SFakeCtx& ctx, const char* code, double target,
	WTSVariant* cfg, uint32_t actTime = 100000000, double lastPx = 10.50,
	double bidPx = 10.49, double askPx = 10.51)
{
	unit.init(&ctx, code, cfg);
	cfg->release();
	unit.set_position(code, target);
	unit.on_tick(mkSTick(code, actTime, lastPx, bidPx, askPx));
	unit.on_channel_ready();
}
}

//////////////////////////////////////////////////////////////////////////
// 名字契约
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, names_and_fact_binding)
{
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtStockVWapExeUnit");   // L150
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");        // L141
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 普通股：_min_open_lots 被 min(cfg, _min_hands=100) 钳到 100，成交量 = min(可用, 100)
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, normal_stock_clamps_min_open_lots_down_to_100)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 500, mkSCfg(5, 0, 0, 200.0, 1000.0));
	// init: min_hands=100(非科创) -> _min_open_lots = min(200,100) = 100
	// do_calc: _is_t0 -> vailyPos=realPos=1000 -> curQty = min(1000,100) = 100
	// diffQty = 500-1000 = -500 < 0 -> 期望是卖出，却按 curQty>0 走了 buy（缺陷 BUG-R7-008）
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);      // price_mode=0 -> 最新价
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 分布文件里的分桶值完全不参与成交量：25 与 100000 得到同一笔数量
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, vwap_bucket_distribution_does_not_affect_fired_qty)
{
	SFakeCtx c1, c2;
	c1.comm = mkSComm("STK");
	c2.comm = mkSComm("STK");
	c1.pos = c2.pos = 1000;
	c1.avail = c2.avail = 1000;
	WtStockVWapExeUnit u1, u2;
	{
		SVwapFile vfA(25.0);
		bootS(u1, c1, kStk, 4000, mkSCfg());          // diffQty=3000，分桶 25
	}
	{
		SVwapFile vfB(100000.0);
		bootS(u2, c2, kStk, 4000, mkSCfg());          // 同一目标，分桶 10 万
	}
	// _Vwap_vol = aimQty - curPos 分别是 25-1000=-975 和 100000-1000=99000，
	// 但 L609 无条件把 curQty 覆写成 min(vailyPos,_min_open_lots)=100
	ASSERT_EQ(c1.buys.size(), 1u);
	ASSERT_EQ(c2.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(c1.buys[0].qty, 100.0);
	EXPECT_DOUBLE_EQ(c2.buys[0].qty, c1.buys[0].qty);   // 分片计划被丢弃（BUG-R7-007）
	c1.comm->release();
	c2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 科创板：_min_open_lots = max(cfg, 200)
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, kcb_stock_clamps_min_open_lots_up_to_200)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 5000;
	ctx.avail = 5000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kKcb, 6000, mkSCfg(5, 0, 0, 50.0, 1000.0));
	// split("SHSE.STK.688001",".")[2]="688001" >= 688000 -> _is_KC -> max(50, 200) = 200
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 200.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 可转债：get_minOrderQty 走 CBOND 分支 -> min_hands=10
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, convertible_bond_min_hands_is_10)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("CBOND");
	ctx.pos = 5000;
	ctx.avail = 5000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kBond, 6000, mkSCfg(5, 0, 0, 50.0, 1000.0));
	// CBOND -> min_hands=10 -> 非科创 -> _min_open_lots = min(50,10) = 10
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 空仓买入目标：可用仓 0 -> curQty=0 -> 落到 sell(qty=0) 分支
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, flat_position_buy_target_emits_zero_qty_sell)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 0;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// diffQty=1000>0，但 L530 的 isBuy 由 undone(=0) 决定 -> false -> L609 curQty=min(0,100)=0
	// L659 `curQty > 0` 为假 -> sell(code, px, 0)
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 0.0);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 碎股：可用仓小于最小手数时按可用量整单出（L601-603 的分支）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, odd_lot_uses_available_position_as_qty)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 40;
	ctx.avail = 40;
	WtStockVWapExeUnit unit;
	// 先落一个非 0 目标，再用 clear_all_position 把目标打到 0（set_position(0) 会被判等丢弃）
	unit.init(&ctx, kStk, mkSCfg(5, 0, 0, 100.0, 1000.0));
	unit.set_position(kStk, 400);
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.on_channel_ready();
	uint32_t before = (uint32_t)ctx.buys.size();
	unit.clear_all_position(kStk);
	// _target_pos=0 -> diffQty=-40；L521 的完成判定被 !(target==0 && real<min_hands && real>target)
	// 这一条排除掉（40<100 且 40>0），所以继续算；curQty=min(40,100)=40 -> buy 40
	ASSERT_EQ(ctx.buys.size(), before + 1u);
	EXPECT_DOUBLE_EQ(ctx.buys.back().qty, 40.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 完成判定：按整手取整后目标与实仓相等即置 _is_finish（L521-526）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, rounding_to_whole_lots_marks_finish_and_stops)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 950;
	ctx.avail = 950;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 1000, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// round_hands(1000,100)=(int)((1000+50)/100)*100=1000
	// round_hands(950,100)=(int)((950+50)/100)*100=1000 -> 相等 -> _is_finish
	EXPECT_TRUE(ctx.hasLog("target position"));
	EXPECT_TRUE(ctx.hasLog("set finish"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 持仓不足以卖出时目标被上抬（L501-508，非 T+0）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, insufficient_sellable_position_adjusts_target_up)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	WTSCommodityInfo* c = mkSComm("STK");
	c->setTradingMode(TM_Both);          // 不走 _is_t0 -> vailyPos 取 getPosition(code,false)
	ctx.comm = c;
	ctx.pos = 1000;
	ctx.avail = 200;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 0, mkSCfg(5, 0, 0, 100.0, 1000.0));
	// target_pos = max(realPos-vailyPos, _target_pos) = max(800, 0) = 800 != 0
	EXPECT_TRUE(ctx.hasLog("can sell hold pos not enough"));
	EXPECT_TRUE(ctx.hasLog("target adjust"));
	// curQty = min(vailyPos=200, _min_open_lots=100) = 100 -> buy 100（方向同样错位）
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 价格模式与偏移：mode1 在减仓时取卖一；"offset" 键（不是 "price_offset"）参与加价
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, price_mode_and_offset_key)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit u1, u2;
	bootS(u1, ctx, kStk, 500, mkSCfg(5, 1, 0, 100.0, 1000.0));
	// diffQty = 500-1000 = -500 -> L617 isBuy=false -> mode1 -> askprice(0)（L626）
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);
	bootS(u2, ctx, kStk, 500, mkSCfg(5, 1, 4, 100.0, 1000.0));
	// 非打板走 L635-638：targetPx = 10.51 + 0.01*4*(-1) = 10.47
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.47);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);        // min(vailyPos=1000, _min_open_lots=100)
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨停收敛：买入方向的委托价被压到涨停价且订单不可撤（L646-651）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, buy_price_capped_to_upper_limit)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 300, 100.0, 1000.0));
	unit.set_position(kStk, 5000);          // diffQty=4000>0 -> isBuy=true
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51, 10.60, 0));
	unit.on_channel_ready();
	// targetPx = 10.50 + 0.01*300 = 13.50 > 10.60 -> 收敛
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.60);
	EXPECT_TRUE(ctx.hasLog("modified to upper limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 跌停收敛（L652-657）：走卖分支（undone==0 -> isBuy=false）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, sell_price_floored_to_lower_limit)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg(5, 0, 300, 100.0, 1000.0));
	unit.set_position(kStk, 100);           // diffQty=-900 -> isBuy=false
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51, 0, 10.20));
	unit.on_channel_ready();
	// mode0 -> 10.50 - 0.01*300 = 7.50 < 10.20 -> 收敛到 10.20；curQty=min(1000,100)=100>0 -> buy
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.20);
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 零价回退（L641-642）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, zero_last_price_falls_back_to_preclose)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.set_position(kStk, 500);
	unit.on_tick(mkSTick(kStk, 100000000, 0.0, 0.0, 0.0));
	unit.on_channel_ready();
	// price()==0 -> 用 preclose()=10.0
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_channel_ready 三分支 + 撤单计数与重发（L322-345、L348-358、L361-366、L284-296）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, channel_ready_branches)
{
	SVwapFile vf(25.0);
	// 分支1：外部未完成单 -> 全撤并抬起 _cancel_cnt
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.undone = 300.0;
		ctx.cancelBydirRetCnt = 2;
		WtStockVWapExeUnit unit;
		unit.init(&ctx, kStk, mkSCfg());
		unit.set_position(kStk, 100);
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.hasLog("unmanaged orders of"));
		EXPECT_TRUE(ctx.hasLog("cancelcnt -> 2"));
		EXPECT_TRUE(ctx.hasLog("尚有未完成的撤单指令"));   // L512（随后的 do_calc）
		EXPECT_TRUE(ctx.buys.empty());
		ctx.comm->release();
	}
	// 分支2：undone=0 且 OMS 有单 -> 清本地错单
	{
		SFakeCtx ctx;
		ctx.comm = mkSComm("STK");
		ctx.pos = 1000;
		ctx.avail = 1000;
		WtStockVWapExeUnit unit;
		bootS(unit, ctx, kStk, 500, mkSCfg());
		ASSERT_EQ(ctx.buys.size(), 1u);
		unit.on_channel_ready();
		EXPECT_TRUE(ctx.hasLog("not confirmed in trading channel, clear all"));
		ctx.comm->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// on_order：撤单回报收敛后重发；成交回报复位 _cancel_times（L261-302）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, on_order_reports_and_refire)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 500, mkSCfg(5, 0));
	ASSERT_EQ(ctx.buys.size(), 1u);          // 本地单 7000，_this_target = 1000+100 = 1100
	unit.on_order(4242, kStk, true, 0, 10.50, true);       // 未知 id -> L261
	unit.on_order(7000, kStk, true, 0, 10.50, false);      // 全额成交
	EXPECT_TRUE(ctx.hasLog("has filled"));   // L280
	unit.on_order(7000, kStk, true, 5, 10.50, false);      // 已摘除 -> 忽略
	EXPECT_EQ(ctx.countLog("has filled"), 1u);
	unit.on_tick(mkSTick(kStk, 100100000, 10.60, 10.59, 10.61));  // 换 tick
	unit.on_order(7001, kStk, true, 5, 10.60, true);       // 撤单回报（_cancel_cnt==0）
	// _this_target=1100，realPos 仍是 1000 -> 不等 -> _cancel_times=1 -> fire_at_once(max(100,100))
	// fire_at_once: mode0 -> 10.60 + 0.01*1 = 10.61
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.61);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// on_trade 只写日志（L455-459）；on_entrust 拒单重算（L463-475）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, on_trade_logs_and_on_entrust_recalculates)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 500, mkSCfg());
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_trade(7000, kStk, true, 100, 10.50);
	EXPECT_TRUE(ctx.hasLog("Order 7000 of SHSE.STK.600000 traded: 100.00 @ 10.50"));   // L458
	unit.on_entrust(9999, kStk, false, "not mine");        // 非本单元 -> L468
	unit.on_entrust(7000, kStk, false, "rejected");        // erase -> do_calc -> 同一 tick 被拦
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("No tick of"));                 // L573
	unit.on_entrust(7000, kStk, true, "ok");
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// set_position 的三道防御（L766-778）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, set_position_guards)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.on_channel_ready();                               // diffQty=0 -> L515 return
	unit.on_tick(mkSTick(kStk, 100000000, 10.50, 10.49, 10.51));
	unit.set_position("other", 100);                       // 代码不符 -> L766
	unit.set_position(kStk, -5);                           // 负目标 -> L774
	EXPECT_TRUE(ctx.hasLog("is an error stock target position"));
	unit.set_position(kStk, 1000);
	unit.set_position(kStk, 1000);                         // 重复目标 -> L770
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);
	unit.on_channel_lost();                                // L811 空实现
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// clear_all_position 的合约过滤（L824-825）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, clear_all_position_filters_code)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	ctx.pos = 1000;
	ctx.avail = 1000;
	WtStockVWapExeUnit unit;
	bootS(unit, ctx, kStk, 500, mkSCfg());
	uint32_t n = (uint32_t)ctx.buys.size();
	unit.clear_all_position("SHSE.STK.000001");            // 非本合约 -> 直接 return
	EXPECT_EQ(ctx.buys.size(), n);
	unit.clear_all_position(kStk);                         // _target_pos=0 -> do_calc
	EXPECT_EQ(ctx.buys.size(), n + 1u);                    // 仍然错位发买（BUG-R7-008）
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（空指针解引用，不能启用）：通道先于首笔 tick 就绪
// do_calc L517 在 L547 的 _last_tick==NULL 检查之前就解引用 _last_tick->price()
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, DISABLED_channel_ready_before_first_tick_crashes)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	unit.init(&ctx, kStk, mkSCfg());
	unit.set_position(kStk, 1000);                         // 未就绪 -> L497 return
	unit.on_channel_ready();                               // 期望：L518 崩（BUG-R7-005）
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（越界/异常，不能启用）：标准股票代码只有 2 段
// init L201 与 get_minOrderQty 都取 split(stdCode,".")[2]
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkvw, DISABLED_two_segment_stock_code_out_of_range)
{
	SVwapFile vf(25.0);
	SFakeCtx ctx;
	ctx.comm = mkSComm("STK");
	WtStockVWapExeUnit unit;
	unit.init(&ctx, "SHSE.600000", mkSCfg());              // 期望：越界/stoi 异常（BUG-R7-009）
	unit.on_channel_ready();
	ctx.comm->release();
}
