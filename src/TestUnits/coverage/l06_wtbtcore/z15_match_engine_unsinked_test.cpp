/*
 * BUG-I02-NEW-093（卡15最终复查）：未注册 IMatchSink 的 MatchEngine 收到 tick 时不得崩溃。
 *
 * 旧实现中，未经 init/regisSink 的撮合引擎在已有缓存 tick 时为 buy/sell 生成
 * state 0 订单；下一笔 handle_tick 进入 fire_orders 后对 NULL _sink 调
 * handle_entrust 并崩溃（SIGSEGV）。
 *
 * 本文件只断言不存在争议的部分：无回调时不解引用 NULL，挂单保持原状，注册回调后的
 * 下一笔 tick 照常激活并上报。未注册回调时是否应拒绝下单尚无合同；既有
 * R20ExecMocker.CancelByIdDoesNotRestoreUndoneTwice 依赖“未初始化 ExecMocker 可下单”，
 * 因此这里不改变 buy/sell 的接收行为。
 *
 * 崩溃场景在子进程中执行（EXPECT_EXIT），旧实现的子进程以信号终止即为红控。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/ExecMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/MatchEngine.h>

namespace
{
	const char* const kZ15Code = "SHFE.rb.2405";

	WTSTickData* z15_make_tick(double last, double bid1, double ask1, uint32_t qty, uint32_t msTime)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(WTSTickStruct));
		strcpy(ts.code, kZ15Code);
		ts.action_date = 20240102;
		ts.action_time = msTime;
		ts.trading_date = 20240102;
		ts.price = last;
		ts.pre_close = last - 5.0;
		ts.open = last - 1.0;
		ts.high = last + 1.0;
		ts.low = last - 2.0;
		ts.volume = 100;
		ts.total_volume = 1000;
		ts.turn_over = last * 100;
		ts.total_turnover = last * 1000;
		for (int32_t i = 0; i < 10; i++)
		{
			ts.bid_prices[i] = bid1 - i;
			ts.ask_prices[i] = ask1 + i;
			ts.bid_qty[i] = qty;
			ts.ask_qty[i] = qty;
		}
		return WTSTickData::create(ts);
	}

	class Z15CountingSink : public IMatchSink
	{
	public:
		void handle_trade(uint32_t, const char*, bool, double, double, double, uint64_t) override { ++trades; }
		void handle_order(uint32_t, const char*, bool, double, double, bool, uint64_t) override { ++orders; }
		void handle_entrust(uint32_t, const char*, bool, const char*, uint64_t) override { ++entrusts; }

		uint32_t trades = 0;
		uint32_t orders = 0;
		uint32_t entrusts = 0;
	};

	// 子进程退出码：0 = 符合预期；其余为具体失败点。
	int z15_unsinked_matcher_scenario()
	{
		MatchEngine engine;
		WTSTickData* t1 = z15_make_tick(3800.0, 3799.0, 3801.0, 50, 93000000);
		WTSTickData* t2 = z15_make_tick(3801.0, 3800.0, 3802.0, 50, 93000500);
		WTSTickData* t3 = z15_make_tick(3802.0, 3801.0, 3803.0, 50, 93001000);
		if (t1 == NULL || t2 == NULL || t3 == NULL)
			return 10;

		engine.handle_tick(kZ15Code, t1);
		const OrderIDs buyIds = engine.buy(kZ15Code, 3801.0, 3, 20240102093000000ULL);
		// 旧实现在此进入 fire_orders 并解引用 NULL _sink。
		engine.handle_tick(kZ15Code, t2);
		const std::vector<MatchEngine::OrderSnapshot> pending = engine.get_order_snapshots(kZ15Code);

		// 注册回调后，下一笔 tick 照常激活并上报该挂单。
		Z15CountingSink sink;
		engine.regisSink(&sink);
		engine.handle_tick(kZ15Code, t3);

		t1->release();
		t2->release();
		t3->release();
		if (buyIds.size() != 1)
			return 11;
		if (pending.size() != 1 || pending[0].localid != buyIds[0] || pending[0].state != 0 || pending[0].left != 3.0)
			return 12;
		if (sink.entrusts < 1 || sink.orders < 1)
			return 13;
		return 0;
	}

	// 与 r20 DISABLED_FireOrdersSinkNullAfterPendingOrderAndSecondTick 相同的公开调用序列：
	// 未 init 的 ExecMocker：tick -> buy -> tick。
	int z15_uninitialized_exec_mocker_scenario()
	{
		HisDataReplayer replayer;
		ExecMocker mocker(&replayer);
		WTSTickData* t1 = z15_make_tick(3800.0, 3799.0, 3801.0, 50, 93000000);
		WTSTickData* t2 = z15_make_tick(3801.0, 3800.0, 3802.0, 50, 93000500);
		if (t1 == NULL || t2 == NULL)
			return 20;

		mocker.handle_tick(kZ15Code, t1, 0);
		const OrderIDs ids = mocker.buy(kZ15Code, 3801.0, 3);
		mocker.handle_tick(kZ15Code, t2, 0);
		const double undone = mocker.getUndoneQty(kZ15Code);

		t1->release();
		t2->release();
		if (ids.size() != 1)
			return 21;
		// 未上报任何回报，未完成量保持为下单量（与 r20 撤单用例的记账语义一致）。
		if (undone != 3.0)
			return 22;
		return 0;
	}
}

TEST(Z15MatchEngineUnsinked, TickWithoutSinkKeepsPendingOrdersAndDoesNotCrash)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(std::_Exit(z15_unsinked_matcher_scenario()), ::testing::ExitedWithCode(0), "");
}

TEST(Z15MatchEngineUnsinked, UninitializedExecMockerTickBuyTickDoesNotCrash)
{
	GTEST_FLAG_SET(death_test_style, "threadsafe");
	EXPECT_EXIT(std::_Exit(z15_uninitialized_exec_mocker_scenario()), ::testing::ExitedWithCode(0), "");
}

// 正控：注册回调后，同一序列照常登记订单并在下一笔 tick 上报委托回报。
TEST(Z15MatchEngineUnsinked, RegisteredSinkStillAcceptsAndFiresOrders)
{
	MatchEngine engine;
	Z15CountingSink sink;
	engine.regisSink(&sink);
	WTSTickData* t1 = z15_make_tick(3800.0, 3799.0, 3801.0, 50, 93000000);
	WTSTickData* t2 = z15_make_tick(3801.0, 3800.0, 3802.0, 50, 93000500);
	ASSERT_NE(nullptr, t1);
	ASSERT_NE(nullptr, t2);

	engine.handle_tick(kZ15Code, t1);
	const OrderIDs buyIds = engine.buy(kZ15Code, 3801.0, 3, 20240102093000000ULL);
	ASSERT_EQ(1u, buyIds.size());
	engine.handle_tick(kZ15Code, t2);
	EXPECT_GE(sink.entrusts, 1u);
	EXPECT_GE(sink.orders, 1u);

	t1->release();
	t2->release();
}
