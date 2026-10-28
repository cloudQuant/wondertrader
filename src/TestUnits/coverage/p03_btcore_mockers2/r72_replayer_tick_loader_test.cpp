/**
 * r72_replayer_tick_loader_test.cpp  (lane R72)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的 loader/tick 面（p07 明确留白的部分）
 *   - get_tick_slice(L2470-L2545) 的游标/etime 全矩阵
 *   - cacheRawTicksFromLoader(L3443-L3468) 成功、空成功
 *   - checkTicks(L2828-L2879) 的 loader 命中、_count==0 与跨日重装
 *   - run_by_bars + replayHftDatas(L1904-L2011) 的 loader tick 成功臂
 *   - get_kline_slice 对 loader 抛异常的两条 catch(L2231-L2242)
 *
 * 全部走公开 API，不注入私有成员；数据只在系统临时目录。
 */
#include <gtest/gtest.h>

#include "r72_common.h"

namespace
{
	const char* const kCode = "SSE.STK.000001";
	const uint32_t kDates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	std::vector<WTSTickStruct> day_ticks(uint32_t date, double base)
	{
		std::vector<WTSTickStruct> ret;
		ret.emplace_back(r72::make_tick_struct(kCode, date, 93000000u, base, base - 0.01, base + 0.01));
		ret.emplace_back(r72::make_tick_struct(kCode, date, 93100000u, base + 0.5, base + 0.49, base + 0.51));
		ret.emplace_back(r72::make_tick_struct(kCode, date, 93200000u, base + 1.0, base + 0.99, base + 1.01));
		return ret;
	}
}

class R72ReplayerTickLoader : public r72::Fixture
{
};

/*
 * get_tick_slice 的 etime 定位矩阵（HisDataReplayer.cpp L2470-L2545）：
 *   etime 落在两笔之间 -> 回退到前一笔；
 *   etime 早于首笔 -> 返回 NULL；
 *   etime 晚于末笔 -> 取末尾 count 笔；
 *   不带 etime 且游标刚初始化 -> _cursor<2 返回 NULL；
 *   跨日后 checkTicks 清空重装（L2840-L2844）。
 */
TEST_F(R72ReplayerTickLoader, TickSliceCursorAndEtimeMatrix)
{
	r72::Loader loader;
	loader.ticks[std::string(kCode) + "#20200102"] = day_ticks(20200102u, 10.0);

	{
		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_tick_a");
		ASSERT_TRUE(rp.prepare());

		WTSTickSlice* s = rp.get_tick_slice(kCode, 2, 202001020931ull);
		ASSERT_NE(nullptr, s);
		ASSERT_EQ(1u, s->size());
		EXPECT_DOUBLE_EQ(10.0, s->at(0)->price);
		s->release();
		EXPECT_EQ(1u, loader.tickCalls);
	}

	{
		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_tick_b");
		ASSERT_TRUE(rp.prepare());

		// etime 早于首笔：lower_bound 命中下标 0，无法回退 -> NULL
		EXPECT_EQ(nullptr, rp.get_tick_slice(kCode, 2, 202001020929ull));
		EXPECT_EQ(2u, loader.tickCalls);
	}

	{
		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_tick_c");
		ASSERT_TRUE(rp.prepare());

		// etime 晚于末笔：lower_bound 到尾，取最后 count 笔（10、11）
		WTSTickSlice* s = rp.get_tick_slice(kCode, 2, 202001021000ull);
		ASSERT_NE(nullptr, s);
		ASSERT_EQ(2u, s->size());
		EXPECT_DOUBLE_EQ(10.0, s->at(0)->price);
		EXPECT_DOUBLE_EQ(11.0, s->at(1)->price);
		s->release();
	}

	{
		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_tick_d");
		ASSERT_TRUE(rp.prepare());

		// 不带 etime、游标刚定位到第 1 笔 -> _cursor<2，返回 NULL
		EXPECT_EQ(nullptr, rp.get_tick_slice(kCode, 2));
	}

	{
		r72::Loader loader2;
		loader2.ticks[std::string(kCode) + "#20200102"] = day_ticks(20200102u, 10.0);
		loader2.ticks[std::string(kCode) + "#20200103"] = day_ticks(20200103u, 20.0);

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001030930ull, 202001031500ull, true);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader2));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_tick_e");
		ASSERT_TRUE(rp.prepare());

		WTSTickData* last = rp.get_last_tick(kCode);
		ASSERT_NE(nullptr, last);
		EXPECT_EQ(20200103u, last->actiondate());
		EXPECT_DOUBLE_EQ(20.0, last->price());
		last->release();
		EXPECT_EQ(1u, loader2.tickCalls);
	}
}

/*
 * loader 同时提供日线和 tick 时，run_by_bars -> replayHftDatas 成功：
 * tick 被逐笔回放、_tick_simulated 置 false、onMinuteEnd 仍按日闭盘，
 * 且回放中的 get_day_price 走 tick 分支（HisDataReplayer.cpp L1904-L2011、
 * L3036-L3066）。tick 表里放一笔 15:00 让 replayHftDatas 由 etime 正常 break。
 */
TEST_F(R72ReplayerTickLoader, LoaderBarsAndTicksDriveBarReplay)
{
	r72::Loader loader;
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
	{
		bars.emplace_back(r72::make_bar(kDates[i], 1500u, 10.5 + i));
		std::vector<WTSTickStruct> ticks = day_ticks(kDates[i], 10.5 + i);
		// 15:00 后再放一笔：getNextTickTime 的游标差一（p07 BUG-P07-001/005）会把
		// 最后一笔当「没有下一笔」，这样 15:00 仍然是能被看到的下一笔。
		ticks.emplace_back(r72::make_tick_struct(kCode, kDates[i], 150100000u,
			10.5 + i + 1.5, 10.5 + i + 1.49, 10.5 + i + 1.51));
		loader.ticks[std::string(kCode) + "#" + std::to_string(kDates[i])] = ticks;
	}
	loader.finalBars[kCode] = bars;

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r72::Sink sink;
	double dayOpen = 0.0, dayHigh = 0.0, dayLow = 0.0, dayLast = 0.0;
	sink.rp = &rp;
	sink.onTick = [&]() {
		if (dayOpen == 0.0 && sink.rp != nullptr)
		{
			dayOpen = sink.rp->get_day_price(kCode, 0);
			dayHigh = sink.rp->get_day_price(kCode, 1);
			dayLow = sink.rp->get_day_price(kCode, 2);
			dayLast = sink.rp->get_day_price(kCode, 3);
		}
	};
	rp.register_sink(&sink, "r72_bar_ticks");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kCode, "d1", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();
	// 主K线登记时会自动订阅 tick；没有订阅的话 replayHftDatas 不会遍历任何合约
	rp.sub_tick(1, kCode);

	rp.run(false);

	EXPECT_EQ(1u, sink.initCalls);
	EXPECT_GE(sink.tickCalls, 1u);
	EXPECT_GE(sink.beginCalls, 4u);
	EXPECT_GE(sink.endCalls, 4u);
	EXPECT_GE(sink.barCalls, 4u);
	EXPECT_LE(sink.barCalls, 5u);
	EXPECT_EQ(1u, sink.doneCalls);
	EXPECT_FALSE(rp.is_tick_simulated());
	EXPECT_EQ(20200108u, rp.get_date());
	EXPECT_EQ(1u, loader.finalCalls);

	EXPECT_GT(dayOpen, 0.0);
	EXPECT_GT(dayHigh, dayOpen);
	EXPECT_LT(dayLow, dayOpen);
	EXPECT_GT(dayLast, 0.0);
}

/*
 * tick loader「返回成功但不回调」：cacheRawTicksFromLoader 把 _count 置 0 后返回 true
 * （L3443-L3468）；首次切片在 L2532 早退，第二次 checkTicks 命中 _count==0（L2845-L2846）
 * 直接拒绝，不再打 loader。
 */
TEST_F(R72ReplayerTickLoader, EmptyTickLoaderZeroCount)
{
	r72::Loader loader;
	loader.emptyTickSuccess = true;

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();
	r72::Sink sink;
	rp.register_sink(&sink, "r72_empty_ticks");
	ASSERT_TRUE(rp.prepare());

	EXPECT_EQ(nullptr, rp.get_tick_slice(kCode, 2));
	EXPECT_EQ(1u, loader.tickCalls);

	EXPECT_EQ(nullptr, rp.get_tick_slice(kCode, 2));
	EXPECT_EQ(1u, loader.tickCalls);
	EXPECT_EQ(nullptr, rp.get_last_tick(kCode));
}

/*
 * loader 在 loadFinalHisBars 抛异常时，get_kline_slice 的两条 catch
 * （std::exception 与 ...，HisDataReplayer.cpp L2231-L2242）都要兜住，
 * 然后回落到 csv 分支并因无文件返回 NULL。
 */
TEST_F(R72ReplayerTickLoader, ThrowingFinalBarsLoaderCaught)
{
	for (int mode = 1; mode <= 2; mode++)
	{
		r72::Loader loader;
		loader.throwFinal = mode;

		HisDataReplayer rp;
		WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
		ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
		cfg->release();
		r72::Sink sink;
		rp.register_sink(&sink, "r72_throw");
		ASSERT_TRUE(rp.prepare());

		EXPECT_EQ(nullptr, rp.get_kline_slice(kCode, "d1", 2, 1, false));
		EXPECT_EQ(1u, loader.finalCalls);
	}
}
