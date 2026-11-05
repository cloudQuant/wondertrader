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

#include <fstream>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/EventNotifier.h>
#include <WtBtCore/HisDataReplayer.h>

#include "r131_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";

	bool write_m5_csv_with_warmup(const std::filesystem::path& base, const char* stdCode)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		if (ec)
			return false;
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_m5.csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		out << "20200102,92500,3797,3801,3796,3798,99,376002,0,0,0\n";
		const uint32_t hhmm[5] = { 93100u, 93200u, 93500u, 93600u, 93700u };
		for (uint32_t i = 0; i < 5; ++i)
		{
			const double close = 3800.0 + i;
			out << "20200102," << hhmm[i] << "," << (close - 1.0) << ","
				<< (close + 2.0) << "," << (close - 2.0) << "," << close << ","
				<< (100 + i) << "," << (100 + i) * close << ",0,0,0\n";
		}
		return out.good();
	}
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
	ASSERT_TRUE(write_m5_csv_with_warmup(_base, kRb));

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
	EXPECT_EQ(1, slice->size());
	slice->release();

	r.rp.run(false);

	EXPECT_EQ(1u, r.sink.replay_done_calls);
	EXPECT_EQ(1u, r.sink.init_calls);
}
