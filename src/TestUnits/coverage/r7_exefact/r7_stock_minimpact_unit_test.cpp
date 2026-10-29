// R7 lane：WtStockMinImpactExeUnit（股票/科创板 最小冲击执行单元）
// 目标文件: src/WtExeFact/WtStockMinImpactExeUnit.cpp / .h
// 覆盖面: init 的产品最小手数与 min_order 钳位（普通/科创/可转债）、档位与拆单量
//         （lots / byrate / 资金余额上限 / 碎股）、四种价格模式与零价回退、撤单次数加价、
//         下单间隔闸门、on_order/on_entrust/on_account/check_unmanager_order/on_tick 各分支。
//
// 注意本单元 getPosition 的取法与 WtStockVWapExeUnit 相反：
//   L524 curPos  = getPosition(stdCode, false)  // 注释写「总仓位」
//   L526 vailyPos = getPosition(stdCode, true)   // 注释写「可用/昨仓」
// 所以替身里 bOnlyValid==true 返回 availPos，false 返回 totalPos；常规用例把两者设成同值，
// 使 target_pos = max(curPos-vailyPos, _target_pos) 不产生额外修正。
//
// 时序：只显式喂 tick，_now 由 on_tick 内部取一次产品侧挂钟；span 配 0 时
// `(_now - _last_place_time < 0)` 恒假 -> do_calc 必然继续；span 配 60000 时第二次 do_calc
// 必然被 L587 拦下（同一笔 tick 内 _now 不变），两条都可判定。
// _expire_secs 一律置 0，避免走 check_orders 的超时撤单（那需要真实时间流逝，见缺陷清单）。
//
// 引用计数：替身创建的 tick 额外 retain 一次且用例侧不 release，避免与单元内部的
// release 组合出「二次释放」；comm 同理（get_minOrderQty 会多 release 一次借来的指针）。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExeFact/WtStockMinImpactExeUnit.h"

using namespace wtp;

namespace
{
const char* const kSmStk = "SHSE.STK.600000";    // 普通股 -> min_hands 100
const char* const kSmKcb = "SHSE.STK.688050";    // 科创板 -> min_hands 200
const char* const kSmBnd = "SHSE.CBOND.113050";  // 可转债 -> min_hands 10

class MFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool bOnlyValid = true, int32_t = 3) override
	{
		return bOnlyValid ? availPos : totalPos;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }
	OrderIDs buy(const char* c, double p, double q, bool f) override
	{
		buys.push_back({ c, p, q });
		lastForce = f;
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* c, double p, double q, bool f) override
	{
		sells.push_back({ c, p, q });
		lastForce = f;
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
	uint64_t getCurTime() override { return 20240104093000000ull; }

	double totalPos = 0;
	double availPos = 0;
	double undone = 0;
	uint32_t nextId = 6000;
	uint32_t cancelBydirRetCnt = 2;
	bool lastForce = false;
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

struct MCommCleanup
{
	MFakeCtx& ctx;
	~MCommCleanup() { if (ctx.comm) ctx.comm->release(); }
};

WTSCommodityInfo* mkMComm(const char* product)
{
	// create 参数顺序是 pid/product, name, exchange, session, trading template。
	// get_minOrderQty 按 getProduct() 区分 STK / CBOND，故产品代码必须位于第一个参数。
	WTSCommodityInfo* c = WTSCommodityInfo::create(product, "R7SMIN", "SHSE", "NIGHT_DAY", "CHN");
	c->retain();                     // 抵消 get_minOrderQty 的多余 release（见文件头）
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Stock);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Long);      // -> init L150 置 _is_t0
	return c;
}

WTSTickData* mkMTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double bidQty, double askPx, double askQty,
	double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240104;
	ts.trading_date = 20240104;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = bidQty;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = askQty;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	WTSTickData* t = WTSTickData::create(ts);
	t->retain();                     // 用例侧多持一份，全程不 release
	return t;
}

// L98-133 读取的键（pricemode: -1 最优 / 0 最新 / 1 对手 / 2 自动）
WTSVariant* mkMCfg(int32_t priceMode = 0, int32_t offset = 0, uint32_t span = 0,
	bool byRate = false, double lots = 500.0, double rate = 0.0,
	double minOrder = 100.0, uint32_t expire = 0)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", offset);
	cfg->append("expire", expire);
	cfg->append("pricemode", priceMode);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	cfg->append("min_order", minOrder);
	return cfg;
}

// init -> 落目标 -> 给 tick -> 通道就绪（就绪后 L217 立刻 do_calc）
void bootM(WtStockMinImpactExeUnit& unit, MFakeCtx& ctx, const char* code, double target,
	WTSVariant* cfg, uint32_t actTime = 100000000, double lastPx = 10.50,
	double bidPx = 10.49, double bidQty = 1000, double askPx = 10.51, double askQty = 800,
	double upper = 0, double lower = 0)
{
	unit.init(&ctx, code, cfg);
	cfg->release();
	unit.set_position(code, target);
	unit.on_tick(mkMTick(code, actTime, lastPx, bidPx, bidQty, askPx, askQty, upper, lower));
	unit.on_channel_ready();
}
}

//////////////////////////////////////////////////////////////////////////
// 名字契约
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, names_and_fact_binding)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	EXPECT_STREQ(unit.getName(), "WtStockMinImpactExeUnit");   // L70
	EXPECT_STREQ(unit.getFactName(), "WtExeFact");             // L61
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// init 的 min_order 钳位：普通股 min(cfg, 100)
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, init_clamps_min_order_for_normal_stock)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	// L145 走 min(200, _min_hands=100) -> 100
	bootM(unit, ctx, kSmStk, 0, mkMCfg(0, 0, 0, false, 500.0, 0.0, 200.0));
	EXPECT_TRUE(ctx.hasLog("min_order: 100.00"));               // L155 的 {:.2f}
	EXPECT_TRUE(ctx.hasLog("MiniImpactExecUnit"));
	MFakeCtx ctx2;
	ctx2.comm = mkMComm("STK");
	WtStockMinImpactExeUnit u2;
	bootM(u2, ctx2, kSmStk, 0, mkMCfg(0, 0, 0, false, 500.0, 0.0, 50.0));
	EXPECT_TRUE(ctx2.hasLog("min_order: 50.00"));               // min(50,100) = 50
	ctx.comm->release();
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// init 的 min_order 钳位：科创板 max(cfg, 200)、可转债 min(cfg, 10)
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, init_clamps_min_order_for_kcb_and_cbond)
{
	MFakeCtx ck, cb;
	ck.comm = mkMComm("STK");
	cb.comm = mkMComm("CBOND");
	WtStockMinImpactExeUnit uk, uc;
	bootM(uk, ck, kSmKcb, 0, mkMCfg(0, 0, 0, false, 500.0, 0.0, 50.0));
	EXPECT_TRUE(ck.hasLog("min_order: 200.00"));    // L140 max(50, _min_hands=200)
	bootM(uc, cb, kSmBnd, 0, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	EXPECT_TRUE(cb.hasLog("min_order: 10.00"));     // 可转债 min_hands=10 -> min(100,10)
	ck.comm->release();
	cb.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 通道未就绪：do_calc 只记 wait channel ready（L507-511）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, calc_waits_for_channel_ready)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg());
	unit.set_position(kSmStk, 3000);                // 无 tick -> L503 静默 return
	EXPECT_TRUE(ctx.buys.empty());
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	EXPECT_TRUE(ctx.hasLog("wait channel ready"));   // L509
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 按 lots 建仓：this_qty = min(lots, |diff|) 再取整到 min_order 的整数倍（L593-610）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, buy_by_lots_rounds_to_min_order_multiple)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 3000, mkMCfg(0, 0, 0, false, 550.0, 0.0, 100.0));
	// lots=550, diff=3000 -> this_qty=550 -> round_hands(550,100)=(int)(600/100)*100=600
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 600.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);      // pricemode=0(LASTPX) -> 最新价（L663-667）
	EXPECT_EQ(ctx.buys[0].code, kSmStk);
	EXPECT_TRUE(ctx.sells.empty());
	EXPECT_TRUE(ctx.hasLog("mon push buy order"));   // L707
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非人民币账户的资金回报被忽略 -> _avaliable 保持 0 -> 资金上限分支不参与（L247、L611）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, non_cny_account_update_is_ignored)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	unit.on_account("USD", 0, 0, 0, 2500.0, 0, 0, 0, 0, 0, 0);   // 非 CNY -> L247 忽略
	unit.set_position(kSmStk, 3000);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	EXPECT_FALSE(ctx.hasLog("avaliable update"));      // L249 未触发
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 500.0);          // _avaliable 仍为 0 -> 不受资金约束
	unit.on_account("CNY", 0, 0, 0, 2500.0, 0, 0, 0, 0, 0, 0);
	EXPECT_TRUE(ctx.hasLog("avaliable update"));       // 收到 CNY 资金更新；不依赖浮点日志格式
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 资金余额约束生效：2500/10.50 = 238.09 -> floor 到 200 股
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, cny_balance_limits_position_size)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	unit.on_account("CNY", 0, 0, 0, 2500.0, 0, 0, 0, 0, 0, 0);
	unit.set_position(kSmStk, 3000);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	// max_can_buy = (int)(238.095/100)*100 = 200 -> this_qty = min(200, round_hands(500,100)=500)
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 200.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 减仓：走卖分支，量 = min(可用, lots)（L620-634）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, sell_path_uses_available_and_rounds)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	ctx.totalPos = 1000;
	ctx.availPos = 1000;
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	unit.set_position(kSmStk, 800);                  // 先落一个非 0 目标（避免被判等丢弃）
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	unit.clear_all_position(kSmStk);                 // _target_pos=0 -> diffPos=-1000
	// 第一次(就绪时) diff = 800-1000 = -200 -> this_qty=min(500,200)=200 -> sell 200
	// 第二次(clear)   diff = 0-1000 = -1000 -> this_qty=min(500,1000)=500 -> min(1000,500)=500
	ASSERT_EQ(ctx.sells.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 200.0);
	EXPECT_DOUBLE_EQ(ctx.sells[1].qty, 500.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.50);
	EXPECT_TRUE(ctx.buys.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 碎股清仓：可用仓小于 min_order 时整单出（L624-626），且完成判定被排除（L546）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, odd_lot_sell_liquidates_remainder)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	ctx.totalPos = 40;
	ctx.availPos = 40;
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	unit.set_position(kSmStk, 140);                  // diff=100 -> 买 100
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	unit.clear_all_position(kSmStk);                 // target 0, curPos 40 < 100 -> 不判完成
	// diffPos=-40 -> this_qty=min(500,40)=40 -> vailyPos(40)<min_order(100) -> this_qty=40
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 40.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 完成判定：按整手取整后目标==持仓 -> _is_finish 并停止（L546-551）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, rounding_match_marks_finish)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	ctx.totalPos = 1030;
	ctx.availPos = 1030;
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 1000, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	// round_hands(1000,100)=(int)(1050/100)*100=1000；round_hands(1030,100)=(int)(1080/100)*100=1000
	EXPECT_TRUE(ctx.hasLog("set finish"));
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// byrate 档位：book = 对手量 * rate -> 取整 -> 再与 min_order 取大（L594-601）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, buy_by_rate_uses_counter_side_book)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 3000, mkMCfg(0, 0, 0, true, 500.0, 0.5, 100.0));
	// isBuy -> book = askqty(0)*0.5 = 800*0.5 = 400 -> round_hands(400,100)=400 -> max(100,400)=400
	// this_qty = min(400, 3000) = 400 -> round_hands(400,100) = 400
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 400.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// byrate 的取整下限：rate 极小 -> round 成 0 -> max(min_order, 0) 抬回 100
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, tiny_rate_is_raised_back_to_min_order)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 3000, mkMCfg(0, 0, 0, true, 500.0, 0.02, 100.0));
	// book = 800*0.02 = 16 -> round_hands(16,100)=(int)(66/100)*100 = 0 -> max(100,0) = 100
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 自动价模式（pricemode=2）：买卖失衡决定取 ask 还是 bid（L641-655）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, auto_price_mode_picks_side_by_book_imbalance)
{
	MFakeCtx up, dn;
	up.comm = mkMComm("STK");
	dn.comm = mkMComm("STK");
	WtStockMinImpactExeUnit u1, u2;
	// mp = (1000-800)/(1000+800) = 0.111 > 0 -> isUp -> askprice(0)
	bootM(u1, up, kSmStk, 3000, mkMCfg(2, 0, 0, false, 500.0, 0.0, 100.0));
	ASSERT_EQ(up.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(up.buys[0].price, 10.51);
	// mp = (300-900)/(300+900) = -0.5 < 0 -> !isUp -> bidprice(0)
	bootM(u2, dn, kSmStk, 3000, mkMCfg(2, 0, 0, false, 500.0, 0.0, 100.0),
		100000000, 10.50, 10.49, 300, 10.51, 900);
	ASSERT_EQ(dn.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(dn.buys[0].price, 10.49);
	up.comm->release();
	dn.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 最优价(-1) 与 对手价(1)+offset 的委托价（L658-662、L668-672）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, best_and_market_price_modes)
{
	MFakeCtx b, m;
	b.comm = mkMComm("STK");
	m.comm = mkMComm("STK");
	WtStockMinImpactExeUnit u1, u2;
	bootM(u1, b, kSmStk, 3000, mkMCfg(-1, 0, 0, false, 500.0, 0.0, 100.0));
	ASSERT_EQ(b.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(b.buys[0].price, 10.49);         // BESTPX 买入 -> bidprice(0)
	bootM(u2, m, kSmStk, 3000, mkMCfg(1, 3, 0, false, 500.0, 0.0, 100.0));
	ASSERT_EQ(m.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(m.buys[0].price, 10.54);         // MARKET -> ask + 0.01*3
	MFakeCtx s;
	s.comm = mkMComm("STK");
	s.totalPos = 1000;
	s.availPos = 1000;
	WtStockMinImpactExeUnit u3;
	bootM(u3, s, kSmStk, 200, mkMCfg(1, 3, 0, false, 500.0, 0.0, 100.0));
	// diffPos=-800 -> 卖 -> sellPx = bid - 0.01*3 = 10.46
	ASSERT_EQ(s.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(s.sells[0].price, 10.46);
	b.comm->release();
	m.comm->release();
	s.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 零价回退：最新价与盘口全 0 -> 用 preclose（L676-680）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, zero_prices_fall_back_to_preclose)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 3000, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0),
		100000000, 0.0, 0.0, 0, 0.0, 0);
	// LASTPX -> buyPx=sellPx=price()=0 -> L676/L679 都用 preclose()=10.0
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨跌停：买价压到涨停、卖价抬到跌停（L687-699）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, limit_prices_clamp_both_sides)
{
	MFakeCtx u, d;
	u.comm = mkMComm("STK");
	d.comm = mkMComm("STK");
	WtStockMinImpactExeUnit u1, u2;
	bootM(u1, u, kSmStk, 3000, mkMCfg(1, 300, 0, false, 500.0, 0.0, 100.0),
		100000000, 10.50, 10.49, 1000, 10.51, 800, 10.60, 0);
	// MARKET -> 10.51 + 0.01*300 = 13.51 > 10.60 -> 涨停
	ASSERT_EQ(u.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(u.buys[0].price, 10.60);
	EXPECT_TRUE(u.hasLog("modified to upper limit price"));
	d.totalPos = 1000;
	d.availPos = 1000;
	bootM(u2, d, kSmStk, 200, mkMCfg(1, 300, 0, false, 500.0, 0.0, 100.0),
		100000000, 10.50, 10.49, 1000, 10.51, 800, 0, 10.20);
	// 卖 -> sellPx = 10.49 - 3.00 = 7.49 < 10.20 -> 跌停；curQty=min(500,800)=500
	ASSERT_EQ(d.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(d.sells[0].price, 10.20);
	EXPECT_TRUE(d.hasLog("modified to lower limit price"));
	u.comm->release();
	d.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 下单间隔闸门：span=60000 时同一笔 tick 内第二次计算被拦（L587-591）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, entrust_span_blocks_second_calc)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(0, 0, 60000, false, 500.0, 0.0, 100.0));
	unit.set_position(kSmStk, 3000);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);                   // _last_place_time(0) -> 首笔放行
	unit.set_position(kSmStk, 5000);                  // _now 未变 -> 0 < 60000 -> 拦下
	EXPECT_EQ(ctx.buys.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("entrust span"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 旧 tick 不重算（L312-319 的 tick 时间闸门）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, stale_tick_is_filtered_in_on_tick)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg());
	unit.set_position(kSmStk, 3000);
	unit.on_tick(mkMTick(kSmStk, 100001000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.60, 10.59, 1000, 10.61, 800));   // 时间倒退
	EXPECT_TRUE(ctx.hasLog("No tick of"));
	EXPECT_EQ(ctx.countLog("mon push buy order"), 1u);
	unit.on_tick(NULL);
	unit.on_trade(6000, kSmStk, true, 100, 10.50);     // L381 空实现
	unit.on_channel_lost();                            // L225 空实现
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 撤单回报：摘单 + _cancel_times++ + 重算，加价一档（L184-204、L682-683）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, cancel_report_bumps_price_by_cancel_times)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	bootM(unit, ctx, kSmStk, 3000, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);
	unit.on_order(6000, kSmStk, true, 100, 10.50, true);
	EXPECT_TRUE(ctx.hasLog("6000 canceled, earse from mon"));      // L188
	EXPECT_TRUE(ctx.hasLog("recalc will be done"));                // L201
	// _cancel_times=1 -> buyPx += 0.01*1
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	unit.on_order(4242, kSmStk, true, 0, 10.50, false);            // L176-180 非管理单
	EXPECT_TRUE(ctx.hasLog("didnt in mon"));
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 成交回报复位 _cancel_times（L194-195）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, full_fill_report_resets_cancel_times)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	MCommCleanup commCleanup{ ctx };
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0);
	unit.init(&ctx, kSmStk, cfg);
	cfg->release();
	unit.set_position(kSmStk, 3000);
	WTSTickData* firstTick = mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800);
	unit.on_tick(firstTick);
	firstTick->release(); // 释放 create 与 mkMTick 持有的引用，执行器保留自己的引用
	firstTick->release();
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	ASSERT_DOUBLE_EQ(ctx.totalPos, 0.0);
	ASSERT_DOUBLE_EQ(ctx.availPos, 0.0);
	ASSERT_GT(ctx.buys[0].qty, 0.0);

	// 先撤掉首笔单，建立非零 _cancel_times；撤单回调会按 +1 tick 重算下单。
	unit.on_order(6000, kSmStk, true, ctx.buys[0].qty, ctx.buys[0].price, true);
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);

	// 第二笔单全成前，fake 仓位同步反映成交数量；目标仍有 3000 - qty 的剩余买量。
	ctx.totalPos = ctx.buys[1].qty;
	ctx.availPos = ctx.buys[1].qty;
	unit.on_order(6001, kSmStk, true, 0, ctx.buys[1].price, false);
	EXPECT_TRUE(ctx.hasLog("6001 done, earse from mon"));

	WTSTickData* nextTick = mkMTick(kSmStk, 100001000, 10.50, 10.49, 1000, 10.51, 800);
	unit.on_tick(nextTick);
	nextTick->release();
	nextTick->release();
	ASSERT_EQ(ctx.buys.size(), 3u);
	// 全成回报复位撤单计数；仓位同步后目标仍未完成，下一笔价格回到 tick 的 10.50。
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.50);
}

//////////////////////////////////////////////////////////////////////////
// 成功委托回报不应重复提交尚有未成交买量的订单。
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, successful_entrust_callback_does_not_duplicate_outstanding_buy)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	MCommCleanup commCleanup{ ctx };
	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0);
	unit.init(&ctx, kSmStk, cfg);
	cfg->release();
	unit.set_position(kSmStk, 3000);
	WTSTickData* tick = mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800);
	unit.on_tick(tick);
	tick->release();
	tick->release();
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	ASSERT_GT(ctx.buys[0].qty, 0.0);
	// 模拟 ExecMocker 提交首单后报告的未成交买量，回调重算时应等待该订单完成。
	ctx.undone = ctx.buys[0].qty;
	unit.on_entrust(6000, kSmStk, true, "accepted");
	EXPECT_EQ(ctx.buys.size(), 1u);
}

//////////////////////////////////////////////////////////////////////////
// 未知 ID 的失败委托回报应直接忽略（L395-398）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, unknown_failed_entrust_is_ignored)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	MCommCleanup commCleanup{ ctx };
	ASSERT_NE(ctx.comm, nullptr);
	ASSERT_TRUE(ctx.buys.empty());
	ASSERT_DOUBLE_EQ(ctx.totalPos, 0.0);
	ASSERT_DOUBLE_EQ(ctx.availPos, 0.0);
	ASSERT_DOUBLE_EQ(ctx.undone, 0.0);

	WtStockMinImpactExeUnit unit;
	WTSVariant* cfg = mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0);
	unit.init(&ctx, kSmStk, cfg);
	cfg->release();
	unit.set_position(kSmStk, 3000);
	WTSTickData* tick = mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800);
	unit.on_tick(tick);
	tick->release();
	tick->release();
	unit.on_channel_ready();
	ASSERT_EQ(ctx.buys.size(), 1u);
	ASSERT_EQ(ctx.buys[0].code, kSmStk);
	const uint32_t ordersBeforeUnknownFailure = static_cast<uint32_t>(ctx.buys.size());

	unit.on_entrust(9999, kSmStk, false, "not mine");

	EXPECT_FALSE(ctx.hasLog("entrust failed erase from mon"));
	EXPECT_EQ(ctx.buys.size(), ordersBeforeUnknownFailure);
}

//////////////////////////////////////////////////////////////////////////
// 反向未完成单先撤（L556-571）；同向未完成单等待（L574-578）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, opposite_and_same_direction_live_orders)
{
	MFakeCtx op, sm;
	op.comm = mkMComm("STK");
	sm.comm = mkMComm("STK");
	WtStockMinImpactExeUnit u1, u2;
	op.undone = -300.0;                                 // 场内有卖单，目标是买
	op.cancelBydirRetCnt = 1;
	bootM(u1, op, kSmStk, 3000, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	// on_channel_ready -> check_unmanager_order 先撤一轮
	ASSERT_GE(op.bydirCancels.size(), 1u);
	EXPECT_TRUE(op.hasLog("live opposite order of"));   // L567（do_calc 里再来一次）
	EXPECT_TRUE(op.buys.empty());
	sm.undone = 300.0;                                  // 同向
	bootM(u2, sm, kSmStk, 3000, mkMCfg(0, 0, 0, false, 500.0, 0.0, 100.0));
	EXPECT_TRUE(sm.hasLog("wait..."));                  // L576
	EXPECT_TRUE(sm.buys.empty());
	op.comm->release();
	sm.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// check_unmanager_order：可撤/不可撤两种配置（L267-279）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, unmanaged_orders_canceled_only_when_configured)
{
	MFakeCtx y, n;
	y.comm = mkMComm("STK");
	n.comm = mkMComm("STK");
	WTSVariant* cfgYes = mkMCfg();
	WTSVariant* cfgNo = mkMCfg();
	cfgNo->append("is_cancel_unmanaged_order", false);
	WtStockMinImpactExeUnit u1, u2;
	u1.init(&y, kSmStk, cfgYes);
	cfgYes->release();
	y.undone = 500.0;
	u1.on_channel_ready();
	EXPECT_TRUE(y.hasLog("Unmanaged live orders with qty"));   // L269
	ASSERT_EQ(y.bydirCancels.size(), 1u);
	EXPECT_TRUE(y.bydirCancels[0].second);                     // undone>0 -> 撤买单
	u2.init(&n, kSmStk, cfgNo);
	cfgNo->release();
	n.undone = 500.0;
	u2.on_channel_ready();
	EXPECT_FALSE(n.hasLog("Unmanaged live orders with qty"));
	EXPECT_TRUE(n.bydirCancels.empty());
	y.comm->release();
	n.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 非本合约的目标/清仓被过滤（L418-419、L475-476）；负目标被拒（L436-440）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, target_guards)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg());
	unit.set_position("SHSE.STK.000001", 3000);         // 代码不符
	unit.clear_all_position("SHSE.STK.000001");         // 代码不符
	unit.set_position(kSmStk, -1);                      // 负目标
	EXPECT_TRUE(ctx.hasLog("is an error stock target position"));
	unit.set_position(kSmStk, 0);                       // 与 curPos(0) 相等 -> L432 丢弃
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	EXPECT_TRUE(ctx.buys.empty());
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 清仓中不允许把目标打回 0（L423-427）
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, clearing_state_rejects_zero_target)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	ctx.totalPos = 1000;
	ctx.availPos = 1000;
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg());
	unit.set_position(kSmStk, 500);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	unit.clear_all_position(kSmStk);                    // _is_clear = true
	EXPECT_EQ(ctx.countLog("mon push sell order"), 2u); // 就绪时 500-... 一单 + 清仓一单
	unit.set_position(kSmStk, 0);                       // 清仓中打回 0 被拒
	EXPECT_TRUE(ctx.hasLog("position can not be set to 0"));   // L425
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（越界读，不能启用）：pricemode 超出 -1..2 时 L155 读 PriceModeNames[越界]
// PriceModeNames[_price_mode + 1]，pricemode=3 -> 下标 4
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, DISABLED_illegal_pricemode_reads_out_of_range_name_table)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, kSmStk, mkMCfg(3, 0, 0, false, 500.0, 0.0, 100.0));   // BUG-R7-010
	unit.set_position(kSmStk, 3000);
	unit.on_tick(mkMTick(kSmStk, 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷复现（越界/异常，不能启用）：标准股票代码只有 2 段
// init L122 取 StrUtil::split(stdCode,".")[2]
//////////////////////////////////////////////////////////////////////////
TEST(r7_stkmin, DISABLED_two_segment_stock_code_out_of_range)
{
	MFakeCtx ctx;
	ctx.comm = mkMComm("STK");
	WtStockMinImpactExeUnit unit;
	unit.init(&ctx, "SHSE.600000", mkMCfg());          // BUG-R7-009
	unit.set_position("SHSE.600000", 3000);
	unit.on_tick(mkMTick("SHSE.600000", 100000000, 10.50, 10.49, 1000, 10.51, 800));
	unit.on_channel_ready();
	ctx.comm->release();
}
