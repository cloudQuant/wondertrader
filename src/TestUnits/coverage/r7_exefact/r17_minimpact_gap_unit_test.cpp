// R17 lane：WtMinImpactExeUnit 残余未覆盖分支（全量报表 missed 41 行的可达子集）
// 目标文件: src/WtExeFact/WtMinImpactExeUnit.cpp / .h（CalcFlag 占用分支 L343-345）
// 前置: n05_minimpact_calc_test / n05_channel_ready_test 已覆盖主发单链与 on_channel_ready 三分支；
//       本文件只打残余：清仓兜底 L397-412、span 闸门 L393-395、stale tick L419-424、
//       minopenlots 抬高 L455-459、mode-1 L492-496、零价兜底 L512-516、
//       mode2 撤单加价 L487-488、CalcFlag 重入 L343-345。
// 手法: FakeExecCtx 的 getPosition 按 posType 分流（posType==1 返回多头仓），
//       使 net=0 而 long>0 时清仓兜底分支可达（n05 的常值 fake 到不了 L410-411）。
// 引用纪律: comm/tick 每处 create 与 release 一一对应；不读挂钟、不起线程。
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
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
#include "WtExeFact/WtMinImpactExeUnit.h"
#undef private

using namespace wtp;

namespace
{
class GapExecCtx : public ExecuteContext
{
public:
	GapExecCtx()
	{
		sess = WTSSessionInfo::create("R7_TEST", "R7 test session");
		sess->addTradingSection(900, 1500);
	}
	~GapExecCtx() override { if (sess) sess->release(); }

	struct Rec
	{
		std::string code;
		double price;
		double qty;
		bool force;
	};

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	// posType==1（多头可用）返回 longPos；其余（净仓查询）返回 pos
	double getPosition(const char*, bool = true, int32_t posType = 3) override
	{
		return (posType == 1) ? longPos : pos;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool bForceClose) override
	{
		buys.push_back({ stdCode, price, qty, bForceClose });
		OrderIDs ids{ nextId++ };
		lastIds = ids;
		if (onAfterBuy)
			onAfterBuy();                 // 制造 do_calc 重入
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
	double longPos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 8100;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	bool throwOnSessionLookup = false;
	bool throwOnLog = false;
	bool cancelByIdRet = true;
	uint32_t cancelBydirRetCnt = 1;
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

WTSTickData* mkTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double bidQty, double askPx, double askQty)
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
	ts.upper_limit = 0;
	ts.lower_limit = 0;
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

struct Harness
{
	GapExecCtx ctx;
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
// pricemode 合法范围 -1..2 及非法值禁用：修复 PriceModeNames 越界索引
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, legal_price_modes_keep_names_and_invalid_modes_disable_execution)
{
	Harness h;
	const int32_t modes[] = { -1, 0, 1, 2 };
	const char* names[] = { "BESTPX", "LASTPX", "MARKET", "AUTOPX" };
	for (size_t i = 0; i < 4; ++i)
	{
		h.ctx.logs.clear();
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, modes[i], 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		EXPECT_TRUE(unit.initializationSucceeded()) << modes[i];
		EXPECT_TRUE(h.ctx.hasLog(std::string("order price @ ") + names[i])) << modes[i];
		EXPECT_FALSE(h.ctx.hasLog("Invalid pricemode")) << modes[i];
	}

	const int32_t invalidModes[] = { -2, 3 };
	for (int32_t invalidMode : invalidModes)
	{
		h.ctx.logs.clear();
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, invalidMode, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded()) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("Invalid pricemode")) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("execution disabled")) << invalidMode;
		EXPECT_TRUE(h.ctx.hasLog("order price @ INVALID")) << invalidMode;
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
// 清仓兜底 L397-412：净仓=0 但多头=3 -> newVol=-min(3,lots=2) -> 强制平卖 2
// （n05 常值 getPosition 让该分支不可达；这里按 posType 分流才可达）
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, clear_branch_forced_sell_when_net_flat_long_exists)
{
	Harness h;
	h.ctx.pos = 0;        // 净仓 0 -> eq(curPos, newVol=0) 成立
	h.ctx.longPos = 3;    // getPosition(code,true,1) -> 3
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);                                   // 目标 0：eq -> 非清仓 -> L400-401 return
		EXPECT_TRUE(h.ctx.sells.empty());

		unit.clear_all_position(kCode);                     // L405-411 可达
		// newVol = -min(lPos=3, lots=2) = -2 -> 卖 2，bForceClose=true，mode0 取最新价 3505
		ASSERT_EQ(h.ctx.sells.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].qty, 2.0);
		EXPECT_DOUBLE_EQ(h.ctx.sells[0].price, 3505.0);
		EXPECT_TRUE(h.ctx.sells[0].force);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// span 闸门 L393-395：+600ms 的 tick 不发单，+1600ms 放行
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, entrust_span_blocks_second_order)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 1000, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 8);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 2.0);

		WTSTickData* t2 = mkTick(kCode, 93000600, 3506, 3501, 100, 3511, 50);
		unit.on_tick(t2);                                   // now-last=600 < 1000 -> L394-395
		EXPECT_EQ(h.ctx.buys.size(), 1u);

		WTSTickData* t3 = mkTick(kCode, 93001600, 3507, 3502, 100, 3512, 50);
		unit.on_tick(t3);                                   // 1600 >= 1000 -> 放行
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3507.0);
		t1->release();
		t2->release();
		t3->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// stale tick 闸门 L419-424：同一笔 tick 上加大目标不触发第二笔
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, stale_tick_gate_blocks_recalc)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 3);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		ASSERT_EQ(h.ctx.buys.size(), 1u);

		unit.set_position(kCode, 9);                        // curTickTime == _last_tick_time -> 拦
		unit.on_tick(t1);                                   // 同 tick 重进一次（释放/保留配对）
		unit.on_tick(t1);
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// minopenlots 抬高 L455-459：开仓量 3 < 5 -> 抬到 5（超出目标差量是刻意的）
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, open_lots_raised_to_min_open_lots)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 10.0, 0.0, 5.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 3);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// min(lots=10, |3-0|)=3 -> isOpen 且 3 < 5 -> this_qty=5
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 5.0);
		t1->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// mode-1 最优价 L492-496 + 零价兜底 L512-516（preclose=3400）
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, mode_minus1_bid_and_zero_price_fallback)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, -1, 0, false, 2.0, 0.0);
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 4);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// mode-1 买入取 bid=3500（对手价才是 ask），offset=0
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3500.0);

		unit.set_position(kCode, 8);
		WTSTickData* t2 = mkTick(kCode, 93001000, 0.0, 0.0, 100, 0.0, 50);
		unit.on_tick(t2);
		// buyPx=bid=0 -> L512-513：price==0 -> preclose=3400
		ASSERT_EQ(h.ctx.buys.size(), 2u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[1].price, 3400.0);
		t1->release();
		t2->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 超时订单身份未验证时 fail-closed：不调用裸 localid 自动撤单并保留监控项。
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, timeout_without_verified_identity_keeps_monitor_and_never_cancels)
{
	Harness h;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 1, 2, 0, false, 3.0, 0.0); // expire=1s
		unit.init(&h.ctx, kCode, cfg);
		cfg->release();

		unit.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t1);
		// mp>0 -> 双侧 ask=3510，cancel_times=0
		ASSERT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].price, 3510.0);
		uint32_t id1 = h.ctx.lastIds[0];
		h.ctx.undone = h.ctx.buys[0].qty;                 // fake 回填活单未完成量

		h.ctx.curTime += 1500;
		WTSTickData* t2 = mkTick(kCode, 93001500, 3505, 3500, 100, 3510, 50);
		unit.on_tick(t2);                                   // 超时只告警，不调用旧裸 ID 撤单
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id1));
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_TRUE(h.ctx.hasLog("legacy localid is unverified; order remains monitored"));

		h.ctx.curTime += 1000;
		WTSTickData* t3 = mkTick(kCode, 93003000, 3505, 3500, 100, 3510, 50);
		unit.on_channel_ready();                           // 重连不清除单元生命周期隔离
		unit.set_position(kCode, 6.0);                  // 目标更新也不得解除隔离或重新发单
		unit.on_tick(t3);                                   // 记录仍在 monitor 中，下一 tick 再次被检查
		EXPECT_TRUE(h.ctx.byidCancels.empty());
		EXPECT_TRUE(h.ctx.bydirCancels.empty());
		EXPECT_TRUE(unit._orders_mon.has_order(id1));
		EXPECT_TRUE(unit._orders_mon.has_unverified_timeout());
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		t1->release();
		t2->release();
		t3->release();
	}
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// CalcFlag 占用分支 L343-345：buy 回调内重入 set_position，外层只成一笔
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, calc_flag_blocks_reentrant_calc)
{
	Harness h;
	{
		WtMinImpactExeUnit* unit = NULL;
		h.ctx.onAfterBuy = [&]() {
			unit->set_position(kCode, 20);                  // _in_calc 已置位 -> 内层直接 return
			unit->on_entrust(65535, kCode, false, "x");     // 非监控单 -> 无副作用
		};
		WtMinImpactExeUnit unitObj;
		unit = &unitObj;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unitObj.init(&h.ctx, kCode, cfg);
		cfg->release();

		unitObj.set_position(kCode, 5);
		WTSTickData* t1 = mkTick(kCode, 93000000, 3505, 3500, 100, 3510, 50);
		unitObj.on_tick(t1);
		// 重入被 flag 拦截 + 内层 set_position 触发的那次 do_calc 也不会补单
		EXPECT_EQ(h.ctx.buys.size(), 1u);
		EXPECT_DOUBLE_EQ(h.ctx.buys[0].qty, 2.0);           // 外层用的是重入前的差量 min(2,5)=2
		t1->release();
		h.ctx.onAfterBuy = NULL;
	}
	h.comm->release();
}

TEST(r17_mimp_gap, failed_concurrent_calc_guard_does_not_release_owner)
{
	WtMinImpactExeUnit unit;
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
	{
		WtMinImpactExeUnit::CalcFlag owner(&unit._in_calc);
		ASSERT_FALSE(static_cast<bool>(owner));

		bool contenderSawBusy = false;
		std::thread contender([&]() {
			WtMinImpactExeUnit::CalcFlag nested(&unit._in_calc);
			contenderSawBusy = static_cast<bool>(nested);
		});
		contender.join();

		EXPECT_TRUE(contenderSawBusy);
		EXPECT_TRUE(unit._in_calc.load(std::memory_order_acquire));
	}
	EXPECT_FALSE(unit._in_calc.load(std::memory_order_acquire));
}

//////////////////////////////////////////////////////////////////////////
// V2 初始化状态：缺少商品或 session 时报告失败并保持无副作用
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, initialization_status_requires_runtime_resources)
{
	Harness h;
	{
		WtMinImpactExeUnit invalidArguments;
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
		WtMinImpactExeUnit missingCommodity;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		WTSCommodityInfo* comm = h.ctx.comm;
		h.ctx.comm = NULL;
		missingCommodity.init(&h.ctx, kCode, cfg);
		h.ctx.comm = comm;
		cfg->release();
		EXPECT_FALSE(missingCommodity.initializationSucceeded());
		missingCommodity.set_position(kCode, 5);
		missingCommodity.on_channel_ready();
		missingCommodity.on_tick(NULL);
	}
	{
		WtMinImpactExeUnit missingSession;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		WTSSessionInfo* sess = h.ctx.sess;
		h.ctx.sess = NULL;
		missingSession.init(&h.ctx, kCode, cfg);
		h.ctx.sess = sess;
		cfg->release();
		EXPECT_FALSE(missingSession.initializationSucceeded());
		missingSession.set_position(kCode, 5);
		missingSession.on_channel_ready();
		missingSession.on_tick(NULL);
	}
	EXPECT_TRUE(h.ctx.buys.empty());
	EXPECT_TRUE(h.ctx.sells.empty());
	EXPECT_TRUE(h.ctx.bydirCancels.empty());
	EXPECT_TRUE(h.ctx.byidCancels.empty());
	h.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// 异常路径不能释放仍由上下文持有的借用 metadata。
//////////////////////////////////////////////////////////////////////////
TEST(r17_mimp_gap, metadata_references_stay_balanced_on_throw_and_success)
{
	Harness h;
	WTSCommodityInfo* comm = h.comm;
	comm->retain();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		h.ctx.throwOnSessionLookup = true;
		EXPECT_THROW(unit.init(&h.ctx, kCode, cfg), std::runtime_error);
		cfg->release();
	}
	EXPECT_EQ(comm->retainCount(), 2u);
	releaseAllReferences(comm);

	Harness missing;
	comm = missing.comm;
	comm->retain();
	WTSSessionInfo* originalSession = missing.ctx.sess;
	missing.ctx.sess = NULL;
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkCfg(0, 0, 0, 0, false, 2.0, 0.0);
		unit.init(&missing.ctx, kCode, cfg);
		cfg->release();
		EXPECT_FALSE(unit.initializationSucceeded());
	}
	EXPECT_EQ(comm->retainCount(), 2u);
	WTSSessionInfo* emptySession = WTSSessionInfo::create("R7_EMPTY", "empty session");
	emptySession->retain();
	missing.ctx.sess = emptySession;
	missing.ctx.throwOnLog = true;
	{
		WtMinImpactExeUnit unit;
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
		WtMinImpactExeUnit unit;
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
		WtMinImpactExeUnit unit;
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

TEST(r17_mimp_gap, duplicate_init_preserves_success_and_execution)
{
	Harness h;
	WTSCommodityInfo* comm = h.comm;
	WTSSessionInfo* sess = h.ctx.sess;
	{
		WtMinImpactExeUnit unit;
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
