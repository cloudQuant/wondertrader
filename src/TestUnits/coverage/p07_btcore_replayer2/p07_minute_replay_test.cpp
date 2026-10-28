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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
			.parent_path().parent_path().parent_path();
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

	/*
	 * SSE.STK.600000 的 1 分钟线：第 1 根（0931）会被 get_kline_slice 的初始定位
	 * 当成「已收盘」跳掉（与 p04 日线漏掉首根同源），因此回放区间是 bars[1..8]。
	 */
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
	// 游标初始定位吃掉首根，实际闭盘根数 = kM1Cnt - 1
	const uint32_t kM1Closed = kM1Cnt - 1;

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
				out << bars[i].date << ',' << bars[i].time << ",00,"
					<< bars[i].open << ',' << bars[i].high << ','
					<< bars[i].low << ',' << bars[i].close << ','
					<< bars[i].vol << ',' << bars[i].vol * 10.0 << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	WTSVariant* replay_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, bool tickMode = false)
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
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	class RecordingSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
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

		std::vector<std::string> closedCodes;
		std::vector<std::string> closedPeriods;
		std::vector<uint32_t> closedTimes;
		std::vector<double> closedPrices;
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
				tickPrices.push_back(curTick->getTickStruct().price);
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			(void)times;
			closedCodes.emplace_back(stdCode);
			closedPeriods.emplace_back(period);
			if (newBar != nullptr)
			{
				closedTimes.push_back(static_cast<uint32_t>(newBar->time));
				closedPrices.push_back(newBar->close);
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
	replayer.register_sink(&sink, "p07_m1");
	sink.onInit = [&replayer, &sink]() {
		sink.slice = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
	};

	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, sink.slice);
	replayer.run(false);

	// 8 根分钟线闭盘（首根 0931 被初始定位吃掉）
	ASSERT_EQ(kM1Closed, sink.closedPrices.size());
	EXPECT_EQ("m", sink.closedPeriods[0]);
	std::vector<double> expectCloses;
	for (uint32_t i = 1; i < kM1Cnt; i++)
		expectCloses.push_back(kM1Bars[i].close);
	EXPECT_EQ(expectCloses, sink.closedPrices);

	// 每根闭盘对应开高低收 4 笔模拟tick，且价格与K线字段严格一致
	ASSERT_EQ(kM1Closed * 4, sink.tickPrices.size());
	for (uint32_t i = 0; i < kM1Closed; i++)
	{
		const CsvBar& bar = kM1Bars[i + 1];
		EXPECT_EQ(bar.open, sink.tickPrices[i * 4 + 0]) << "pxType 0 @" << i;
		EXPECT_EQ(bar.high, sink.tickPrices[i * 4 + 1]) << "pxType 1 @" << i;
		EXPECT_EQ(bar.low, sink.tickPrices[i * 4 + 2]) << "pxType 2 @" << i;
		EXPECT_EQ(bar.close, sink.tickPrices[i * 4 + 3]) << "pxType 3 @" << i;
	}
	for (uint32_t t = 0; t < kM1Closed; t++)
	{
		EXPECT_EQ(0u, sink.tickPxTypes[t * 4 + 0]);
		EXPECT_EQ(3u, sink.tickPxTypes[t * 4 + 3]);
	}
	for (const auto& c : sink.tickCodes)
		EXPECT_EQ(std::string("SSE.STK.600000"), c);

	// get_cur_price 有数据路径：闭盘瞬间等于当前bar收盘价
	ASSERT_EQ(kM1Closed, sink.curPriceAtClose.size());
	for (uint32_t i = 1; i < kM1Cnt; i++)
		EXPECT_EQ(kM1Bars[i].close, sink.curPriceAtClose[i - 1]);

	/*
	 * get_day_price 有数据路径：_day_cache 按交易日累积开高低。
	 * 第 1 天（bars 1..5）：open=10.0 高=12.5 低=9.8（首根 0931 的 9.7 不参与）
	 * 第 2 天（bars 6..8）：open=12.0 高=13.5 低=11.9
	 */
	ASSERT_EQ(kM1Closed, sink.dayHighAtClose.size());
	const double day1Open[] = {10.0, 10.0, 10.0, 10.0, 10.0};
	const double day1High[] = {10.5, 11.0, 12.0, 12.0, 12.5};
	const double day1Low[] = {9.8, 9.8, 9.8, 9.8, 9.8};
	const double day2Open[] = {12.0, 12.0, 12.0};
	const double day2High[] = {12.4, 13.0, 13.5};
	const double day2Low[] = {11.9, 11.9, 11.9};
	EXPECT_EQ(std::vector<double>(day1Open, day1Open + 5), sink.dayOpenAtClose);
	EXPECT_EQ(std::vector<double>(day1High, day1High + 5), sink.dayHighAtClose);
	EXPECT_EQ(std::vector<double>(day1Low, day1Low + 5), sink.dayLowAtClose);
	for (uint32_t i = 0; i < 3; i++)
	{
		EXPECT_EQ(day2Open[i], sink.dayOpenAtClose[5 + i]) << "day2 open @" << i;
		EXPECT_EQ(day2High[i], sink.dayHighAtClose[5 + i]) << "day2 high @" << i;
		EXPECT_EQ(day2Low[i], sink.dayLowAtClose[5 + i]) << "day2 low @" << i;
	}

	// 调度：每次循环推进一次 onMinuteEnd -> handle_schedule
	EXPECT_EQ(kM1Closed, sink.scheduleCalls);
	EXPECT_EQ(2u, sink.sectionCalls);          // 每个交易日 1500 是小节最后一根
	EXPECT_EQ(1u, sink.beginCalls);            // 只在跨到 20200103 时开一次新交易日
	EXPECT_EQ(2u, sink.endCalls);              // 两个交易日各闭一次
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200103u}), sink.beginTDates);
	EXPECT_EQ(20200103u, replayer.get_date());
	EXPECT_EQ(1500u, replayer.get_raw_time());
	EXPECT_TRUE(replayer.is_tick_simulated());
	EXPECT_FALSE(replayer.is_tick_enabled());

	sink.slice->release();
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
	sliceSizes.clear();

	RecordingSink sink;
	replayer.register_sink(&sink, "p07_cursor");
	sink.onInit = [&replayer]() {
		replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, true);
	};
	// 每次闭盘后取一次切片：游标已经推进到当前bar
	sink.onBarCloseProbe = [&replayer]() {
		WTSKlineSlice* sli = replayer.get_kline_slice("SSE.STK.600000", "m", 100, 1, false);
		sliceSizes.push_back(sli == NULL ? 0 : sli->size());
		if (sli != NULL)
			sli->release();
	};

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_EQ(kM1Closed, sliceSizes.size());
	// 第 i 次闭盘时已经收盘 i+1 根（首根被初始定位吃掉，游标已推进）
	std::vector<uint32_t> expectSizes;
	for (uint32_t i = 0; i < kM1Closed; i++)
		expectSizes.push_back(i + 1);
	EXPECT_EQ(expectSizes, sliceSizes);
}

/*
 * m15 重采样（bar 合并）：订阅 m15 时先装载 m5 原始线，再用 WTSDataFactory 合并。
 * 断言合并后至少存在一根 open/high/low/close/vol 完全等于 0935-0945 三根之和的bar。
 */
TEST_F(P07MinuteReplayTest, m15SliceResamplesFromM5Bars)
{
	ASSERT_TRUE(write_bars_csv(_base, "SSE.STK.600000_m5", kM5Bars, kM5Cnt, true));

	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base, 202001020930ull, 202001021500ull);
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
	for (const auto& c : sink.tickCodes)
	{
		if (c == "SSE.STK.600000")
			cntMain++;
		else if (c == "SSE.STK.600001")
			cntUnsub++;
	}
	// 未订阅合约按开高低收 4 笔一组模拟，且必须是 4 的整数倍
	EXPECT_GT(cntUnsub, 0u);
	EXPECT_EQ(0u, cntUnsub % 4);
	EXPECT_GT(cntMain, 0u);
	EXPECT_EQ(0u, cntMain % 4);

	// 只有主K线会闭盘
	for (const auto& c : sink.closedCodes)
		EXPECT_EQ(std::string("SSE.STK.600000"), c);
	EXPECT_EQ(kDayCnt - 1, sink.closedPrices.size());
	std::vector<double> expectCloses = {11.5, 12.5};
	EXPECT_EQ(expectCloses, sink.closedPrices);
	// 未订阅K线不参与 handle_bar_close，但 onMinuteEnd 会推进它的游标
	if (sink.slice != NULL)
		sink.slice->release();
}

/*
 * 崩溃/越界一类输入统一收进禁用用例（与 p04 同类，引用既有编号，不重复立案）：
 * 分钟CSV只有表头、0 行时，cacheRawBarsFromCSV 仍然执行
 *   stime = barsList->_bars[0].date / etime = _bars[_count - 1].date（_count == 0）
 * —— 空 vector 下标访问是未定义行为（BUG-P04-004），
 * 随后 locate_barindex 在空缓存上算出 UINT32_MAX 越界下标（BUG-P04-005）。
 */
TEST_F(P07MinuteReplayTest, DISABLED_emptyMinuteCsvHitsUnsubscriptedBars)
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
