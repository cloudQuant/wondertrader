/**
 * r113_mocker_match_short_test.cpp  (lane R113 · wave19)
 *
 * 靶子：TraderMocker match_once 的空头路径
 *   - 卖方向取 bidprice/bidqty                        TraderMocker.cpp:367-392
 *   - 空头开仓累加 / 空头平仓扣减与冻结释放            :450-474
 *   - onPushTrade 与持仓回报                           :477-514
 *
 * 仍只走公开 API；login() 的撮合线程由 TearDown 的 release() join。
 */
#include "r113_mocker_common.h"

using R113MockerMatchShort = r113m::MockerTest;

namespace
{
	const r103m::PosRec* find_short(const std::vector<r103m::PosRec>& recs)
	{
		for (const r103m::PosRec& rec : recs)
		{
			if (rec.direction == (int)WDT_SHORT)
				return &rec;
		}
		return NULL;
	}
}

/*
 * 生产: TraderMocker.cpp:367-392（卖方 bid 侧）
 *       450-474（空头开/平仓的持仓更新）
 *       477-514（成交回报、落盘、清 tick）
 */
TEST_F(R113MockerMatchShort, R113Mocker_ShortOpenAndCloseMatchedByUdpTick)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9812, (int)port);
	connect_api();
	login_api();

	/* 开空 4 手：bid 侧量 4 */
	insert_order(4.0, 7000.0, WDT_SHORT, WOT_OPEN);
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 4.0);

	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 4.0; }))
		<< "空头开仓未成交";
	ASSERT_TRUE(sink_.has_state((int)WOS_AllTraded));

	/* 等撮合线程走完当前 tick 的持仓回写，避免与 queryPositions 的无锁遍历交错 */
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.n_position() >= 1u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		ASSERT_EQ(1u, recs.size());
		const r103m::PosRec* rec = find_short(recs);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(4.0, rec->new_pos);
		EXPECT_DOUBLE_EQ(4.0, rec->avail);
	}

	/* 平空 1 手：ask 侧量 5 -> 成交 1 */
	insert_order(1.0, 7000.0, WDT_SHORT, WOT_CLOSE);
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);

	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 5.0; }))
		<< "空头平仓未成交";
	EXPECT_DOUBLE_EQ(5.0, sink_.total_trade_volume());

	bool saw_close = false;
	for (const r113m::TradeSnap& t : sink_.trades())
	{
		if (t.offset == (int)WOT_CLOSE)
		{
			saw_close = true;
			EXPECT_EQ(t.direction, (int)WDT_SHORT);
		}
	}
	EXPECT_TRUE(saw_close);

	/* 等撮合线程走完当前 tick 的持仓回写，避免与 queryPositions 的无锁遍历交错 */
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.n_position() >= 2u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		const r103m::PosRec* rec = find_short(recs);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(3.0, rec->new_pos);
		EXPECT_DOUBLE_EQ(3.0, rec->avail);
	}
}
