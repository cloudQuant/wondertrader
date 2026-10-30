/**
 * r67_replayer_bars_gap_test.cpp  (lane R67)
 *
 * 靶子: src/WtBtCore/HisDataReplayer.cpp 中 csv 模式下尚未被 p04/p07/q02/R43 驱动的
 * 分支（只读生产代码，全部走公开 API）：
 *   - get_kline_slice(L2132) 的 h/f 周期映射(L2186-2202) 与
 *     WTSDataFactory 小时/半小时重采样(L2298-2326)
 *   - _ticker_keys 的「日线 -> 分钟 -> 更小分钟」切换(L2144-2162)
 *   - 已缓存原始 m5 时直接重采样（bHasHisData=true 臂，L2270-2281）
 *   - run_by_bars 以小时线为主K线的分钟分支 + onMinuteEnd 的 h 闭盘分支(L2033-2039)
 *   - cacheRawBarsFromCSV 的 dsb 尺寸校验失败早退(L3793-3800)
 *   - run() 自动选择周期最短主K线(L684-717) 与 dump_btstate 的非日线周期串(L534-540)
 *   - simTicks 的 bCanSim 零成交过滤(L1264-1270)
 *   - simTicks 日线复权代码剥离末位 Q 后回调(L1359-1375)
 * 落盘仅 temp_directory_path()/r67_*，TearDown 清理；dist/common 下的 json 只读。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <WtBtCore/HisDataReplayer.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

#include "r67_common.h"

namespace
{
	const char* const kCode = "SSE.STK.600000";
	const char* const kCodeB = "SSE.STK.600001";
	const char* const kAdjCode = "SSE.STK.600000-";	// 前复权后缀 '-'（CodeHelper.hpp:28）

	const uint32_t kDates[] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	std::vector<WTSBarStruct> daily_bars(double basePx, uint32_t count)
	{
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < count; i++)
			bars.push_back(r67::mk_bar(kDates[i], 1500u,
				basePx + i, basePx + 1.0 + i, basePx - 0.5 + i, basePx + 0.5 + i, 100.0 + 10.0 * i));
		return bars;
	}

	std::vector<WTSBarStruct> m5_bars_one_day(uint32_t date = 20200102u)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 935u, 940u, 945u, 950u, 955u, 1000u, 1005u, 1010u };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(date, times[i],
				10.0 + i, 10.4 + i, 9.9 + i, 10.2 + i, 100.0 * (i + 1)));
		return bars;
	}

	std::vector<WTSBarStruct> m5_bars_complete_halfhour_buckets(uint32_t date, double priceOffset = 0.0)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = {
			935u, 940u, 945u, 950u, 955u, 1000u,
			1005u, 1010u, 1015u, 1020u, 1025u, 1030u
		};
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
		{
			const double price = 10.0 + i + priceOffset;
			bars.push_back(r67::mk_bar(date, times[i], price, price + 0.4,
				price - 0.1, price + 0.2, 100.0 * (i + 1)));
		}
		return bars;
	}

	std::vector<WTSBarStruct> m5_bars_complete_two_hours(uint32_t date)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = {
			935u, 940u, 945u, 950u, 955u, 1000u,
			1005u, 1010u, 1015u, 1020u, 1025u, 1030u,
			1035u, 1040u, 1045u, 1050u, 1055u, 1100u
		};
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
		{
			const double price = 10.0 + i;
			bars.push_back(r67::mk_bar(date, times[i], price, price + 0.4,
				price - 0.1, price + 0.2, 100.0 * (i + 1)));
		}
		return bars;
	}

	std::vector<WTSBarStruct> m5_bars_two_days()
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 935u, 940u, 945u, 950u };
		for (uint32_t d = 0; d < 2; d++)
		{
			for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
				bars.push_back(r67::mk_bar(kDates[d], times[i],
					10.0 + i + d, 10.4 + i + d, 9.9 + i + d, 10.2 + i + d, 100.0 * (i + 1)));
		}
		return bars;
	}

	std::vector<WTSBarStruct> m1_bars_one_day(uint32_t date = 20200102u)
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 931u, 932u, 933u, 934u };
		const double prices[] = { 10.0, 11.0, 99.0, 12.0 };
		const double vols[] = { 10.0, 10.0, 0.0, 10.0 };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(date, times[i],
				prices[i], prices[i], prices[i], prices[i], vols[i]));
		return bars;
	}

	bool any_tick_at(const r67::Sink& sink, double price)
	{
		return std::any_of(sink.ticks.begin(), sink.ticks.end(),
			[price](const r67::Sink::TickRec& rec) { return rec.price == price; });
	}

	uint32_t ticks_of(const r67::Sink& sink, const char* code)
	{
		return static_cast<uint32_t>(std::count_if(sink.ticks.begin(), sink.ticks.end(),
			[code](const r67::Sink::TickRec& rec) { return rec.code == code; }));
	}
}

class R67CsvBars : public r67::Fixture
{
};

class R67HalfHourReplaySink : public r67::Sink
{
public:
	std::vector<WTSBarStruct> halfHourCloses;

	void handle_bar_close(const char* stdCode, const char* period,
		uint32_t times, WTSBarStruct* newBar) override
	{
		if (period != NULL && strcmp(period, "f") == 0 && newBar != NULL)
			halfHourCloses.push_back(*newBar);
		r67::Sink::handle_bar_close(stdCode, period, times, newBar);
	}
};

// h/f 周期都映射到 KP_Minute5 并走 WTSDataFactory 小时/半小时重采样；
// 第二次取 h 命中缓存，不再读 csv
TEST_F(R67CsvBars, HourAndHalfHourResampleFromM5Csv)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5",
		m5_bars_complete_two_hours(20200102u), true));

	HisDataReplayer replayer;
	// At 10:30 the two half-hour bars have closed, but the 11:00 hourly bar
	// has not. Do not require the history query to return future bars.
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001021030ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_hour");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* hour = replayer.get_kline_slice(kCode, "h", 1, 1, true);
	ASSERT_NE(nullptr, hour);
	const int32_t hourCnt = hour->size();
	ASSERT_EQ(1, hourCnt);
	ASSERT_NE(nullptr, hour->at(0));
	EXPECT_EQ(20200102u, hour->at(0)->date);
	EXPECT_EQ(1000u, static_cast<uint32_t>(hour->at(0)->time % 10000u));
	EXPECT_DOUBLE_EQ(10.0, hour->at(0)->open);
	EXPECT_DOUBLE_EQ(15.4, hour->at(0)->high);
	EXPECT_DOUBLE_EQ(9.9, hour->at(0)->low);
	EXPECT_DOUBLE_EQ(15.2, hour->at(0)->close);
	EXPECT_DOUBLE_EQ(2100.0, hour->at(0)->vol);
	hour->release();

	// The second request uses the already loaded series and retains its bounded result.
	WTSKlineSlice* hourAgain = replayer.get_kline_slice(kCode, "h", 1, 1, false);
	if (hourAgain != NULL)
	{
		EXPECT_EQ(hourCnt, hourAgain->size());
		hourAgain->release();
	}

	// 半小时组以 10:00、10:30 收盘；小时对照的第二组应为 11:00。
	WTSKlineSlice* half = replayer.get_kline_slice(kCode, "f", 2, 1, false);
	ASSERT_NE(nullptr, half);
	ASSERT_EQ(2, half->size());
	ASSERT_NE(nullptr, half->at(0));
	ASSERT_NE(nullptr, half->at(1));
	EXPECT_EQ(20200102u, half->at(0)->date);
	EXPECT_EQ(20200102u, half->at(1)->date);
	EXPECT_EQ(1000u, static_cast<uint32_t>(half->at(0)->time % 10000u));
	EXPECT_EQ(1030u, static_cast<uint32_t>(half->at(1)->time % 10000u));
	EXPECT_DOUBLE_EQ(10.0, half->at(0)->open);
	EXPECT_DOUBLE_EQ(15.4, half->at(0)->high);
	EXPECT_DOUBLE_EQ(9.9, half->at(0)->low);
	EXPECT_DOUBLE_EQ(15.2, half->at(0)->close);
	EXPECT_DOUBLE_EQ(2100.0, half->at(0)->vol);
	EXPECT_DOUBLE_EQ(16.0, half->at(1)->open);
	EXPECT_DOUBLE_EQ(21.4, half->at(1)->high);
	EXPECT_DOUBLE_EQ(15.9, half->at(1)->low);
	EXPECT_DOUBLE_EQ(21.2, half->at(1)->close);
	EXPECT_DOUBLE_EQ(5700.0, half->at(1)->vol);
	half->release();

	// At the exact 11:00 close, the second hourly bar is now queryable. Its
	// fixture contains all twelve m5 bars from 10:05 through 11:00.
	HisDataReplayer secondReplayer;
	WTSVariant* secondCfg = r67::make_cfg(_root, _base, 202001021100ull, 202001081500ull, false);
	ASSERT_TRUE(secondReplayer.init(secondCfg));
	secondCfg->release();
	r67::Sink secondSink;
	secondReplayer.register_sink(&secondSink, "r67_hour_at_close");
	ASSERT_TRUE(secondReplayer.prepare());
	WTSKlineSlice* hoursAtClose = secondReplayer.get_kline_slice(kCode, "h", 2, 1, true);
	ASSERT_NE(nullptr, hoursAtClose);
	ASSERT_EQ(2, hoursAtClose->size());
	ASSERT_NE(nullptr, hoursAtClose->at(1));
	EXPECT_EQ(20200102u, hoursAtClose->at(1)->date);
	EXPECT_EQ(1100u, static_cast<uint32_t>(hoursAtClose->at(1)->time % 10000u));
	EXPECT_DOUBLE_EQ(16.0, hoursAtClose->at(1)->open);
	EXPECT_DOUBLE_EQ(27.4, hoursAtClose->at(1)->high);
	EXPECT_DOUBLE_EQ(15.9, hoursAtClose->at(1)->low);
	EXPECT_DOUBLE_EQ(27.2, hoursAtClose->at(1)->close);
	EXPECT_DOUBLE_EQ(15000.0, hoursAtClose->at(1)->vol);
	hoursAtClose->release();
}

TEST_F(R67CsvBars, MinutePeriodQueriesDoNotExposeBarsBeforeTheirFirstClose)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m1", m1_bars_one_day(), true));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5",
		m5_bars_complete_two_hours(20200102u), true));

	// At 09:30 the first source minute bar is 09:31, and the first derived
	// m5/m15/f/h bars close later. Every request must remain empty here.
	HisDataReplayer beforeFirst;
	WTSVariant* beforeCfg = r67::make_cfg(_root, _base, 202001020930ull, 202001021100ull, false);
	ASSERT_TRUE(beforeFirst.init(beforeCfg));
	beforeCfg->release();
	r67::Sink beforeSink;
	std::vector<bool> availableBeforeFirst;
	const std::pair<const char*, uint32_t> periods[] = {
		{ "m", 1u }, { "m", 5u }, { "m", 15u }, { "f", 1u }, { "h", 1u }
	};
	beforeSink.onInit = [&beforeFirst, &availableBeforeFirst, &periods]() {
		for (const auto& period : periods)
		{
			WTSKlineSlice* slice = beforeFirst.get_kline_slice(kCode, period.first, 1, period.second,
				period.first[0] == 'm' && period.second == 1u);
			availableBeforeFirst.push_back(slice != NULL);
			if (slice != NULL)
				slice->release();
		}
	};
	beforeFirst.register_sink(&beforeSink, "r67_no_forward_before_first");
	ASSERT_TRUE(beforeFirst.prepare());
	EXPECT_EQ(std::vector<bool>({ false, false, false, false, false }), availableBeforeFirst);

	// At each exact first-close timestamp, that period's first bar is readable.
	// This pairs the no-lookahead assertions with the inclusive close boundary.
	struct ExactCloseCase
	{
		const char* period;
		uint32_t times;
		uint64_t begin;
		uint32_t expectedClose;
	};
	const ExactCloseCase exactCases[] = {
		{ "m", 1u, 202001020931ull, 931u },
		{ "m", 5u, 202001020935ull, 935u },
		{ "m", 15u, 202001020945ull, 945u },
		{ "f", 1u, 202001021000ull, 1000u },
		{ "h", 1u, 202001021000ull, 1000u }
	};
	for (const ExactCloseCase& testCase : exactCases)
	{
		SCOPED_TRACE(testCase.period);
		HisDataReplayer exactClose;
		WTSVariant* exactCfg = r67::make_cfg(_root, _base, testCase.begin, 202001021100ull, false);
		ASSERT_TRUE(exactClose.init(exactCfg));
		exactCfg->release();
		r67::Sink exactSink;
		bool found = false;
		uint32_t actualClose = 0;
		exactSink.onInit = [&exactClose, &testCase, &found, &actualClose]() {
			WTSKlineSlice* slice = exactClose.get_kline_slice(kCode, testCase.period,
				1, testCase.times, true);
			if (slice != NULL)
			{
				if (slice->size() > 0)
				{
					found = true;
					actualClose = static_cast<uint32_t>(slice->at(slice->size() - 1)->time % 10000u);
				}
				slice->release();
			}
		};
		exactClose.register_sink(&exactSink, "r67_exact_first_close");
		ASSERT_TRUE(exactClose.prepare());
		EXPECT_TRUE(found);
		EXPECT_EQ(testCase.expectedClose, actualClose);
	}
}

TEST_F(R67CsvBars, HalfHourBarsReplayEmitsHalfHourCloses)
{
	std::vector<WTSBarStruct> bars = m5_bars_complete_halfhour_buckets(20200102u);
	std::vector<WTSBarStruct> secondDayBars = m5_bars_complete_halfhour_buckets(20200103u, 1.0);
	bars.insert(bars.end(), secondDayBars.begin(), secondDayBars.end());
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", bars, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001031500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	R67HalfHourReplaySink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "f", 1, 1, true);
		if (main != NULL)
			main->release();
	};
	replayer.register_sink(&sink, "r67_halfhourrun");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);

	uint32_t halfCloses = 0;
	for (const r67::Sink::BarRec& rec : sink.bars)
	{
		if (rec.period == "f")
		{
			halfCloses++;
			EXPECT_EQ(1u, rec.times);
			EXPECT_TRUE(rec.date == 20200102u || rec.date == 20200103u);
		}
		EXPECT_NE("h", rec.period);
	}
	EXPECT_EQ(4u, halfCloses);
	ASSERT_EQ(4u, sink.halfHourCloses.size());
	const uint32_t expectedDates[] = { 20200102u, 20200102u, 20200103u, 20200103u };
	const uint64_t expectedTimes[] = {
		(20200102u - 19900000u) * 10000ull + 1000ull,
		(20200102u - 19900000u) * 10000ull + 1030ull,
		(20200103u - 19900000u) * 10000ull + 1000ull,
		(20200103u - 19900000u) * 10000ull + 1030ull
	};
	const double expectedOpens[] = { 10.0, 16.0, 11.0, 17.0 };
	const double expectedHighs[] = { 15.4, 21.4, 16.4, 22.4 };
	const double expectedLows[] = { 9.9, 15.9, 10.9, 16.9 };
	const double expectedCloses[] = { 15.2, 21.2, 16.2, 22.2 };
	const double expectedVolumes[] = { 2100.0, 5700.0, 2100.0, 5700.0 };
	for (std::size_t i = 0; i < sink.halfHourCloses.size(); i++)
	{
		const WTSBarStruct& bar = sink.halfHourCloses[i];
		EXPECT_EQ(expectedDates[i], bar.date);
		EXPECT_EQ(expectedTimes[i], bar.time);
		EXPECT_DOUBLE_EQ(expectedOpens[i], bar.open);
		EXPECT_DOUBLE_EQ(expectedHighs[i], bar.high);
		EXPECT_DOUBLE_EQ(expectedLows[i], bar.low);
		EXPECT_DOUBLE_EQ(expectedCloses[i], bar.close);
		EXPECT_DOUBLE_EQ(expectedVolumes[i], bar.vol);
	}
	EXPECT_EQ(1u, sink.doneCalls);
}

TEST_F(R67CsvBars, HalfHourReplayStartsAtRequestedBarBoundary)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5",
		m5_bars_complete_halfhour_buckets(20200102u), true));

	struct ReplayStartCase
	{
		const char* name;
		uint64_t begin;
		uint32_t expectedCount;
		uint32_t expectedTimes[2];
	};
	const ReplayStartCase cases[] = {
		{ "before first", 202001020930ull, 2u, { 1000u, 1030u } },
		{ "exactly first", 202001021000ull, 2u, { 1000u, 1030u } },
		{ "between bars", 202001021015ull, 1u, { 1030u, 0u } },
		{ "exactly last", 202001021030ull, 1u, { 1030u, 0u } }
	};

	const double expectedOpens[] = { 10.0, 16.0 };
	const double expectedHighs[] = { 15.4, 21.4 };
	const double expectedLows[] = { 9.9, 15.9 };
	const double expectedCloses[] = { 15.2, 21.2 };
	const double expectedVolumes[] = { 2100.0, 5700.0 };

	for (const ReplayStartCase& replayCase : cases)
	{
		SCOPED_TRACE(replayCase.name);
		HisDataReplayer replayer;
		WTSVariant* cfg = r67::make_cfg(_root, _base, replayCase.begin, 202001021030ull, false);
		ASSERT_TRUE(replayer.init(cfg));
		cfg->release();

		R67HalfHourReplaySink sink;
		sink.onInit = [&replayer]() {
			// This history query advances the same cache cursor that replay used
			// to consume. Replay must reinitialize its own range at run start.
			WTSKlineSlice* main = replayer.get_kline_slice(kCode, "f", 2, 1, true);
			if (main != NULL)
				main->release();
		};
		replayer.register_sink(&sink, "r67_halfhour_boundary");
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);

		ASSERT_EQ(replayCase.expectedCount, sink.halfHourCloses.size());
		for (uint32_t i = 0; i < replayCase.expectedCount; i++)
		{
			const uint32_t bucket = replayCase.expectedTimes[i] == 1000u ? 0u : 1u;
			const WTSBarStruct& bar = sink.halfHourCloses[i];
			EXPECT_EQ(20200102u, bar.date);
			EXPECT_EQ((20200102u - 19900000u) * 10000ull + replayCase.expectedTimes[i], bar.time);
			EXPECT_DOUBLE_EQ(expectedOpens[bucket], bar.open);
			EXPECT_DOUBLE_EQ(expectedHighs[bucket], bar.high);
			EXPECT_DOUBLE_EQ(expectedLows[bucket], bar.low);
			EXPECT_DOUBLE_EQ(expectedCloses[bucket], bar.close);
			EXPECT_DOUBLE_EQ(expectedVolumes[bucket], bar.vol);
		}

		uint32_t fCloses = 0;
		for (const r67::Sink::BarRec& rec : sink.bars)
		{
			if (rec.period == "f")
				fCloses++;
			EXPECT_NE("h", rec.period);
		}
		EXPECT_EQ(replayCase.expectedCount, fCloses);
	}
}

// _ticker_keys 的三级切换：日线 -> m5 -> m1；随后 m15 直接复用已缓存的 m5 原始线
TEST_F(R67CsvBars, MinuteRequestsNarrowTickerKeyAndReuseRawCache)
{
	// Keep a prior closed daily bar available while the replay is at the next
	// day's first derived m15 close. The current-day daily bar remains open,
	// while m1, m5, and m15 each have a bar closed by 09:45.
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 2), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", m5_bars_one_day(20200103u), true));
	std::vector<WTSBarStruct> m1Bars = m1_bars_one_day(20200103u);
	m1Bars.push_back(r67::mk_bar(20200103u, 945u, 15.0, 15.4, 14.9, 15.2, 50.0));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m1", m1Bars, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001030945ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_key");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* daily = replayer.get_kline_slice(kCode, "d", 2, 1, false);
	ASSERT_NE(nullptr, daily);
	EXPECT_GT(daily->size(), 0);
	daily->release();

	// oldKey 以 'd' 开头且新周期是 "m" -> 替换并记录 _min_period
	WTSKlineSlice* m5 = replayer.get_kline_slice(kCode, "m", 2, 5, false);
	ASSERT_NE(nullptr, m5);
	EXPECT_GT(m5->size(), 0);
	m5->release();

	// 同首字母且 times 更小 -> 继续替换
	WTSKlineSlice* m1 = replayer.get_kline_slice(kCode, "m", 2, 1, false);
	ASSERT_NE(nullptr, m1);
	EXPECT_GT(m1->size(), 0);
	m1->release();

	// m15: rawKey(<code>#m#5) 已在缓存 -> bHasHisData=true，直接重采样
	WTSKlineSlice* m15 = replayer.get_kline_slice(kCode, "m", 2, 15, false);
	ASSERT_NE(nullptr, m15);
	EXPECT_GT(m15->size(), 0);
	m15->release();
}

// 小时线作为主K线跑完整回放：run_by_bars 的非日线分支 + onMinuteEnd 的 "h" 闭盘
TEST_F(R67CsvBars, HourBarsReplayEmitsHourCloses)
{
	// 两个交易日各 4 根 m5，重采样后跨日验证多个小时线闭盘。
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", m5_bars_two_days(), true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001031500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "h", 1, 1, true);
		if (main != NULL)
			main->release();
	};
	replayer.register_sink(&sink, "r67_hourrun");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);

	uint32_t hourCloses = 0;
	for (const r67::Sink::BarRec& rec : sink.bars)
	{
		if (rec.period == "h")
		{
			hourCloses++;
			EXPECT_EQ(1u, rec.times);
		}
	}
	EXPECT_GT(hourCloses, 0u);
	EXPECT_EQ(1u, sink.doneCalls);
	// 小时线闭盘必须发生在本次回放覆盖的两个交易日内
	for (const r67::Sink::BarRec& rec : sink.bars)
	{
		if (rec.period == "h")
			EXPECT_TRUE(rec.date == 20200102u || rec.date == 20200103u);
	}
}

// his/day 下同名的 dsb 尺寸小于块头 -> cacheRawBarsFromCSV 直接失败，不回落到 csv
TEST_F(R67CsvBars, ShortBarDsbRejectedBeforeCsv)
{
	ASSERT_TRUE(r67::write_text(_base / "his" / "day" / "SSE" / "600000.dsb", "short"));
	// 即使 csv 存在，dsb 分支一旦命中就不再读 csv；本轮断言的是尺寸校验失败后的 NULL
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 3), false));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_shortdsb");
	ASSERT_TRUE(replayer.prepare());

	EXPECT_EQ(nullptr, replayer.get_kline_slice(kCode, "d", 3, 1, true));
}

// 不指定主K线时 run() 自动选周期最短者（m5 优先于日线），dump_btstate 写出 m5
TEST_F(R67CsvBars, AutoMainPicksShortestPeriodAndDumpsState)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 2), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", m5_bars_two_days(), true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001031500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* daily = replayer.get_kline_slice(kCode, "d", 1, 1, false);
		if (daily != NULL)
			daily->release();
		WTSKlineSlice* m5 = replayer.get_kline_slice(kCode, "m", 1, 5, false);
		if (m5 != NULL)
			m5->release();
	};
	replayer.register_sink(&sink, "r67_automain");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(true);

	uint32_t m5Closes = 0;
	for (const r67::Sink::BarRec& rec : sink.bars)
	{
		if (rec.period == "m" && rec.times == 5)
			m5Closes++;
	}
	EXPECT_GT(m5Closes, 0u);
	EXPECT_EQ(1u, sink.doneCalls);

	const std::filesystem::path env = _out / "r67_automain" / "btenv.json";
	ASSERT_TRUE(std::filesystem::exists(env));
	std::ifstream in(env);
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string content = ss.str();
	EXPECT_NE(std::string::npos, content.find("\"period\""));
	EXPECT_NE(std::string::npos, content.find("m5"));
	EXPECT_NE(std::string::npos, content.find(kCode));
}

// dont_simtick_if_notrade=true 时成交量为 0 的K线不产生模拟tick；
// 同一份数据在开关关闭时会产出该价格，形成对照
TEST_F(R67CsvBars, ZeroVolumeBarsSkippedWithNoSimFlag)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m1", m1_bars_one_day(), true));

	{
		HisDataReplayer replayer;
		WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false, 10, false);
		ASSERT_TRUE(replayer.init(cfg));
		cfg->release();
		r67::Sink sink;
		sink.onInit = [&replayer]() {
			WTSKlineSlice* main = replayer.get_kline_slice(kCode, "m", 1, 1, true);
			if (main != NULL)
				main->release();
		};
		replayer.register_sink(&sink, "r67_nosim_off");
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);
		EXPECT_EQ(16u, sink.ticks.size());	// 931/932/933/934 各 4 笔
		EXPECT_TRUE(any_tick_at(sink, 99.0));
	}

	{
		HisDataReplayer replayer;
		WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false, 10, true);
		ASSERT_TRUE(replayer.init(cfg));
		cfg->release();
		r67::Sink sink;
		sink.onInit = [&replayer]() {
			WTSKlineSlice* main = replayer.get_kline_slice(kCode, "m", 1, 1, true);
			if (main != NULL)
				main->release();
		};
		replayer.register_sink(&sink, "r67_nosim_on");
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);
		EXPECT_EQ(12u, sink.ticks.size());	// 933 的零成交量被 bCanSim 过滤
		EXPECT_FALSE(any_tick_at(sink, 99.0));
	}
}

// 日线复权代码（单字符 Q 后缀）作为主K线：simTicks 回调时剥离后缀，
// 闭盘仍用带后缀的原始代码；同时 checkUnbars 装载未订阅的原始日线
TEST_F(R67CsvBars, AdjustedDailyBarsSimulateStrippedTicks)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kAdjCode) + "_d", daily_bars(10.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(20.0, 3), false));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kAdjCode, "d", 1, 1, true);
		if (main != NULL)
			main->release();
	};
	replayer.register_sink(&sink, "r67_qfq");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);

	ASSERT_FALSE(sink.ticks.empty());
	for (const r67::Sink::TickRec& rec : sink.ticks)
		EXPECT_EQ(std::string(kCode), rec.code);
	ASSERT_FALSE(sink.bars.empty());
	for (const r67::Sink::BarRec& rec : sink.bars)
		EXPECT_EQ(std::string(kAdjCode), rec.code);
}

// 未知品种以及缺少 session 元数据的品种不能生成合成 tick；
// 同一回放里的已知品种仍应正常生成。
TEST_F(R67CsvBars, UnsubBarsWithoutCommodityOrSessionAreSkippedSafely)
{
	const char* const unknown = "SSE.NOPE.999999";
	const char* const noSession = "SSE.R67_NO_SESSION";
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCodeB) + "_d", daily_bars(20.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(unknown) + "_d", daily_bars(30.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(noSession) + "_d", daily_bars(40.0, 3), false));

	const std::filesystem::path contractsFile = _base / "contracts_missing_session.json";
	ASSERT_TRUE(r67::write_text(contractsFile,
		R"({"SSE":{"R67_NO_SESSION":{"name":"R67 missing session","rules":{"session":"R67_SESSION_DOES_NOT_EXIST","category":0,"pricetick":0.01,"volscale":1}}}})"));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	cfg->get("basefiles")->append("contract", contractsFile.string().c_str());
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	sink.onInit = [&replayer, unknown, noSession]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "d", 1, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(4, kCodeB);
		replayer.sub_tick(5, unknown);
		replayer.sub_tick(6, noSession);
	};
	replayer.register_sink(&sink, "r67_unknown");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_GT(ticks_of(sink, kCodeB), 0u);
	EXPECT_EQ(0u, ticks_of(sink, kCodeB) % 4);
	EXPECT_EQ(0u, ticks_of(sink, unknown));
	EXPECT_EQ(0u, ticks_of(sink, noSession));
}
