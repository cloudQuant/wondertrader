/**
 * l06_match_engine_test.cpp
 * 覆盖 src/WtBtCore/MatchEngine.{h,cpp}:
 * init / regisSink / clear / handle_tick / buy / sell / cancel(两种重载),
 * 以及经由 handle_tick 间接驱动的 fire_orders / match_orders / update_lob / grab_last_tick.
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <WtBtCore/MatchEngine.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

class RecordingMatchSink : public IMatchSink
{
public:
	virtual ~RecordingMatchSink() {}

	struct TradeRec
	{
		uint32_t localid;
		std::string code;
		bool isBuy;
		double vol;
		double fireprice;
		double price;
	};
	struct OrderRec
	{
		uint32_t localid;
		std::string code;
		bool isBuy;
		double leftover;
		double price;
		bool isCanceled;
	};
	struct EntrustRec
	{
		uint32_t localid;
		std::string code;
		bool bSuccess;
	};

	void handle_trade(uint32_t localid, const char* stdCode, bool isBuy,
		double vol, double fireprice, double price, uint64_t /*ordTime*/) override
	{
		trades.push_back({ localid, stdCode, isBuy, vol, fireprice, price });
	}

	void handle_order(uint32_t localid, const char* stdCode, bool isBuy,
		double leftover, double price, bool isCanceled, uint64_t /*ordTime*/) override
	{
		orders.push_back({ localid, stdCode, isBuy, leftover, price, isCanceled });
	}

	void handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess,
		const char* /*message*/, uint64_t /*ordTime*/) override
	{
		entrusts.push_back({ localid, stdCode, bSuccess });
	}

	std::vector<TradeRec> trades;
	std::vector<OrderRec> orders;
	std::vector<EntrustRec> entrusts;
};

class MatchEngineTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		sink = new RecordingMatchSink();
		engine.regisSink(sink);
	}

	void TearDown() override
	{
		engine.clear();
		delete sink;
		sink = nullptr;
	}

	static WTSTickData* makeTick(const char* code, double price, double vol,
		double bidpx, double bidqty, double askpx, double askqty)
	{
		WTSTickStruct ts;
		strcpy(ts.exchg, "SSE");
		strcpy(ts.code, code);
		ts.trading_date = 20240102;
		ts.action_date = 20240102;
		ts.action_time = 93000000;
		ts.price = price;
		ts.open = price - 0.05;
		ts.high = price + 0.1;
		ts.low = price - 0.1;
		ts.total_volume = vol;
		ts.volume = vol;
		ts.pre_close = price - 0.02;
		ts.upper_limit = price * 1.1;
		ts.lower_limit = price * 0.9;
		if (bidpx > 0.0)
		{
			ts.bid_prices[0] = bidpx;
			ts.bid_qty[0] = bidqty;
		}
		if (askpx > 0.0)
		{
			ts.ask_prices[0] = askpx;
			ts.ask_qty[0] = askqty;
		}
		return WTSTickData::create(ts);
	}

	void feedTick(const char* code, WTSTickData* tick)
	{
		engine.handle_tick(code, tick);
		tick->release();
	}

	static void setCancelRate(MatchEngine& eng, double rate)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("cancelrate", rate);
		eng.init(cfg);
		cfg->release();
	}

	MatchEngine engine;
	RecordingMatchSink* sink;
};

TEST_F(MatchEngineTest, initIgnoresNullConfig)
{
	engine.init(NULL);
	// 未注入任何 tick 时, 撮合入口不应崩溃
	engine.handle_tick("SSE.STK.600000", NULL);
	EXPECT_TRUE(sink->trades.empty());
}

TEST_F(MatchEngineTest, buySellWithoutCachedTickReturnEmpty)
{
	OrderIDs buyIds = engine.buy("SSE.STK.600000", 10.0, 100, 20240102093000000ULL);
	OrderIDs sellIds = engine.sell("SSE.STK.600000", 10.1, 100, 20240102093000000ULL);
	EXPECT_TRUE(buyIds.empty());
	EXPECT_TRUE(sellIds.empty());
}

TEST_F(MatchEngineTest, passiveBuyQueuesBehindBookAndFillsInTwoSteps)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600000";

	WTSTickData* t1 = makeTick(code, 10.0, 1000, 10.0, 500, 10.1, 600);
	feedTick(code, t1);

	// 挂买一价: 排队量取买一/卖一加权的近似挂单深度, 撤单率为 0 时不扣减
	OrderIDs ids = engine.buy(code, 10.0, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	uint32_t lid = ids[0];

	// tick2: 成交量 200 <= 排队位置 500, 只消耗排队, 不成交
	WTSTickData* t2 = makeTick(code, 10.0, 200, 10.0, 500, 10.1, 600);
	feedTick(code, t2);
	EXPECT_TRUE(sink->trades.empty());
	// fire_orders 激活时发出一次 entrust + 一次 order
	EXPECT_EQ(1u, sink->entrusts.size());
	EXPECT_EQ(1u, sink->orders.size());
	EXPECT_EQ(100.0, sink->orders[0].leftover);
	EXPECT_FALSE(sink->orders[0].isCanceled);

	// tick3: 成交量 600 > 剩余排队 200, 成交 100 全部
	WTSTickData* t3 = makeTick(code, 10.0, 600, 10.0, 500, 10.1, 600);
	feedTick(code, t3);
	ASSERT_EQ(1u, sink->trades.size());
	EXPECT_EQ(lid, sink->trades[0].localid);
	EXPECT_TRUE(sink->trades[0].isBuy);
	EXPECT_DOUBLE_EQ(100.0, sink->trades[0].vol);
	EXPECT_DOUBLE_EQ(10.0, sink->trades[0].price);
	ASSERT_EQ(2u, sink->orders.size());
	EXPECT_DOUBLE_EQ(0.0, sink->orders[1].leftover);

	// 已全部成交的订单被删除, 后续 tick 不再触发回调
	size_t tradeCnt = sink->trades.size();
	WTSTickData* t4 = makeTick(code, 10.0, 700, 10.0, 500, 10.1, 600);
	feedTick(code, t4);
	EXPECT_EQ(tradeCnt, sink->trades.size());
}

TEST_F(MatchEngineTest, passiveBuyPartialFillThenBetterPriceFill)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600001";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.1, 100);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.0, 50, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	WTSTickData* t2 = makeTick(code, 10.0, 60, 10.0, 100, 10.1, 100);
	feedTick(code, t2);	// 60 <= 排队 100, 不成交
	EXPECT_TRUE(sink->trades.empty());

	WTSTickData* t3 = makeTick(code, 10.0, 150, 10.0, 100, 10.1, 100);
	feedTick(code, t3);	// 越过剩余排队 40 → 可成交量 110, 与剩余 50 取小 → 全部成交
	ASSERT_EQ(1u, sink->trades.size());
	EXPECT_DOUBLE_EQ(50.0, sink->trades[0].vol);
	EXPECT_DOUBLE_EQ(10.0, sink->trades[0].price);
	const size_t orderCnt = sink->orders.size();
	// 全部成交后订单被删除, 后续 tick 不再产生任何回调
	WTSTickData* t4 = makeTick(code, 9.99, 30, 9.99, 100, 10.09, 100);
	feedTick(code, t4);
	EXPECT_EQ(1u, sink->trades.size());
	EXPECT_EQ(orderCnt, sink->orders.size());
}

TEST_F(MatchEngineTest, positiveBuyMatchesAtAskPrice)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600002";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.05, 300);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.1, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	WTSTickData* t2 = makeTick(code, 10.0, 300, 10.0, 100, 10.05, 300);
	feedTick(code, t2);
	ASSERT_EQ(1u, sink->trades.size());
	EXPECT_DOUBLE_EQ(100.0, sink->trades[0].vol);
	EXPECT_DOUBLE_EQ(10.05, sink->trades[0].price);	// 主动按对手价成交
	EXPECT_DOUBLE_EQ(10.0, sink->trades[0].fireprice);	// fireprice 为下单时的最新价
}

TEST_F(MatchEngineTest, passiveSellQueueThenSweep)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600003";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.1, 200);
	feedTick(code, t1);

	OrderIDs ids = engine.sell(code, 10.1, 80, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	// 卖价 10.1 与最新价 10.0 不等 → 排队量取卖一 200
	WTSTickData* t2 = makeTick(code, 10.1, 150, 10.0, 100, 10.1, 200);
	feedTick(code, t2);	// 150 <= 200, 只消耗排队
	EXPECT_TRUE(sink->trades.empty());

	WTSTickData* t3 = makeTick(code, 10.1, 90, 10.0, 100, 10.1, 200);
	feedTick(code, t3);	// 90 > 剩余排队 50 → 可成交 40, 部分成交
	ASSERT_EQ(1u, sink->trades.size());
	EXPECT_FALSE(sink->trades[0].isBuy);
	EXPECT_DOUBLE_EQ(40.0, sink->trades[0].vol);
	ASSERT_EQ(2u, sink->orders.size());
	EXPECT_DOUBLE_EQ(40.0, sink->orders[1].leftover);

	WTSTickData* t4 = makeTick(code, 10.2, 20, 10.1, 100, 10.2, 200);
	feedTick(code, t4);	// 最新价 10.2 > 限价 10.1, 非主动卖按剩余量成交
	ASSERT_EQ(2u, sink->trades.size());
	EXPECT_DOUBLE_EQ(40.0, sink->trades[1].vol);
	EXPECT_DOUBLE_EQ(10.2, sink->trades[1].price);
}

TEST_F(MatchEngineTest, positiveSellMatchesAtBidPrice)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600004";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 9.95, 300, 10.0, 100);
	feedTick(code, t1);

	OrderIDs ids = engine.sell(code, 9.9, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	WTSTickData* t2 = makeTick(code, 10.0, 300, 9.95, 300, 10.0, 100);
	feedTick(code, t2);
	ASSERT_EQ(1u, sink->trades.size());
	EXPECT_FALSE(sink->trades[0].isBuy);
	EXPECT_DOUBLE_EQ(9.95, sink->trades[0].price);
}

TEST_F(MatchEngineTest, zeroVolumeTickSkipsMatching)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600005";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.05, 100);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.0, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	WTSTickData* t2 = makeTick(code, 10.0, 0, 10.0, 100, 10.05, 100);
	feedTick(code, t2);
	EXPECT_TRUE(sink->trades.empty());
	// fire_orders 仍然激活了订单
	EXPECT_EQ(1u, sink->entrusts.size());
}

TEST_F(MatchEngineTest, zeroAskQuantityDoesNotCreateSyntheticBuyFill)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600011";

	// 正常双边盘口用于注册可主动买入的初始 ask。
	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.05, 100);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.05, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	// 总成交量非零以进入撮合；对手 ask 数量为零时不得生成成交。
	WTSTickData* t2 = makeTick(code, 10.0, 100, 10.0, 100, 10.05, 0);
	feedTick(code, t2);

	EXPECT_TRUE(sink->trades.empty());
	ASSERT_EQ(1u, sink->orders.size());
	EXPECT_EQ(ids[0], sink->orders[0].localid);
	EXPECT_DOUBLE_EQ(30.0, sink->orders[0].leftover);
}

TEST_F(MatchEngineTest, zeroBidQuantityDoesNotCreateSyntheticSellFill)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600012";

	// 正常双边盘口用于注册可主动卖出的初始 bid。
	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 100, 10.05, 100);
	feedTick(code, t1);

	OrderIDs ids = engine.sell(code, 9.95, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	// 总成交量非零以进入撮合；对手 bid 数量为零时不得生成成交。
	WTSTickData* t2 = makeTick(code, 10.0, 100, 10.0, 0, 10.05, 100);
	feedTick(code, t2);

	EXPECT_TRUE(sink->trades.empty());
	ASSERT_EQ(1u, sink->orders.size());
	EXPECT_EQ(ids[0], sink->orders[0].localid);
	EXPECT_DOUBLE_EQ(30.0, sink->orders[0].leftover);
}

TEST_F(MatchEngineTest, cancelByIdMarksOrderAndReportsCancellation)
{
	setCancelRate(engine, 0.1);
	const char* code = "SSE.STK.600006";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 1000, 10.1, 1000);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.0, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	const uint32_t lid = ids[0];

	// 撤单率 0.1 生效: 排队量被按比例削减, 大单不会被成交量穿透
	WTSTickData* t2 = makeTick(code, 10.0, 900, 10.0, 1000, 10.1, 1000);
	feedTick(code, t2);
	EXPECT_TRUE(sink->trades.empty());

	double cancelled = engine.cancel(lid);
	EXPECT_DOUBLE_EQ(100.0, cancelled);

	// 撤单回报即使成交量为 0 也应处理
	WTSTickData* t3 = makeTick(code, 10.0, 0, 10.0, 1000, 10.1, 1000);
	feedTick(code, t3);
	bool foundCanceled = false;
	for (const auto& o : sink->orders)
	{
		if (o.isCanceled)
		{
			foundCanceled = true;
			EXPECT_EQ(lid, o.localid);
			EXPECT_DOUBLE_EQ(0.0, o.leftover);
		}
	}
	EXPECT_TRUE(foundCanceled);

	// 订单已被删除, 再撤返回 0
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(999999));
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(lid));
}

TEST_F(MatchEngineTest, cancelBySideCollectsActiveOrdersOnly)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600007";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 1000, 10.1, 1000);
	feedTick(code, t1);

	OrderIDs buys = engine.buy(code, 10.0, 50, 20240102093000000ULL);
	OrderIDs sells = engine.sell(code, 10.1, 60, 20240102093000000ULL);
	ASSERT_EQ(1u, buys.size());
	ASSERT_EQ(1u, sells.size());

	// 激活
	WTSTickData* t2 = makeTick(code, 10.0, 0, 10.0, 1000, 10.1, 1000);
	feedTick(code, t2);

	double cbTotal = 0.0;
	OrderIDs cancelled = engine.cancel(code, true, 0.0, [&cbTotal](double vol) { cbTotal += vol; });
	EXPECT_EQ(1u, cancelled.size());
	EXPECT_EQ(buys[0], cancelled[0]);
	EXPECT_DOUBLE_EQ(50.0, cbTotal);

	cbTotal = 0.0;
	OrderIDs cancelledSell = engine.cancel(code, false, 0.0, [&cbTotal](double vol) { cbTotal += vol; });
	EXPECT_EQ(1u, cancelledSell.size());
	EXPECT_DOUBLE_EQ(-60.0, cbTotal);	// 卖出撤单数量为负
}

TEST_F(MatchEngineTest, clearDropsAllOrders)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600008";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 1000, 10.1, 1000);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.0, 50, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	engine.clear();

	const size_t entrustCnt = sink->entrusts.size();
	WTSTickData* t2 = makeTick(code, 10.0, 2000, 10.0, 1000, 10.1, 1000);
	feedTick(code, t2);
	EXPECT_EQ(entrustCnt, sink->entrusts.size());
	EXPECT_TRUE(sink->trades.empty());
}

TEST_F(MatchEngineTest, updateLobHandlesEmptyBookWithoutCrash)
{
	setCancelRate(engine, 0.0);
	const char* code = "SSE.STK.600009";

	// 无盘口挂单的单笔成交 tick, update_lob 应立即 break
	WTSTickData* t1 = makeTick(code, 10.0, 500, 0.0, 0.0, 0.0, 0.0);
	feedTick(code, t1);
	// 再次喂入同构 tick, 保证 _items 为空时不会误擦除
	WTSTickData* t2 = makeTick(code, 10.05, 500, 0.0, 0.0, 0.0, 0.0);
	feedTick(code, t2);
	// 正常盘口 tick, 走完整买卖十档填充与中间价清理分支
	WTSTickData* t3 = makeTick(code, 10.0, 500, 9.99, 100, 10.01, 100);
	feedTick(code, t3);
	EXPECT_TRUE(sink->trades.empty());
}

TEST_F(MatchEngineTest, queueReductionAppliesCancelRate)
{
	setCancelRate(engine, 0.5);
	const char* code = "SSE.STK.600010";

	WTSTickData* t1 = makeTick(code, 10.0, 100, 10.0, 400, 10.1, 400);
	feedTick(code, t1);

	OrderIDs ids = engine.buy(code, 10.0, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	(void)ids;
	// 撤单率 0.5 → 加权排队约 400, 削减一半约 200;
	// 下一个 tick 量 150 <= 200 不应成交
	WTSTickData* t2 = makeTick(code, 10.0, 150, 10.0, 400, 10.1, 400);
	feedTick(code, t2);
	EXPECT_TRUE(sink->trades.empty());

	// 250 超过剩余排队(<200), 应产生成交
	WTSTickData* t3 = makeTick(code, 10.0, 250, 10.0, 400, 10.1, 400);
	feedTick(code, t3);
	EXPECT_EQ(1u, sink->trades.size());
}
