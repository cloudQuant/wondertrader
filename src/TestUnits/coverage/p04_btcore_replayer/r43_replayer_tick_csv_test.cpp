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

	WTSVariant* r43_cfg(const std::filesystem::path& root, const std::filesystem::path& base, bool tick)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", tick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
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

		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t replayDoneCalls {0};
		uint32_t tickCalls {0};

		std::vector<double> tickPrices;
		std::vector<uint32_t> tickDates;
		std::vector<uint32_t> closedDates;
		std::vector<double> closedPrices;
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
			(void)stdCode; (void)pxType;
			tickCalls++;
			if (curTick != nullptr)
			{
				tickPrices.push_back(curTick->price());
				tickDates.push_back(curTick->actiondate());
			}
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
	ASSERT_TRUE(write_tick_dsb(_base, code, 20200102u));

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

		// etime 分支：用 10:00 作为截止时间，应取到 09:31/09:32 两笔
		WTSTickSlice* slice = query.get_tick_slice(code, 2, 202001021000ull);
		ASSERT_NE(nullptr, slice);
		ASSERT_EQ(2u, slice->size());
		EXPECT_DOUBLE_EQ(11.0, slice->at(1)->price);
		slice->release();
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

	// 回放游标推进规则：每笔先推进再回调，最后一笔不进回调
	ASSERT_EQ(kTickCnt - 1, sink.tickCalls);
	EXPECT_DOUBLE_EQ(10.0, sink.tickPrices[0]);
	EXPECT_DOUBLE_EQ(11.0, sink.tickPrices[1]);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_DOUBLE_EQ(11.0, replayer.get_cur_price(code));
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
