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
#include <vector>

#include <WtBtCore/HisDataReplayer.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

#include "r67_common.h"

namespace
{
	const char* const kCode = "SSE.STK.600000";
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

	std::vector<WTSBarStruct> m5_bars_one_day()
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 935u, 940u, 945u, 950u, 955u, 1000u, 1005u, 1010u };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(20200102u, times[i],
				10.0 + i, 10.4 + i, 9.9 + i, 10.2 + i, 100.0 * (i + 1)));
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

	std::vector<WTSBarStruct> m1_bars_one_day()
	{
		std::vector<WTSBarStruct> bars;
		const uint32_t times[] = { 931u, 932u, 933u, 934u };
		const double prices[] = { 10.0, 11.0, 99.0, 12.0 };
		const double vols[] = { 10.0, 10.0, 0.0, 10.0 };
		for (uint32_t i = 0; i < sizeof(times) / sizeof(times[0]); i++)
			bars.push_back(r67::mk_bar(20200102u, times[i],
				prices[i], prices[i], prices[i], prices[i], vols[i]));
		return bars;
	}

	bool any_tick_at(const r67::Sink& sink, double price)
	{
		return std::any_of(sink.ticks.begin(), sink.ticks.end(),
			[price](const r67::Sink::TickRec& rec) { return rec.price == price; });
	}
}

class R67CsvBars : public r67::Fixture
{
};

// h/f 周期都映射到 KP_Minute5 并走 WTSDataFactory 小时/半小时重采样；
// 第二次取 h 命中缓存，不再读 csv
TEST_F(R67CsvBars, HourAndHalfHourResampleFromM5Csv)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", m5_bars_one_day(), true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	replayer.register_sink(&sink, "r67_hour");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* hour = replayer.get_kline_slice(kCode, "h", 2, 1, true);
	ASSERT_NE(nullptr, hour);
	const int32_t hourCnt = hour->size();
	EXPECT_GT(hourCnt, 0);
	hour->release();

	// 二次取同一 key：走「已定位」分支。首根小时线时间(1000)晚于当前 0930，
	// 游标回退到 0 后按契约返回 NULL（与 p07 m15 同口径，只断言非首次装载）
	WTSKlineSlice* hourAgain = replayer.get_kline_slice(kCode, "h", 2, 1, false);
	if (hourAgain != NULL)
	{
		EXPECT_EQ(hourCnt, hourAgain->size());
		hourAgain->release();
	}

	// "f" 与 "h" 共用 PERIOD_TIMES_HOUR（BUG-Q02-005），当前两者条数相同；
	// 修复为半小时后条数只会更多，故用 GE 保持双向兼容
	WTSKlineSlice* half = replayer.get_kline_slice(kCode, "f", 2, 1, false);
	ASSERT_NE(nullptr, half);
	EXPECT_GT(half->size(), 0);
	EXPECT_GE(half->size(), hourCnt);
	half->release();
}

// _ticker_keys 的三级切换：日线 -> m5 -> m1；随后 m15 直接复用已缓存的 m5 原始线
TEST_F(R67CsvBars, MinuteRequestsNarrowTickerKeyAndReuseRawCache)
{
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m5", m5_bars_one_day(), true));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_m1", m1_bars_one_day(), true));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
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
	// 两个交易日各 4 根 m5，重采样后至少 2 根小时线，
	// 避免「首根被初始定位吃掉后 cursor==size」触发 run_by_bars 的越界读
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
			hourCloses++;
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
		EXPECT_EQ(12u, sink.ticks.size());	// 932/933/934 各 4 笔
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
		EXPECT_EQ(8u, sink.ticks.size());	// 933 的零成交量被 bCanSim 过滤
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

// DISABLED_: 未订阅合约的品种不在 commodity 底座中（commInfo==NULL）时，
// simTickWithUnsubBars 的日线分支先解引用 commInfo->isStock()（HisDataReplayer.cpp:1469-1471），
// 必然空指针崩溃；见 bugs/R67.md BUG-R67-002。
TEST_F(R67CsvBars, DISABLED_UnknownCommodityUnsubBarsDerefsNullCommodity)
{
	GTEST_SKIP() << "simTickWithUnsubBars derefs NULL commInfo for unknown commodity";
	const char* const unknown = "SSE.NOPE.999999";
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(kCode) + "_d", daily_bars(10.0, 3), false));
	ASSERT_TRUE(r67::write_bars_csv(_base, std::string(unknown) + "_d", daily_bars(30.0, 3), false));

	HisDataReplayer replayer;
	WTSVariant* cfg = r67::make_cfg(_root, _base, 202001020930ull, 202001081500ull, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	r67::Sink sink;
	sink.onInit = [&replayer, unknown]() {
		WTSKlineSlice* main = replayer.get_kline_slice(kCode, "d", 1, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(5, unknown);
	};
	replayer.register_sink(&sink, "r67_unknown");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
}
