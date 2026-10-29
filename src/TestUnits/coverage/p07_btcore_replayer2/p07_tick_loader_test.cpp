/**
 * p07_tick_loader_test.cpp
 * 覆盖 HisDataReplayer 的「逐tick回放 + 扩展装载器（IBtDataLoader）」两条主链，
 * p04 只走到这些函数的空缓存入口：
 *   run_by_ticks(L741) + replayHftDatasByDay(L1769) + getNextTickTime(L1536)
 *   checkTicks(L2828) 的 loader 分支 + cacheRawTicksFromLoader(L3443)
 *   get_last_tick(L2881) / get_day_price(L3034) / get_cur_price(L3021) 的有数据路径
 *   cacheFinalBarsFromLoader(L3589)：loader 装载分支 + isAutoTrans 的 dsb 转储/回读分支
 *   get_kline_slice(L2229-L2243) 的「先走 extloader」分支
 *   init 里的 loadStkAdjFactorsFromLoader(L302) 回调 + loadFees/calc_fee(L2983) 命中费率表
 * 说明：本 lane 只链 WtBtCore/WTSTools/WTSUtils，IBtDataLoader 是 WtBtCore 自己的接口，
 * 这里用桩实现，不引用 WtCore/WtUftCore/WtDtCore 的任何类。
 * 落盘全部在 temp_directory_path()/p07_* 下，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
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
			("p07_ld_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	struct TickRow
	{
		uint32_t date;
		uint32_t hhmm;
		double open;
		double high;
		double low;
		double price;
		double volume;
	};

	/*
	 * 20200102 五笔、20200103 两笔；时间戳都是交易时间内的 HHMMSS*1000。
	 * action_time/100000 必须落在 SD0930 的交易时段里，否则 getNextTickTime 会跳过。
	 */
	const TickRow kTicks[] = {
		{20200102u, 931u, 10.0, 10.5,  9.8, 10.2,  100},
		{20200102u, 932u, 10.2, 11.0, 10.1, 10.8,  200},
		{20200102u, 933u, 10.8, 12.0, 10.6, 11.5,  300},
		{20200102u, 1458u, 11.5, 11.8, 11.2, 11.6, 400},
		{20200102u, 1459u, 11.6, 12.5, 11.4, 12.0, 500},
		{20200103u, 931u, 12.0, 12.4, 11.9, 12.2, 600},
		{20200103u, 932u, 12.2, 13.0, 12.1, 12.8, 700},
	};
	const uint32_t kDay1Ticks = 5;
	const uint32_t kDay2Ticks = 2;

	struct LoaderBar
	{
		uint32_t date;
		uint32_t hhmm;
		double open;
		double high;
		double low;
		double close;
		double vol;
	};

	// 与 p07_minute_replay_test 相同的 9 根 1 分钟线（首根会被初始定位吃掉）
	const LoaderBar kM1[] = {
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
	const uint32_t kM1Cnt = sizeof(kM1) / sizeof(kM1[0]);

	/**
	 * 桩装载器：tick 与 1 分钟线都由内存给出，不依赖任何存储引擎/动态库。
	 */
	class StubBtLoader : public IBtDataLoader
	{
	public:
		bool		_autoTrans {true};
		uint32_t	_finalBarCalls {0};
		uint32_t	_rawBarCalls {0};
		uint32_t	_tickCalls {0};
		uint32_t	_adjAllCalls {0};
		uint32_t	_adjOneCalls {0};
		bool		_giveTicks {true};
		std::vector<uint32_t> _loadedTickDates;

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_finalBarCalls++;
			std::vector<WTSBarStruct> bars;
			if (period == KP_Minute1)
			{
				for (uint32_t i = 0; i < kM1Cnt; i++)
				{
					WTSBarStruct bs;
					memset(&bs, 0, sizeof(bs));
					bs.date = kM1[i].date;
					bs.time = (uint64_t)((kM1[i].date - 19900000) * 10000) + kM1[i].hhmm;
					bs.open = kM1[i].open;
					bs.high = kM1[i].high;
					bs.low = kM1[i].low;
					bs.close = kM1[i].close;
					bs.vol = kM1[i].vol;
					bs.money = kM1[i].vol * 10.0;
					bars.emplace_back(bs);
				}
			}
			else if (period == KP_DAY)
			{
				const uint32_t dates[] = {20200102u, 20200103u};
				for (uint32_t i = 0; i < 2; i++)
				{
					WTSBarStruct bs;
					memset(&bs, 0, sizeof(bs));
					bs.date = dates[i];
					bs.time = dates[i];
					bs.open = 10.0 + i;
					bs.high = 11.0 + i;
					bs.low = 9.5 + i;
					bs.close = 10.5 + i;
					bs.vol = 100 * (i + 1);
					bars.emplace_back(bs);
				}
			}
			else
			{
				return false;
			}

			if (stdCode == NULL || bars.empty())
				return false;

			cb(obj, bars.data(), (uint32_t)bars.size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_rawBarCalls++;
			(void)obj; (void)stdCode; (void)period; (void)cb;
			return false;
		}

		bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override
		{
			_adjAllCalls++;
			uint32_t dates[] = {20200106u, 20200203u};
			double factors[] = {2.0, 1.5};
			cb(obj, "SSE.STK.600000", dates, factors, 2);
			return true;
		}

		bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
		{
			_adjOneCalls++;
			uint32_t dates[] = {20200106u};
			double factors[] = {2.0};
			cb(obj, stdCode, dates, factors, 1);
			return true;
		}

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			(void)stdCode;
			_tickCalls++;
			_loadedTickDates.push_back(uDate);
			if (!_giveTicks)
				return false;

			std::vector<WTSTickStruct> ticks;
			for (uint32_t i = 0; i < sizeof(kTicks) / sizeof(kTicks[0]); i++)
			{
				if (kTicks[i].date != uDate)
					continue;
				WTSTickStruct ts;
				memset(&ts, 0, sizeof(ts));
				snprintf(ts.code, sizeof(ts.code), "%s", "600000");
				ts.action_date = kTicks[i].date;
				ts.action_time = kTicks[i].hhmm * 100000;
				ts.open = kTicks[i].open;
				ts.high = kTicks[i].high;
				ts.low = kTicks[i].low;
				ts.price = kTicks[i].price;
				ts.volume = kTicks[i].volume;
				ts.total_volume = kTicks[i].volume;
				ticks.emplace_back(ts);
			}

			if (ticks.empty())
				return false;

			cb(obj, ticks.data(), (uint32_t)ticks.size());
			return true;
		}

		bool isAutoTrans() override { return _autoTrans; }
	};

	WTSVariant* loader_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, bool tickMode, const std::string& fees)
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
		cfg->append("fees", fees.c_str());
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	bool write_fee_template(const std::filesystem::path& base, std::string& outPath)
	{
		outPath = (base / "fees_p07.json").string();
		std::ofstream out(outPath);
		if (!out.is_open())
			return false;
		out << "{\n"
			<< "\t\"SSE.STK\":{\"byvolume\":false,\"open\":0.0003,\"close\":0.0013,\"closetoday\":0.0013,\"minfee\":5},\n"
			<< "\t\"SZSE.STK\":{\"byvolume\":false,\"open\":0.00025,\"close\":0.00025,\"closetoday\":0.00025,\"minfee\":0},\n"
			<< "\t\"SSE.IDX\":{\"byvolume\":true,\"open\":2,\"close\":3,\"closetoday\":1,\"minfee\":0}\n"
			<< "}\n";
		out.close();
		return true;
	}

	class TickSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		std::function<void(uint32_t /*tickSeq*/)> onTickProbe;
		HisDataReplayer* rp {nullptr};
		std::string probeCode;
		bool peekLastTick {false};

		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t replayDoneCalls {0};

		std::vector<double> tickPrices;
		std::vector<double> tickOpens;
		std::vector<double> tickHighs;
		std::vector<double> tickLows;
		std::vector<double> lastTickPrices;
		std::vector<double> dayOpen;
		std::vector<double> dayHigh;
		std::vector<double> dayLow;
		std::vector<double> dayLast;
		std::vector<double> curPrices;
		std::vector<uint32_t> tickDates;
		std::vector<uint32_t> tickTimes;
		std::vector<double> closedPrices;
		std::vector<uint32_t> closedTimes;

		void handle_init() override
		{
			initCalls++;
			if (onInit)
				onInit();
		}

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			(void)stdCode; (void)pxType;
			if (curTick == nullptr)
				return;
			const WTSTickStruct& ts = curTick->getTickStruct();
			tickPrices.push_back(ts.price);
			tickOpens.push_back(ts.open);
			tickHighs.push_back(ts.high);
			tickLows.push_back(ts.low);
			tickDates.push_back(ts.action_date);
			tickTimes.push_back(ts.action_time / 100000);
			if (rp != nullptr && !probeCode.empty())
			{
				curPrices.push_back(rp->get_cur_price(probeCode.c_str()));
				// tick 模式下 flag 0/1/2/3 分别读取 open/high/low/price。
				if (peekLastTick)
				{
					dayOpen.push_back(rp->get_day_price(probeCode.c_str(), 0));
					dayHigh.push_back(rp->get_day_price(probeCode.c_str(), 1));
					dayLow.push_back(rp->get_day_price(probeCode.c_str(), 2));
					dayLast.push_back(rp->get_day_price(probeCode.c_str(), 3));
					WTSTickData* last = rp->get_last_tick(probeCode.c_str());
					lastTickPrices.push_back(last == nullptr ? 0.0 : last->getTickStruct().price);
					if (last != NULL)
						last->release();
				}
			}
			if (onTickProbe)
				onTickProbe((uint32_t)tickPrices.size());
		}

		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct* newBar) override
		{
			if (newBar != nullptr)
			{
				closedTimes.push_back(static_cast<uint32_t>(newBar->time % 10000));
				closedPrices.push_back(newBar->close);
			}
		}

		void handle_schedule(uint32_t, uint32_t) override { scheduleCalls++; }
		void handle_session_begin(uint32_t) override { beginCalls++; }
		void handle_session_end(uint32_t) override { endCalls++; }
		void handle_replay_done() override { replayDoneCalls++; }
	};
}

class P07TickLoaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/sessions.json"));
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/stk_comms.json"));
		_base = make_temp_dir("ld");
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
 * 逐tick回放主循环：只订阅tick（不订阅K线）+ tick模式 -> run() 派发到 run_by_ticks。
 * replayHftDatasByDay 每个交易日把该日的tick按时间序回放；
 * 两日 fixture 的 5+2 笔都在 SD0930 交易时段和回测窗口内，应逐笔回放一次。
 */
TEST_F(P07TickLoaderTest, tickOnlyReplayDrivesHftLoopPerDay)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	std::string fees;
	ASSERT_TRUE(write_fee_template(_base, fees));
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, true, fees);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	sink.onInit = [&replayer]() {
		// prepare() 会先 reset 并清空订阅；与策略初始化一致，在 handle_init 中订阅。
		replayer.sub_tick(11, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_ticks");

	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	EXPECT_TRUE(replayer.is_tick_enabled());
	// init 里带 loader 时会通过扩展装载器拉一次全量复权因子
	EXPECT_EQ(1u, loader._adjAllCalls);

	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), loader._loadedTickDates);
	ASSERT_EQ(kDay1Ticks + kDay2Ticks, sink.tickPrices.size());
	EXPECT_EQ(std::vector<double>({10.2, 10.8, 11.5, 11.6, 12.0, 12.2, 12.8}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u, 20200102u, 20200102u,
		20200103u, 20200103u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u, 1458u, 1459u, 931u, 932u}), sink.tickTimes);

	// 每个有tick的交易日各一对 session_begin/end，收尾再补 1 次 replay_done
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 没有订阅K线 -> 不应有闭盘
	EXPECT_EQ(0u, sink.closedPrices.size());

	// update_price 由回放中的每笔tick驱动，最后一笔是 20200103 的第 2 笔
	EXPECT_DOUBLE_EQ(12.8, replayer.get_cur_price("SSE.STK.600000"));
	// 20200104 已超出 end_tdate，装载器没有该日数据 -> 缓存被清空 -> 取不到last tick
	EXPECT_EQ(nullptr, replayer.get_last_tick("SSE.STK.600000"));
}

/*
 * 回调收到的 tick 应是当前最新行情；tick 模式的 get_last_tick 和
 * get_day_price(0..3) 都应对应这同一条 tick。总回调数由上面的
 * tickOnlyReplayDrivesHftLoopPerDay 单独检查，本用例按实际收到的每个回调比较字段，
 * 避免尾笔数量问题掩盖 getter 是否错位。
 */
TEST_F(P07TickLoaderTest, lastTickAndDayPriceMatchCurrentCallbackTick)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	sink.rp = &replayer;
	sink.probeCode = "SSE.STK.600000";
	sink.peekLastTick = true;
	sink.onInit = [&replayer]() {
		// prepare() 会 reset 并清空订阅；在策略初始化回调中登记订阅。
		replayer.sub_tick(11, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_peek");

	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u}), loader._loadedTickDates);
	ASSERT_FALSE(sink.tickPrices.empty());
	EXPECT_EQ(20200102u, sink.tickDates.front());
	EXPECT_EQ(931u, sink.tickTimes.front());
	EXPECT_DOUBLE_EQ(10.2, sink.tickPrices.front());
	EXPECT_DOUBLE_EQ(10.0, sink.tickOpens.front());
	EXPECT_DOUBLE_EQ(10.5, sink.tickHighs.front());
	EXPECT_DOUBLE_EQ(9.8, sink.tickLows.front());
	ASSERT_EQ(sink.tickPrices.size(), sink.tickOpens.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.tickHighs.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.tickLows.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickPrices.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayOpen.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayHigh.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayLow.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayLast.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.curPrices.size());

	// 每次回调中的 last tick 与 day-price 字段必须来自回调本身的当前 tick。
	EXPECT_EQ(sink.tickPrices, sink.lastTickPrices);
	EXPECT_EQ(sink.tickOpens, sink.dayOpen);
	EXPECT_EQ(sink.tickHighs, sink.dayHigh);
	EXPECT_EQ(sink.tickLows, sink.dayLow);
	EXPECT_EQ(sink.tickPrices, sink.dayLast);
	// get_cur_price 是 update_price 写入的「已回放」价
	EXPECT_EQ(sink.tickPrices, sink.curPrices);
}

/*
 * loader 装载 1 分钟线并转储 dsb：
 * 第一次运行 cacheFinalBarsFromLoader 走 extloader 分支并写 his/min1/SSE/600000.dsb，
 * 第二次（新对象）走 isAutoTrans 的 dsb 回读分支，两次闭盘结果必须一致。
 */
TEST_F(P07TickLoaderTest, loaderBarsDumpDsbAndReRead)
{
	StubBtLoader loader;
	std::vector<double> firstCloses;
	std::vector<uint32_t> firstTimes;
	{
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, "");
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();

		TickSink sink;
		sink.rp = &replayer;
		sink.probeCode = "SSE.STK.600000";
		replayer.register_sink(&sink, "p07_ld1");
		sink.onInit = [&replayer]() {
			replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
		};

		ASSERT_TRUE(replayer.prepare());
		EXPECT_EQ(1u, loader._finalBarCalls);
		replayer.run(false);

		// extloader 装载了 9 根，首根被吃掉 -> 8 次闭盘、8*4 笔模拟tick
		ASSERT_EQ(kM1Cnt - 1, sink.closedPrices.size());
		std::vector<double> expectCloses;
		for (uint32_t i = 1; i < kM1Cnt; i++)
			expectCloses.push_back(kM1[i].close);
		EXPECT_EQ(expectCloses, sink.closedPrices);
		std::vector<uint32_t> expectTimes;
		for (uint32_t i = 1; i < kM1Cnt; i++)
			expectTimes.push_back(kM1[i].hhmm);
		EXPECT_EQ(expectTimes, sink.closedTimes);
		EXPECT_EQ((kM1Cnt - 1) * 4, sink.tickPrices.size());
		firstCloses = sink.closedPrices;
		firstTimes = sink.closedTimes;
	}

	// 转储产物必须落在临时目录
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
	ASSERT_TRUE(dsbFound) << "loader 自动转储没有产生 .dsb";

	{
		StubBtLoader second;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, "");
		ASSERT_TRUE(replayer.init(cfg, NULL, &second));
		cfg->release();

		TickSink sink;
		replayer.register_sink(&sink, "p07_ld2");
		sink.onInit = [&replayer]() {
			replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
		};
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);

		// 命中 dsb 回读分支：不会再调用 extloader
		EXPECT_EQ(0u, second._finalBarCalls);
		EXPECT_EQ(firstCloses, sink.closedPrices);
		EXPECT_EQ(firstTimes, sink.closedTimes);
	}
}

/*
 * 关闭自动转储时，cacheFinalBarsFromLoader 每次都要走 extloader；
 * 装载器没有 5 分钟线 -> 返回 false -> get_kline_slice 直接返回 NULL。
 */
TEST_F(P07TickLoaderTest, loaderWithoutDataYieldsNullSlice)
{
	StubBtLoader loader;
	loader._autoTrans = false;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	replayer.register_sink(&sink, "p07_nodata");
	ASSERT_TRUE(replayer.prepare());

	// m5：桩装载器只给 m1/d
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600000", "m", 5, 5, false));
	EXPECT_EQ(1u, loader._finalBarCalls);
	// m1 能拿到，且没有转储文件
	WTSKlineSlice* sli = replayer.get_kline_slice("SSE.STK.600000", "m", 3, 1, true);
	ASSERT_NE(nullptr, sli);
	EXPECT_EQ(1, sli->size());
	sli->release();
	// 目录会被 cacheFinalBarsFromLoader 预创建，但 isAutoTrans=false 时不能落 .dsb
	std::error_code ec;
	uint32_t dsbCnt = 0;
	for (auto& entry : std::filesystem::recursive_directory_iterator(_base / "his", ec))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".dsb")
			dsbCnt++;
	}
	EXPECT_EQ(0u, dsbCnt);
}

/*
 * calc_fee 的有数据路径（p04 只做了空/未命中）：
 * 金额计费按 price*qty*volscale*rate 再套最低手续费，按量计费直接 rate*qty，
 * 结果统一四舍五入到 2 位小数。SSE.STK volscale=1（dist/common/stk_comms.json）。
 */
TEST_F(P07TickLoaderTest, calcFeeUsesLoadedFeeTemplate)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	std::string fees;
	ASSERT_TRUE(write_fee_template(_base, fees));
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, fees);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	// 1000*0.0003 = 0.3 -> 低于最低 5 块
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 0));
	// 1000*0.0013 = 1.3 -> 仍然低于最低 5 块
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 1));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 2));
	// 抬高成交额让最低收费不起作用：200000*0.0003 = 60
	EXPECT_DOUBLE_EQ(60.0, replayer.calc_fee("SSE.STK.600000", 20.0, 10000.0, 0));
	// minfee=0 -> 直接按比例：10000*0.00025 = 2.5
	EXPECT_DOUBLE_EQ(2.5, replayer.calc_fee("SZSE.STK.000001", 20.0, 500.0, 0));
	// byvolume：open=2 -> 2*3 = 6
	EXPECT_DOUBLE_EQ(6.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 0));
	EXPECT_DOUBLE_EQ(9.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 1));
	EXPECT_DOUBLE_EQ(3.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 2));
	// offset 非法 -> 0
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 9));
	// 费率表里没有的品种 -> 0
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("DCE.IDX.999999", 10.0, 3.0, 0));
}

/*
 * 崩溃/挂死风险统一收进这一个禁用用例，启用用例里零残留：
 * replayHftDatas(L1904) 的外层是 `for(;;)`，每轮都用同一个 `stime` 去算 nextTime，
 * 只有「本次确实回放了某笔数据」时游标才前进；
 * 如果订阅列表里存在 action_date 与 _cur_tdate 不一致的tick（数据源串日），
 * `lastTime <= nextTime` 恒不成立 -> 游标永不推进 -> 死循环（本波已有多条 HANG 同类）。
 * 这里不构造该输入，只做归档；启用用例全部走 run_by_ticks 的按日循环（有日期边界，可收敛）。
 */
TEST_F(P07TickLoaderTest, DISABLED_mismatchedTickDateHangsReplayHftDatas)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	replayer.register_sink(&sink, "p07_hang");
	replayer.sub_tick(11, "SSE.STK.600000");
	replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
	ASSERT_TRUE(replayer.prepare());
	// 有主K线 + tick 模式 -> run_by_bars 内部调 replayHftDatas
	replayer.run(false);
}
