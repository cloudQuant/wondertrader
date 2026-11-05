/**
 * p07_minute_replay_test.cpp
 * 覆盖 HisDataReplayer 的「分钟级回放主循环」（p04 只做了日线）：
 *   cacheRawBarsFromCSV(L3737) 的 KP_Minute1 / KP_Minute5 分支（csv 装载 + his/min1、min5 目录转储）
 *   run_by_bars(L771) 非日线分支（L795-850 逐分钟交易日推进、calcTradingDate/isEndTDate）
 *   simTicks(L1243) 分钟线分支（L1248-1323：_day_cache 累积 + update_price + handle_tick 开高低收）
 *   simTickWithUnsubBars(L1389) 未订阅K线模拟tick分支（由 checkUnbars(L3167)+sub_tick(L3089) 喂数据）
 *   onMinuteEnd(L2013) 分钟线闭盘与 _unbars_cache 游标推进（L2023-2057、L2085-2126）
 *   get_kline_slice(L2132) 的 m15 重采样合并分支（L2213-2221、L2298-2326）与分钟游标二次定位（L2441-2454）
 *   get_day_price(L3034) / get_cur_price(L3021) 的「有数据」路径（p04 只覆盖空缓存路径）
 * 时间约定：SD0930（dist/common/sessions.json）0930-1130、1300-1500，收盘 1500。
 * WTSBarStruct.time = (date-19900000)*10000 + HHMM，故 nextBarTime = time + 199000000000。
 * 所有落盘都在 temp_directory_path()/p07_* 下，TearDown 清理，不写仓库。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path lane_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("p07_min_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	struct CsvBar
	{
		uint32_t date;
		uint32_t time;
		double open;
		double high;
		double low;
		double close;
		double vol;
	};

	// SSE.STK.600000 的九根 1 分钟线，首根也属于请求回放区间。
	const CsvBar kM1Bars[] = {
		{20200102u, 931u,   9.9, 10.1,  9.7,  10.0,  10},
		{20200102u, 932u,  10.0, 10.5,  9.8,  10.2,  20},
		{20200102u, 933u,  10.2, 11.0, 10.1,  10.8,  30},
		{20200102u, 1458u, 10.8, 12.0, 10.6,  11.5,  40},
		{20200102u, 1459u, 11.5, 11.8, 11.2,  11.6,  50},
		{20200102u, 1500u, 11.6, 12.5, 11.4,  12.0,  60},
		{20200103u, 931u,  12.0, 12.4, 11.9,  12.2,  70},
		{20200103u, 932u,  12.2, 13.0, 12.1,  12.8,  80},
		{20200103u, 1500u, 12.8, 13.5, 12.6,  13.0,  90},
	};
	const uint32_t kM1Cnt = sizeof(kM1Bars) / sizeof(kM1Bars[0]);
	const uint32_t kM1Closed = kM1Cnt;

	// SSE.STK.600000 的 5 分钟线，用于 m15 重采样（bar 合并）
	const CsvBar kM5Bars[] = {
		{20200102u, 935u,  10.0, 10.4,  9.9, 10.2, 100},
		{20200102u, 940u,  10.2, 11.0, 10.1, 10.8, 200},
		{20200102u, 945u,  10.8, 11.5, 10.6, 11.4, 300},
		{20200102u, 950u,  11.4, 12.0, 11.3, 11.9, 400},
	};
	const uint32_t kM5Cnt = sizeof(kM5Bars) / sizeof(kM5Bars[0]);

	// 未订阅合约的日线（走 checkUnbars + simTickWithUnsubBars）
	const CsvBar kDayBars[] = {
		{20200102u, 1500u, 10.0, 11.0,  9.5, 10.5, 100},
		{20200103u, 1500u, 10.5, 12.0, 10.4, 11.5, 200},
		{20200106u, 1500u, 11.5, 13.0, 11.4, 12.5, 300},
	};
	const uint32_t kDayCnt = sizeof(kDayBars) / sizeof(kDayBars[0]);

	bool write_bars_csv(const std::filesystem::path& base, const std::string& fileTag,
		const CsvBar* bars, uint32_t cnt, bool withRows)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (fileTag + ".csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		if (withRows)
		{
			for (uint32_t i = 0; i < cnt; i++)
				out << bars[i].date << ',' << bars[i].time << ','
					<< bars[i].open << ',' << bars[i].high << ','
					<< bars[i].low << ',' << bars[i].close << ','
					<< bars[i].vol << ',' << bars[i].vol * 10.0 << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	WTSVariant* replay_cfg_text(const std::filesystem::path& root, const std::filesystem::path& base,
		const char* stime, const char* etime, bool tickMode = false,
		const char* commodityFile = "stk_comms.json")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tickMode);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common" / commodityFile).string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	WTSVariant* replay_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, bool tickMode = false)
	{
		const std::string beginText = std::to_string(stime);
		const std::string endText = std::to_string(etime);
		return replay_cfg_text(root, base, beginText.c_str(), endText.c_str(), tickMode);
	}

	class RecordingSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		std::function<void(const char*, uint32_t /*pxType*/)> onTickProbe;
		std::function<void()> onBarCloseProbe;
		WTSKlineSlice* slice {nullptr};
		HisDataReplayer* rp {nullptr};
		std::string dayPriceCode;

		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t sectionCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t replayDoneCalls {0};

		std::vector<std::string> tickCodes;
		std::vector<uint32_t> tickPxTypes;
		std::vector<double> tickPrices;
		std::vector<uint32_t> tickDates;
		std::vector<uint32_t> tickTimes;

		std::vector<std::string> closedCodes;
		std::vector<std::string> closedPeriods;
		std::vector<uint32_t> closedDates;
		std::vector<uint32_t> closedTimes;
		std::vector<uint32_t> closedMultipliers;
		std::vector<double> closedOpens;
		std::vector<double> closedHighs;
		std::vector<double> closedLows;
		std::vector<double> closedPrices;
		std::vector<double> closedVolumes;
		std::vector<double> curPriceAtClose;
		std::vector<double> dayOpenAtClose;
		std::vector<double> dayHighAtClose;
		std::vector<double> dayLowAtClose;
		std::vector<uint32_t> scheduleDates;
		std::vector<uint32_t> scheduleTimes;

		void handle_init() override
		{
			initCalls++;
			if (onInit)
				onInit();
		}

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			tickCodes.emplace_back(stdCode);
			tickPxTypes.push_back(pxType);
			if (curTick != nullptr)
			{
				const WTSTickStruct& tick = curTick->getTickStruct();
				tickPrices.push_back(tick.price);
				tickDates.push_back(tick.action_date);
				tickTimes.push_back(tick.action_time / 100000u);
			}
			if (onTickProbe)
				onTickProbe(stdCode, pxType);
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			closedCodes.emplace_back(stdCode);
			closedPeriods.emplace_back(period);
			closedMultipliers.push_back(times);
			if (newBar != nullptr)
			{
				closedDates.push_back(newBar->date);
				closedTimes.push_back(static_cast<uint32_t>(newBar->time));
				closedOpens.push_back(newBar->open);
				closedHighs.push_back(newBar->high);
				closedLows.push_back(newBar->low);
				closedPrices.push_back(newBar->close);
				closedVolumes.push_back(newBar->vol);
			}
			if (onBarCloseProbe)
				onBarCloseProbe();
			if (rp != nullptr && !dayPriceCode.empty())
			{
				curPriceAtClose.push_back(rp->get_cur_price(dayPriceCode.c_str()));
				dayOpenAtClose.push_back(rp->get_day_price(dayPriceCode.c_str(), 0));
				dayHighAtClose.push_back(rp->get_day_price(dayPriceCode.c_str(), 1));
				dayLowAtClose.push_back(rp->get_day_price(dayPriceCode.c_str(), 2));
			}
		}

		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			scheduleCalls++;
			scheduleDates.push_back(uDate);
			scheduleTimes.push_back(uTime);
		}

		void handle_session_begin(uint32_t curTDate) override
		{
			beginCalls++;
			beginTDates.push_back(curTDate);
		}

		void handle_session_end(uint32_t /*curTDate*/) override { endCalls++; }
		void handle_section_end(uint32_t, uint32_t) override { sectionCalls++; }
		void handle_replay_done() override { replayDoneCalls++; }

		std::vector<uint32_t> beginTDates;
	};
}

// Narrow test seam for exercising a secondary close timestamp that the
// bounded primary replay loop correctly never reaches on its own.
struct HisDataReplayerTestAccess
{
	static void on_minute_end(HisDataReplayer& replayer, uint32_t date,
		uint32_t time, uint32_t endTDate)
	{
		replayer.onMinuteEnd(date, time, endTDate);
	}

	static bool replay_hft_data(HisDataReplayer& replayer, uint64_t stime, uint64_t etime)
	{
		return replayer.replayHftDatas(stime, etime);
	}

	static std::size_t replay_tick_bucket_count(const HisDataReplayer& replayer)
	{
		std::size_t count = 0;
		for (const auto& codeEntry : replayer._replay_ticks_cache)
			count += codeEntry.second.size();
		return count;
	}
};

class P07MinuteReplayTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("csv");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;
};

/*
 * 分钟线主循环：1 分钟K线作为主K线，逐分钟推进并模拟开高低收 4 笔tick。
 */
TEST_F(P07MinuteReplayTest, minuteBarsReplaySimulatesOhlcTicks)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001031500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.rp = &replayer;
	sink.dayPriceCode = "SSE.STK.600000";
	bool initialSliceBeforeFirstWasNull = false;
	std::vector<uint32_t> visibleSliceTimes;
	std::vector<double> visibleSliceCloses;
	replayer.register_sink(&sink, "p07_m1");
	sink.onInit = [&replayer, &initialSliceBeforeFirstWasNull]() {
		WTSKlineSlice* firstSlice = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
		initialSliceBeforeFirstWasNull = firstSlice == NULL;
		if (firstSlice != NULL)
			firstSlice->release();
	};
	sink.onBarCloseProbe = [&replayer, &visibleSliceTimes, &visibleSliceCloses]() {
		WTSKlineSlice* closedSlice = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, false);
		if (closedSlice != NULL && closedSlice->size() > 0)
		{
			const WTSBarStruct* latest = closedSlice->at(closedSlice->size() - 1);
			visibleSliceTimes.push_back(static_cast<uint32_t>(latest->time % 10000u));
			visibleSliceCloses.push_back(latest->close);
			closedSlice->release();
		}
		else
		{
			visibleSliceTimes.push_back(0u);
			visibleSliceCloses.push_back(0.0);
			if (closedSlice != NULL)
				closedSlice->release();
		}
	};

	ASSERT_TRUE(replayer.prepare());
	EXPECT_TRUE(initialSliceBeforeFirstWasNull);
	replayer.run(false);

	// 回放区间包含全部九根，首根 09:31 也必须闭盘。
	ASSERT_EQ(kM1Closed, sink.closedPrices.size());
	EXPECT_EQ("m", sink.closedPeriods[0]);
	std::vector<double> expectCloses;
	std::vector<uint32_t> expectCloseDates;
	std::vector<uint32_t> expectCloseTimes;
	for (uint32_t i = 0; i < kM1Cnt; i++)
	{
		expectCloses.push_back(kM1Bars[i].close);
		expectCloseDates.push_back(kM1Bars[i].date);
		expectCloseTimes.push_back((kM1Bars[i].date - 19900000u) * 10000u + kM1Bars[i].time);
	}
	EXPECT_EQ(expectCloses, sink.closedPrices);
	EXPECT_EQ(expectCloseDates, sink.closedDates);
	EXPECT_EQ(expectCloseTimes, sink.closedTimes);
	EXPECT_EQ(expectCloses, visibleSliceCloses);
	std::vector<uint32_t> expectedVisibleTimes;
	for (uint32_t i = 0; i < kM1Cnt; i++)
		expectedVisibleTimes.push_back(kM1Bars[i].time);
	EXPECT_EQ(expectedVisibleTimes, visibleSliceTimes);

	// 每根闭盘对应开高低收 4 笔模拟tick，且价格与K线字段严格一致
	ASSERT_EQ(kM1Closed * 4, sink.tickPrices.size());
	for (uint32_t i = 0; i < kM1Closed; i++)
	{
		const CsvBar& bar = kM1Bars[i];
		EXPECT_EQ(bar.open, sink.tickPrices[i * 4 + 0]) << "pxType 0 @" << i;
		EXPECT_EQ(bar.high, sink.tickPrices[i * 4 + 1]) << "pxType 1 @" << i;
		EXPECT_EQ(bar.low, sink.tickPrices[i * 4 + 2]) << "pxType 2 @" << i;
		EXPECT_EQ(bar.close, sink.tickPrices[i * 4 + 3]) << "pxType 3 @" << i;
		for (uint32_t pxType = 0; pxType < 4; pxType++)
		{
			const uint32_t tick = i * 4 + pxType;
			EXPECT_EQ(bar.date, sink.tickDates[tick]);
			EXPECT_EQ(bar.time, sink.tickTimes[tick]);
			EXPECT_EQ(pxType, sink.tickPxTypes[tick]);
		}
		const uint32_t closeTick = i * 4 + 3;
		EXPECT_EQ(sink.closedDates[i], sink.tickDates[closeTick]);
		EXPECT_EQ(bar.time, sink.tickTimes[closeTick]);
		EXPECT_DOUBLE_EQ(sink.closedPrices[i], sink.tickPrices[closeTick]);
	}
	for (const auto& c : sink.tickCodes)
		EXPECT_EQ(std::string("SSE.STK.600000"), c);

	// get_cur_price 有数据路径：闭盘瞬间等于当前bar收盘价
	ASSERT_EQ(kM1Closed, sink.curPriceAtClose.size());
	for (uint32_t i = 0; i < kM1Cnt; i++)
		EXPECT_EQ(kM1Bars[i].close, sink.curPriceAtClose[i]);

	/*
	 * get_day_price 有数据路径：_day_cache 按交易日累积开高低。
	 * 第 1 天（bars 0..5）：open=9.9 高=12.5 低=9.7
	 * 第 2 天（bars 6..8）：open=12.0 高=13.5 低=11.9
	 */
	ASSERT_EQ(kM1Closed, sink.dayHighAtClose.size());
	const double day1Open[] = {9.9, 9.9, 9.9, 9.9, 9.9, 9.9};
	const double day1High[] = {10.1, 10.5, 11.0, 12.0, 12.0, 12.5};
	const double day1Low[] = {9.7, 9.7, 9.7, 9.7, 9.7, 9.7};
	const double day2Open[] = {12.0, 12.0, 12.0};
	const double day2High[] = {12.4, 13.0, 13.5};
	const double day2Low[] = {11.9, 11.9, 11.9};
	EXPECT_EQ(std::vector<double>(day1Open, day1Open + 6),
		std::vector<double>(sink.dayOpenAtClose.begin(), sink.dayOpenAtClose.begin() + 6));
	EXPECT_EQ(std::vector<double>(day1High, day1High + 6),
		std::vector<double>(sink.dayHighAtClose.begin(), sink.dayHighAtClose.begin() + 6));
	EXPECT_EQ(std::vector<double>(day1Low, day1Low + 6),
		std::vector<double>(sink.dayLowAtClose.begin(), sink.dayLowAtClose.begin() + 6));
	for (uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ(day2Open[i], sink.dayOpenAtClose[6 + i]) << "day2 open @" << i;
		EXPECT_EQ(day2High[i], sink.dayHighAtClose[6 + i]) << "day2 high @" << i;
		EXPECT_EQ(day2Low[i], sink.dayLowAtClose[6 + i]) << "day2 low @" << i;
	}

	// 调度：每次循环推进一次 onMinuteEnd -> handle_schedule
	EXPECT_EQ(kM1Closed, sink.scheduleCalls);
	EXPECT_EQ(2u, sink.sectionCalls);          // 每个交易日 1500 是小节最后一根
	EXPECT_EQ(2u, sink.beginCalls);            // 首日开盘与跨到 20200103 时各一次
	EXPECT_EQ(2u, sink.endCalls);              // 两个交易日各闭一次
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
	EXPECT_EQ(20200103u, replayer.get_date());
	EXPECT_EQ(1500u, replayer.get_raw_time());
	EXPECT_TRUE(replayer.is_tick_simulated());
	EXPECT_FALSE(replayer.is_tick_enabled());

}

/*
 * 分钟游标二次定位：回放过程中再次取 m1 切片，走 get_kline_slice 的
 * 「已定位过」推进分支（L2412-2455），切片长度应等于已收盘根数。
 */
TEST_F(P07MinuteReplayTest, minuteSliceCursorAdvancesDuringReplay)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001031500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	static std::vector<uint32_t> sliceSizes;
	static std::vector<uint32_t> sliceLastTimes;
	static std::vector<double> sliceLastCloses;
	sliceSizes.clear();
	sliceLastTimes.clear();
	sliceLastCloses.clear();

	RecordingSink sink;
	replayer.register_sink(&sink, "p07_cursor");
	sink.onInit = [&replayer]() {
		WTSKlineSlice* initial = replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, true);
		if (initial != NULL)
			initial->release();
	};
	// 每次闭盘后取一次切片：游标已经推进到当前bar
	sink.onBarCloseProbe = [&replayer]() {
		WTSKlineSlice* sli = replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, false);
		sliceSizes.push_back(sli == NULL ? 0 : sli->size());
		if (sli != NULL && sli->size() > 0)
		{
			sliceLastTimes.push_back(static_cast<uint32_t>(sli->at(sli->size() - 1)->time));
			sliceLastCloses.push_back(sli->at(sli->size() - 1)->close);
		}
		else
		{
			sliceLastTimes.push_back(0);
			sliceLastCloses.push_back(0.0);
		}
		if (sli != NULL)
			sli->release();
	};

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_EQ(kM1Closed, sliceSizes.size());
	// 回调内切片包含刚闭盘的当前根，且游标在回放期间持续前进。
	std::vector<uint32_t> expectSizes;
	for (uint32_t i = 0; i < kM1Closed; i++)
		expectSizes.push_back(i + 1);
	EXPECT_EQ(expectSizes, sliceSizes);
	ASSERT_EQ(kM1Closed, sliceLastTimes.size());
	ASSERT_EQ(kM1Closed, sliceLastCloses.size());
	for (uint32_t i = 0; i < kM1Closed; i++)
	{
		EXPECT_EQ((kM1Bars[i].date - 19900000u) * 10000u + kM1Bars[i].time, sliceLastTimes[i]);
		EXPECT_DOUBLE_EQ(kM1Bars[i].close, sliceLastCloses[i]);
	}
}

TEST_F(P07MinuteReplayTest, sameTimeCloseCallbacksReadCrossPeriodAndDrainAllDueBars)
{
	const CsvBar mainBars[] = {
		{20200102u, 931u, 10.0, 10.5, 9.5, 10.2, 100},
		{20200102u, 935u, 11.0, 11.5, 10.5, 11.2, 200},
		{20200102u, 950u, 12.0, 12.5, 11.5, 12.2, 300}
	};
	const CsvBar secondaryBars[] = {
		{20200102u, 935u, 20.0, 20.5, 19.5, 20.2, 400},
		{20200102u, 940u, 21.0, 21.5, 20.5, 21.2, 500},
		{20200102u, 945u, 22.0, 22.5, 21.5, 22.2, 600}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars, 3, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", secondaryBars, 3, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020950ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	std::vector<uint32_t> crossReadTimes;
	std::vector<double> crossReadCloses;
	std::vector<uint32_t> crossReadAtTimes;
	std::vector<bool> crossReadAvailable;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* secondary = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 5, false);
		if (secondary != NULL)
			secondary->release();
	};
	sink.onBarCloseProbe = [&replayer, &crossReadTimes, &crossReadCloses, &crossReadAtTimes, &crossReadAvailable]() {
		crossReadAtTimes.push_back(replayer.get_raw_time());
		WTSKlineSlice* secondary = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 5, false);
		if (secondary != NULL && secondary->size() > 0)
		{
			crossReadAvailable.push_back(true);
			const WTSBarStruct* last = secondary->at(secondary->size() - 1);
			crossReadTimes.push_back(static_cast<uint32_t>(last->time % 10000u));
			crossReadCloses.push_back(last->close);
			secondary->release();
		}
		else
		{
			crossReadAvailable.push_back(false);
			crossReadTimes.push_back(0);
			crossReadCloses.push_back(0.0);
		}
	};
	replayer.register_sink(&sink, "p07_multi_due");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	std::vector<uint32_t> mainCloseTimes;
	std::vector<double> mainCloses;
	std::vector<uint32_t> secondaryCloseTimes;
	std::vector<double> secondaryCloses;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		const uint32_t hhmm = sink.closedTimes[i] % 10000u;
		if (sink.closedMultipliers[i] == 1u)
		{
			mainCloseTimes.push_back(hhmm);
			mainCloses.push_back(sink.closedPrices[i]);
		}
		else if (sink.closedMultipliers[i] == 5u)
		{
			secondaryCloseTimes.push_back(hhmm);
			secondaryCloses.push_back(sink.closedPrices[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 950u}), mainCloseTimes);
	EXPECT_EQ(std::vector<double>({10.2, 11.2, 12.2}), mainCloses);
	EXPECT_EQ(std::vector<uint32_t>({935u, 940u, 945u}), secondaryCloseTimes);
	EXPECT_EQ(std::vector<double>({20.2, 21.2, 22.2}), secondaryCloses);
	ASSERT_EQ(sink.closedPeriods.size(), crossReadTimes.size());
	ASSERT_EQ(sink.closedPeriods.size(), crossReadCloses.size());
	ASSERT_EQ(sink.closedPeriods.size(), crossReadAtTimes.size());
	ASSERT_EQ(sink.closedPeriods.size(), crossReadAvailable.size());
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		if (crossReadAtTimes[i] == 931u)
		{
			// The first m5 close is still in the future at 09:31.
			EXPECT_FALSE(crossReadAvailable[i]);
			EXPECT_EQ(0u, crossReadTimes[i]);
		}
		else if (crossReadAtTimes[i] == 935u)
		{
			EXPECT_TRUE(crossReadAvailable[i]);
			EXPECT_EQ(935u, crossReadTimes[i]);
			EXPECT_DOUBLE_EQ(20.2, crossReadCloses[i]);
		}
		if (crossReadAtTimes[i] == 950u)
		{
			// At 09:50 two secondary bars are due. A close callback may read the
			// secondary slice, but both due events must still be delivered.
			EXPECT_EQ(945u, crossReadTimes[i]);
			EXPECT_DOUBLE_EQ(22.2, crossReadCloses[i]);
		}
	}
}

TEST_F(P07MinuteReplayTest, simulatedTickCallbackQueriesAndAddsSeriesWithoutDroppingCloses)
{
	const CsvBar mainBars[] = {
		{20200102u, 931u, 10.0, 10.5,  9.5, 10.2, 100},
		{20200102u, 935u, 11.0, 11.5, 10.5, 11.2, 200},
		{20200102u, 940u, 12.0, 12.5, 11.5, 12.2, 300},
		{20200102u, 945u, 13.0, 13.5, 12.5, 13.2, 400},
		{20200102u, 950u, 14.0, 14.5, 13.5, 14.2, 500}
	};
	const CsvBar m5Bars[] = {
		{20200102u, 935u, 20.0, 20.5, 19.5, 20.2, 400},
		{20200102u, 940u, 21.0, 21.5, 20.5, 21.2, 500},
		{20200102u, 945u, 22.0, 22.5, 21.5, 22.2, 600},
		{20200102u, 950u, 23.0, 23.5, 22.5, 23.2, 700}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars, 5, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", m5Bars, 4, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020950ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool tickCallbackRan = false;
	bool currentPeriodWasNullBeforeClose = false;
	bool crossPeriodWasNullBeforeClose = false;
	bool newPeriodWasNullBeforeClose = false;
	std::vector<uint32_t> tickQueryTimes;
	std::vector<uint32_t> tickMainSliceTimes;
	std::vector<uint32_t> tickM5SliceTimes;
	std::vector<uint32_t> tickM15SliceTimes;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* m5 = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 5, false);
		if (m5 != NULL)
			m5->release();
	};
	sink.onTickProbe = [&replayer, &tickCallbackRan, &currentPeriodWasNullBeforeClose,
		&crossPeriodWasNullBeforeClose, &newPeriodWasNullBeforeClose, &tickQueryTimes,
		&tickMainSliceTimes, &tickM5SliceTimes, &tickM15SliceTimes](const char*, uint32_t pxType) {
		if (pxType != 0u)
			return;
		const uint32_t now = replayer.get_raw_time();
		tickQueryTimes.push_back(now);
		tickCallbackRan = tickCallbackRan || now == 931u;
		WTSKlineSlice* current = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, false);
		if (now == 931u)
			currentPeriodWasNullBeforeClose = current == NULL;
		tickMainSliceTimes.push_back(current == NULL || current->size() == 0
			? 0u : static_cast<uint32_t>(current->at(current->size() - 1)->time % 10000u));
		if (current != NULL)
			current->release();
		WTSKlineSlice* cross = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 5, false);
		if (now == 931u)
			crossPeriodWasNullBeforeClose = cross == NULL;
		tickM5SliceTimes.push_back(cross == NULL || cross->size() == 0
			? 0u : static_cast<uint32_t>(cross->at(cross->size() - 1)->time % 10000u));
		if (cross != NULL)
			cross->release();
		// This query adds a new resampled series while simTicks is iterating.
		WTSKlineSlice* m15 = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 15, false);
		if (now == 931u)
			newPeriodWasNullBeforeClose = m15 == NULL;
		tickM15SliceTimes.push_back(m15 == NULL || m15->size() == 0
			? 0u : static_cast<uint32_t>(m15->at(m15->size() - 1)->time % 10000u));
		if (m15 != NULL)
			m15->release();
	};
	replayer.register_sink(&sink, "p07_tick_reentrant_query");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(tickCallbackRan);
	EXPECT_TRUE(currentPeriodWasNullBeforeClose);
	EXPECT_TRUE(crossPeriodWasNullBeforeClose);
	EXPECT_TRUE(newPeriodWasNullBeforeClose);
	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 940u, 945u, 950u}), tickQueryTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 935u, 940u, 945u, 950u}), tickMainSliceTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u, 940u, 945u, 950u}), tickM5SliceTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u, 0u, 0u, 945u}), tickM15SliceTimes);
	std::vector<uint32_t> mainCloseTimes;
	std::vector<uint32_t> m5CloseTimes;
	std::vector<uint32_t> m15CloseTimes;
	std::vector<double> m15Open;
	std::vector<double> m15High;
	std::vector<double> m15Low;
	std::vector<double> m15Close;
	std::vector<double> m15Volume;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		const uint32_t hhmm = sink.closedTimes[i] % 10000u;
		if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 1u)
			mainCloseTimes.push_back(hhmm);
		else if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 5u)
			m5CloseTimes.push_back(hhmm);
		else if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 15u)
		{
			m15CloseTimes.push_back(hhmm);
			m15Open.push_back(sink.closedOpens[i]);
			m15High.push_back(sink.closedHighs[i]);
			m15Low.push_back(sink.closedLows[i]);
			m15Close.push_back(sink.closedPrices[i]);
			m15Volume.push_back(sink.closedVolumes[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 940u, 945u, 950u}), mainCloseTimes);
	EXPECT_EQ(std::vector<uint32_t>({935u, 940u, 945u, 950u}), m5CloseTimes);
	EXPECT_EQ(std::vector<uint32_t>({945u}), m15CloseTimes);
	EXPECT_EQ(std::vector<double>({20.0}), m15Open);
	EXPECT_EQ(std::vector<double>({22.5}), m15High);
	EXPECT_EQ(std::vector<double>({19.5}), m15Low);
	EXPECT_EQ(std::vector<double>({22.2}), m15Close);
	EXPECT_EQ(std::vector<double>({1500.0}), m15Volume);
}

TEST_F(P07MinuteReplayTest, tickCallbackSubscriptionsStartAtNextCompleteBar)
{
	const CsvBar driverBars[] = {
		{20200102u, 935u, 10.0, 10.5,  9.5, 10.2, 100},
		{20200102u, 936u, 11.0, 11.5, 10.5, 11.2, 200}
	};
	const CsvBar targetM5Bars[] = {
		{20200102u, 935u, 40.0, 40.5, 39.5, 40.2, 300}
	};
	const CsvBar targetM1Bars[] = {
		{20200102u, 935u, 50.0, 50.5, 49.5, 50.2, 500},
		{20200102u, 936u, 51.0, 51.5, 50.5, 51.2, 600}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", driverBars, 2, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_m5", targetM5Bars, 1, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_m1", targetM1Bars, 2, true));

	const std::string driverCode = "SSE.STK.600000";
	const std::string targetCode = "SSE.STK.600001";
	for (uint32_t subscribePxType = 0; subscribePxType < 4; ++subscribePxType)
	{
		SCOPED_TRACE(::testing::Message() << "subscribe at OHLC phase " << subscribePxType);
		HisDataReplayer replayer;
		WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020936ull);
		ASSERT_TRUE(replayer.init(cfg));
		cfg->release();

		RecordingSink sink;
		bool subscribed = false;
		uint32_t subscribeDate = 0;
		uint32_t subscribeTime = 0;
		sink.onInit = [&replayer, &driverCode, &targetCode]() {
			WTSKlineSlice* main = replayer.get_kline_slice(driverCode.c_str(), "m", 10, 1, true);
			if (main != NULL)
				main->release();
			WTSKlineSlice* targetM5 = replayer.get_kline_slice(targetCode.c_str(), "m", 10, 5, false);
			if (targetM5 != NULL)
				targetM5->release();
		};
		sink.onTickProbe = [&replayer, &sink, &subscribed, &subscribeDate, &subscribeTime, &driverCode,
			&targetCode, subscribePxType](const char* stdCode, uint32_t pxType) {
			if (subscribed || driverCode != stdCode || pxType != subscribePxType
				|| replayer.get_raw_time() != 935u)
				return;

			subscribed = true;
			subscribeDate = sink.tickDates.back();
			subscribeTime = replayer.get_raw_time();
			// This changes the target contract's selected ticker series from m5
			// to m1 while the m5 bar is still producing its OHLC batch.
			WTSKlineSlice* targetM1 = replayer.get_kline_slice(
				targetCode.c_str(), "m", 10, 1, false);
			if (targetM1 != NULL)
				targetM1->release();
		};
		replayer.register_sink(&sink, "p07_tick_subscribe_next_bar");
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);

		ASSERT_TRUE(subscribed);
		EXPECT_EQ(20200102u, subscribeDate);
		EXPECT_EQ(935u, subscribeTime);

		std::vector<uint32_t> mainTickDates, mainTickTimes, mainPxTypes;
		std::vector<double> mainTickPrices;
		std::vector<uint32_t> targetTickDates, targetTickTimes, targetPxTypes;
		std::vector<double> targetTickPrices;
		for (std::size_t i = 0; i < sink.tickCodes.size(); ++i)
		{
			if (sink.tickCodes[i] == driverCode)
			{
				mainTickDates.push_back(sink.tickDates[i]);
				mainTickTimes.push_back(sink.tickTimes[i]);
				mainPxTypes.push_back(sink.tickPxTypes[i]);
				mainTickPrices.push_back(sink.tickPrices[i]);
			}
			else if (sink.tickCodes[i] == targetCode)
			{
				targetTickDates.push_back(sink.tickDates[i]);
				targetTickTimes.push_back(sink.tickTimes[i]);
				targetPxTypes.push_back(sink.tickPxTypes[i]);
				targetTickPrices.push_back(sink.tickPrices[i]);
			}
		}
		EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u, 20200102u,
			20200102u, 20200102u, 20200102u, 20200102u}), mainTickDates);
		EXPECT_EQ(std::vector<uint32_t>({935u, 935u, 935u, 935u, 936u, 936u, 936u, 936u}), mainTickTimes);
		EXPECT_EQ(std::vector<uint32_t>({0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u}), mainPxTypes);
		EXPECT_EQ(std::vector<double>({10.0, 10.5, 9.5, 10.2, 11.0, 11.5, 10.5, 11.2}), mainTickPrices);

		// The target's pre-existing m5 bar keeps its complete 09:35 OHLC
		// sequence, despite m1 being registered during one of those callbacks.
		// The new m1 series starts with its complete natural 09:36 bar.
		EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u, 20200102u,
			20200102u, 20200102u, 20200102u, 20200102u}), targetTickDates);
		EXPECT_EQ(std::vector<uint32_t>({935u, 935u, 935u, 935u, 936u, 936u, 936u, 936u}), targetTickTimes);
		EXPECT_EQ(std::vector<uint32_t>({0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u}), targetPxTypes);
		EXPECT_EQ(std::vector<double>({40.0, 40.5, 39.5, 40.2, 51.0, 51.5, 50.5, 51.2}), targetTickPrices);

		std::vector<uint32_t> driverCloseDates, driverCloseTimes;
		std::vector<uint32_t> targetM1CloseDates, targetM1CloseTimes;
		std::vector<uint32_t> targetM5CloseDates, targetM5CloseTimes;
		std::vector<double> targetM1Closes, targetM5Closes;
		for (std::size_t i = 0; i < sink.closedCodes.size(); ++i)
		{
			if (sink.closedCodes[i] == driverCode)
			{
				driverCloseDates.push_back(sink.closedDates[i]);
				driverCloseTimes.push_back(sink.closedTimes[i]);
			}
			else if (sink.closedCodes[i] == targetCode && sink.closedMultipliers[i] == 1u)
			{
				targetM1CloseDates.push_back(sink.closedDates[i]);
				targetM1CloseTimes.push_back(sink.closedTimes[i]);
				targetM1Closes.push_back(sink.closedPrices[i]);
			}
			else if (sink.closedCodes[i] == targetCode && sink.closedMultipliers[i] == 5u)
			{
				targetM5CloseDates.push_back(sink.closedDates[i]);
				targetM5CloseTimes.push_back(sink.closedTimes[i]);
				targetM5Closes.push_back(sink.closedPrices[i]);
			}
		}
		const uint32_t encoded935 = (20200102u - 19900000u) * 10000u + 935u;
		const uint32_t encoded936 = (20200102u - 19900000u) * 10000u + 936u;
		EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u}), driverCloseDates);
		EXPECT_EQ(std::vector<uint32_t>({encoded935, encoded936}), driverCloseTimes);
		EXPECT_EQ(std::vector<uint32_t>({20200102u}), targetM5CloseDates);
		EXPECT_EQ(std::vector<uint32_t>({encoded935}), targetM5CloseTimes);
		EXPECT_EQ(std::vector<double>({40.2}), targetM5Closes);
		EXPECT_EQ(std::vector<uint32_t>({20200102u}), targetM1CloseDates);
		EXPECT_EQ(std::vector<uint32_t>({encoded936}), targetM1CloseTimes);
		EXPECT_EQ(std::vector<double>({51.2}), targetM1Closes);
	}
}

TEST_F(P07MinuteReplayTest, m15RequestDoesNotEmitCloseForInternalRawM5)
{
	const CsvBar mainBars[] = {
		{20200102u, 931u, 10.0, 10.5,  9.5, 10.2, 100},
		{20200102u, 935u, 11.0, 11.5, 10.5, 11.2, 200},
		{20200102u, 940u, 12.0, 12.5, 11.5, 12.2, 300},
		{20200102u, 945u, 13.0, 13.5, 12.5, 13.2, 400}
	};
	const CsvBar rawM5Bars[] = {
		{20200102u, 935u, 20.0, 20.5, 19.5, 20.2, 400},
		{20200102u, 940u, 21.0, 21.5, 20.5, 21.2, 500},
		{20200102u, 945u, 22.0, 22.5, 21.5, 22.2, 600}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars, 4, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", rawM5Bars, 3, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020945ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool requestedM15 = false;
	bool m15WasUnavailableBeforeFirstClose = false;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
	};
	sink.onTickProbe = [&replayer, &requestedM15, &m15WasUnavailableBeforeFirstClose](const char*, uint32_t pxType) {
		if (requestedM15 || pxType != 0u || replayer.get_raw_time() != 931u)
			return;
		requestedM15 = true;
		WTSKlineSlice* m15 = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 15, false);
		m15WasUnavailableBeforeFirstClose = m15 == NULL;
		if (m15 != NULL)
			m15->release();
	};
	replayer.register_sink(&sink, "p07_m15_internal_m5");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(requestedM15);
	EXPECT_TRUE(m15WasUnavailableBeforeFirstClose);
	std::vector<uint32_t> m1CloseTimes;
	std::vector<uint32_t> m5CloseTimes;
	std::vector<uint32_t> m15CloseTimes;
	std::vector<double> m15Opens;
	std::vector<double> m15Highs;
	std::vector<double> m15Lows;
	std::vector<double> m15Closes;
	std::vector<double> m15Volumes;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		const uint32_t hhmm = sink.closedTimes[i] % 10000u;
		if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 1u)
			m1CloseTimes.push_back(hhmm);
		else if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 5u)
			m5CloseTimes.push_back(hhmm);
		else if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 15u)
		{
			m15CloseTimes.push_back(hhmm);
			m15Opens.push_back(sink.closedOpens[i]);
			m15Highs.push_back(sink.closedHighs[i]);
			m15Lows.push_back(sink.closedLows[i]);
			m15Closes.push_back(sink.closedPrices[i]);
			m15Volumes.push_back(sink.closedVolumes[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 940u, 945u}), m1CloseTimes);
	EXPECT_TRUE(m5CloseTimes.empty());
	EXPECT_EQ(std::vector<uint32_t>({945u}), m15CloseTimes);
	EXPECT_EQ(std::vector<double>({20.0}), m15Opens);
	EXPECT_EQ(std::vector<double>({22.5}), m15Highs);
	EXPECT_EQ(std::vector<double>({19.5}), m15Lows);
	EXPECT_EQ(std::vector<double>({22.2}), m15Closes);
	EXPECT_EQ(std::vector<double>({1500.0}), m15Volumes);
}

TEST_F(P07MinuteReplayTest, simulatedTickCallbackExceptionAllowsReplayRetry)
{
	const CsvBar bars[] = {
		{20200102u, 931u, 10.0, 10.5, 9.5, 10.2, 100},
		{20200102u, 932u, 10.2, 11.0, 10.1, 10.8, 200}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", bars, 2, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020932ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool throwOnce = true;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
	};
	sink.onTickProbe = [&throwOnce](const char*, uint32_t) {
		if (throwOnce)
		{
			throwOnce = false;
			throw std::runtime_error("simulated tick callback failure");
		}
	};
	replayer.register_sink(&sink, "p07_tick_exception");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_THROW(replayer.run(false), std::runtime_error);
	EXPECT_TRUE(sink.closedPrices.empty());
	sink.tickPrices.clear();
	sink.tickDates.clear();
	sink.tickTimes.clear();
	sink.tickCodes.clear();
	sink.tickPxTypes.clear();

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u}), sink.closedDates);
	const uint32_t encodedDate = (20200102u - 19900000u) * 10000u;
	EXPECT_EQ(std::vector<uint32_t>({encodedDate + 931u, encodedDate + 932u}), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({10.2, 10.8}), sink.closedPrices);
}

TEST_F(P07MinuteReplayTest, unsubscribedTickCallbackExceptionReloadsBarsOnRetry)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_d", kDayBars, kDayCnt, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_d", kDayBars, kDayCnt, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001061500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool throwOnUnsubOnce = true;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "d", 1, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(7, "SSE.STK.600001");
	};
	sink.onTickProbe = [&throwOnUnsubOnce](const char* code, uint32_t) {
		if (throwOnUnsubOnce && std::string(code) == "SSE.STK.600001")
		{
			throwOnUnsubOnce = false;
			throw std::runtime_error("unsubscribed simulated tick callback failure");
		}
	};
	replayer.register_sink(&sink, "p07_unsub_tick_exception");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_THROW(replayer.run(false), std::runtime_error);
	sink.tickCodes.clear();
	sink.tickPrices.clear();
	sink.tickDates.clear();
	sink.tickTimes.clear();
	sink.tickPxTypes.clear();
	sink.closedCodes.clear();
	sink.closedPeriods.clear();
	sink.closedDates.clear();
	sink.closedTimes.clear();
	sink.closedMultipliers.clear();
	sink.closedOpens.clear();
	sink.closedHighs.clear();
	sink.closedLows.clear();
	sink.closedPrices.clear();
	sink.closedVolumes.clear();

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	std::vector<double> expectedPrices;
	std::vector<uint32_t> expectedDates;
	for (uint32_t i = 0; i < kDayCnt; i++)
	{
		const double ohlc[] = { kDayBars[i].open, kDayBars[i].high, kDayBars[i].low, kDayBars[i].close };
		for (double price : ohlc)
		{
			expectedPrices.push_back(price);
			expectedDates.push_back(kDayBars[i].date);
		}
	}
	std::vector<double> unsubPrices;
	std::vector<uint32_t> unsubDates;
	std::vector<uint32_t> unsubTimes;
	for (std::size_t i = 0; i < sink.tickCodes.size(); i++)
	{
		if (sink.tickCodes[i] == "SSE.STK.600001")
		{
			unsubPrices.push_back(sink.tickPrices[i]);
			unsubDates.push_back(sink.tickDates[i]);
			unsubTimes.push_back(sink.tickTimes[i]);
		}
	}
	EXPECT_EQ(expectedPrices, unsubPrices);
	EXPECT_EQ(expectedDates, unsubDates);
	EXPECT_EQ(std::vector<uint32_t>(12, 930u), unsubTimes);
}

TEST_F(P07MinuteReplayTest, unsubscribedTickCallbackQueriesDoNotConsumeCurrentOrLaterCloses)
{
	const CsvBar mainBars[] = {
		{20200102u, 931u, 10.0, 10.5,  9.5, 10.2, 100},
		{20200102u, 935u, 11.0, 11.5, 10.5, 11.2, 200},
		{20200102u, 940u, 12.0, 12.5, 11.5, 12.2, 300},
		{20200102u, 945u, 13.0, 13.5, 12.5, 13.2, 400}
	};
	const CsvBar secondaryM5Bars[] = {
		{20200102u, 935u, 20.0, 20.5, 19.5, 20.2, 400},
		{20200102u, 940u, 21.0, 21.5, 20.5, 21.2, 500},
		{20200102u, 945u, 22.0, 22.5, 21.5, 22.2, 600}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars, 4, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", secondaryM5Bars, 3, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_m1", mainBars, 4, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020945ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	std::vector<uint32_t> queryTimes;
	std::vector<uint32_t> visibleMainTimes;
	std::vector<double> visibleMainCloses;
	std::vector<uint32_t> visibleM5Times;
	std::vector<double> visibleM5Closes;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* m5 = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 5, false);
		if (m5 != NULL)
			m5->release();
		replayer.sub_tick(7u, "SSE.STK.600001");
	};
	sink.onTickProbe = [&replayer, &queryTimes, &visibleMainTimes, &visibleMainCloses,
		&visibleM5Times, &visibleM5Closes](const char* code, uint32_t pxType) {
		if (std::string(code) != "SSE.STK.600001" || pxType != 0u)
			return;
		queryTimes.push_back(replayer.get_raw_time());
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, false);
		if (main != NULL && main->size() > 0)
		{
			const WTSBarStruct* last = main->at(main->size() - 1);
			visibleMainTimes.push_back(static_cast<uint32_t>(last->time % 10000u));
			visibleMainCloses.push_back(last->close);
			main->release();
		}
		else
		{
			visibleMainTimes.push_back(0u);
			visibleMainCloses.push_back(0.0);
			if (main != NULL)
				main->release();
		}

		WTSKlineSlice* m5 = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 5, false);
		if (m5 != NULL && m5->size() > 0)
		{
			const WTSBarStruct* last = m5->at(m5->size() - 1);
			visibleM5Times.push_back(static_cast<uint32_t>(last->time % 10000u));
			visibleM5Closes.push_back(last->close);
			m5->release();
		}
		else
		{
			visibleM5Times.push_back(0u);
			visibleM5Closes.push_back(0.0);
			if (m5 != NULL)
				m5->release();
		}
	};
	replayer.register_sink(&sink, "p07_unsub_tick_cursor_query");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 940u, 945u}), queryTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 935u, 940u, 945u}), visibleMainTimes);
	EXPECT_EQ(std::vector<double>({0.0, 11.2, 12.2, 13.2}), visibleMainCloses);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u, 940u, 945u}), visibleM5Times);
	EXPECT_EQ(std::vector<double>({0.0, 0.0, 21.2, 22.2}), visibleM5Closes);
	std::vector<uint32_t> mainCloseTimes;
	std::vector<uint32_t> m5CloseTimes;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		const uint32_t hhmm = sink.closedTimes[i] % 10000u;
		if (sink.closedCodes[i] != "SSE.STK.600000")
			continue;
		if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 1u)
			mainCloseTimes.push_back(hhmm);
		else if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 5u)
			m5CloseTimes.push_back(hhmm);
	}
	EXPECT_EQ(std::vector<uint32_t>({931u, 935u, 940u, 945u}), mainCloseTimes);
	EXPECT_EQ(std::vector<uint32_t>({935u, 940u, 945u}), m5CloseTimes);
	for (const std::string& code : sink.closedCodes)
		EXPECT_EQ(std::string("SSE.STK.600000"), code);
}

TEST_F(P07MinuteReplayTest, callbackActivatingCachedRawM5SeriesKeepsFutureCloses)
{
	const CsvBar mainBars[] = {
		{20200102u, 931u, 10.0, 10.5, 9.5, 10.2, 100},
		{20200102u, 935u, 11.0, 11.5, 10.5, 11.2, 200},
		{20200102u, 940u, 12.0, 12.5, 11.5, 12.2, 300},
		{20200102u, 945u, 13.0, 13.5, 12.5, 13.2, 400},
		{20200102u, 950u, 14.0, 14.5, 13.5, 14.2, 500}
	};
	const CsvBar m5Bars[] = {
		{20200102u, 935u, 20.0, 20.5, 19.5, 20.2, 400},
		{20200102u, 940u, 21.0, 21.5, 20.5, 21.2, 500},
		{20200102u, 945u, 22.0, 22.5, 21.5, 22.2, 600},
		{20200102u, 950u, 23.0, 23.5, 22.5, 23.2, 700}
	};
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars, 5, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", m5Bars, 4, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020950ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool queriedRawM5 = false;
	bool rawM5BeforeFirstWasNull = false;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
		// This populates the raw m5 cache while leaving its replay cursor
		// uninitialized; the close callback activates that already cached series.
		WTSKlineSlice* m15 = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 15, false);
		if (m15 != NULL)
			m15->release();
	};
	sink.onBarCloseProbe = [&replayer, &queriedRawM5, &rawM5BeforeFirstWasNull]() {
		if (queriedRawM5)
			return;
		WTSKlineSlice* rawM5 = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 5, false);
		rawM5BeforeFirstWasNull = rawM5 == NULL;
		queriedRawM5 = true;
		if (rawM5 != NULL)
			rawM5->release();
	};
	replayer.register_sink(&sink, "p07_activate_raw_m5");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(queriedRawM5);
	EXPECT_TRUE(rawM5BeforeFirstWasNull);
	std::vector<uint32_t> m5CloseTimes;
	std::vector<double> m5Closes;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		if (sink.closedMultipliers[i] == 5u)
		{
			m5CloseTimes.push_back(sink.closedTimes[i] % 10000u);
			m5Closes.push_back(sink.closedPrices[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({935u, 940u, 945u, 950u}), m5CloseTimes);
	EXPECT_EQ(std::vector<double>({20.2, 21.2, 22.2, 23.2}), m5Closes);
}

TEST_F(P07MinuteReplayTest, throwingBarCloseCallbackAbortsAndClearsRunningState)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020931ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
	};
	sink.onBarCloseProbe = []() { throw std::runtime_error("bar-close callback failure"); };
	replayer.register_sink(&sink, "p07_throwing_close");
	ASSERT_TRUE(replayer.prepare());

	EXPECT_THROW(replayer.run(false), std::runtime_error);
	// K38 BT-B（2026-11-04 用户裁定）：每代恰好一次 handle_replay_done（回调异常的一代亦送达，状态 FAILED）；原断言见证据 before/。
	EXPECT_EQ(1u, sink.replayDoneCalls);
	sink.onBarCloseProbe = nullptr;
	sink.closedCodes.clear();
	sink.closedPeriods.clear();
	sink.closedDates.clear();
	sink.closedTimes.clear();
	sink.closedMultipliers.clear();
	sink.closedPrices.clear();
	sink.tickCodes.clear();
	sink.tickPxTypes.clear();
	sink.tickPrices.clear();
	sink.tickDates.clear();
	sink.tickTimes.clear();
	sink.beginCalls = 0;
	sink.beginTDates.clear();
	EXPECT_TRUE(replayer.prepare());
	replayer.run(false);
	ASSERT_EQ(1u, sink.closedPrices.size());
	EXPECT_EQ(20200102u, sink.closedDates[0]);
	EXPECT_EQ(931u, sink.closedTimes[0] % 10000u);
	EXPECT_DOUBLE_EQ(kM1Bars[0].close, sink.closedPrices[0]);
	ASSERT_EQ(4u, sink.tickPrices.size());
	EXPECT_EQ(std::vector<double>({kM1Bars[0].open, kM1Bars[0].high, kM1Bars[0].low, kM1Bars[0].close}), sink.tickPrices);
	// K38 BT-B：失败一代与重试成功一代各送达一次 done。
	EXPECT_EQ(2u, sink.replayDoneCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.beginTDates);
}

TEST_F(P07MinuteReplayTest, throwingInitCallbackLeavesReplayerRetryable)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020931ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	bool throwOnInit = true;
	sink.onInit = [&replayer, &throwOnInit]() {
		if (throwOnInit)
			throw std::runtime_error("init callback failure");
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
		if (main != NULL)
			main->release();
	};
	replayer.register_sink(&sink, "p07_throwing_init");

	EXPECT_THROW(replayer.prepare(), std::runtime_error);
	throwOnInit = false;
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	ASSERT_EQ(1u, sink.closedPrices.size());
	EXPECT_EQ(931u, sink.closedTimes[0] % 10000u);
	EXPECT_DOUBLE_EQ(kM1Bars[0].close, sink.closedPrices[0]);
	// K38 BT-B：prepare 中 init 回调抛异常的一代与重试成功一代各送达一次 done。
	EXPECT_EQ(2u, sink.replayDoneCalls);
}

TEST_F(P07MinuteReplayTest, closeCallbackCanAddSeriesWithoutInvalidatingReplay)
{
	std::vector<CsvBar> mainBars;
	for (uint32_t offset = 0; offset < 60; offset++)
	{
		const uint32_t minutes = 9u * 60u + 31u + offset;
		const uint32_t hhmm = (minutes / 60u) * 100u + minutes % 60u;
		const double price = 10.0 + offset;
		mainBars.push_back({20200102u, hhmm, price, price + 0.5, price - 0.5, price + 0.2, 100.0});
	}
	const uint32_t m5Times[] = {
		935u, 940u, 945u, 950u, 955u, 1000u,
		1005u, 1010u, 1015u, 1020u, 1025u, 1030u,
		1035u, 1040u, 1045u, 1050u, 1055u, 1100u
	};
	std::vector<CsvBar> m5Bars;
	for (uint32_t i = 0; i < sizeof(m5Times) / sizeof(m5Times[0]); i++)
	{
		const double price = 20.0 + i;
		m5Bars.push_back({20200102u, m5Times[i], price, price + 0.5, price - 0.5, price + 0.2, 100.0});
	}
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", mainBars.data(),
		static_cast<uint32_t>(mainBars.size()), true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", m5Bars.data(),
		static_cast<uint32_t>(m5Bars.size()), true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001021030ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	std::vector<uint32_t> callbackAtTimes;
	std::vector<uint32_t> callbackMainTimes;
	std::vector<double> callbackMainCloses;
	std::vector<uint32_t> callbackHourTimes;
	std::vector<double> callbackHourCloses;
	bool addedM15FromCallback = false;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* hour = replayer.get_kline_slice("SSE.STK.600000", "h", 10, 1, false);
		if (hour != NULL)
			hour->release();
	};
	sink.onBarCloseProbe = [&replayer, &callbackAtTimes, &callbackMainTimes,
		&callbackMainCloses, &callbackHourTimes, &callbackHourCloses, &addedM15FromCallback]() {
		const uint32_t now = replayer.get_raw_time();
		callbackAtTimes.push_back(now);

		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, false);
		if (main != NULL && main->size() > 0)
		{
			const WTSBarStruct* last = main->at(main->size() - 1);
			callbackMainTimes.push_back(static_cast<uint32_t>(last->time % 10000u));
			callbackMainCloses.push_back(last->close);
			main->release();
		}
		else
		{
			callbackMainTimes.push_back(0);
			callbackMainCloses.push_back(0.0);
		}

		WTSKlineSlice* hour = replayer.get_kline_slice("SSE.STK.600000", "h", 10, 1, false);
		if (hour != NULL && hour->size() > 0)
		{
			const WTSBarStruct* last = hour->at(hour->size() - 1);
			callbackHourTimes.push_back(static_cast<uint32_t>(last->time % 10000u));
			callbackHourCloses.push_back(last->close);
			hour->release();
		}
		else
		{
			callbackHourTimes.push_back(0);
			callbackHourCloses.push_back(0.0);
		}

		if (!addedM15FromCallback && now == 1000u)
		{
			WTSKlineSlice* m15 = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 15, false);
			if (m15 != NULL)
				m15->release();
			addedM15FromCallback = true;
		}
	};
	replayer.register_sink(&sink, "p07_late_series");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_TRUE(addedM15FromCallback);
	ASSERT_EQ(sink.closedPeriods.size(), callbackAtTimes.size());
	ASSERT_EQ(sink.closedPeriods.size(), callbackMainTimes.size());
	ASSERT_EQ(sink.closedPeriods.size(), callbackMainCloses.size());
	ASSERT_EQ(sink.closedPeriods.size(), callbackHourTimes.size());
	ASSERT_EQ(sink.closedPeriods.size(), callbackHourCloses.size());
	for (std::size_t i = 0; i < callbackAtTimes.size(); i++)
	{
		const uint32_t now = callbackAtTimes[i];
		const uint32_t offset = (now / 100u) * 60u + now % 100u - (9u * 60u + 31u);
		ASSERT_LT(offset, mainBars.size());
		EXPECT_EQ(now, callbackMainTimes[i]);
		EXPECT_DOUBLE_EQ(mainBars[offset].close, callbackMainCloses[i]);
		if (now >= 1000u)
		{
			EXPECT_EQ(1000u, callbackHourTimes[i]);
			EXPECT_DOUBLE_EQ(25.2, callbackHourCloses[i]);
		}
	}

	std::vector<uint32_t> m15Times;
	std::vector<double> m15Closes;
	for (std::size_t i = 0; i < sink.closedPeriods.size(); i++)
	{
		if (sink.closedPeriods[i] == "m" && sink.closedMultipliers[i] == 15u)
		{
			m15Times.push_back(sink.closedTimes[i] % 10000u);
			m15Closes.push_back(sink.closedPrices[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({1015u, 1030u}), m15Times);
	EXPECT_EQ(std::vector<double>({28.2, 31.2}), m15Closes);
}

/*
 * m15 重采样（bar 合并）：订阅 m15 时先装载 m5 原始线，再用 WTSDataFactory 合并。
 * 断言合并后至少存在一根 open/high/low/close/vol 完全等于 0935-0945 三根之和的bar。
 */
TEST_F(P07MinuteReplayTest, m15SliceResamplesFromM5Bars)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", kM5Bars, kM5Cnt, true));

	HisDataReplayer replayer;
	// The first m15 bucket formed by 09:35/09:40/09:45 m5 rows closes at 09:45.
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020945ull, 202001021500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p07_resample");
	ASSERT_TRUE(replayer.prepare());

	WTSKlineSlice* sli = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 15, false);
	ASSERT_NE(nullptr, sli);
	ASSERT_GT(sli->size(), 0);
	const int32_t mergedCnt = sli->size();

	bool bMergedFound = false;
	for (int32_t i = 0; i < mergedCnt; i++)
	{
		WTSBarStruct* bar = sli->at(i);
		if (bar == NULL)
			continue;
		if (bar->open == 10.0 && bar->high == 11.5 && bar->low == 9.9 &&
			bar->close == 11.4 && bar->vol == 600.0)
			bMergedFound = true;
	}
	// 0935/0940/0945 三根 5 分钟线合并成一根 15 分钟线
	EXPECT_TRUE(bMergedFound) << "m15 合并结果不符合预期，共 " << sli->size() << " 根";
	sli->release();

	// 同一 key 第二次取切片走「缓存命中」分支，不再重新装载
	WTSKlineSlice* again = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 15, false);
	if (again != NULL)
	{
		EXPECT_EQ(mergedCnt, again->size());
		again->release();
	}
}

/*
 * 未订阅K线的模拟tick：只订阅 600001 的tick（sub_tick），主K线是 600000 的日线。
 * checkUnbars 会把 600001 的日线装进 _unbars_cache，simTickWithUnsubBars 逐日模拟开高低收，
 * 但 600001 不会产生 handle_bar_close（未订阅K线只喂价格）。
 */
TEST_F(P07MinuteReplayTest, unsubscribedTickCodeFeedsSimTicks)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_d", kDayBars, kDayCnt, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_d", kDayBars, kDayCnt, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001061500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p07_unsub");
	sink.onInit = [&replayer, &sink]() {
		sink.slice = replayer.get_kline_slice("SSE.STK.600000", "d", 1, 1, true);
		// tick 未开启 -> sub_tick 会把该合约塞进 _unsubbed_in_need
		replayer.sub_tick(7, "SSE.STK.600001");
	};

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	uint32_t cntMain = 0;
	uint32_t cntUnsub = 0;
	std::vector<double> mainTickPrices;
	std::vector<double> unsubTickPrices;
	std::vector<uint32_t> mainTickDates;
	std::vector<uint32_t> unsubTickDates;
	std::vector<uint32_t> mainTickTimes;
	std::vector<uint32_t> unsubTickTimes;
	for (std::size_t i = 0; i < sink.tickCodes.size(); i++)
	{
		const auto& c = sink.tickCodes[i];
		if (c == "SSE.STK.600000")
		{
			cntMain++;
			mainTickPrices.push_back(sink.tickPrices[i]);
			mainTickDates.push_back(sink.tickDates[i]);
			mainTickTimes.push_back(sink.tickTimes[i]);
		}
		else if (c == "SSE.STK.600001")
		{
			cntUnsub++;
			unsubTickPrices.push_back(sink.tickPrices[i]);
			unsubTickDates.push_back(sink.tickDates[i]);
			unsubTickTimes.push_back(sink.tickTimes[i]);
		}
	}
	// Three actual daily rows are replayed for each code. Main ticks use the
	// session close timestamp; unsubscribed daily ticks use the session open.
	std::vector<double> expectedPrices;
	std::vector<uint32_t> expectedDates;
	for (uint32_t i = 0; i < kDayCnt; i++)
	{
		const double ohlc[] = { kDayBars[i].open, kDayBars[i].high, kDayBars[i].low, kDayBars[i].close };
		for (double price : ohlc)
		{
			expectedPrices.push_back(price);
			expectedDates.push_back(kDayBars[i].date);
		}
	}
	EXPECT_EQ(12u, cntUnsub);
	EXPECT_EQ(12u, cntMain);
	EXPECT_EQ(expectedPrices, mainTickPrices);
	EXPECT_EQ(expectedPrices, unsubTickPrices);
	EXPECT_EQ(expectedDates, mainTickDates);
	EXPECT_EQ(expectedDates, unsubTickDates);
	EXPECT_EQ(std::vector<uint32_t>(12, 1500u), mainTickTimes);
	EXPECT_EQ(std::vector<uint32_t>(12, 930u), unsubTickTimes);

	// 只有主K线会闭盘
	for (const auto& c : sink.closedCodes)
		EXPECT_EQ(std::string("SSE.STK.600000"), c);
	EXPECT_EQ(kDayCnt, sink.closedPrices.size());
	std::vector<double> expectCloses = {10.5, 11.5, 12.5};
	EXPECT_EQ(expectCloses, sink.closedPrices);
	// 未订阅K线不参与 handle_bar_close，但 onMinuteEnd 会推进它的游标
	if (sink.slice != NULL)
		sink.slice->release();
}

TEST_F(P07MinuteReplayTest, secondPrecisionRangeRoundsBarBoundsAndKeepsMinuteEndInclusive)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg_text(_root, _base, "20200102093130", "202001020932");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* bars = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (bars != NULL)
			bars->release();
	};
	replayer.register_sink(&sink, "p07_range_config");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	const uint32_t encodedDate = (20200102u - 19900000u) * 10000u;
	EXPECT_EQ(std::vector<uint32_t>({encodedDate + 932u}), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({10.2}), sink.closedPrices);
}

TEST_F(P07MinuteReplayTest, explicitRangeUsesSameSecondPrecisionBarBounds)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001020950ull);
	replayer.set_time_range(20200102093130ull, 20200102093245ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* bars = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (bars != NULL)
			bars->release();
	};
	replayer.register_sink(&sink, "p07_range_setter");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	const uint32_t encodedDate = (20200102u - 19900000u) * 10000u;
	EXPECT_EQ(std::vector<uint32_t>({encodedDate + 932u}), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({10.2}), sink.closedPrices);
}

TEST_F(P07MinuteReplayTest, secondOnlyRangeHasNoWholeMinuteBars)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 20200102093201ull, 20200102093259ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* bars = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (bars != NULL)
			bars->release();
	};
	replayer.register_sink(&sink, "p07_range_empty_minutes");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(sink.closedTimes.empty());
	EXPECT_TRUE(sink.closedPrices.empty());
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07MinuteReplayTest, secondPrecisionStartRoundsAcrossCalendarDate)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m1", kM1Bars, kM1Cnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg_text(_root, _base, "20200102235930", "202001030931");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* bars = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (bars != NULL)
			bars->release();
	};
	replayer.register_sink(&sink, "p07_range_date_carry");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200103u}), sink.closedDates);
	const uint32_t encodedDate = (20200103u - 19900000u) * 10000u;
	EXPECT_EQ(std::vector<uint32_t>({encodedDate + 931u}), sink.closedTimes);
}

TEST_F(P07MinuteReplayTest, dailyBarsMustCloseInsideTheRequestedMinuteBounds)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_d", kDayBars, kDayCnt, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_d", kDayBars, kDayCnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg_text(_root, _base, "202001020930", "202001021200");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "d", 10, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* secondary = replayer.get_kline_slice("SSE.STK.600001", "d", 10, 1, false);
		if (secondary != NULL)
			secondary->release();
	};
	replayer.register_sink(&sink, "p07_daily_bounds");
	ASSERT_TRUE(replayer.prepare());
	// Exercise the secondary-series callback path at its real 15:00 close,
	// which lies outside this 09:30-12:00 interval. The normal replay loop
	// correctly stops at noon, so use the narrow test friend seam to deliver
	// that otherwise unreachable callback timestamp.
	HisDataReplayerTestAccess::on_minute_end(replayer, 20200102u, 1500u, 20200102u);

	EXPECT_TRUE(sink.closedDates.empty());
	EXPECT_TRUE(sink.closedPrices.empty());

	replayer.run(false);
	EXPECT_TRUE(sink.closedDates.empty());
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07MinuteReplayTest, dailyBarsStartingAfterCloseBeginOnTheNextNaturalDate)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_d", kDayBars, kDayCnt, true));
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600001_d", kDayBars, kDayCnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg_text(_root, _base, "20200102150030", "202001031500");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "d", 10, 1, true);
		if (main != NULL)
			main->release();
		WTSKlineSlice* secondary = replayer.get_kline_slice("SSE.STK.600001", "d", 10, 1, false);
		if (secondary != NULL)
			secondary->release();
	};
	replayer.register_sink(&sink, "p07_daily_start_after_close");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_EQ(2u, sink.closedDates.size());
	for (uint32_t date : sink.closedDates)
		EXPECT_EQ(20200103u, date);
	for (double close : sink.closedPrices)
		EXPECT_DOUBLE_EQ(11.5, close);
	EXPECT_NE(sink.closedCodes.end(), std::find(sink.closedCodes.begin(), sink.closedCodes.end(), "SSE.STK.600000"));
	EXPECT_NE(sink.closedCodes.end(), std::find(sink.closedCodes.begin(), sink.closedCodes.end(), "SSE.STK.600001"));
}

TEST_F(P07MinuteReplayTest, nightSessionDailyBarUsesItsTradingDateClose)
{
	ASSERT_TRUE(write_bars_csv(_base, "SHFE.au.2212_d", kDayBars, kDayCnt, true));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg_text(_root, _base, "202001022100", "202001031500", false,
		"commodities.json");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SHFE.au.2212", "d", 10, 1, true);
		if (main != NULL)
			main->release();
	};
	replayer.register_sink(&sink, "p07_night_daily_bounds");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	// FN0230's 20200103 trading-day bar closes at 15:00 on the natural date
	// after its 20200102 night session. The 20200102 daily bar closed before
	// this range, so only the 20200103 bar is delivered.
	EXPECT_EQ(std::vector<uint32_t>({20200103u}), sink.closedDates);
	EXPECT_EQ(std::vector<double>({11.5}), sink.closedPrices);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

/* 迭代03 最终验收复核后启用（原禁用：BUG-P04-004 / BUG-P04-005 空分钟 CSV）。 */
TEST_F(P07MinuteReplayTest, emptyMinuteCsvHitsUnsubscriptedBars)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600002_m1", kM1Bars, 0, false));
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001031500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	RecordingSink sink;
	replayer.register_sink(&sink, "p07_emptym1");
	ASSERT_TRUE(replayer.prepare());
	replayer.get_kline_slice("SSE.STK.600002", "m", 1, 1, true);
	replayer.run(false);
}
