/**
 * r43_replayer_tick_csv_test.cpp
 * Lane R43：补 HisDataReplayer 中「csv 模式下的 tick 数据文件」与「回放中强制终止」
 * 两块既有 p04/p07/q02 用例没有驱动到的分支：
 *   cacheRawTicksFromCSV(L3470-3587) —— bin/ticks 下 dsb 命中读取、尺寸校验失败早退、
 *                                     文件不存在告警
 *   get_tick_slice(L2470-2545)       —— etime 参数分支（L2487-2491）
 *   checkOrderDetails/Queues/Transactions(L2702/L2744/L2786) —— csv 模式禁用 level2 的告警分支
 *   sub_tick/sub_order_detail/sub_order_queue/sub_transaction(L3089-3165)
 *                                    —— .QFQ/.HFQ 后缀剥离（flag 1/2）与空码守卫
 *   run_by_bars(L919-920)            —— 回调中 stop() 后按终止标记退出
 * 数据全部由本文件在 temp_directory_path()/r43_* 下生成，TearDown 清理。
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
#include <WtDataStorage/DataDefine.h>
#include <WTSUtils/WTSCmpHelper.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path r43_root()
	{
		// 同 r43_replayer_init_basefiles_test.cpp：__FILE__ 当前是仓库内绝对路径，
		// 从源文件目录上溯 5 级到仓库根；若构建改为相对源路径需同步调整。
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path r43_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("r43_replayer_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	WTSVariant* r43_cfg(const std::filesystem::path& root, const std::filesystem::path& base, bool tick,
		uint64_t stime = 202001020930ull, uint64_t etime = 202001081500ull, bool includeFutures = false)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		if (includeFutures)
		{
			WTSVariant* commodities = WTSVariant::createArray();
			commodities->append((root / "dist/common/stk_comms.json").string().c_str());
			commodities->append((root / "dist/common/commodities.json").string().c_str());
			basefiles->append("commodity", commodities);
			commodities->release();
		}
		else
			basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	struct TickRow
	{
		uint32_t hhmm;
		double price;
	};

	struct TimedTickRow
	{
		uint32_t date;
		uint32_t hhmm;
		uint32_t second;
		uint32_t millisecond;
		double price;
	};

	const TickRow kTicks[] = {
		{931u, 10.0},
		{932u, 11.0},
		{933u, 12.0},
	};
	const uint32_t kTickCnt = sizeof(kTicks) / sizeof(kTicks[0]);

	std::vector<WTSTickStruct> make_ticks(uint32_t uDate, const char* code)
	{
		std::vector<WTSTickStruct> ticks;
		for (uint32_t i = 0; i < kTickCnt; i++)
		{
			WTSTickStruct ts;
			memset(&ts, 0, sizeof(ts));
			strcpy(ts.exchg, "SSE");
			strcpy(ts.code, code);
			ts.action_date = uDate;
			ts.action_time = kTicks[i].hhmm * 100000;
			ts.price = kTicks[i].price;
			ts.open = kTicks[i].price;
			ts.high = kTicks[i].price;
			ts.low = kTicks[i].price;
			ts.volume = 100.0;
			ts.total_volume = 100.0 * (i + 1);
			ticks.emplace_back(ts);
		}
		return ticks;
	}

	WTSTickStruct make_timed_tick(const char* code, const TimedTickRow& row,
		uint32_t tradingDate = 0, const char* exchange = "SSE")
	{
		WTSTickStruct tick;
		memset(&tick, 0, sizeof(tick));
		strcpy(tick.exchg, exchange);
		strcpy(tick.code, code);
		tick.action_date = row.date;
		tick.action_time = row.hhmm * 100000 + row.second * 1000 + row.millisecond;
		tick.trading_date = tradingDate == 0 ? row.date : tradingDate;
		tick.price = row.price;
		tick.open = row.price;
		tick.high = row.price;
		tick.low = row.price;
		tick.volume = 1;
		tick.total_volume = 1;
		return tick;
	}

	uint64_t encoded_hft_time(const TimedTickRow& row)
	{
		const uint64_t actionTime = static_cast<uint64_t>(row.hhmm) * 100000 + row.second * 1000 + row.millisecond;
		return static_cast<uint64_t>(row.date) * 1000000000 + actionTime;
	}

	// <base>/bin/ticks/<code>_tick_<date>.dsb，格式同 cacheRawTicksFromCSV 期望的
	// HisTickBlockV2 + zstd 压缩体
	bool write_ticks_dsb(const std::filesystem::path& base, const char* code, uint32_t uDate,
		const std::vector<WTSTickStruct>& ticks)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "bin" / "ticks", ec);

		std::string cmp = WTSCmpHelper::compress_data(ticks.data(), sizeof(WTSTickStruct) * ticks.size());

		std::string content(sizeof(HisTickBlockV2), 0);
		HisTickBlockV2* blk = (HisTickBlockV2*)content.data();
		strcpy(blk->_blk_flag, BLK_FLAG);
		blk->_type = BT_HIS_Ticks;
		blk->_version = BLOCK_VERSION_CMP_V2;
		blk->_size = cmp.size();
		content.append(cmp);

		std::ofstream out(base / "bin" / "ticks" / (std::string(code) + "_tick_" + std::to_string(uDate) + ".dsb"),
			std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), (std::streamsize)content.size());
		out.close();
		return true;
	}

	bool write_tick_dsb(const std::filesystem::path& base, const char* code, uint32_t uDate)
	{
		return write_ticks_dsb(base, code, uDate, make_ticks(uDate, code));
	}

	bool write_short_tick_dsb(const std::filesystem::path& base, const char* code, uint32_t uDate)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "bin" / "ticks", ec);
		std::ofstream out(base / "bin" / "ticks" / (std::string(code) + "_tick_" + std::to_string(uDate) + ".dsb"),
			std::ios::binary);
		if (!out.is_open())
			return false;
		out << "short";
		out.close();
		return true;
	}

	struct DayBar
	{
		uint32_t date;
		double close;
	};

	const DayBar kDayBars[] = {
		{20200102u, 10.5},
		{20200103u, 11.5},
		{20200106u, 12.5},
		{20200107u, 13.5},
		{20200108u, 14.5},
	};
	const uint32_t kDayCnt = sizeof(kDayBars) / sizeof(kDayBars[0]);

	bool write_daily_csv(const std::filesystem::path& base, const char* stdCode)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (uint32_t i = 0; i < kDayCnt; i++)
		{
			out << kDayBars[i].date << ",150000,"
				<< (10.0 + i) << "," << (11.0 + i) << "," << (9.0 + i) << ","
				<< kDayBars[i].close << "," << (1000 + i) << "," << ((1000 + i) * 10.0) << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	class R43Sink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		std::function<void()> onBarClose;
		std::function<void()> onSessionBeginProbe;
		std::function<void(uint32_t)> onSessionBeginDateProbe;
		std::function<void()> onTickProbe;

		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t replayDoneCalls {0};
		uint32_t tickCalls {0};

		std::vector<double> tickPrices;
		std::vector<std::string> tickCodes;
		std::vector<uint32_t> tickDates;
		std::vector<uint64_t> tickNaturalTimes;
		std::vector<uint32_t> closedDates;
		std::vector<double> closedPrices;
		std::vector<uint32_t> beginTDates;
		std::vector<uint32_t> beginClockDates;
		std::vector<uint32_t> beginClockTimes;
		std::vector<uint32_t> beginClockSeconds;
		std::vector<uint32_t> beginGetterTDates;
		std::vector<uint32_t> beginCallbackTDates;
		std::vector<uint32_t> beginDefaultHftSliceSizes;
		std::vector<uint32_t> beginHftSliceSizes;
		std::vector<double> beginHftSlicePrices;
		std::vector<uint32_t> tickHistorySizes;
		std::vector<double> tickHistoryPrices;
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
			(void)stdCode; (void)pxType;
			tickCalls++;
			tickCodes.push_back(stdCode == nullptr ? "" : stdCode);
			if (curTick != nullptr)
			{
				tickPrices.push_back(curTick->price());
				tickDates.push_back(curTick->actiondate());
				tickNaturalTimes.push_back(static_cast<uint64_t>(curTick->actiondate()) * 1000000000 + curTick->actiontime());
			}
			if (onTickProbe)
				onTickProbe();
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			(void)stdCode; (void)period; (void)times;
			if (newBar != nullptr)
			{
				closedDates.push_back(newBar->date);
				closedPrices.push_back(newBar->close);
			}
			if (onBarClose)
				onBarClose();
		}

		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			scheduleDates.push_back(uDate);
			scheduleTimes.push_back(uTime);
		}
		void handle_session_begin(uint32_t curTDate) override
		{
			beginCalls++;
			beginTDates.push_back(curTDate);
			if (onSessionBeginProbe)
				onSessionBeginProbe();
			if (onSessionBeginDateProbe)
				onSessionBeginDateProbe(curTDate);
		}
		void handle_session_end(uint32_t) override { endCalls++; }
		void handle_replay_done() override { replayDoneCalls++; }
	};
}

class R43ReplayerTickCsvTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = r43_root();
		_base = r43_temp_dir("data");
		_out = r43_temp_dir("out");
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

// cacheRawTicksFromCSV 的 dsb 命中分支 + get_tick_slice 的 etime 定位 + run_by_ticks 回放
TEST_F(R43ReplayerTickCsvTest, csvTickDsbReplaysWholeDay)
{
	const char* code = "SSE.STK.000001";
	const char* oneTickCode = "SSE.STK.000002";
	const char* betweenTickCode = "SSE.STK.000003";
	ASSERT_TRUE(write_tick_dsb(_base, code, 20200102u));
	std::vector<WTSTickStruct> oneTick = make_ticks(20200102u, oneTickCode);
	oneTick.resize(1);
	ASSERT_TRUE(write_ticks_dsb(_base, oneTickCode, 20200102u, oneTick));
	ASSERT_TRUE(write_ticks_dsb(_base, betweenTickCode, 20200102u,
		std::vector<WTSTickStruct>{
			make_timed_tick("000003", {20200102u, 931u, 30u, 0u, 13.0}),
			make_timed_tick("000003", {20200102u, 933u, 0u, 0u, 14.0})}));

	// 第一段只验证切片定位：get_tick_slice 会把游标推到截止时间之后，
	// 所以不能和回放共用同一个对象。
	{
		HisDataReplayer query;
		WTSVariant* cfg = r43_cfg(_root, _base, true);
		ASSERT_TRUE(query.init(cfg));
		cfg->release();
		R43Sink sink;
		query.register_sink(&sink, "r43_tickquery");
		ASSERT_TRUE(query.prepare());
		// 订阅放在 prepare 之后：prepare 内部的 reset() 会清空订阅表
		query.sub_tick(1, code);
		query.sub_tick(2, oneTickCode);
		query.sub_tick(3, betweenTickCode);

		// A cutoff after the final 09:33 record returns the final two records,
		// including the final tick that the old cursor arithmetic dropped.
		WTSTickSlice* slice = query.get_tick_slice(code, 2, 202001021000ull);
		ASSERT_NE(nullptr, slice);
		ASSERT_EQ(2u, slice->size());
		EXPECT_DOUBLE_EQ(11.0, slice->at(0)->price);
		EXPECT_DOUBLE_EQ(12.0, slice->at(1)->price);
		slice->release();

		WTSTickSlice* finalTick = query.get_tick_slice(code, 1, 202001021000ull);
		ASSERT_NE(nullptr, finalTick);
		ASSERT_EQ(1u, finalTick->size());
		EXPECT_DOUBLE_EQ(12.0, finalTick->at(0)->price);
		finalTick->release();

		WTSTickSlice* allTicks = query.get_tick_slice(code, 10, 202001021000ull);
		ASSERT_NE(nullptr, allTicks);
		ASSERT_EQ(3u, allTicks->size());
		EXPECT_DOUBLE_EQ(10.0, allTicks->at(0)->price);
		EXPECT_DOUBLE_EQ(11.0, allTicks->at(1)->price);
		EXPECT_DOUBLE_EQ(12.0, allTicks->at(2)->price);
		allTicks->release();

		// The cutoff is strict: a record exactly at 09:32 is not visible.
		WTSTickSlice* exactBoundary = query.get_tick_slice(code, 10, 202001020932ull);
		ASSERT_NE(nullptr, exactBoundary);
		ASSERT_EQ(1u, exactBoundary->size());
		EXPECT_DOUBLE_EQ(10.0, exactBoundary->at(0)->price);
		exactBoundary->release();

		// 09:32 lies between these two records, so only the 09:31:30 tick is visible.
		WTSTickSlice* betweenRecords = query.get_tick_slice(betweenTickCode, 10, 202001020932ull);
		ASSERT_NE(nullptr, betweenRecords);
		ASSERT_EQ(1u, betweenRecords->size());
		EXPECT_DOUBLE_EQ(13.0, betweenRecords->at(0)->price);
		betweenRecords->release();

		// The same after-last boundary must also work with a one-record block.
		WTSTickSlice* oneRecord = query.get_tick_slice(oneTickCode, 1, 202001021000ull);
		ASSERT_NE(nullptr, oneRecord);
		ASSERT_EQ(1u, oneRecord->size());
		EXPECT_DOUBLE_EQ(10.0, oneRecord->at(0)->price);
		oneRecord->release();
	}

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	R43Sink sink;
	replayer.register_sink(&sink, "r43_tickcsv");
	ASSERT_TRUE(replayer.prepare());
	// 订阅必须在 prepare 之后登记，否则会被 reset() 清掉导致 checkAllTicks 恒 false
	replayer.sub_tick(1, code);

	replayer.run(false);

	// 最后一笔也必须回调，且按原始数据完整推进最新价。
	ASSERT_EQ(kTickCnt, sink.tickCalls);
	EXPECT_DOUBLE_EQ(10.0, sink.tickPrices[0]);
	EXPECT_DOUBLE_EQ(11.0, sink.tickPrices[1]);
	EXPECT_DOUBLE_EQ(12.0, sink.tickPrices[2]);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_DOUBLE_EQ(12.0, replayer.get_cur_price(code));
}

TEST_F(R43ReplayerTickCsvTest, pureTickRangeIncludesEveryMillisecondOfFourteenDigitEndSecond)
{
	const char* code = "SSE.STK.000001";
	const std::vector<TimedTickRow> rows = {
		{20200102u, 930u, 44u, 999u, 1.0},
		{20200102u, 930u, 45u, 0u, 2.0},
		{20200102u, 930u, 45u, 123u, 3.0},
		{20200102u, 930u, 45u, 999u, 4.0},
		{20200102u, 930u, 46u, 0u, 5.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (const TimedTickRow& row : rows)
		ticks.push_back(make_timed_tick("000001", row));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200102u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 20200102093045ull, 20200102093045ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_tick14_second");
	sink.onSessionBeginProbe = [&replayer, &sink]() {
		sink.beginClockDates.push_back(replayer.get_date());
		sink.beginClockTimes.push_back(replayer.get_min_time());
		sink.beginClockSeconds.push_back(replayer.get_secs());
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({
		encoded_hft_time(rows[1]), encoded_hft_time(rows[2]), encoded_hft_time(rows[3])
	}), sink.tickNaturalTimes);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.beginClockDates);
	EXPECT_EQ(std::vector<uint32_t>({930u}), sink.beginClockTimes);
	EXPECT_EQ(std::vector<uint32_t>({45000u}), sink.beginClockSeconds);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(R43ReplayerTickCsvTest, sessionBeginClockUsesRangeStartThenNextNaturalSessionOpen)
{
	const char* code = "SSE.STK.000001";
	const TimedTickRow day1{20200102u, 931u, 0u, 0u, 10.0};
	const TimedTickRow day2{20200103u, 931u, 0u, 0u, 11.0};
	ASSERT_TRUE(write_ticks_dsb(_base, code, day1.date,
		std::vector<WTSTickStruct>{make_timed_tick("000001", day1)}));
	ASSERT_TRUE(write_ticks_dsb(_base, code, day2.date,
		std::vector<WTSTickStruct>{make_timed_tick("000001", day2)}));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 20200102093045ull, 20200103093200ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_session_clock_day_boundary");
	sink.onSessionBeginProbe = [&replayer, &sink]() {
		sink.beginClockDates.push_back(replayer.get_date());
		sink.beginClockTimes.push_back(replayer.get_min_time());
		sink.beginClockSeconds.push_back(replayer.get_secs());
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginClockDates);
	EXPECT_EQ(std::vector<uint32_t>({930u, 930u}), sink.beginClockTimes);
	EXPECT_EQ(std::vector<uint32_t>({45000u, 0u}), sink.beginClockSeconds);
	EXPECT_EQ(std::vector<uint64_t>({encoded_hft_time(day1), encoded_hft_time(day2)}), sink.tickNaturalTimes);
}

TEST_F(R43ReplayerTickCsvTest, twelveDigitBoundsIncludeWholeStartAndEndMinutes)
{
	const char* code = "SSE.STK.000001";
	const std::vector<TimedTickRow> rows = {
		{20200102u, 930u, 0u, 0u, 10.0},
		{20200102u, 930u, 59u, 999u, 11.0},
		{20200102u, 931u, 0u, 0u, 12.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (const TimedTickRow& row : rows)
		ticks.push_back(make_timed_tick("000001", row));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200102u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 202001020930ull, 202001020930ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_tick12_minute");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({encoded_hft_time(rows[0]), encoded_hft_time(rows[1])}), sink.tickNaturalTimes);
}

TEST_F(R43ReplayerTickCsvTest, mixedPrecisionUsesMinuteStartAndSecondInclusiveEnd)
{
	const char* code = "SSE.STK.000001";
	const std::vector<TimedTickRow> rows = {
		{20200102u, 930u, 0u, 0u, 20.0},
		{20200102u, 930u, 14u, 999u, 21.0},
		{20200102u, 930u, 15u, 0u, 22.0},
		{20200102u, 930u, 15u, 999u, 23.0},
		{20200102u, 930u, 16u, 0u, 24.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (const TimedTickRow& row : rows)
		ticks.push_back(make_timed_tick("000001", row));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200102u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 202001020930ull, 20200102093015ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_tick_mixed_precision");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({
		encoded_hft_time(rows[0]), encoded_hft_time(rows[1]),
		encoded_hft_time(rows[2]), encoded_hft_time(rows[3])
	}), sink.tickNaturalTimes);
}

TEST_F(R43ReplayerTickCsvTest, mainBarsWithoutAClosedBarStillReplayInRangeTicksOnly)
{
	const char* code = "SSE.STK.000001";
	ASSERT_TRUE(write_daily_csv(_base, code));
	const std::vector<TimedTickRow> rows = {
		{20200102u, 930u, 30u, 0u, 1.0},
		{20200102u, 930u, 30u, 999u, 2.0},
		{20200102u, 930u, 31u, 0u, 3.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (const TimedTickRow& row : rows)
		ticks.push_back(make_timed_tick("000001", row));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200102u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 20200102093030ull, 20200102093030ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_empty_bar_range");
	sink.onInit = [&replayer, code]() {
		WTSKlineSlice* main = replayer.get_kline_slice(code, "d", 1u, 1u, true);
		if (main != nullptr)
			main->release();
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({ encoded_hft_time(rows[0]), encoded_hft_time(rows[1]) }), sink.tickNaturalTimes);
	EXPECT_TRUE(sink.closedDates.empty());
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(R43ReplayerTickCsvTest, overnightTickRangeLoadsByTradingDateAndFiltersByActionDate)
{
	const char* code = "SHFE.au.2006";
	const std::vector<TimedTickRow> rows = {
		{20200102u, 2100u, 0u, 0u, 30.0},
		{20200102u, 2100u, 0u, 999u, 31.0},
		{20200102u, 2100u, 1u, 0u, 32.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (const TimedTickRow& row : rows)
		ticks.push_back(make_timed_tick("au2006", row, 20200103u, "SHFE"));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200103u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 20200102210000ull, 20200102210000ull, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_tick_overnight");
	sink.onSessionBeginProbe = [&replayer, &sink]() {
		sink.beginClockDates.push_back(replayer.get_date());
		sink.beginClockTimes.push_back(replayer.get_min_time());
		sink.beginClockSeconds.push_back(replayer.get_secs());
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({encoded_hft_time(rows[0]), encoded_hft_time(rows[1])}), sink.tickNaturalTimes);
	EXPECT_EQ(std::vector<uint32_t>({20200103u}), sink.beginTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.beginClockDates);
	EXPECT_EQ(std::vector<uint32_t>({2100u}), sink.beginClockTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u}), sink.beginClockSeconds);
}

TEST_F(R43ReplayerTickCsvTest, nightSessionBeginClockUsesNaturalOpenAcrossTradingDates)
{
	const char* code = "SHFE.au.2006";
	const TimedTickRow night1{20200106u, 2100u, 46u, 0u, 30.0};
	const TimedTickRow night2{20200107u, 2100u, 15u, 0u, 31.0};
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200107u,
		std::vector<WTSTickStruct>{make_timed_tick("au2006", night1, 20200107u, "SHFE")}));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200108u,
		std::vector<WTSTickStruct>{make_timed_tick("au2006", night2, 20200108u, "SHFE")}));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 20200106210045ull, 20200107210100ull, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_night_session_clock");
	sink.onSessionBeginProbe = [&replayer, &sink]() {
		sink.beginClockDates.push_back(replayer.get_date());
		sink.beginClockTimes.push_back(replayer.get_min_time());
		sink.beginClockSeconds.push_back(replayer.get_secs());
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200107u, 20200108u}), sink.beginTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200106u, 20200107u}), sink.beginClockDates);
	EXPECT_EQ(std::vector<uint32_t>({2100u, 2100u}), sink.beginClockTimes);
	EXPECT_EQ(std::vector<uint32_t>({45000u, 0u}), sink.beginClockSeconds);
	EXPECT_EQ(std::vector<uint64_t>({encoded_hft_time(night1), encoded_hft_time(night2)}), sink.tickNaturalTimes);
}

TEST_F(R43ReplayerTickCsvTest, mainBarsUseReplayableHftSessionForMixedNaturalDate)
{
	const char* mainCode = "SSE.STK.000001";
	const char* hftCode = "SHFE.au.2006";
	ASSERT_TRUE(write_daily_csv(_base, mainCode));
	const TimedTickRow nightFirst{20200102u, 2100u, 30u, 0u, 30.0};
	const TimedTickRow nightSecond{20200102u, 2100u, 45u, 0u, 31.0};
	ASSERT_TRUE(write_ticks_dsb(_base, hftCode, 20200103u,
		std::vector<WTSTickStruct>{
			make_timed_tick("au2006", nightFirst, 20200103u, "SHFE"),
			make_timed_tick("au2006", nightSecond, 20200103u, "SHFE")}));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 202001020930ull, 202001031500ull, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_mixed_main_hft_session_clock");
	sink.onInit = [&replayer, mainCode]() {
		WTSKlineSlice* main = replayer.get_kline_slice(mainCode, "d", 1u, 1u, true);
		if (main != nullptr)
			main->release();
	};
	sink.onSessionBeginProbe = [&replayer, &sink]() {
		sink.beginClockDates.push_back(replayer.get_date());
		sink.beginClockTimes.push_back(replayer.get_min_time());
		sink.beginClockSeconds.push_back(replayer.get_secs());
	};
	sink.onTickProbe = [&replayer, &sink, hftCode]() {
		if (sink.tickCodes.empty() || sink.tickCodes.back() != hftCode)
			return;
		WTSTickSlice* history = replayer.get_tick_slice(hftCode, 10u);
		sink.tickHistorySizes.push_back(history == nullptr ? 0u : history->size());
		if (history != nullptr)
		{
			if (history->size() > 0)
				sink.tickHistoryPrices.push_back(history->at(history->size() - 1)->price);
			history->release();
		}
	};
	sink.onSessionBeginDateProbe = [&replayer, &sink, hftCode](uint32_t callbackTDate) {
		sink.beginCallbackTDates.push_back(callbackTDate);
		sink.beginGetterTDates.push_back(replayer.get_trading_date());
		WTSTickSlice* defaultHistory = replayer.get_tick_slice(hftCode, 1u);
		sink.beginDefaultHftSliceSizes.push_back(defaultHistory == nullptr ? 0u : defaultHistory->size());
		if (defaultHistory != nullptr)
			defaultHistory->release();
		WTSTickSlice* history = replayer.get_tick_slice(hftCode, 2u, 202001022101ull);
		if (history == nullptr)
		{
			sink.beginHftSliceSizes.push_back(0u);
			return;
		}
		sink.beginHftSliceSizes.push_back(history->size());
		for (uint32_t i = 0; i < history->size(); i++)
			sink.beginHftSlicePrices.push_back(history->at(i)->price);
		history->release();
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1u, hftCode);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
	EXPECT_EQ(sink.beginCallbackTDates, sink.beginGetterTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u}), sink.beginClockDates);
	EXPECT_EQ(std::vector<uint32_t>({930u, 2100u}), sink.beginClockTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u}), sink.beginClockSeconds);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u}), sink.beginDefaultHftSliceSizes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 2u}), sink.beginHftSliceSizes);
	EXPECT_EQ(std::vector<double>({30.0, 31.0}), sink.beginHftSlicePrices);
	// At the first exact tick cutoff the current record is excluded; at the
	// second, the previous record is visible but the current one remains strict.
	EXPECT_EQ(std::vector<uint32_t>({0u, 1u}), sink.tickHistorySizes);
	EXPECT_EQ(std::vector<double>({30.0}), sink.tickHistoryPrices);
	const uint64_t jan2Close = static_cast<uint64_t>(20200102u) * 1000000000 + 1500u * 100000u;
	const uint64_t jan3Close = static_cast<uint64_t>(20200103u) * 1000000000 + 1500u * 100000u;
	ASSERT_EQ(10u, sink.tickCodes.size());
	EXPECT_EQ(std::vector<std::string>({mainCode, mainCode, mainCode, mainCode, hftCode, hftCode,
		mainCode, mainCode, mainCode, mainCode}), sink.tickCodes);
	EXPECT_EQ(std::vector<uint64_t>({jan2Close, jan2Close, jan2Close, jan2Close,
		encoded_hft_time(nightFirst), encoded_hft_time(nightSecond),
		jan3Close, jan3Close, jan3Close, jan3Close}), sink.tickNaturalTimes);
	EXPECT_EQ(std::vector<double>({10.0, 11.0, 9.0, 10.5, 30.0, 31.0, 11.0, 12.0, 10.0, 11.5}), sink.tickPrices);
	ASSERT_EQ(2u, sink.beginClockDates.size());
	const uint64_t firstBegin = static_cast<uint64_t>(sink.beginClockDates[0]) * 10000 + sink.beginClockTimes[0];
	const uint64_t secondBegin = static_cast<uint64_t>(sink.beginClockDates[1]) * 10000 + sink.beginClockTimes[1];
	EXPECT_LE(firstBegin, secondBegin);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.closedDates);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

// dsb 文件存在但小于块头 -> 尺寸校验失败，回放无任何 tick
TEST_F(R43ReplayerTickCsvTest, shortTickDsbRejected)
{
	const char* code = "SSE.STK.000001";
	ASSERT_TRUE(write_short_tick_dsb(_base, code, 20200102u));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	R43Sink sink;
	replayer.register_sink(&sink, "r43_shortdsb");
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);

	EXPECT_EQ(nullptr, replayer.get_tick_slice(code, 1));
	EXPECT_EQ(nullptr, replayer.get_last_tick(code));

	replayer.run(false);
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

// 复权后缀剥离（.QFQ -> flag 1 / .HFQ -> flag 2）与空码守卫；csv 模式 level2 一律返回空
TEST_F(R43ReplayerTickCsvTest, trailingSuffixSubscriptionsStripFlag)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	// prepare() 会调用 _listener->handle_init()（HisDataReplayer.cpp:669），必须先注册 sink；
	// reset() 又会清空订阅表，所以订阅动作放到 prepare 之后（后缀剥离发生在订阅时）。
	R43Sink sink;
	replayer.register_sink(&sink, "r43_suffix");
	ASSERT_TRUE(replayer.prepare());

	// 空码直接早退（L3091/L3115/L3133/L3151）
	replayer.sub_tick(1, "");
	replayer.sub_order_detail(1, "");
	replayer.sub_order_queue(1, "");
	replayer.sub_transaction(1, "");

	// 后缀分支：QFQ 走 flag=1，HFQ 走 flag=2
	replayer.sub_tick(2, "SSE.STK.000001.QFQ");
	replayer.sub_tick(3, "SSE.STK.000001.HFQ");
	replayer.sub_order_detail(2, "SSE.STK.000001.QFQ");
	replayer.sub_order_queue(3, "SSE.STK.000001.HFQ");
	replayer.sub_transaction(4, "SSE.STK.000001.QFQ");

	// 没有tick文件 -> 三族切片都为空；level2 在 csv 模式下直接拒绝（L2721/L2763/L2805）
	EXPECT_EQ(nullptr, replayer.get_tick_slice("SSE.STK.000001", 1));
	EXPECT_EQ(nullptr, replayer.get_order_detail_slice("SSE.STK.000001", 1));
	EXPECT_EQ(nullptr, replayer.get_order_queue_slice("SSE.STK.000001", 1));
	EXPECT_EQ(nullptr, replayer.get_transaction_slice("SSE.STK.000001", 1));

	// 复权代码的K线请求会先登记未订阅集合（L2164-2170），无 csv 文件时返回空
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.000001.QFQ", "d", 1, 1, false));
	replayer.stop();
}

// 主K线 + tick 模式：run_by_bars 内部走 replayHftDatas(L1904-2011) 逐笔回放，
// tick 用尽后返回 false -> 回落到模拟tick；最后一根K线的交易日无tick数据
TEST_F(R43ReplayerTickCsvTest, barsWithTicksUseHftReplayPath)
{
	const char* code = "SSE.STK.000001";
	ASSERT_TRUE(write_daily_csv(_base, code));

	// 20200103 的 tick：09:31/09:32 落在K线区间内，15:00/15:01 用来触发
	// replayHftDatas 的 `nextTime/100000 >= etime` 退出分支
	std::vector<WTSTickStruct> ticks;
	const uint32_t times[4] = {931u, 932u, 1500u, 1501u};
	const double prices[4] = {20.1, 20.2, 20.5, 20.6};
	for (uint32_t i = 0; i < 4; i++)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, "SSE");
		strcpy(ts.code, "000001");
		ts.action_date = 20200103u;
		ts.action_time = times[i] * 100000;
		ts.price = prices[i];
		ts.open = prices[i];
		ts.high = prices[i];
		ts.low = prices[i];
		ts.volume = 100.0;
		ts.total_volume = 100.0;
		ticks.emplace_back(ts);
	}
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200103u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	R43Sink sink;
	replayer.register_sink(&sink, "r43_bartick");
	sink.onInit = [&replayer, code]() {
		replayer.get_kline_slice(code, "d", 1, 1, true);
	};
	ASSERT_TRUE(replayer.prepare());
	// Tick 子订阅不改变主K线日期范围：日线区间仍从 20200102 首根开始。
	replayer.sub_tick(1, code);

	replayer.run(false);

	std::vector<uint32_t> expectedDates;
	std::vector<double> expectedCloses;
	for (uint32_t i = 0; i < kDayCnt; i++)
	{
		expectedDates.push_back(kDayBars[i].date);
		expectedCloses.push_back(kDayBars[i].close);
	}
	EXPECT_EQ(kDayCnt, sink.closedDates.size());
	EXPECT_EQ(expectedDates, sink.closedDates);
	EXPECT_EQ(expectedCloses, sink.closedPrices);
	EXPECT_EQ(expectedDates, sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>(kDayCnt, 1500u), sink.scheduleTimes);
	if (!sink.closedDates.empty() && !sink.closedPrices.empty())
	{
		EXPECT_EQ(kDayBars[0].date, sink.closedDates.front());
		EXPECT_EQ(kDayBars[kDayCnt - 1].date, sink.closedDates.back());
		EXPECT_DOUBLE_EQ(kDayBars[0].close, sink.closedPrices.front());
		EXPECT_DOUBLE_EQ(kDayBars[kDayCnt - 1].close, sink.closedPrices.back());
	}
	// 前两笔来自 replayHftDatas 的真实tick，价格必须与 dsb 一致
	std::vector<double> day03TickPrices;
	for (std::size_t i = 0; i < sink.tickPrices.size() && i < sink.tickDates.size(); i++)
	{
		if (sink.tickDates[i] == 20200103u)
			day03TickPrices.push_back(sink.tickPrices[i]);
	}
	EXPECT_GE(day03TickPrices.size(), 2u);
	if (day03TickPrices.size() >= 2)
	{
		EXPECT_DOUBLE_EQ(20.1, day03TickPrices[0]);
		EXPECT_DOUBLE_EQ(20.2, day03TickPrices[1]);
	}
	// 20200103 之后没有tick文件 -> 回落到模拟tick
	EXPECT_TRUE(replayer.is_tick_simulated());
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(R43ReplayerTickCsvTest, finalMainBarFlushesOnlyInRangeTicksWithoutAnotherBar)
{
	const char* code = "SSE.STK.000001";
	ASSERT_TRUE(write_daily_csv(_base, code));
	const std::vector<TimedTickRow> rows = {
		{20200108u, 1459u, 59u, 999u, 40.0},
		{20200108u, 1500u, 0u, 0u, 41.0},
		{20200108u, 1500u, 30u, 999u, 42.0},
		{20200108u, 1500u, 31u, 0u, 43.0},
		{20200109u, 931u, 45u, 999u, 44.0},
		{20200109u, 931u, 46u, 0u, 45.0},
	};
	std::vector<WTSTickStruct> ticks;
	for (std::size_t i = 0; i < 4; i++)
		ticks.push_back(make_timed_tick("000001", rows[i], 20200108u));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200108u, ticks));
	ticks.clear();
	for (std::size_t i = 4; i < rows.size(); i++)
		ticks.push_back(make_timed_tick("000001", rows[i], 20200109u));
	ASSERT_TRUE(write_ticks_dsb(_base, code, 20200109u, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, true, 202001081459ull, 20200109093145ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	R43Sink sink;
	replayer.register_sink(&sink, "r43_tick_final_flush");
	sink.onInit = [&replayer, code]() {
		replayer.get_kline_slice(code, "d", 1, 1, true);
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.sub_tick(1, code);
	replayer.run(false);

	EXPECT_EQ(std::vector<uint64_t>({
		encoded_hft_time(rows[0]), encoded_hft_time(rows[1]), encoded_hft_time(rows[2]),
		encoded_hft_time(rows[3]), encoded_hft_time(rows[4])
	}), sink.tickNaturalTimes);
	EXPECT_EQ(std::vector<uint32_t>({20200108u}), sink.closedDates);
	EXPECT_EQ(std::vector<double>({kDayBars[kDayCnt - 1].close}), sink.closedPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200108u, 20200109u}), sink.beginTDates);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

// 回调里 stop()：run_by_bars 按终止标记退出（L919-920），不再回放后续K线
TEST_F(R43ReplayerTickCsvTest, stopDuringBarReplayTerminatesCleanly)
{
	const char* code = "SSE.STK.000001";
	ASSERT_TRUE(write_daily_csv(_base, code));

	HisDataReplayer replayer;
	WTSVariant* cfg = r43_cfg(_root, _base, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	R43Sink sink;
	replayer.register_sink(&sink, "r43_stopbar");
	sink.onInit = [&replayer, code]() {
		replayer.get_kline_slice(code, "d", 1, 1, true);
	};
	sink.onBarClose = [&replayer, &sink]() {
		if (sink.closedDates.size() == 2)
			replayer.stop();
	};

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	// on_bar_close 每完成一根就检查数量：首根 20200102 后继续，第二根 20200103 后 stop。
	ASSERT_EQ(2u, sink.closedDates.size());
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.closedDates);
	EXPECT_EQ(std::vector<double>({10.5, 11.5}), sink.closedPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>(2, 1500u), sink.scheduleTimes);
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}
