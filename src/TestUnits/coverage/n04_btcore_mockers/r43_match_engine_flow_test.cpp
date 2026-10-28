/**
 * r43_match_engine_flow_test.cpp
 * Lane R43：MatchEngine.cpp 在 l06 已覆盖 95.88% 之后剩余的撮合分支：
 *   match_orders(L136-139)  —— 被动买价严格低于限价时按剩余量成交
 *   match_orders(L142-143)  —— 可成交量算出 0 时保底成交 1 手
 *   match_orders(L200-201)  —— 卖方向同样的 0 量保底
 *   cancel(L364-392)        —— 按方向+数量撤单时 `left` 递减与 break 条件
 *   cancel(L370-371)        —— 未激活（state!=1）的订单不参与撤单
 * 全部输入由本文件构造，不依赖行情文件与外部服务。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <WtBtCore/MatchEngine.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	class R43MatchSink : public IMatchSink
	{
	public:
		struct TradeRec
		{
			uint32_t localid;
			bool isBuy;
			double vol;
			double fireprice;
			double price;
		};
		struct OrderRec
		{
			uint32_t localid;
			bool isBuy;
			double leftover;
			bool isCanceled;
		};

		std::vector<TradeRec> trades;
		std::vector<OrderRec> orders;
		uint32_t entrusts {0};

		void handle_trade(uint32_t localid, const char* stdCode, bool isBuy,
			double vol, double fireprice, double price, uint64_t) override
		{
			(void)stdCode;
			trades.push_back({ localid, isBuy, vol, fireprice, price });
		}

		void handle_order(uint32_t localid, const char* stdCode, bool isBuy,
			double leftover, double price, bool isCanceled, uint64_t) override
		{
			(void)stdCode; (void)price;
			orders.push_back({ localid, isBuy, leftover, isCanceled });
		}

		void handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess,
			const char* message, uint64_t) override
		{
			(void)localid; (void)stdCode; (void)bSuccess; (void)message;
			entrusts++;
		}
	};

	WTSTickData* r43_tick(const char* code, double price, double vol,
		double bidpx, double bidqty, double askpx, double askqty)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, "SSE");
		strcpy(ts.code, code);
		ts.trading_date = 20240102;
		ts.action_date = 20240102;
		ts.action_time = 93000000;
		ts.price = price;
		ts.open = price;
		ts.high = price;
		ts.low = price;
		ts.total_volume = vol;
		ts.volume = vol;
		if (bidpx > 0.0)
		{
			ts.bid_prices[0] = bidpx;
			ts.bid_qty[0] = (uint32_t)bidqty;
		}
		if (askpx > 0.0)
		{
			ts.ask_prices[0] = askpx;
			ts.ask_qty[0] = (uint32_t)askqty;
		}
		return WTSTickData::create(ts);
	}
}

class R43MatchEngineFlowTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("cancelrate", 0.0);
		engine.regisSink(&sink);
		engine.init(cfg);
		cfg->release();
	}

	void TearDown() override
	{
		engine.clear();
	}

	void feed(const char* code, WTSTickData* tick)
	{
		engine.handle_tick(code, tick);
		tick->release();
	}

	MatchEngine engine;
	R43MatchSink sink;
};

// 被动买价严格低于限价（且不等于最新价）：按委托剩余量成交，不受盘口量限制
TEST_F(R43MatchEngineFlowTest, passiveBuyBelowLimitFillsRemainingQty)
{
	const char* code = "SSE.STK.610001";
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 600));

	// 10.05 既不等于最新价也不触及卖一，排队量为 0；成交时走 volume=_left 分支
	OrderIDs ids = engine.buy(code, 10.05, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	feed(code, r43_tick(code, 10.0, 50, 10.0, 500, 10.1, 600));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_DOUBLE_EQ(100.0, sink.trades[0].vol);
	EXPECT_DOUBLE_EQ(10.0, sink.trades[0].price);
	ASSERT_FALSE(sink.orders.empty());
	EXPECT_DOUBLE_EQ(0.0, sink.orders.back().leftover);
}

// 主动买遇到卖一量为 0：可成交量归零时保底成交 1 手
TEST_F(R43MatchEngineFlowTest, activeBuyWithZeroAskQtyFillsOneLot)
{
	const char* code = "SSE.STK.610002";
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 0));

	OrderIDs ids = engine.buy(code, 10.1, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 0));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_DOUBLE_EQ(1.0, sink.trades[0].vol);
	EXPECT_DOUBLE_EQ(10.1, sink.trades[0].price);
	ASSERT_FALSE(sink.orders.empty());
	EXPECT_DOUBLE_EQ(99.0, sink.orders.back().leftover);
}

// 主动卖遇到买一量为 0：同样保底 1 手，且方向为卖
TEST_F(R43MatchEngineFlowTest, activeSellWithZeroBidQtyFillsOneLot)
{
	const char* code = "SSE.STK.610003";
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 0, 10.0, 500));

	OrderIDs ids = engine.sell(code, 9.9, 40, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	feed(code, r43_tick(code, 10.0, 1000, 9.9, 0, 10.0, 500));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_FALSE(sink.trades[0].isBuy);
	EXPECT_DOUBLE_EQ(1.0, sink.trades[0].vol);
	EXPECT_DOUBLE_EQ(9.9, sink.trades[0].price);
	ASSERT_FALSE(sink.orders.empty());
	EXPECT_DOUBLE_EQ(39.0, sink.orders.back().leftover);
}

// 按方向+数量撤单：数量大于单笔剩余时继续累减，两笔订单都要撤掉
TEST_F(R43MatchEngineFlowTest, cancelByQtyWalksOrdersAndDecrementsLeft)
{
	const char* code = "SSE.STK.610004";
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 600));

	OrderIDs first = engine.buy(code, 10.0, 100, 20240102093000000ULL);
	OrderIDs second = engine.buy(code, 10.0, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, first.size());
	ASSERT_EQ(1u, second.size());

	// 先激活两笔订单（state 0 -> 1），撤单只处理激活后的订单
	feed(code, r43_tick(code, 10.0, 0, 10.0, 500, 10.1, 600));
	const uint32_t entrustsBefore = sink.entrusts;
	EXPECT_EQ(2u, entrustsBefore);

	double cbTotal = 0.0;
	OrderIDs cancelled = engine.cancel(code, true, 250.0, [&cbTotal](double vol) { cbTotal += vol; });
	EXPECT_EQ(2u, cancelled.size());
	EXPECT_DOUBLE_EQ(200.0, cbTotal);

	// 撤单在下一笔 tick 才真正落地（state 9 -> 99 并删除）
	feed(code, r43_tick(code, 10.0, 0, 10.0, 500, 10.1, 600));
	uint32_t cancelCalls = 0;
	for (const auto& o : sink.orders)
	{
		if (o.isCanceled)
			cancelCalls++;
	}
	EXPECT_EQ(2u, cancelCalls);
}

// 未激活订单（state==0）不参与按方向撤单
TEST_F(R43MatchEngineFlowTest, cancelBySideSkipsInactiveOrders)
{
	const char* code = "SSE.STK.610005";
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 600));

	OrderIDs ids = engine.buy(code, 10.0, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	uint32_t cbCalls = 0;
	// 还没有 tick 激活订单 -> state 仍为 0 -> continue，不返回任何撤单 id
	OrderIDs none = engine.cancel(code, true, 0.0, [&cbCalls](double) { cbCalls++; });
	EXPECT_TRUE(none.empty());
	EXPECT_EQ(0u, cbCalls);

	// 激活后同一入口可以撤掉
	feed(code, r43_tick(code, 10.0, 0, 10.0, 500, 10.1, 600));
	OrderIDs cancelled = engine.cancel(code, true, 0.0, [&cbCalls](double) { cbCalls++; });
	EXPECT_EQ(1u, cancelled.size());
	EXPECT_EQ(1u, cbCalls);
}

/*
 * 缺陷归档（见 bugs/R43.md BUG-R43-002）：update_lob 在盘口只有一侧报价时，
 * `sit = lower_bound(_bid_px); if (sit->first == _bid_px) sit++;` 之后没有判 end，
 * 紧接的 `sit->first <= eit->first` 解引用 end()（未定义行为）。
 * 该路径只影响私有委托账本、没有公开读取口，无法用断言稳定证伪，
 * 因此这里只保留单侧盘口输入，默认不启用。
 */
TEST_F(R43MatchEngineFlowTest, DISABLED_oneSidedBookDereferencesEnd)
{
	const char* code = "SSE.STK.610006";
	// 只有买一、没有卖一 -> _items 只有一个价位，sit++ 后指向 end()
	feed(code, r43_tick(code, 10.0, 100, 10.0, 500, 0.0, 0.0));
	feed(code, r43_tick(code, 10.0, 100, 10.0, 500, 0.0, 0.0));
}
