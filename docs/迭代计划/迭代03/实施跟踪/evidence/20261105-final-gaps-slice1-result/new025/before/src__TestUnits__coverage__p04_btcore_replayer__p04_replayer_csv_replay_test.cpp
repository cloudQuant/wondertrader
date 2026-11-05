/**
 * p04_replayer_csv_replay_test.cpp
 * 用临时目录里的 CSV 日线驱动 HisDataReplayer 的回放主循环与数据装配：
 *   get_kline_slice(L2132)  —— 主K线登记、_ticker_keys、_codes_in_subbed、游标初始定位
 *   cacheRawBarsFromCSV(L3737) —— csv 装载分支 + 转储 .dsb
 *   run_by_bars(L771)       —— 交易日切换(session_begin/end)、section_end、
 *                              onMinuteEnd(L2013) 日线闭盘与游标推进、结束条件
 *   locate_barindex(L593)   —— 起止下标定位
 *   reset(L459)             —— 时间字段归零
 * 时间约定：SD0930（dist/common/sessions.json）收盘 1500，故日线回放时
 * nextBarTime = date*10000 + 1500，5 根日线 -> 5 次 session_begin/end/section_end/schedule。
 * 所有落盘文件都在 temp_directory_path()/p04_* 下，TearDown 清理，不写仓库。
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
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include <WtDataStorage/DataDefine.h>

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
			("p04_replay_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	// 5 个交易日（跳过 20200104/05 周末）的固定日线
	struct DayBar
	{
		uint32_t date;
		double open;
		double high;
		double low;
		double close;
	};

	const DayBar kDayBars[] = {
		{20200102u, 10.0, 11.0,  9.5, 10.5},
		{20200103u, 10.5, 12.0, 10.4, 11.5},
		{20200106u, 11.5, 13.0, 11.4, 12.5},
		{20200107u, 12.5, 14.0, 12.4, 13.5},
		{20200108u, 13.5, 15.0, 13.4, 14.5},
	};
	const uint32_t kBarCnt = sizeof(kDayBars) / sizeof(kDayBars[0]);

	// <base>/csv/SSE.STK.000001_d.csv —— cacheRawBarsFromCSV 的入参路径
	bool write_daily_csv(const std::filesystem::path& base, const char* stdCode, bool withRows)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		if (withRows)
		{
			for (uint32_t i = 0; i < kBarCnt; i++)
			{
				out << kDayBars[i].date << ",150000,"
					<< kDayBars[i].open << "," << kDayBars[i].high << ","
					<< kDayBars[i].low << "," << kDayBars[i].close << ","
					<< (1000 + i) << "," << (1000 + i) * 10.0 << ",0,0,0\n";
			}
		}
		out.close();
		return true;
	}

	bool write_zero_byte_daily_csv(const std::filesystem::path& base, const char* stdCode)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"), std::ios::binary);
		if (!out.is_open())
			return false;
		out.close();
		return true;
	}

	bool write_raw_bar_dsb(const std::filesystem::path& file, BlockType type,
		const std::vector<WTSBarStruct>& bars)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		if (ec)
			return false;

		std::string content(sizeof(BlockHeader) + bars.size() * sizeof(WTSBarStruct), '\0');
		BlockHeader header{};
		std::memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
		header._type = static_cast<uint16_t>(type);
		header._version = BLOCK_VERSION_RAW_V2;
		std::memcpy(content.data(), &header, sizeof(header));
		if (!bars.empty())
			std::memcpy(content.data() + sizeof(BlockHeader), bars.data(), bars.size() * sizeof(WTSBarStruct));

		std::ofstream out(file, std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		out.close();
		return out.good();
	}

	WTSBarStruct make_bar(uint32_t date, uint32_t hhmm, double price)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = static_cast<uint64_t>(date - 19900000u) * 10000u + hhmm;
		bar.open = price;
		bar.high = price + 0.1;
		bar.low = price - 0.1;
		bar.close = price;
		bar.settle = price;
		bar.vol = 100.0;
		bar.money = price * bar.vol;
		return bar;
	}

	class InstDirRestore
	{
	public:
		InstDirRestore() : _previous(WtHelper::getInstDir()) {}
		~InstDirRestore() { WtHelper::setInstDir(_previous.c_str()); }

	private:
		std::string _previous;
	};

	bool use_data_storage_module_from_build(const std::filesystem::path& root)
	{
		const std::string module = DLLHelper::wrap_module("WtDataStorage");
		std::vector<std::filesystem::path> candidates;
		const char* testModuleDir = std::getenv("WT_P04_DATA_STORAGE_DIR");
		if (testModuleDir != nullptr && *testModuleDir != '\0')
			candidates.emplace_back(testModuleDir);
		if (!WtHelper::getInstDir().empty())
			candidates.emplace_back(WtHelper::getInstDir());
#ifndef _WIN32
		// I03-NEW-105: the build that produced this test binary keeps its modules
		// in <build>/bin, one level above <build>/bin/TestUnits.
		Dl_info info;
		if (dladdr(reinterpret_cast<void*>(&use_data_storage_module_from_build), &info) != 0 && info.dli_fname != nullptr)
			candidates.emplace_back(std::filesystem::path(info.dli_fname).parent_path().parent_path());
#endif
		const char* const relativeDirs[] = {
			"build-cov/build_x64/Debug/bin",
			"build-cov/build_x64/Release/bin",
			"build_x64/Debug/bin",
			"build_x64/Release/bin",
		};
		for (const char* relativeDir : relativeDirs)
			candidates.emplace_back(root / relativeDir);

		for (const std::filesystem::path& dir : candidates)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir / module, ec) && !ec)
			{
				const std::string instDir = dir.string() + "/";
				WtHelper::setInstDir(instDir.c_str());
				return true;
			}
		}
		return false;
	}

	WTSVariant* replay_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		const char* mode = "csv")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", mode);
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());	// 卡1 A+C：prepare 起点 checked 映射需日历（同 I03-NEW-018）
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	class RecordingSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		WTSKlineSlice* slice {nullptr};
		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t sectionCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t replayDoneCalls {0};
		uint32_t tickCalls {0};
		std::vector<uint32_t> closedDates;
		std::vector<double> closedPrices;
		std::vector<std::string> closedPeriods;
		std::vector<uint32_t> beginTDates;
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
			(void)stdCode; (void)curTick; (void)pxType;
			tickCalls++;
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			(void)stdCode; (void)times;
			closedPeriods.emplace_back(period);
			if (newBar != nullptr)
			{
				closedDates.push_back(newBar->date);
				closedPrices.push_back(newBar->close);
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

		void handle_session_end(uint32_t curTDate) override
		{
			(void)curTDate;
			endCalls++;
		}

		void handle_section_end(uint32_t curTDate, uint32_t curTime) override
		{
			(void)curTDate; (void)curTime;
			sectionCalls++;
		}

		void handle_replay_done() override { replayDoneCalls++; }
	};
}

class P04ReplayCsvTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("csv");
		_out = make_temp_dir("csvout");
		WtHelper::setOutputDir(_out.string().c_str());
		ASSERT_TRUE(write_daily_csv(_base, "SSE.STK.000001", true));
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

TEST_F(P04ReplayCsvTest, dailyBarsReplayFullTradingDayCycle)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_daily");
	sink.onInit = [&replayer, &sink]() {
		// handle_init 内登记主K线：真实策略就是这么用的
		sink.slice = replayer.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	};

	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(1u, sink.initCalls);
	// 时段必须能解析出来，否则 run_by_bars 里 sInfo 是空指针（见 bugs）
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.000001", true));

	replayer.run(false);

	// 区间从首根交易日 09:30 开始，到末根 15:00 结束；两端日线都应闭盘回调。
	EXPECT_EQ(kBarCnt, sink.beginCalls);
	EXPECT_EQ(kBarCnt, sink.endCalls);
	EXPECT_EQ(kBarCnt, sink.sectionCalls);
	EXPECT_EQ(kBarCnt, sink.scheduleCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);

	std::vector<uint32_t> expectDates;
	std::vector<double> expectCloses;
	for (uint32_t i = 0; i < kBarCnt; i++)
	{
		expectDates.push_back(kDayBars[i].date);
		expectCloses.push_back(kDayBars[i].close);
	}
	EXPECT_EQ(expectDates, sink.closedDates);
	EXPECT_EQ(expectCloses, sink.closedPrices);
	EXPECT_EQ(expectDates, sink.beginTDates);
	EXPECT_EQ(expectDates, sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>(kBarCnt, 1500u), sink.scheduleTimes);
	if (!sink.closedDates.empty() && !sink.closedPrices.empty())
	{
		EXPECT_EQ(kDayBars[0].date, sink.closedDates.front());
		EXPECT_EQ(kDayBars[kBarCnt - 1].date, sink.closedDates.back());
		EXPECT_DOUBLE_EQ(kDayBars[0].close, sink.closedPrices.front());
		EXPECT_DOUBLE_EQ(kDayBars[kBarCnt - 1].close, sink.closedPrices.back());
	}
	for (const auto& p : sink.closedPeriods)
		EXPECT_EQ(std::string("d"), p);

	// 回放结束后的时间游标停在最后一根日线的收盘时刻
	EXPECT_EQ(20200108u, replayer.get_date());
	EXPECT_EQ(1500u, replayer.get_raw_time());
	EXPECT_EQ(0u, replayer.get_secs());
	EXPECT_EQ(20200108u, replayer.get_trading_date());
	// 未开启tick且没有真实tick数据 -> 走模拟tick分支（reset 把 _tick_simulated 置 true）
	EXPECT_TRUE(replayer.is_tick_simulated());
	EXPECT_FALSE(replayer.is_tick_enabled());

	if (sink.slice != nullptr)
		sink.slice->release();
}

TEST_F(P04ReplayCsvTest, prepareAfterRunResetsClock)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_reset");
	sink.onInit = [&replayer]() {
		replayer.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	};

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	ASSERT_EQ(20200108u, replayer.get_date());
	const uint32_t firstRunCloses = (uint32_t)sink.closedDates.size();
	EXPECT_EQ(kBarCnt, firstRunCloses);
	std::vector<uint32_t> expectedDates;
	std::vector<double> expectedCloses;
	for (uint32_t i = 0; i < kBarCnt; i++)
	{
		expectedDates.push_back(kDayBars[i].date);
		expectedCloses.push_back(kDayBars[i].close);
	}
	EXPECT_EQ(expectedDates, sink.closedDates);
	EXPECT_EQ(expectedCloses, sink.closedPrices);

	// 第二次 prepare 走 reset()：时间字段归零后重新按 stime 推进，
	// 且K线缓存不清空（只还原游标），所以第二轮闭盘根数相同
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(20200102u, replayer.get_date());
	EXPECT_EQ(930u, replayer.get_raw_time());
	EXPECT_EQ(0u, replayer.get_secs());
	EXPECT_EQ(20200102u, replayer.get_trading_date());
	EXPECT_EQ(2u, sink.initCalls);

	replayer.run(false);
	std::vector<uint32_t> expectedDatesTwice = expectedDates;
	expectedDatesTwice.insert(expectedDatesTwice.end(), expectedDates.begin(), expectedDates.end());
	std::vector<double> expectedClosesTwice = expectedCloses;
	expectedClosesTwice.insert(expectedClosesTwice.end(), expectedCloses.begin(), expectedCloses.end());
	EXPECT_EQ(kBarCnt * 2, sink.closedDates.size());
	EXPECT_EQ(expectedDatesTwice, sink.closedDates);
	EXPECT_EQ(expectedClosesTwice, sink.closedPrices);
	EXPECT_EQ(2u, sink.replayDoneCalls);
}

TEST_F(P04ReplayCsvTest, csvBarsTransferedToDsbAndReRead)
{
	// 第一轮：走 csv 分支并把数据转储成 .dsb
	{
		HisDataReplayer first;
		WTSVariant* cfg = replay_cfg(_root, _base);
		ASSERT_TRUE(first.init(cfg));
		cfg->release();
		RecordingSink sink;
		first.register_sink(&sink, "p04_first");
		sink.onInit = [&first]() { first.get_kline_slice("SSE.STK.000001", "d", 1, 1, true); };
		ASSERT_TRUE(first.prepare());
		first.run(false);
		EXPECT_EQ(kBarCnt, sink.closedDates.size());
		std::vector<uint32_t> expectDates;
		std::vector<double> expectCloses;
		for (uint32_t i = 0; i < kBarCnt; i++)
		{
			expectDates.push_back(kDayBars[i].date);
			expectCloses.push_back(kDayBars[i].close);
		}
		EXPECT_EQ(expectDates, sink.closedDates);
		EXPECT_EQ(expectCloses, sink.closedPrices);
	}

	std::error_code ec;
	bool dsbFound = false;
	for (auto& entry : std::filesystem::recursive_directory_iterator(_base / "his", ec))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".dsb")
		{
			dsbFound = true;
			EXPECT_GT(std::filesystem::file_size(entry.path(), ec), (std::uintmax_t)0);
		}
	}
	// 转储产物必须落在临时目录里
	ASSERT_TRUE(dsbFound) << "csv->dsb 转储没有产生文件";

	// 第二轮（新对象）：同名 .dsb 已存在 -> 走 proc_block_data 直读分支
	HisDataReplayer second;
	WTSVariant* cfg2 = replay_cfg(_root, _base);
	ASSERT_TRUE(second.init(cfg2));
	cfg2->release();
	RecordingSink sink2;
	second.register_sink(&sink2, "p04_second");
	sink2.onInit = [&second, &sink2]() {
		sink2.slice = second.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	};
	ASSERT_TRUE(second.prepare());
	second.run(false);

	std::vector<double> expectCloses;
	std::vector<uint32_t> expectDates;
	for (uint32_t i = 0; i < kBarCnt; i++)
	{
		expectCloses.push_back(kDayBars[i].close);
		expectDates.push_back(kDayBars[i].date);
	}
	EXPECT_EQ(kBarCnt, sink2.closedDates.size());
	EXPECT_EQ(expectDates, sink2.closedDates);
	EXPECT_EQ(expectCloses, sink2.closedPrices);
	if (sink2.slice != nullptr)
		sink2.slice->release();
}

TEST_F(P04ReplayCsvTest, missingCsvFileYieldsNullSlice)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = replay_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	replayer.set_time_range(202001020930, 202001081500);

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_nofile");
	ASSERT_TRUE(replayer.prepare());

	// 不存在的合约：csv/ 下没有对应文件 -> cacheRawBarsFromCSV 返回 false -> NULL
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600000", "d", 1, 1, false));
	// 未知品种同样拿不到切片
	EXPECT_EQ(nullptr, replayer.get_kline_slice("DCE.IDX.999999", "d", 2, 1, false));
	// 时段缺失的合约在 get_kline_slice 里就会返回 NULL（L2291 分支）
	EXPECT_EQ(nullptr, replayer.get_kline_slice("XXX.YYY.000001", "d", 1, 1, false));
	// 没有主K线时 run() 仍安全收尾（不进入 run_by_bars）
	replayer.run(false);
	EXPECT_EQ(0u, sink.closedDates.size());
}

TEST_F(P04ReplayCsvTest, failedMainBarsLoadDoesNotPoisonReplayAndCanRetry)
{
	// Isolate the pre-fix null BarsList dereference so a regression fails this
	// test instead of terminating the whole coverage process.
	EXPECT_EXIT(
		{
			struct rlimit noCoreDumps{};
			if (::setrlimit(RLIMIT_CORE, &noCoreDumps) != 0)
				std::_Exit(90);

			HisDataReplayer* replayer = new HisDataReplayer();
			WTSVariant* cfg = replay_cfg(_root, _base);
			if (cfg == nullptr)
				std::_Exit(91);
			const bool initialized = replayer->init(cfg);
			cfg->release();
			if (!initialized)
				std::_Exit(92);

			RecordingSink sink;
			replayer->register_sink(&sink, "p04_missing_main");
			if (!replayer->prepare())
				std::_Exit(93);

			// Marking a missing series as main used to leave _main_key set even
			// though no cache entry existed; run() then dereferenced a null entry.
			if (replayer->get_kline_slice("SSE.STK.000004", "d", 1, 1, true) != nullptr)
				std::_Exit(1);
			replayer->run(false);
			if (sink.replayDoneCalls != 1 || !sink.closedDates.empty())
				std::_Exit(2);

			// A later successful load of that same main series must still be
			// selectable and replay normally on the same replayer instance.
			if (!write_daily_csv(_base, "SSE.STK.000004", true))
				std::_Exit(3);
			if (!replayer->prepare())
				std::_Exit(4);
			WTSKlineSlice* slice = replayer->get_kline_slice("SSE.STK.000004", "d", 1, 1, true);
			if (slice != nullptr)
				slice->release();
			replayer->run(false);
			if (sink.replayDoneCalls != 2 || sink.closedDates.size() != kBarCnt)
				std::_Exit(5);

			std::_Exit(0);
		},
		::testing::ExitedWithCode(0), "");
}

TEST_F(P04ReplayCsvTest, failedBinaryRawBarsLoadCanBeRetriedSafely)
{
	InstDirRestore restoreInstDir;
	if (!use_data_storage_module_from_build(_root))
		GTEST_SKIP() << "requires " << DLLHelper::wrap_module("WtDataStorage")
			<< " in WT_P04_DATA_STORAGE_DIR, this build's bin directory, or build-cov/build_x64/{Debug,Release}/bin";

	const std::filesystem::path minute5File = _base / "his/min5/SSE/000001.dsb";
	ASSERT_TRUE(write_raw_bar_dsb(minute5File, BT_HIS_Minute5, {}));
	ASSERT_TRUE(std::filesystem::exists(minute5File));

	HisDataReplayer replayer;
	// The first repaired bar is at 09:35. Start at 09:40 so it is observable
	// as historical data; get_kline_slice intentionally returns null when the
	// first available minute bar is still in the future.
	replayer.set_time_range(202001020940, 202001081500);
	WTSVariant* cfg = replay_cfg(_root, _base, "bin");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_bin_retry");
	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.000001", true));
	if (const char* testModuleDir = std::getenv("WT_P04_DATA_STORAGE_DIR"))
		EXPECT_EQ(std::filesystem::path(testModuleDir).string() + "/", WtHelper::getInstDir());

	// A real WtDataStorage read succeeds on this header-only file but returns no
	// records. It must not publish an empty raw cache entry.
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.000001", "m", 10, 10, false));

	std::vector<WTSBarStruct> minuteBars;
	for (uint32_t i = 0; i < 12; ++i)
	{
		const uint32_t minutes = 9 * 60 + 35 + i * 5;
		const uint32_t hhmm = (minutes / 60) * 100 + minutes % 60;
		minuteBars.push_back(make_bar(20200102, hhmm, 10.0 + i * 0.01));
	}
	ASSERT_TRUE(write_raw_bar_dsb(minute5File, BT_HIS_Minute5, minuteBars));

	// Probe the raw m5 cache first. This makes a successful DSB read observable
	// independently of the later m10 resampling and its current-time cursor.
	WTSKlineSlice* repairedRaw = replayer.get_kline_slice("SSE.STK.000001", "m", 10, 5, false);
	ASSERT_NE(nullptr, repairedRaw) << "same instance should reload the repaired DSB through WtDataStorage";
	EXPECT_GT(repairedRaw->size(), 0);
	ASSERT_NE(nullptr, repairedRaw->at(0));
	EXPECT_DOUBLE_EQ(10.0, repairedRaw->at(0)->close);
	repairedRaw->release();

	WTSKlineSlice* recovered = replayer.get_kline_slice("SSE.STK.000001", "m", 10, 10, false);
	ASSERT_NE(nullptr, recovered) << "same instance should retry WtDataStorage after raw bars are repaired";
	EXPECT_GT(recovered->size(), 0);
	recovered->release();

	// A normal daily main series still loads from the same real storage reader.
	const std::vector<WTSBarStruct> dailyBars = {
		make_bar(20200101, 150000, 10.0),
		make_bar(20200102, 150000, 10.5),
		make_bar(20200103, 150000, 11.5),
	};
	ASSERT_TRUE(write_raw_bar_dsb(_base / "his/day/SSE/000001.dsb", BT_HIS_Day, dailyBars));
	WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	ASSERT_NE(nullptr, main);
	EXPECT_EQ(1u, main->size());
	main->release();
}

TEST_F(P04ReplayCsvTest, emptyCsvReturnsNullAndReplayCompletesInChild)
{
	ASSERT_TRUE(write_daily_csv(_base, "SSE.STK.000002", false));
	ASSERT_TRUE(write_zero_byte_daily_csv(_base, "SSE.STK.000003"));

	// Empty CSV handling used to index the empty bar vector. Isolate the regression
	// check so a product crash is reported as a failed expectation, never success.
	EXPECT_EXIT(
		{
			struct rlimit noCoreDumps{};
			if (::setrlimit(RLIMIT_CORE, &noCoreDumps) != 0)
				std::_Exit(90);
			auto writeStage = [](const char* marker, std::size_t length) {
				const auto written = ::write(STDERR_FILENO, marker, length);
				(void)written;
			};

			HisDataReplayer* replayer = new HisDataReplayer();
			WTSVariant* cfg = replay_cfg(_root, _base);
			if (cfg == nullptr)
				std::_Exit(91);
			writeStage("INIT_START\n", sizeof("INIT_START\n") - 1);
			const bool initialized = replayer->init(cfg);
			cfg->release();
			if (!initialized)
				std::_Exit(92);
			writeStage("INIT_OK\n", sizeof("INIT_OK\n") - 1);

			RecordingSink sink;
			replayer->register_sink(&sink, "p04_emptycsv");
			writeStage("PREPARE_START\n", sizeof("PREPARE_START\n") - 1);
			if (!replayer->prepare())
				std::_Exit(93);
			writeStage("PREPARE_OK\n", sizeof("PREPARE_OK\n") - 1);

			writeStage("BEFORE_READ\n", sizeof("BEFORE_READ\n") - 1);
			WTSKlineSlice* headerOnlySlice = replayer->get_kline_slice("SSE.STK.000002", "d", 1, 1, false);
			WTSKlineSlice* zeroByteSlice = replayer->get_kline_slice("SSE.STK.000003", "d", 1, 1, false);
			writeStage("AFTER_READ\n", sizeof("AFTER_READ\n") - 1);
			if (headerOnlySlice != nullptr || zeroByteSlice != nullptr)
				std::_Exit(1);
			writeStage("NULL_SLICE_OK\n", sizeof("NULL_SLICE_OK\n") - 1);
			if (std::filesystem::exists(_base / "his/day/SSE/000002.dsb") ||
				std::filesystem::exists(_base / "his/day/SSE/000003.dsb"))
				std::_Exit(3);
			writeStage("BEFORE_RUN\n", sizeof("BEFORE_RUN\n") - 1);
			replayer->run(false);
			writeStage("AFTER_RUN\n", sizeof("AFTER_RUN\n") - 1);
			std::_Exit(sink.replayDoneCalls == 1 && sink.closedDates.empty() ? 0 : 2);
		},
		::testing::ExitedWithCode(0), "");
}
