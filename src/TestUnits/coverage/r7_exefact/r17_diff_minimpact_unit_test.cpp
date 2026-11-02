// R17 lane：WtDiffMinImpactExeUnit 差量最小冲击执行单元行为测试
// 目标文件: src/WtExeFact/WtDiffMinImpactExeUnit.cpp / .h（报表 missed 260+9 行）
//           顺带驱动 src/WtExeFact/WtOrdMon.h 内联 has_order/clear_orders（missed 11 行）
// 说明: 全部用例只引用 WtExeFact 符号；宿主替身沿用 n05 FakeExecCtx 形态（本文件自带一份，
//       匿名命名空间隔离），额外提供 onAfterBuy 钩子制造 do_calc 重入以覆盖 CalcFlag 占用分支。
//       超时扫描走 _ctx->getCurTime()（可注入时间），不读挂钟、不起线程；无身份令牌时只告警并保留监控。
//       明确订单状态回报后终态订单应退出监控；相关断言见本文件的订单回报用例。
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
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
template <typename T>
struct ReleaseWTSObject
{
	void operator()(T* obj) const
	{
		if (obj)
			obj->release();
	}
};

class FakeExecCtx : public ExecuteContext
{
public:
	FakeExecCtx()
	{
		sess = WTSSessionInfo::create("R7_TEST", "R7 test session");
		sess->addTradingSection(900, 1500);
	}
	~FakeExecCtx() override { if (sess) sess->release(); }

	struct Rec
	{
		std::string code;
		double price;
		double qty;
		bool force;
	};

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
		if (onAfterBuy)
			onAfterBuy();           // 制造 do_calc 重入（CalcFlag 占用分支）
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

	void writeLog(const char* message) override
	{
		if (throwOnLog)
			throw std::runtime_error("writeLog failure");
		logs.push_back(message ? message : "");
	}
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override
	{
		if (throwOnSessionLookup)
			throw std::runtime_error("getSessionInfo failure");
		return sess;
	}
	uint64_t getCurTime() override { return curTime; }

public:
	double pos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 7000;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	bool cancelByIdRet = true;
	uint32_t cancelBydirRetCnt = 1;
	bool throwOnSessionLookup = false;
	bool throwOnLog = false;
	std::function<void()> onAfterBuy;
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<uint32_t> byidCancels;
	std::vector<std::string> logs;
	OrderIDs lastIds;

	bool hasLog(const std::string& needle) const
	{
		for (const std::string& message : logs)
			if (message.find(needle) != std::string::npos)
				return true;
		return false;
	}
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

// 分月合约按三段式形（CodeHelper.hpp:601-616）；tick 内 code 与单元 _code 一致
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
	uint32_t span, bool byRate, double lots, double rate)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", offset);
	cfg->append("expire", expire);
	cfg->append("pricemode", priceMode);
	cfg->append("span", span);
	cfg->append("byrate", byRate);
	cfg->append("lots", lots);
	cfg->append("rate", rate);
	return cfg;
}

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

const char* kCode = "SHFE.rb.2405";

template <typename T>
void releaseAllReferences(T* obj)
{
	if (!obj)
		return;
	const uint32_t refs = obj->retainCount();
	for (uint32_t i = 0; i < refs; ++i)
		obj->release();
}
}

//////////////////////////////////////////////////////////////////////////
// 命名契约 + 目标差量守卫（异码/DBL_MAX/同值/清仓全不发单）
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, names_and_position_guards)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		EXPECT_STREQ(unit.getName(), "WtDiffMinImpactExeUnit");
		EXPECT_STREQ(unit.getFactName(), "WtExeFact");     // FACT_NAME 见 WtExeFact.cpp:21
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position("SHFE.rb.2406", 5);              // 异码过滤 L492-493
		unit.set_position(kCode, DBL_MAX);                 // 清仓不支持 L495-499
		unit.set_position(kCode, 0);                       // 0==初始 _left_diff，L503 直接跳过
		unit.clear_all_position(kCode);                    // 只写日志 L518-522
		unit.clear_all_position("SHFE.rb.2406");           // 异码过滤
		EXPECT_TRUE(h.ctx.buys.empty());
		EXPECT_TRUE(h.ctx.sells.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 差量买入：mode0 最新价；on_trade 按成交量回收 _left_diff
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, buy_by_lots_mode0_and_on_trade_updates_diff)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 5.0, 0.0);  // lots=5 才能让 on_trade 的差量回收可见
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);                      // 无 tick -> L346-350 早退
		EXPECT_TRUE(h.ctx.buys.empty());

		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);                                 // qty=min(5,5)=5, px=3505
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 5.0);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3505.0);
		uint32_t id1 = h.ctx.lastIds[0];

		unit.on_trade(id1, kCode, true, 2.0, 3505);       // _left_diff 5-2=3
		unit.on_trade(65535, kCode, true, 100.0, 3505);   // 非监控订单 -> L269-270 忽略
		WTSTickData* t2 = mkTick(kCode, 93001000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);                                 // qty=min(5,|3|)=3（若 diff 未被回收会是 5）
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].qty, 3.0);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3506.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 差量卖出：mode-1 最优价取卖一 ask；平仓修正 min(qty,|curPos|)
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, sell_mode_minus1_and_close_cap)
{
	Harness h;
	{
		h.ctx.pos = 4;
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, -1, 0, false, 5.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, -6);                     // diff=-6 -> 卖
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// L424-425 sellPx=askprice(0)=3510；offset=0 不改价
		// L370 qty=min(lots=5,|diff|=6)=5；L386-389 curPos=4>0 且卖出 -> min(5,|4|)=4
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3510.0);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].qty, 4.0);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode1 对手价：买=ask+offset，卖=bid-offset；两侧各只应用一次 -> BUG-R17-003
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, mode1_applies_offset_once_to_both_sides)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(2, 0, 1, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// 买：L434 buyPx=ask=3510 -> L448 +1*2 = 3512
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3512.0);

		h.ctx.pos = 8;
		unit.set_position(kCode, -8);                     // 卖 8
		WTSTickData* t2 = mkTick(kCode, 93001000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t2);
		// 卖：L435 取 bid=3500 -> L449 只扣一次 offset=2，得到 3498
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3498.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode1 offset=0 正向控制：保留 ask/bid 基准价，不发生额外偏移
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, mode1_zero_offset_keeps_opposite_top_prices)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 1, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 3);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3510.0);

		h.ctx.pos = 3;
		unit.set_position(kCode, -3);
		WTSTickData* t2 = mkTick(kCode, 93001000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3501.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode1 offset=-2 反向控制：买卖两侧仍只各偏移一次
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, mode1_negative_offset_is_applied_once_to_both_sides)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(-2, 0, 1, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 3);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3508.0);

		h.ctx.pos = 3;
		unit.set_position(kCode, -3);
		WTSTickData* t2 = mkTick(kCode, 93001000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3503.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 发单间隔 span 闸门：+600ms 的 tick 被 L394-395 拦，+1600ms 放行
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, entrust_span_blocks_then_fires)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 1000, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 8);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 2.0);

		WTSTickData* t2 = mkTick(kCode, 93000600, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);                                 // 600ms < span=1000 -> 不发单
		EXPECT_EQ(h.ctx.buys.size(), 1u);

		WTSTickData* t3 = mkTick(kCode, 93001600, 3507, 3502, 100, 3512, 50);
		unit.on_tick(t3);                                 // 1600ms >= span -> 放行
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3507.0);
		t1->release();
		t2->release();
		t3->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 同一笔 tick 重进：stale 闸门 L361-366 拦住第二笔
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, stale_tick_gate_blocks_recalc)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);

		unit.set_position(kCode, 8);                      // 差量变大但 tick 没更新 -> L361-366
		unit.on_tick(t1);                                 // 同 tick 重进，_last_tick_time 不变
		unit.on_tick(t1);
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// byrate：0.05*8=0.4 -> round=0 -> 抬回 1；卖出侧 0.05*100=5
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, byrate_floor_and_sell_side_qty)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, true, 100.0, 0.05);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);                      // 买：对手 askqty=8
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 8);
		unit.on_tick(t1);
		// L373-376: round(8*0.05)=0 -> <1 抬到 1 -> min(1,|5|)=1
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 1.0);

		unit.set_position(kCode, -5);                     // 卖：对手 bidqty=100
		WTSTickData* t2 = mkTick(kCode, 93001000, 3505, 3500, 100, 3510, 8);
		unit.on_tick(t2);
		// L373 bidqty=100*0.05=5 -> min(5,|diff|=5)=5（无成交回报，diff 仍是 -5）
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].qty, 5.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode2 自动价：买盘厚 -> 双侧取 ask；撤单一次后追单加 priceTick*_cancel_times
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, mode2_auto_price_and_cancel_times_premium)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 2, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// L394 mp=(100-50)/150>0 -> 双侧 askprice=3510；cancel_times=0 不加价
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3510.0);
		uint32_t id1 = h.ctx.lastIds[0];

		unit.on_order(id1, kCode, true, 3.0, 3510, true); // 撤单回报 -> _cancel_times=1
		WTSTickData* t2 = mkTick(kCode, 93001000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t2);
		// L417 buyPx=3510 + priceTick(1)*cancel_times(1) = 3511
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3511.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode2 三价全 0：双侧回落 preclose=3400（L411-415）
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, mode2_zero_prices_fall_back_to_preclose)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 2, 0, false, 3.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 3);
		WTSTickData* t1 = mkTick(kCode, 93000000, 0.0, 0.0, 100, 0.0, 100);
		unit.on_tick(t1);
		// mp=(100-100)/200=0 -> isUp=false -> 双侧 bidprice(0)=0 -> L411-415 取 preclose 3400
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3400.0);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 涨停修正后 isCanCancel=false：超时扫描跳过该单（L455-460 + WtOrdMon 不可撤分支）
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, upper_limit_buy_not_cancellable_by_expire_scan)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(5, 1, 1, 0, false, 3.0, 0.0); // offset=5 会越过涨停 3512
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50, 3512, 3400);
		unit.on_tick(t1);
		// 3510+5=3515 > 涨停 3512 -> 修正为 3512，订单标记不可撤
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3512.0);

		h.ctx.curTime += 2000;                            // 超过 expire=1s
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50, 3513, 3400);
		unit.on_tick(t2);
		// 不可撤 -> check_orders 跳过，_cancel_cnt 保持 0，重算继续发新单
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3513.0);    // 3511+5=3516 > 新涨停 3513 -> 修正为 3513
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// channel_ready 分支1：有未完成单但本地无监控 -> 按方向全撤 + cancel_cnt 挡住重算
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, channel_ready_cancels_unmanaged_and_blocks_calc)
{
	Harness h;
	{
		h.ctx.undone = 3;
		h.ctx.cancelBydirRetCnt = 2;
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.on_channel_ready();
		// L161-176: undone=+3 -> isBuy=true 撤买单，push 2 个 id，_cancel_cnt=2
		ASSERT_EQ(h.ctx.bydirCancels.size(), 1u);
		EXPECT_TRUE(h.ctx.bydirCancels[0].second);
		const OrderIDs ids = h.ctx.lastIds;
		ASSERT_EQ(ids.size(), 2u);

		unit.set_position(kCode, 5);                      // cancel_cnt!=0 -> L318-322 挡住
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		EXPECT_TRUE(h.ctx.buys.empty());

		h.ctx.undone = 0;                                 // 两笔撤单回报逐个消耗 cancel_cnt
		unit.on_order(ids[0], kCode, true, 3.0, 3500, true);
		EXPECT_TRUE(h.ctx.buys.empty());                   // 仍有一笔撤单待确认
		unit.on_order(ids[1], kCode, true, 3.0, 3500, true);
		ASSERT_EQ(h.ctx.buys.size(), 1u);                 // 最后一笔 ACK 到齐即用已有 tick 重算
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 2.0);
		h.ctx.undone = 2;                                  // fake 反映刚受理的 2 手订单仍在途
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);                                 // 在途订单存在时，新 tick 不重复下单
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// channel_ready 分支2：无未完成单但本地有监控 -> 清错单，超时扫描不再碰幽灵单
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, channel_ready_drops_unconfirmed_local_orders)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);                                 // 本地监控 id1（undone 恒 0 = 断线没送出去）
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		uint32_t ghost = h.ctx.lastIds[0];

		unit.on_channel_ready();                          // undone=0 且有监控单 -> L178-189 清掉

		h.ctx.curTime += 2000;                            // 幽灵单若还在监控会被超时撤
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		EXPECT_TRUE(h.ctx.byidCancels.empty());           // 已被 clear_orders 摘除
		// _left_diff 仍是 4，新 tick 放行后补发第 2 笔
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_NE(ghost, h.ctx.lastIds[0]);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// channel_ready 分支3：状态一致（有未完成单也有监控单）-> 只记日志，重算被 L337 挡
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, channel_ready_consistent_state_no_action)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);

		h.ctx.undone = 2;                                 // 与本地监控一致
		unit.on_channel_ready();                          // L190-194 -> do_calc -> L337-341 挡
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_TRUE(h.ctx.bydirCancels.empty());

		unit.on_entrust(h.ctx.lastIds[0], kCode, true, "ok");   // 成功回报不处理 L287
		unit.on_entrust(65535, kCode, false, "no such");        // 非监控单忽略 L290-291
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 委托失败回报：从监控器摘除、重算在新 tick 上补单
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, on_entrust_fail_erases_and_refires)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 6);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		uint32_t dead = h.ctx.lastIds[0];

		unit.on_entrust(dead, kCode, false, "rejected");  // L287-296 erase + do_calc(同tick被拦)
		h.ctx.curTime += 2000;
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		// 已摘除 -> 超时扫描不撤它；差量未成交 -> 补发 min(2,6)=2
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].qty, 2.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// CalcFlag 重入：buy 回调里再 set_position 触发嵌套 do_calc -> L312-316 占用分支
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, calc_flag_blocks_reentrant_calc)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit* unit = NULL;
		h.ctx.onAfterBuy = [&]() {
			unit->set_position(kCode, 7);                 // 重入：flag 占用 -> 只写日志返回
			unit->on_entrust(65535, kCode, false, "x");   // 非监控单，无副作用
		};
		WtDiffMinImpactExeUnit unitObj;
		unit = &unitObj;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 3.0, 0.0);
		unitObj.init(&h.ctx, kCode, cfg);
		cfg->release();

		unitObj.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unitObj.on_tick(t1);
		// 外层只发 1 笔；内层 do_calc 被 flag 拦截，没有第 2 笔
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
		h.ctx.onAfterBuy = NULL;
	}
	h.comm->release();
}

TEST(r17_diff_minimpact, failed_concurrent_calc_guard_does_not_release_owner)
{
	WtDiffMinImpactExeUnit unit;
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
	{
		WtDiffMinImpactExeUnit::CalcFlag owner(&unit._in_calc);
		ASSERT_FALSE(static_cast<bool>(owner));

		bool contenderSawBusy = false;
		std::thread contender([&]() {
			WtDiffMinImpactExeUnit::CalcFlag nested(&unit._in_calc);
			contenderSawBusy = static_cast<bool>(nested);
		});
		contender.join();

		EXPECT_TRUE(contenderSawBusy);
		EXPECT_TRUE(unit._in_calc.load(std::memory_order_acquire));
	}
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
}

//////////////////////////////////////////////////////////////////////////
// 旧 localid 无法验证时，timeout 只告警、不撤单并保留监控记录。
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, timeout_without_verified_identity_keeps_monitor_and_never_cancels)
{
	Harness h;
	std::unique_ptr<WTSCommodityInfo, ReleaseWTSObject<WTSCommodityInfo>> commOwner(h.comm);
	{
		WtDiffMinImpactExeUnit unit;
		std::unique_ptr<WTSVariant, ReleaseWTSObject<WTSVariant>> cfg(
			mkCfg(0, 1, 0, 0, false, 3.0, 0.0));  // expire=1s
		ASSERT_NE(nullptr, cfg.get());
		unit.init(&h.ctx, kCode, cfg.get());

		const uint32_t id = 7351;
		unit._orders_mon.push_order(&id, 1, h.ctx.curTime);
		std::unique_ptr<WTSTickData, ReleaseWTSObject<WTSTickData>> t1(
			mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50));
		unit.on_tick(t1.get());
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id));
		EXPECT_FALSE(h.ctx.hasLog("Expired order 7351"));

		h.ctx.curTime += 999;
		std::unique_ptr<WTSTickData, ReleaseWTSObject<WTSTickData>> t2(
			mkTick(kCode, 93001500, 3505, 3500, 100, 3510, 50));
		unit.on_tick(t2.get());
		EXPECT_FALSE(h.ctx.hasLog("Expired order 7351"));
		h.ctx.curTime += 1;
		std::unique_ptr<WTSTickData, ReleaseWTSObject<WTSTickData>> t3(
			mkTick(kCode, 93003000, 3505, 3500, 100, 3510, 50));
		unit.on_tick(t3.get());
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_EQ(h.ctx.buys.size(), 0u);
		EXPECT_EQ(h.ctx.sells.size(), 0u);
		EXPECT_EQ(std::count_if(h.ctx.logs.begin(), h.ctx.logs.end(), [](const std::string& line) {
			return line.find("Expired order 7351") != std::string::npos;
		}), 1);
		unit.on_channel_ready(); // account reconciliation may clear the monitor entry, but not the quarantine latch
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		unit.set_position(kCode, 1.0); // 目标更新不能解除 quarantine
		unit.do_calc();
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
		EXPECT_EQ(h.ctx.buys.size(), 0u);
		EXPECT_EQ(h.ctx.sells.size(), 0u);
	}
}

//////////////////////////////////////////////////////////////////////////
// pricemode 合法范围 -1..2 及非法值禁用：覆盖 PriceModeNames 越界修复
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, legal_price_modes_map_safely_and_invalid_modes_disable_execution)
{
	Harness h;
	const int32_t modes[] = { -1, 0, 1, 2 };
	const char* names[] = { "BESTPX", "LASTPX", "MARKET", "AUTOPX" };
	for (size_t i = 0; i < 4; ++i)
	{
		h.ctx.logs.clear();
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, modes[i], 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		EXPECT_TRUE(unit.initializationSucceeded()) << modes[i];
		EXPECT_TRUE(h.ctx.hasLog(std::string("order price: ") + names[i])) << modes[i];
		EXPECT_FALSE(h.ctx.hasLog("Invalid pricemode")) << modes[i];
	}

	const int32_t invalidModes[] = { -2, 3 };
	for (int32_t invalidMode : invalidModes)
	{
		h.ctx.logs.clear();
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, invalidMode, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded()) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("Invalid pricemode")) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("execution disabled")) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("order price: INVALID")) << invalidMode;
		if (invalidMode == 3)
		{
			h.ctx.undone = 2.0;
			unit.set_position(kCode, 5);
			WTSTickData* tick = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
			unit.on_channel_ready();
			unit.on_tick(tick);
			EXPECT_TRUE(h.ctx.buys.empty());
			EXPECT_TRUE(h.ctx.sells.empty());
			EXPECT_TRUE(h.ctx.bydirCancels.empty());
			EXPECT_TRUE(h.ctx.byidCancels.empty());
			tick->release();
		}
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 商品缺失时版本化初始化失败，后续行情和目标回调不发单
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, null_comm_info_fails_initialization_safely)
{
	Harness h;
	WTSCommodityInfo* comm = h.ctx.comm;
	h.ctx.comm = NULL;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(2, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.set_position(kCode, 4);
		unit.on_tick(NULL);
		unit.on_channel_ready();
		EXPECT_TRUE(h.ctx.buys.empty());
		EXPECT_TRUE(h.ctx.sells.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
	}
	h.ctx.comm = comm;
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 无交易时段不能被当作成功初始化并接受执行回调
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, missing_session_fails_initialization)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit invalidArguments;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		invalidArguments.init(NULL, kCode, cfg);
		EXPECT_FALSE(invalidArguments.initializationSucceeded());
		invalidArguments.init(&h.ctx, NULL, cfg);
		EXPECT_FALSE(invalidArguments.initializationSucceeded());
		invalidArguments.init(&h.ctx, "", cfg);
		EXPECT_FALSE(invalidArguments.initializationSucceeded());
		invalidArguments.init(&h.ctx, kCode, NULL);
		EXPECT_FALSE(invalidArguments.initializationSucceeded());
		cfg->release();
	}
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		WTSSessionInfo* sess = h.ctx.sess;
		h.ctx.sess = NULL;
		unit.init(&h.ctx, kCode, cfg);
		h.ctx.sess = sess;
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
		unit.set_position(kCode, 3);
		unit.on_channel_ready();
		unit.on_tick(NULL);
		EXPECT_TRUE(h.ctx.buys.empty());
		EXPECT_TRUE(h.ctx.sells.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 异常路径不能释放仍由上下文持有的借用 metadata。
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, metadata_references_stay_balanced_on_throw_and_success)
{
	Harness h;
	WTSCommodityInfo* comm = h.comm;
	comm->retain(); // 额外哨兵引用，回归时可在错误 release 后安全检查计数
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		h.ctx.throwOnSessionLookup = true;
		EXPECT_THROW(unit.init(&h.ctx, kCode, cfg), std::runtime_error);
		cfg->release();
	}
	EXPECT_EQ(comm->retainCount(), 2u); // fixture + sentinel
	releaseAllReferences(comm);

	Harness missing;
	comm = missing.comm;
	comm->retain();
	WTSSessionInfo* originalSession = missing.ctx.sess;
	missing.ctx.sess = NULL;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&missing.ctx, kCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
	}
	EXPECT_EQ(comm->retainCount(), 2u); // ordinary missing metadata also releases no borrowed pointer
	WTSSessionInfo* emptySession = WTSSessionInfo::create("R7_EMPTY", "empty session");
	emptySession->retain(); // sentinel，避免回归中的多余 release 令对象销毁后无法检查
	missing.ctx.sess = emptySession;
	missing.ctx.throwOnLog = true;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		EXPECT_THROW(unit.init(&missing.ctx, kCode, cfg), std::runtime_error);
		cfg->release();
	}
	missing.ctx.sess = originalSession;
	EXPECT_EQ(comm->retainCount(), 2u);
	EXPECT_EQ(emptySession->retainCount(), 2u);
	releaseAllReferences(comm);
	releaseAllReferences(emptySession);

	Harness logFailure;
	comm = logFailure.comm;
	comm->retain();
	WTSSessionInfo* logFailureSession = logFailure.ctx.sess;
	logFailureSession->retain();
	logFailure.ctx.throwOnLog = true;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		EXPECT_THROW(unit.init(&logFailure.ctx, kCode, cfg), std::runtime_error);
		EXPECT_EQ(comm->retainCount(), 2u);
		EXPECT_EQ(logFailureSession->retainCount(), 2u);
		cfg->release();
	}
	EXPECT_EQ(comm->retainCount(), 2u);
	EXPECT_EQ(logFailureSession->retainCount(), 2u);
	logFailure.ctx.sess = NULL;
	releaseAllReferences(comm);
	releaseAllReferences(logFailureSession);

	Harness success;
	comm = success.comm;
	WTSSessionInfo* sess = success.ctx.sess;
	EXPECT_EQ(comm->retainCount(), 1u);
	EXPECT_EQ(sess->retainCount(), 1u);
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&success.ctx, kCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		EXPECT_EQ(comm->retainCount(), 2u);
		EXPECT_EQ(sess->retainCount(), 2u);
	}
	EXPECT_EQ(comm->retainCount(), 1u);
	EXPECT_EQ(sess->retainCount(), 1u);
	comm->release();
}

TEST(r17_diff_minimpact, duplicate_init_preserves_success_and_execution)
{
	Harness h;
	WTSCommodityInfo* comm = h.comm;
	WTSSessionInfo* sess = h.ctx.sess;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		ASSERT_TRUE(unit.initializationSucceeded());
		ASSERT_EQ(comm->retainCount(), 2u);
		ASSERT_EQ(sess->retainCount(), 2u);

		WTSVariant* duplicateCfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, duplicateCfg);
		duplicateCfg->release();
		EXPECT_TRUE(unit.initializationSucceeded());
		EXPECT_EQ(comm->retainCount(), 2u);
		EXPECT_EQ(sess->retainCount(), 2u);

		unit.set_position(kCode, 3);
		WTSTickData* tick = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(tick);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 2.0);
		tick->release();
	}
	EXPECT_EQ(comm->retainCount(), 1u);
	EXPECT_EQ(sess->retainCount(), 1u);
	comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 终态回报保留迟到成交关联，但订单不再参加超时扫描；通道确认无挂单后清理。
//////////////////////////////////////////////////////////////////////////
TEST(r17_diff_minimpact, terminal_order_keeps_late_trade_association_without_expiry)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 0, 0, false, 4.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		const uint32_t canceledId = h.ctx.lastIds[0];

		unit.on_order(canceledId, kCode, true, 4.0, 3510, true);
		EXPECT_TRUE(unit._orders_mon.has_order(canceledId));
		unit.on_trade(canceledId, kCode, true, 1.0, 3510); // late partial fill remains associated
		EXPECT_DOUBLE_EQ(unit._left_diff, 3.0);

		h.ctx.curTime += 2000;
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		EXPECT_FALSE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(unit._orders_mon.has_order(canceledId));
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].qty, 3.0);

		const uint32_t nextId = h.ctx.lastIds[0];
		unit.on_entrust(nextId, kCode, false, "rejected");
		unit.on_channel_ready(); // no live orders: stale association can now be cleaned
		EXPECT_FALSE(unit._orders_mon.has_order(canceledId));
		t1->release();
		t2->release();
	}
	h.comm->release();
}

TEST(r17_diff_minimpact, filled_order_keeps_late_trade_until_reconciliation)
{
	Harness h;
	{
		WtDiffMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 0, 0, false, 4.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		const uint32_t filledId = h.ctx.lastIds[0];

		unit.on_order(filledId, kCode, true, 0.0, 3510, false);
		EXPECT_TRUE(unit._orders_mon.has_order(filledId));
		unit.on_trade(filledId, kCode, true, 4.0, 3510); // trade arrives after the full-fill order report
		EXPECT_DOUBLE_EQ(unit._left_diff, 0.0);

		h.ctx.curTime += 2000;
		WTSTickData* t2 = mkTick(kCode, 93002000, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);
		EXPECT_FALSE(unit._orders_mon.has_unverified_timeout());
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(filledId));

		unit.on_channel_ready();
		EXPECT_FALSE(unit._orders_mon.has_order(filledId));
		t1->release();
		t2->release();
	}
	h.comm->release();
}
