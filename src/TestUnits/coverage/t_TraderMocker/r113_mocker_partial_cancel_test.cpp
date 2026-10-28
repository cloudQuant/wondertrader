/**
 * r113_mocker_partial_cancel_test.cpp  (lane R113 · wave19)
 *
 * 靶子：撮合线程与撤单的并发交汇
 *   - match_once 部分成交：volLeft 保留、状态 PartTraded_Queuing   TraderMocker.cpp:397-434
 *   - orderAction 持 _mtx_awaits 找到订单、开仓直接撤销             :750-828
 *   - ordInfo->setStateMsg("撤单成功") / WOS_Canceled 推送          :809-817
 *
 * 先以 ask 量 1 吃一笔，再撤掉剩余 9 手；全程公开 API + 本机 UDP。
 */
#include "r113_mocker_common.h"

using R113MockerPartialCancel = r113m::MockerTest;

/*
 * 生产: TraderMocker.cpp:397-434（部分成交状态）
 *       750-828（撤单查找、开仓直接撤销、状态与回报）
 */
TEST_F(R113MockerPartialCancel, R113Mocker_PartialFillThenCancelPushesCanceled)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9813, (int)port);
	connect_api();
	login_api();

	/* 开多 10 手，但只给 1 手卖量：撮合后 1 手成交、9 手挂余 */
	insert_order(10.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string order_id = sink_.last_order_id();
	ASSERT_FALSE(order_id.empty());

	send_tick(7000.0, 7000.0, 1.0, 7000.0, 5.0);

	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.has_part_traded_left(9.0); }))
		<< "未观察到 1/10 手部分成交";
	EXPECT_DOUBLE_EQ(1.0, sink_.total_trade_volume());
	EXPECT_FALSE(sink_.has_state((int)WOS_AllTraded));

	/* 撤掉剩余 9 手：orderAction 在 io 线程按 orderID 找回 _awaits 条目。
	 * 注意 action 的引用要保留到撤单回报之后：产品代码在 lambda 里
	 * `action->release()` 之后仍调用 `action->getOrderID()`（BUG-R113-003），
	 * 提前释放会让该行变成 use-after-free。 */
	WTSEntrustAction* action = WTSEntrustAction::create();
	action->setOrderID(order_id.c_str());
	action->setEntrustID("r113-entrust");
	ASSERT_EQ(0, api_.orderAction(action));

	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.has_canceled_message("撤单成功");
	})) << "撤单未推送 WOS_Canceled/撤单成功";

	action->release();

	/* 最终订单状态与成交查询都必须看到 Canceled */
	const uint32_t q = sink_.n_orders_query();
	ASSERT_EQ(0, api_.queryOrders());
	ASSERT_TRUE(r113m::wait_until([this, q]() { return sink_.n_orders_query() >= q + 1u; }));
	{
		const std::vector<r113m::OrderSnap> recs = sink_.queried_orders();
		ASSERT_FALSE(recs.empty());
		EXPECT_EQ((int)WOS_Canceled, recs.back().state);
		EXPECT_EQ("撤单成功", recs.back().state_msg);
		EXPECT_DOUBLE_EQ(9.0, recs.back().vol_left);
	}
}
