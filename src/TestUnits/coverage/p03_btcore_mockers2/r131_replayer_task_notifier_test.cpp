/**
 * r131_replayer_task_notifier_test.cpp  (lane R131 · wave21)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的公开调度/通知残余分支
 *   - run_by_tasks 日频分支收尾的 notifyEvent("BT_END")（1097）；
 *   - run_by_tasks 分钟频分支收尾的 notifyEvent("BT_END")（1235）；
 *   - run_by_bars 的 notify_state 分钟5周期映射（572-575）：
 *     get_kline_slice("m", 5) + 真实 m5 csv 回放，_period=KP_Minute5 -> "m5"。
 *
 * notifier 用静态存储期、未 init 的 EventNotifier（_publisher 为空，notify 只做空操作/JSON 组装），
 * 不加载 MQ、不起线程。任务显式传 session="SD0930"，避开 BUG-P04-007 的 TRADING 缺省。
 */
#include <gtest/gtest.h>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/EventNotifier.h>
#include <WtBtCore/HisDataReplayer.h>

#include "r131_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
}

class R131Replayer : public r62::ReplayFixture
{
protected:
	// 独立回放器：r62::ReplayFixture 的 _rp 没有 notifier，这里需要带 notifier 的实例。
	// notifier 用静态存储期（先零初始化），规避 R20 台账「EventNotifier 函数指针未初始化」条。
	struct Rig
	{
		HisDataReplayer rp;
		r131::NullSink sink;
	};
};

// 生产: HisDataReplayer.cpp:1089-1098（日频任务收尾，notifyEvent BT_END）
TEST_F(R131Replayer, DailyTaskNotifiesEndWithoutMq)
{
	Rig r;
	static EventNotifier notifier;
	WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
	ASSERT_TRUE(r.rp.init(cfg, &notifier));
	cfg->release();

	r.rp.register_sink(&r.sink, "r131_task_daily");
	r.rp.register_task(1, 0, 1500, "d", "CHINA", "SD0930");

	ASSERT_TRUE(r.rp.prepare());
	r.rp.run(false);

	EXPECT_EQ(1u, r.sink.replay_done_calls);
}

// 生产: HisDataReplayer.cpp:1227-1238（分钟任务收尾，notifyEvent BT_END）
TEST_F(R131Replayer, MinuteTaskNotifiesEndWithoutMq)
{
	Rig r;
	static EventNotifier notifier;
	WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
	ASSERT_TRUE(r.rp.init(cfg, &notifier));
	cfg->release();

	r.rp.register_sink(&r.sink, "r131_task_min");
	r.rp.register_task(2, 0, 5, "min", "CHINA", "SD0930");

	ASSERT_TRUE(r.rp.prepare());
	r.rp.run(false);

	EXPECT_EQ(1u, r.sink.replay_done_calls);
}

// 生产: HisDataReplayer.cpp:569-575（notify_state 的 KP_Minute5 -> "m"{times*5}）
TEST_F(R131Replayer, Minute5BarReplayNotifiesM5State)
{
	ASSERT_TRUE(r131::write_m5_csv(_base, kRb));

	Rig r;
	static EventNotifier notifier;
	WTSVariant* cfg = r62::make_replay_cfg(_root, _base);
	ASSERT_TRUE(r.rp.init(cfg, &notifier));
	cfg->release();

	r.rp.register_sink(&r.sink, "r131_rep_m5");

	// 先 prepare（reset 会清空 _main_key），再登记主K线，最后 run_by_bars
	ASSERT_TRUE(r.rp.prepare());

	WTSKlineSlice* slice = r.rp.get_kline_slice(kRb, "m", 1, 5, true);
	ASSERT_NE(nullptr, slice);
	EXPECT_EQ(1u, slice->size());
	slice->release();

	r.rp.run(false);

	EXPECT_EQ(1u, r.sink.replay_done_calls);
	EXPECT_EQ(1u, r.sink.init_calls);
}
