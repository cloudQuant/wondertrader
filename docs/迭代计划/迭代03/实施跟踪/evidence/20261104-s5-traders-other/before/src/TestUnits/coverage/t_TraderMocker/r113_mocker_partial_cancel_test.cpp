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

namespace
{
	class ReleasePoisonEntrustAction final : public WTSEntrustAction
	{
	public:
		void release() override
		{
			const uint32_t refs = m_uRefs.fetch_sub(1, std::memory_order_acq_rel);
			if (refs == 1)
			{
				// Keep this test-owned object alive after its final logical release,
				// but make any later order-ID read observable to the test.
				setOrderID("released-action-poison");
				final_release_seen.store(true, std::memory_order_release);
			}
		}

		std::atomic<bool> final_release_seen{ false };
	};
}

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

	/* 先阻塞 io 线程上的 query 回调，使 orderAction 入队后能确定调用方
	 * 先释放引用，再让撤单闭包消费它持有的最后一个引用。 */
	sink_.block_next_orders_query();
	struct QueryUnblocker
	{
		r113m::Sink& sink;
		~QueryUnblocker() { sink.unblock_orders_query(); }
	} query_unblocker{ sink_ };
	const uint32_t q_before = sink_.n_orders_query();
	ASSERT_EQ(0, api_.queryOrders());
	ASSERT_TRUE(sink_.wait_orders_query_blocked()) << "查询回调未能阻塞 io 线程";

	WTSEntrustAction* action = WTSEntrustAction::create();
	action->setOrderID(order_id.c_str());
	action->setEntrustID("r113-entrust");
	ASSERT_EQ(0, api_.orderAction(action));
	/* query callback 仍阻塞 io worker，因此此时闭包持有 action 最后引用。 */
	action->release();
	sink_.unblock_orders_query();
	EXPECT_TRUE(r113m::wait_until([this, q_before]() {
		return sink_.n_orders_query() >= q_before + 1u;
	}));

	ASSERT_TRUE(r113m::wait_until([this]() {
		return sink_.has_canceled_message("撤单成功");
	})) << "撤单未推送 WOS_Canceled/撤单成功";

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

TEST_F(R113MockerPartialCancel, R113Mocker_CancelCopiesOrderIdBeforeFinalActionRelease)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9815, (int)port);
	connect_api();
	login_api();

	insert_order(10.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string order_id = sink_.last_order_id();
	ASSERT_FALSE(order_id.empty());
	send_tick(7000.0, 7000.0, 1.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.has_part_traded_left(9.0); }));

	// Hold the single io worker until after the caller has released its reference.
	sink_.block_next_orders_query();
	struct QueryUnblocker
	{
		r113m::Sink& sink;
		~QueryUnblocker() { sink.unblock_orders_query(); }
	} query_unblocker{ sink_ };
	const uint32_t queries_before = sink_.n_orders_query();
	ASSERT_EQ(0, api_.queryOrders());
	ASSERT_TRUE(sink_.wait_orders_query_blocked());

	auto* action = new ReleasePoisonEntrustAction();
	action->setOrderID(order_id.c_str());
	action->setEntrustID("r113-poison-entrust");
	const uint32_t pushes_before = sink_.n_push_order();
	ASSERT_EQ(0, api_.orderAction(action));
	action->release(); // the queued handler now owns the only logical reference

	// Enqueue a second query behind the action. Its completion proves the handler
	// returned after the final release, so the test can safely destroy its object.
	ASSERT_EQ(0, api_.queryOrders());
	sink_.unblock_orders_query();
	ASSERT_TRUE(r113m::wait_until([this, queries_before]() {
		return sink_.n_orders_query() >= queries_before + 2u;
	}));
	EXPECT_TRUE(action->final_release_seen.load(std::memory_order_acquire));
	EXPECT_STREQ("released-action-poison", action->getOrderID());
	delete action;

	const auto pushes = sink_.orders();
	uint32_t canceled_pushes = 0;
	for (const auto& push : pushes)
	{
		if (push.id == order_id && push.state == (int)WOS_Canceled
			&& push.state_msg == "撤单成功")
			canceled_pushes++;
	}
	EXPECT_EQ(pushes_before + 1u, sink_.n_push_order());
	EXPECT_EQ(1u, canceled_pushes);

	// A second cancel must find the original ID removed from _awaits. If code
	// read the poisoned ID after final release, the original await would remain.
	const uint32_t errors_before = sink_.n_error();
	WTSEntrustAction* duplicate = WTSEntrustAction::createCancelAction(
		"r113-duplicate", order_id.c_str());
	ASSERT_NE(nullptr, duplicate);
	ASSERT_EQ(0, api_.orderAction(duplicate));
	duplicate->release();
	ASSERT_TRUE(r113m::wait_until([this, errors_before]() {
		return sink_.n_error() >= errors_before + 1u;
	}));
	EXPECT_EQ(pushes_before + 1u, sink_.n_push_order());
}

TEST_F(R113MockerPartialCancel, TradedVolumeAccumulatesAcrossPushAndQuerySnapshots)
{
	const uint16_t port = r103m::pick_free_udp_port();
	ASSERT_NE(0, port);
	set_udp_port((int)port);

	init_api(9814, (int)port);
	connect_api();
	login_api();

	insert_order(10.0, 7000.0, WDT_LONG, WOT_OPEN);
	const std::string order_id = sink_.last_order_id();
	ASSERT_FALSE(order_id.empty());

	auto find_order = [](const std::vector<r113m::OrderSnap>& orders, const std::string& id) {
		for (auto it = orders.rbegin(); it != orders.rend(); ++it)
		{
			if (it->id == id)
				return *it;
		}
		return r113m::OrderSnap{};
	};
	auto query_orders = [this]() {
		const uint32_t before = sink_.n_orders_query();
		EXPECT_EQ(0, api_.queryOrders());
		EXPECT_TRUE(r113m::wait_until([this, before]() {
			return sink_.n_orders_query() >= before + 1u;
		}));
		return sink_.queried_orders();
	};

	const r113m::OrderSnap initial_push = find_order(sink_.orders(), order_id);
	EXPECT_EQ(order_id, initial_push.id);
	EXPECT_DOUBLE_EQ(0.0, initial_push.vol_traded);
	EXPECT_DOUBLE_EQ(10.0, initial_push.vol_left);
	const r113m::OrderSnap initial_query = find_order(sink_.queried_orders(), order_id);
	EXPECT_EQ(order_id, initial_query.id);
	EXPECT_DOUBLE_EQ(0.0, initial_query.vol_traded);
	EXPECT_DOUBLE_EQ(10.0, initial_query.vol_left);

	/* Three separate market-data events produce three fills: 1 + 2 + 7 = 10. */
	send_tick(7000.0, 7000.0, 1.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 1.0; }));
	r113m::OrderSnap pushed = find_order(sink_.orders(), order_id);
	EXPECT_EQ(order_id, pushed.id);
	EXPECT_DOUBLE_EQ(1.0, pushed.vol_traded);
	EXPECT_DOUBLE_EQ(9.0, pushed.vol_left);
	r113m::OrderSnap queried = find_order(query_orders(), order_id);
	EXPECT_EQ(order_id, queried.id);
	EXPECT_DOUBLE_EQ(1.0, queried.vol_traded);
	EXPECT_DOUBLE_EQ(9.0, queried.vol_left);

	send_tick(7000.0, 7000.0, 2.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 3.0; }));
	pushed = find_order(sink_.orders(), order_id);
	EXPECT_EQ(order_id, pushed.id);
	EXPECT_DOUBLE_EQ(3.0, pushed.vol_traded);
	EXPECT_DOUBLE_EQ(7.0, pushed.vol_left);
	queried = find_order(query_orders(), order_id);
	EXPECT_EQ(order_id, queried.id);
	EXPECT_DOUBLE_EQ(3.0, queried.vol_traded);
	EXPECT_DOUBLE_EQ(7.0, queried.vol_left);

	send_tick(7000.0, 7000.0, 7.0, 7000.0, 5.0);
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.total_trade_volume() >= 10.0; }));
	ASSERT_TRUE(r113m::wait_until([this]() { return sink_.has_state((int)WOS_AllTraded); }));
	pushed = find_order(sink_.orders(), order_id);
	EXPECT_EQ(order_id, pushed.id);
	EXPECT_EQ((int)WOS_AllTraded, pushed.state);
	EXPECT_DOUBLE_EQ(10.0, pushed.vol_traded);
	EXPECT_DOUBLE_EQ(0.0, pushed.vol_left);
	queried = find_order(query_orders(), order_id);
	EXPECT_EQ(order_id, queried.id);
	EXPECT_EQ((int)WOS_AllTraded, queried.state);
	EXPECT_DOUBLE_EQ(10.0, queried.vol_traded);
	EXPECT_DOUBLE_EQ(0.0, queried.vol_left);

	const std::vector<r113m::TradeSnap> trades = sink_.trades();
	ASSERT_GE(trades.size(), 3u);
	EXPECT_DOUBLE_EQ(10.0, sink_.total_trade_volume());
	for (const r113m::TradeSnap& trade : trades)
		EXPECT_EQ(order_id, trade.ref_order);
}
