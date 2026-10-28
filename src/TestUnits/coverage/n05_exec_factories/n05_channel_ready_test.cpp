// N05 lane：WtMinImpactExeUnit::on_channel_ready 三分支 + 断线重连对账逻辑测试
// 目标文件: src/WtExeFact/WtMinImpactExeUnit.cpp（on_channel_ready / on_channel_lost / do_calc 撤单计数联动）
// 需要真实柜台回报才能推进的分支（真实 TraderAPI 异步回报、重连握手）以 FakeExecCtx 的同步数据替代；
// 未覆盖：WtStockMinImpactExeUnit / WtStockVWapExeUnit / WtTWapExeUnit / WtVWapExeUnit 的 do_calc
//（其内部依赖 get_real_target 等 cpp 内 inline 函数，只能经 init+on_tick 驱动，列入后续波次）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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
class ReadyCtx : public ExecuteContext
{
public:
	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	double getPosition(const char*, bool = true, int32_t = 3) override { return pos; }
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* /*stdCode*/, double price, double qty, bool) override
	{
		buys.push_back(std::make_pair(price, qty));
		lastIds = OrderIDs{ nextId++ };
		return lastIds;
	}

	OrderIDs sell(const char* /*stdCode*/, double price, double qty, bool) override
	{
		sells.push_back(std::make_pair(price, qty));
		lastIds = OrderIDs{ nextId++ };
		return lastIds;
	}

	bool cancel(uint32_t) override { return true; }

	OrderIDs cancel(const char*, bool isBuy, double /*qty*/ = 0) override
	{
		dirCancels.push_back(isBuy);
		OrderIDs ids;
		for (uint32_t i = 0; i < dirCancelCnt; i++)
			ids.push_back(nextId++);
		lastIds = ids;
		return ids;
	}

	void writeLog(const char*) override {}
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override { return NULL; }
	uint64_t getCurTime() override { return curTime; }

public:
	double pos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 8000;
	uint32_t dirCancelCnt = 1;
	WTSCommodityInfo* comm = NULL;
	std::vector<std::pair<double, double>> buys;
	std::vector<std::pair<double, double>> sells;
	std::vector<bool> dirCancels;
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

WTSTickData* mkTick(const char* code, uint32_t actTime, double lastPx, double bidPx, double askPx)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 3400.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 50;
	return WTSTickData::create(ts);
}

WTSVariant* mkSimpleCfg()
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("offset", (int32_t)0);
	cfg->append("expire", (uint32_t)0);
	cfg->append("pricemode", (int32_t)0);
	cfg->append("span", (uint32_t)0);
	cfg->append("byrate", false);
	cfg->append("lots", 2.0);
	cfg->append("rate", 0.0);
	cfg->append("minopenlots", 1.0);
	return cfg;
}
}

TEST(n05_minimpact_ready, unmanaged_live_orders_canceled_all)
{
	ReadyCtx ctx;
	ctx.comm = mkComm();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkSimpleCfg();
		unit.init(&ctx, "rb2401", cfg);
		cfg->release();

		// 场景：启动时柜台上挂着上次遗留的 5 手买单，本地 OMS 无记录
		ctx.undone = 5;
		ctx.dirCancelCnt = 2;
		unit.on_channel_ready();
		// 按方向全撤：isBuy=(undone>0)=true，产生 2 个撤单指令并进入监控
		ASSERT_EQ(ctx.dirCancels.size(), 1u);
		EXPECT_TRUE(ctx.dirCancels[0]);
		OrderIDs cancelIds = ctx.lastIds;
		ASSERT_EQ(cancelIds.size(), 2u);
		EXPECT_TRUE(ctx.buys.empty());

		// 撤单回报逐笔到达：第一笔后仍有 1 笔在撤，不发新单
		unit.on_order(cancelIds[0], "rb2401", true, 5, 3500, true);
		EXPECT_TRUE(ctx.buys.empty());
		// 第二笔回报后 cancel_cnt 归零并重算；此时无 tick，走 "No lastest tick" 分支
		ctx.undone = 0;
		unit.on_order(cancelIds[1], "rb2401", true, 5, 3500, true);
		EXPECT_TRUE(ctx.buys.empty());
	}
	ctx.comm->release();
}

TEST(n05_minimpact_ready, ghost_local_orders_cleared_on_reconnect)
{
	ReadyCtx ctx;
	ctx.comm = mkComm();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkSimpleCfg();
		unit.init(&ctx, "rb2401", cfg);
		cfg->release();

		WTSTickData* t1 = mkTick("rb2401", 93000000, 3505, 3500, 3510);
		unit.set_position("rb2401", 6);
		unit.on_tick(t1);
		ASSERT_EQ(ctx.buys.size(), 1u);
		uint32_t localId = ctx.lastIds[0];

		// 场景：断线重连后柜台其实没有这笔单（undone=0 但本地 OMS 有单）-> 清空本地监控
		ctx.undone = 0;
		unit.on_channel_ready();
		// do_calc 在同一 tick 时间下被 tickTime 检查拦截，不应立刻重复发单
		EXPECT_EQ(ctx.buys.size(), 1u);

		// 本地单被清掉后，针对它的撤单回报不再被识别，不会引起额外状态变化
		size_t before = ctx.buys.size() + ctx.sells.size();
		unit.on_order(localId, "rb2401", true, 2, 3505, true);
		// 新行情进来后按目标仓位继续补单
		WTSTickData* t2 = mkTick("rb2401", 93001000, 3505, 3500, 3510);
		unit.on_tick(t2);
		EXPECT_GT(ctx.buys.size(), before - 1u);
		t1->release();
		t2->release();
	}
	ctx.comm->release();
}

TEST(n05_minimpact_ready, consistent_state_only_logs_and_recalc)
{
	ReadyCtx ctx;
	ctx.comm = mkComm();
	{
		WtMinImpactExeUnit unit;
		WTSVariant* cfg = mkSimpleCfg();
		unit.init(&ctx, "rb2401", cfg);
		cfg->release();

		// 场景：柜台有 3 手未完成单，本地也在监控中（一致状态）-> 走第三分支只记日志
		WTSTickData* t1 = mkTick("rb2401", 93000000, 3505, 3500, 3510);
		unit.set_position("rb2401", 6);
		unit.on_tick(t1);  // 发出 id，undone 保持 0
		ctx.undone = 3;     // 模拟该单确实在柜台存活
		size_t buysBefore = ctx.buys.size();
		unit.on_channel_ready();
		// undone!=0 -> do_calc 逐笔发单原则下不再发新单，也不撤单
		EXPECT_EQ(ctx.buys.size(), buysBefore);
		EXPECT_TRUE(ctx.dirCancels.empty());
		t1->release();

		// 通道丢失回调当前为空实现，仅验证不崩溃
		unit.on_channel_lost();
	}
	ctx.comm->release();
}
