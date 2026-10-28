/**
 * r113_mocker_match_long_test.cpp  (lane R113 · wave19)
 *
 * 靶子：TraderMocker 公开 login() 线程驱动的 match_once 多头路径
 *   - login 起撮合线程 + 登录回报                     TraderMocker.cpp:723-743
 *   - match_once 主循环：_orders/_ticks/_codes        :328-340
 *   - 买方向取 askprice/askqty、_use_newpx            :367-392
 *   - 限价过滤、splitVolume、成交量/状态更新          :394-434
 *   - 持仓累加、成交/订单回报、落盘、清 tick          :436-514
 *   - 析构释放 _trades                                :98-107
 *
 * 只用公开 API：connect + login + orderInsert + query*，tick 由本机回环 UDP 送入。
 * 不用私有 match_once，不镜像布局；release() 在 TearDown 中 join 撮合线程。
 */
#include "r113_mocker_common.h"

using R113MockerMatchLong = r113m::MockerTest;

namespace
{
	const r103m::PosRec* find_dir(const std::vector<r103m::PosRec>& recs, WTSDirectionType dir)
	{
		for (const r103m::PosRec& rec : recs)
		{
			if (rec.direction == (int)dir)
				return &rec;
		}
		return NULL;
	}
}

/*
 * 生产: TraderMocker.cpp:723-743（login 线程与登录回报）
 *       328-340（撮合入口与 fullcode 拆分）
 *       367-392（买方 ask 侧、_use_newpx）
 *       394-434（限价过滤、拆量、订单状态）
 *       436-514（持仓累加、onPushOrder/onPushTrade、save_positions、_ticks.clear）
 *       98-107（析构释放 _trades）
 */
TEST_F(R113MockerMatchLong, R113Mocker_LongOpenAndCloseMatchedByUdpTick)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9811, (int)port);
	connect_api();
	login_api();

	/* 开多 5 手：ask 侧量 5，价格与委托价相同 -> 全部成交 */
	insert_order(5.0, 7000.0, WDT_LONG, WOT_OPEN);
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);

	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 5.0; }))
		<< "多头开仓未成交";
	ASSERT_TRUE(sink_.has_state((int)WOS_AllTraded));
	EXPECT_DOUBLE_EQ(5.0, sink_.total_trade_volume());

	for (const r113m::TradeSnap& t : sink_.trades())
	{
		EXPECT_EQ(t.code, "ag2512");
		EXPECT_EQ(t.direction, (int)WDT_LONG);
		EXPECT_EQ(t.offset, (int)WOT_OPEN);
	}

	/* 等撮合线程走完当前 tick 的持仓回写，避免与 queryPositions 的无锁遍历交错 */
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.n_position() >= 1u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		ASSERT_EQ(1u, recs.size());
		const r103m::PosRec* rec = find_dir(recs, WDT_LONG);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(5.0, rec->new_pos);
		EXPECT_DOUBLE_EQ(5.0, rec->avail);
	}

	/* 平多 2 手：bid 侧量 5 -> 成交 2，冻结同步释放 */
	insert_order(2.0, 7000.0, WDT_LONG, WOT_CLOSE);
	send_tick(7000.0, 7000.0, 5.0, 7000.0, 5.0);

	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 7.0; }))
		<< "多头平仓未成交";
	EXPECT_DOUBLE_EQ(7.0, sink_.total_trade_volume());

	bool saw_close = false;
	for (const r113m::TradeSnap& t : sink_.trades())
	{
		if (t.offset == (int)WOT_CLOSE)
		{
			saw_close = true;
			EXPECT_EQ(t.direction, (int)WDT_LONG);
		}
	}
	EXPECT_TRUE(saw_close);

	/* 等撮合线程走完当前 tick 的持仓回写，避免与 queryPositions 的无锁遍历交错 */
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(0, api_.queryPositions());
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.n_position() >= 2u; }));
	{
		const std::vector<r103m::PosRec> recs = sink_.last_positions();
		const r103m::PosRec* rec = find_dir(recs, WDT_LONG);
		ASSERT_NE(nullptr, rec);
		EXPECT_DOUBLE_EQ(3.0, rec->new_pos);
		EXPECT_DOUBLE_EQ(3.0, rec->avail);
	}

	/* 成交查询：_trades 已由撮合线程建表 */
	const uint32_t tq = sink_.n_trades_query();
	ASSERT_EQ(0, api_.queryTrades());
	ASSERT_TRUE(r113m::wait_until([this, tq]() { return sink_.n_trades_query() >= tq + 1u; }));
	EXPECT_GT(sink_.queried_trades(), 0u);
}
