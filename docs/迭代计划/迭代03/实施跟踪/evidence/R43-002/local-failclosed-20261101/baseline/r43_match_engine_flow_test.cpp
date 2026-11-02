/**
 * r43_match_engine_flow_test.cpp
 * Lane R43：MatchEngine.cpp 在 l06 已覆盖 95.88% 之后剩余的撮合分支：
 *   match_orders(L136-139)  —— 被动买价严格低于限价时按剩余量成交
 *   match_orders           —— 零盘口量不合成成交，后续有效量可继续撮合
 *   cancel(L364-392)        —— 按方向+数量撤单时 `left` 递减与 break 条件
 *   cancel(L370-371)        —— 未激活（state!=1）的订单不参与撤单
 * 全部输入由本文件构造，不依赖行情文件与外部服务。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
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
			std::string code;
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
		std::function<void(uint32_t)> onTrade;
		std::function<void(uint32_t)> onEntrust;
		std::function<void(uint32_t, bool)> onOrder;

		void handle_trade(uint32_t localid, const char* stdCode, bool isBuy,
			double vol, double fireprice, double price, uint64_t) override
		{
			trades.push_back({ localid, stdCode, isBuy, vol, fireprice, price });
			if (onTrade)
				onTrade(localid);
		}

		void handle_order(uint32_t localid, const char* stdCode, bool isBuy,
			double leftover, double price, bool isCanceled, uint64_t) override
		{
			(void)stdCode; (void)price;
			orders.push_back({ localid, isBuy, leftover, isCanceled });
			if (onOrder)
				onOrder(localid, isCanceled);
		}

		void handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess,
			const char* message, uint64_t) override
		{
			(void)localid; (void)stdCode; (void)bSuccess; (void)message;
			entrusts++;
			if (onEntrust)
				onEntrust(localid);
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

class MatchEngineTestAccess
{
public:
	static const std::map<uint32_t, double>* book_items(
		const MatchEngine& engine, const char* code)
	{
		auto it = engine._lmt_ord_books.find(code);
		if (it == engine._lmt_ord_books.end())
			return nullptr;
		return &it->second._items;
	}
};

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

// 主动买遇到卖一量为 0：不合成成交，后续盘口量可正常撮合
TEST_F(R43MatchEngineFlowTest, activeBuyWithZeroAskQtyFillsOneLot)
{
	const char* code = "SSE.STK.610002";
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 0));

	OrderIDs ids = engine.buy(code, 10.1, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	feed(code, r43_tick(code, 10.0, 0, 10.0, 500, 10.1, 0));
	EXPECT_TRUE(sink.trades.empty());
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 0));
	EXPECT_TRUE(sink.trades.empty());
	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 2));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_EQ(code, sink.trades[0].code);
	EXPECT_DOUBLE_EQ(2.0, sink.trades[0].vol);
	EXPECT_DOUBLE_EQ(10.1, sink.trades[0].price);
	ASSERT_FALSE(sink.orders.empty());
	EXPECT_DOUBLE_EQ(98.0, sink.orders.back().leftover);

	feed(code, r43_tick(code, 10.0, 1000, 10.0, 500, 10.1, 200));
	ASSERT_EQ(2u, sink.trades.size());
	EXPECT_DOUBLE_EQ(98.0, sink.trades.back().vol);
	EXPECT_DOUBLE_EQ(0.0, sink.orders.back().leftover);
}

// 主动卖遇到买一量为 0：不合成成交，后续盘口量可正常撮合
TEST_F(R43MatchEngineFlowTest, activeSellWithZeroBidQtyFillsOneLot)
{
	const char* code = "SSE.STK.610003";
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 0, 10.0, 500));

	OrderIDs ids = engine.sell(code, 9.9, 40, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	feed(code, r43_tick(code, 10.0, 0, 9.9, 0, 10.0, 500));
	EXPECT_TRUE(sink.trades.empty());
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 0, 10.0, 500));
	EXPECT_TRUE(sink.trades.empty());
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 3, 10.0, 500));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_EQ(code, sink.trades[0].code);
	EXPECT_FALSE(sink.trades[0].isBuy);
	EXPECT_DOUBLE_EQ(3.0, sink.trades[0].vol);
	EXPECT_DOUBLE_EQ(9.9, sink.trades[0].price);
	ASSERT_FALSE(sink.orders.empty());
	EXPECT_DOUBLE_EQ(37.0, sink.orders.back().leftover);
}

TEST_F(R43MatchEngineFlowTest, zeroDepthFallbackIsScopedToTickContract)
{
	const char* codeA = "SSE.STK.610010";
	const char* codeB = "SSE.STK.610011";
	feed(codeA, r43_tick(codeA, 10.0, 1000, 10.0, 500, 10.1, 0));
	feed(codeB, r43_tick(codeB, 10.0, 1000, 10.0, 500, 10.1, 0));
	ASSERT_EQ(1u, engine.buy(codeA, 10.1, 2, 20240102093000000ULL).size());
	ASSERT_EQ(1u, engine.buy(codeB, 10.1, 2, 20240102093000000ULL).size());

	feed(codeA, r43_tick(codeA, 10.0, 1000, 10.0, 500, 10.1, 0));
	EXPECT_TRUE(sink.trades.empty());
	feed(codeA, r43_tick(codeA, 10.0, 1000, 10.0, 500, 10.1, 2));
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_EQ(codeA, sink.trades[0].code);
	EXPECT_DOUBLE_EQ(2.0, sink.trades[0].vol);

	feed(codeB, r43_tick(codeB, 10.0, 1000, 10.0, 500, 10.1, 0));
	ASSERT_EQ(1u, sink.trades.size());
	feed(codeB, r43_tick(codeB, 10.0, 1000, 10.0, 500, 10.1, 1));
	ASSERT_EQ(2u, sink.trades.size());
	EXPECT_EQ(codeB, sink.trades[1].code);
	EXPECT_DOUBLE_EQ(1.0, sink.trades[1].vol);
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

TEST_F(R43MatchEngineFlowTest, cancelByIdAcceptsAnUnactivatedOrderOnlyOnce)
{
	const char* code = "SSE.STK.610019";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	OrderIDs ids = engine.buy(code, 9.9, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	// ID cancellation may cancel state 0, but once it enters state 9 the same
	// request must not return the remaining quantity a second time.
	EXPECT_DOUBLE_EQ(30.0, engine.cancel(ids.front()));
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(ids.front()));
	const auto pending = engine.get_order_snapshots(code);
	ASSERT_EQ(1u, pending.size());
	EXPECT_EQ(9u, pending.front().state);
	EXPECT_DOUBLE_EQ(30.0, pending.front().left);
	EXPECT_EQ(0u, sink.entrusts);

	// The first same-code tick delivers the cancel response without activating it.
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	EXPECT_TRUE(engine.get_order_snapshots(code).empty());
	ASSERT_EQ(1u, sink.orders.size());
	EXPECT_EQ(ids.front(), sink.orders.front().localid);
	EXPECT_TRUE(sink.orders.front().isCanceled);
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(ids.front()));
}

TEST_F(R43MatchEngineFlowTest, oneSidedBidIsSafeAndDoesNotCreateAFillWithoutAnAsk)
{
	const char* code = "SSE.STK.610006";
	feed(code, r43_tick(code, 10.0, 100, 10.0, 500, 0.0, 0.0));

	const auto* items = MatchEngineTestAccess::book_items(engine, code);
	ASSERT_NE(nullptr, items);
	ASSERT_EQ(1u, items->size());
	EXPECT_DOUBLE_EQ(500.0, items->at(100000u));

	// With no ask quote, a lower buy limit is passive. A later trade above
	// that limit must not be filled using the missing ask's zero price/size.
	OrderIDs ids = engine.buy(code, 9.9, 10, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	feed(code, r43_tick(code, 10.2, 1000, 10.0, 250, 0.0, 0.0));
	EXPECT_TRUE(sink.trades.empty());

	items = MatchEngineTestAccess::book_items(engine, code);
	ASSERT_NE(nullptr, items);
	ASSERT_EQ(1u, items->size());
	EXPECT_DOUBLE_EQ(250.0, items->at(100000u));
}

TEST_F(R43MatchEngineFlowTest, oneSidedAskAndEmptyQuoteSidesAreIteratorSafe)
{
	const char* code = "SSE.STK.610007";
	feed(code, r43_tick(code, 10.0, 100, 0.0, 0.0, 10.1, 600));

	const auto* items = MatchEngineTestAccess::book_items(engine, code);
	ASSERT_NE(nullptr, items);
	ASSERT_EQ(1u, items->size());
	EXPECT_DOUBLE_EQ(600.0, items->at(101000u));

	// Empty quote sides must not produce invalid iterator access after a book
	// has already been created. This does not assert whether old levels should
	// be retained or cleared; that behavior has no confirmed contract.
	feed(code, r43_tick(code, 10.0, 0, 0.0, 0.0, 0.0, 0.0));
	EXPECT_TRUE(sink.trades.empty());
}

TEST_F(R43MatchEngineFlowTest, bidAndAskDepthUseTheirOwnQuantities)
{
	const char* code = "SSE.STK.610008";
	feed(code, r43_tick(code, 10.0, 100, 10.0, 500, 10.1, 600));

	const auto* items = MatchEngineTestAccess::book_items(engine, code);
	ASSERT_NE(nullptr, items);
	ASSERT_EQ(2u, items->size());
	EXPECT_DOUBLE_EQ(500.0, items->at(100000u));
	EXPECT_DOUBLE_EQ(600.0, items->at(101000u));

	feed(code, r43_tick(code, 10.0, 0, 10.0, 250, 10.1, 800));
	items = MatchEngineTestAccess::book_items(engine, code);
	ASSERT_NE(nullptr, items);
	EXPECT_DOUBLE_EQ(250.0, items->at(100000u));
	EXPECT_DOUBLE_EQ(800.0, items->at(101000u));
}

TEST_F(R43MatchEngineFlowTest, orderSnapshotsPreserveGrossStateAndDisappearAfterTerminalReport)
{
	const char* codeA = "SSE.STK.610009";
	const char* codeB = "SSE.STK.610010";
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 40));
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 60, 20.1, 70));

	OrderIDs buy = engine.buy(codeA, 10.1, 100, 20240102093000000ULL);
	OrderIDs sell = engine.sell(codeA, 9.9, 50, 20240102093000000ULL);
	OrderIDs otherCode = engine.sell(codeB, 20.2, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, buy.size());
	ASSERT_EQ(1u, sell.size());
	ASSERT_EQ(1u, otherCode.size());

	auto findById = [](const std::vector<MatchEngine::OrderSnapshot>& orders, uint32_t id)
		-> const MatchEngine::OrderSnapshot*
	{
		for (const MatchEngine::OrderSnapshot& order : orders)
			if (order.localid == id)
				return &order;
		return nullptr;
	};

	const auto pendingA = engine.get_order_snapshots(codeA);
	ASSERT_EQ(2u, pendingA.size());
	const auto* pendingBuy = findById(pendingA, buy.front());
	const auto* pendingSell = findById(pendingA, sell.front());
	ASSERT_NE(nullptr, pendingBuy);
	ASSERT_NE(nullptr, pendingSell);
	EXPECT_TRUE(pendingBuy->buy);
	EXPECT_DOUBLE_EQ(100.0, pendingBuy->qty);
	EXPECT_DOUBLE_EQ(100.0, pendingBuy->left);
	EXPECT_DOUBLE_EQ(0.0, pendingBuy->traded);
	EXPECT_DOUBLE_EQ(10.1, pendingBuy->price);
	EXPECT_EQ(0u, pendingBuy->state);
	EXPECT_EQ(20240102093000000ULL, pendingBuy->time);
	EXPECT_FALSE(pendingSell->buy);
	EXPECT_EQ(0u, pendingSell->state);
	const auto pendingB = engine.get_order_snapshots(codeB);
	ASSERT_EQ(1u, pendingB.size());
	EXPECT_EQ(0u, pendingB.front().state);
	EXPECT_EQ(3u, engine.get_order_snapshots("").size());
	EXPECT_TRUE(engine.get_order_snapshots("SSE.STK.999999").empty());

	// The zero-volume tick activates both codeA orders without filling them.
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 40));
	const auto active = engine.get_order_snapshots(codeA);
	ASSERT_EQ(2u, active.size());
	const auto* activeBuy = findById(active, buy.front());
	const auto* activeSell = findById(active, sell.front());
	ASSERT_NE(nullptr, activeBuy);
	ASSERT_NE(nullptr, activeSell);
	EXPECT_EQ(1u, activeBuy->state);
	EXPECT_EQ(1u, activeSell->state);

	// One side is partially filled; the opposing order completes and is removed.
	feed(codeA, r43_tick(codeA, 10.0, 1000, 9.9, 50, 10.1, 40));
	const auto partiallyFilled = engine.get_order_snapshots(codeA);
	ASSERT_EQ(1u, partiallyFilled.size());
	const auto* partialBuy = findById(partiallyFilled, buy.front());
	ASSERT_NE(nullptr, partialBuy);
	EXPECT_EQ(1u, partialBuy->state);
	EXPECT_DOUBLE_EQ(100.0, partialBuy->qty);
	EXPECT_DOUBLE_EQ(60.0, partialBuy->left);
	EXPECT_DOUBLE_EQ(40.0, partialBuy->traded);
	EXPECT_EQ(nullptr, findById(partiallyFilled, sell.front()));

	EXPECT_DOUBLE_EQ(60.0, engine.cancel(buy.front()));
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(buy.front()));
	const auto cancelPending = engine.get_order_snapshots(codeA);
	ASSERT_EQ(1u, cancelPending.size());
	const auto* cancellingBuy = findById(cancelPending, buy.front());
	ASSERT_NE(nullptr, cancellingBuy);
	EXPECT_EQ(9u, cancellingBuy->state);
	EXPECT_DOUBLE_EQ(60.0, cancellingBuy->left);
	EXPECT_DOUBLE_EQ(40.0, cancellingBuy->traded);

	// The next tick delivers the cancel report, after which the matcher drops it.
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 40));
	EXPECT_TRUE(engine.get_order_snapshots(codeA).empty());
	EXPECT_DOUBLE_EQ(0.0, engine.cancel(buy.front()));
	EXPECT_EQ(1u, engine.get_order_snapshots(codeB).size());
}

TEST_F(R43MatchEngineFlowTest, ticksActivateAndFillOnlyOrdersForTheirOwnCode)
{
	const char* codeA = "SSE.STK.610011";
	const char* codeB = "SSE.STK.610012";
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 50));
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 50, 20.1, 50));

	OrderIDs buyB = engine.buy(codeB, 20.1, 50, 20240102093000000ULL);
	ASSERT_EQ(1u, buyB.size());

	// Even a crossing A tick cannot activate or fill B's pending order.
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 50));
	feed(codeA, r43_tick(codeA, 10.0, 1000, 9.9, 50, 10.1, 50));
	const auto stillPendingB = engine.get_order_snapshots(codeB);
	ASSERT_EQ(1u, stillPendingB.size());
	EXPECT_EQ(0u, stillPendingB.front().state);
	EXPECT_TRUE(sink.trades.empty());
	EXPECT_EQ(0u, sink.entrusts);

	// The matching B tick activates and fills that same order as expected.
	feed(codeB, r43_tick(codeB, 20.0, 1000, 19.9, 50, 20.1, 50));
	EXPECT_TRUE(engine.get_order_snapshots(codeB).empty());
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_EQ(buyB.front(), sink.trades.front().localid);
	EXPECT_DOUBLE_EQ(50.0, sink.trades.front().vol);
	EXPECT_DOUBLE_EQ(20.1, sink.trades.front().price);
}

TEST_F(R43MatchEngineFlowTest, cancelPendingOrderWaitsForItsOwnCodeTick)
{
	const char* codeA = "SSE.STK.610013";
	const char* codeB = "SSE.STK.610014";
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 50));
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 50, 20.1, 50));

	OrderIDs buyB = engine.buy(codeB, 20.1, 50, 20240102093000000ULL);
	ASSERT_EQ(1u, buyB.size());
	EXPECT_DOUBLE_EQ(50.0, engine.cancel(buyB.front()));

	// A tick must not deliver B's cancel response or remove its state-9 order.
	feed(codeA, r43_tick(codeA, 10.0, 1000, 9.9, 50, 10.1, 50));
	const auto cancelPendingB = engine.get_order_snapshots(codeB);
	ASSERT_EQ(1u, cancelPendingB.size());
	EXPECT_EQ(9u, cancelPendingB.front().state);
	EXPECT_DOUBLE_EQ(50.0, cancelPendingB.front().left);
	EXPECT_TRUE(sink.orders.empty());

	// The first B tick is the cancel acknowledgement and removes the order.
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 50, 20.1, 50));
	EXPECT_TRUE(engine.get_order_snapshots(codeB).empty());
	ASSERT_EQ(1u, sink.orders.size());
	EXPECT_EQ(buyB.front(), sink.orders.front().localid);
	EXPECT_TRUE(sink.orders.front().isCanceled);
}

TEST_F(R43MatchEngineFlowTest, codeScopedCancelDoesNotMarkAnotherContractsOrder)
{
	const char* codeA = "SSE.STK.610015";
	const char* codeB = "SSE.STK.610016";
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 50));
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 50, 20.1, 50));
	OrderIDs buyA = engine.buy(codeA, 9.9, 20, 20240102093000000ULL);
	OrderIDs buyB = engine.buy(codeB, 19.9, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, buyA.size());
	ASSERT_EQ(1u, buyB.size());
	feed(codeA, r43_tick(codeA, 10.0, 0, 9.9, 50, 10.1, 50));
	feed(codeB, r43_tick(codeB, 20.0, 0, 19.9, 50, 20.1, 50));

	OrderIDs canceledA = engine.cancel(codeA, true, 0.0, [](double) {});
	ASSERT_EQ(1u, canceledA.size());
	EXPECT_EQ(buyA.front(), canceledA.front());
	const auto ordersA = engine.get_order_snapshots(codeA);
	ASSERT_EQ(1u, ordersA.size());
	EXPECT_EQ(9u, ordersA.front().state);
	const auto ordersB = engine.get_order_snapshots(codeB);
	ASSERT_EQ(1u, ordersB.size());
	EXPECT_EQ(buyB.front(), ordersB.front().localid);
	EXPECT_EQ(1u, ordersB.front().state);
}

TEST_F(R43MatchEngineFlowTest, callbacksCanInsertOrdersDuringActivationAndMatching)
{
	const char* code = "SSE.STK.610017";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 20));
	OrderIDs original = engine.buy(code, 10.1, 20, 20240102093000000ULL);
	ASSERT_EQ(1u, original.size());

	uint32_t insertedOnEntrust = 0;
	uint32_t insertedOnTrade = 0;
	sink.onEntrust = [&](uint32_t localid) {
		if (localid != original.front() || insertedOnEntrust != 0)
			return;
		for (uint32_t i = 0; i < 128; ++i)
		{
			OrderIDs added = engine.buy(code, 9.9, 1, 20240102093000000ULL);
			if (!added.empty())
				insertedOnEntrust++;
		}
	};
	sink.onTrade = [&](uint32_t localid) {
		if (localid != original.front() || insertedOnTrade != 0)
			return;
		for (uint32_t i = 0; i < 128; ++i)
		{
			OrderIDs added = engine.sell(code, 10.2, 1, 20240102093000000ULL);
			if (!added.empty())
				insertedOnTrade++;
		}
	};

	// Order insertion from handle_entrust forces map growth while activation
	// callbacks are being dispatched. These new orders wait until the next tick.
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 20));
	EXPECT_EQ(128u, insertedOnEntrust);
	const auto afterActivation = engine.get_order_snapshots(code);
	ASSERT_EQ(129u, afterActivation.size());
	uint32_t activeCount = 0;
	uint32_t newCount = 0;
	for (const auto& order : afterActivation)
	{
		if (order.localid == original.front())
			EXPECT_EQ(1u, order.state);
		else if (order.state == 0)
			newCount++;
		else if (order.state == 1)
			activeCount++;
	}
	EXPECT_EQ(0u, activeCount);
	EXPECT_EQ(128u, newCount);

	// Matching another tick invokes a trade callback that grows the map again.
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 50, 10.1, 20));
	EXPECT_EQ(128u, insertedOnTrade);
	ASSERT_EQ(1u, sink.trades.size());
	EXPECT_EQ(original.front(), sink.trades.front().localid);
	const auto afterMatch = engine.get_order_snapshots(code);
	ASSERT_EQ(256u, afterMatch.size());
	uint32_t newSellCount = 0;
	uint32_t stillActiveBuyCount = 0;
	for (const auto& order : afterMatch)
	{
		EXPECT_NE(original.front(), order.localid);
		if (!order.buy && order.state == 0)
			newSellCount++;
		if (order.buy && order.state == 1)
			stillActiveBuyCount++;
	}
	EXPECT_EQ(128u, newSellCount);
	EXPECT_EQ(128u, stillActiveBuyCount);
}

TEST_F(R43MatchEngineFlowTest, cancelCallbackCanInsertOrdersWithoutInvalidatingTraversal)
{
	const char* code = "SSE.STK.610018";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	OrderIDs first = engine.buy(code, 9.9, 100, 20240102093000000ULL);
	OrderIDs second = engine.buy(code, 9.9, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, first.size());
	ASSERT_EQ(1u, second.size());
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));

	uint32_t callbackCount = 0;
	uint32_t inserted = 0;
	OrderIDs canceled = engine.cancel(code, true, 150.0, [&](double) {
		callbackCount++;
		if (inserted != 0)
			return;
		for (uint32_t i = 0; i < 64; ++i)
		{
			OrderIDs added = engine.buy(code, 9.9, 1, 20240102093000000ULL);
			if (!added.empty())
				inserted++;
		}
	});
	EXPECT_EQ(2u, canceled.size());
	EXPECT_EQ(2u, callbackCount);
	EXPECT_EQ(64u, inserted);
	const auto snapshot = engine.get_order_snapshots(code);
	ASSERT_EQ(66u, snapshot.size());
	uint32_t cancelPending = 0;
	uint32_t inactive = 0;
	for (const auto& order : snapshot)
	{
		if (order.localid == first.front() || order.localid == second.front())
		{
			EXPECT_EQ(9u, order.state);
			if (order.state == 9)
				cancelPending++;
		}
		else if (order.state == 0)
			inactive++;
	}
	EXPECT_EQ(2u, cancelPending);
	EXPECT_EQ(64u, inactive);
}

TEST_F(R43MatchEngineFlowTest, tradeCallbackCanCancelCurrentOrderAndDefersCancelReport)
{
	const char* code = "SSE.STK.610020";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 40));
	OrderIDs ids = engine.buy(code, 10.1, 100, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 40));

	double canceledQty = 0.0;
	sink.onTrade = [&](uint32_t localid) {
		if (localid == ids.front())
			canceledQty = engine.cancel(localid);
	};
	feed(code, r43_tick(code, 10.0, 1000, 9.9, 50, 10.1, 40));

	EXPECT_DOUBLE_EQ(60.0, canceledQty);
	auto pending = engine.get_order_snapshots(code);
	ASSERT_EQ(1u, pending.size());
	EXPECT_EQ(ids.front(), pending.front().localid);
	EXPECT_EQ(9u, pending.front().state);
	EXPECT_DOUBLE_EQ(60.0, pending.front().left);
	EXPECT_DOUBLE_EQ(40.0, pending.front().traded);
	EXPECT_TRUE(std::none_of(sink.orders.begin(), sink.orders.end(),
		[](const R43MatchSink::OrderRec& order) { return order.isCanceled; }));

	// The cancellation was requested from inside a trade callback, so it is
	// acknowledged only on the next tick for this contract.
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 40));
	EXPECT_TRUE(engine.get_order_snapshots(code).empty());
	ASSERT_EQ(1u, std::count_if(sink.orders.begin(), sink.orders.end(),
		[](const R43MatchSink::OrderRec& order) { return order.isCanceled; }));
}

TEST_F(R43MatchEngineFlowTest, cancelReportCallbackCanRepeatCancelAndClearMatcher)
{
	const char* code = "SSE.STK.610021";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	OrderIDs ids = engine.buy(code, 9.9, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(30.0, engine.cancel(ids.front()));

	double repeatedCancelQty = -1.0;
	uint32_t clearCalls = 0;
	sink.onOrder = [&](uint32_t localid, bool isCanceled) {
		if (localid != ids.front() || !isCanceled)
			return;
		repeatedCancelQty = engine.cancel(localid);
		clearCalls++;
		engine.clear();
	};

	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	EXPECT_DOUBLE_EQ(0.0, repeatedCancelQty);
	EXPECT_EQ(1u, clearCalls);
	EXPECT_TRUE(engine.get_order_snapshots(code).empty());
	ASSERT_EQ(1u, sink.orders.size());
	EXPECT_TRUE(sink.orders.front().isCanceled);
}

TEST_F(R43MatchEngineFlowTest, entrustCallbackCanCancelCurrentOrderWithoutReactivatingIt)
{
	const char* code = "SSE.STK.610022";
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	OrderIDs ids = engine.buy(code, 9.9, 30, 20240102093000000ULL);
	ASSERT_EQ(1u, ids.size());

	double canceledQty = 0.0;
	sink.onEntrust = [&](uint32_t localid) {
		if (localid == ids.front())
			canceledQty = engine.cancel(localid);
	};
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));

	EXPECT_DOUBLE_EQ(30.0, canceledQty);
	auto pending = engine.get_order_snapshots(code);
	ASSERT_EQ(1u, pending.size());
	EXPECT_EQ(ids.front(), pending.front().localid);
	EXPECT_EQ(9u, pending.front().state);
	EXPECT_DOUBLE_EQ(30.0, pending.front().left);
	EXPECT_TRUE(std::none_of(sink.orders.begin(), sink.orders.end(),
		[](const R43MatchSink::OrderRec& order) { return order.isCanceled; }));

	// State 9 is acknowledged only on the next tick, never reset to active by
	// the remainder of the activation callback.
	feed(code, r43_tick(code, 10.0, 0, 9.9, 50, 10.1, 50));
	EXPECT_TRUE(engine.get_order_snapshots(code).empty());
	ASSERT_EQ(1u, std::count_if(sink.orders.begin(), sink.orders.end(),
		[](const R43MatchSink::OrderRec& order) { return order.isCanceled; }));
}
