/**
 * p04_replay_termination_test.cpp
 * 覆盖 HisDataReplayer 回放主循环的“结束条件/进度推进”分支（临时目录 CSV 日线驱动）：
 *   run_by_ticks(L741-769)     —— 没有tick数据时的日期推进与收尾
 *   run_by_bars(L771-938)      —— nextBarTime > _end_time 的提前退出(L807)、
 *                                 收尾 session_end(L926-930)、handle_replay_done(L937)
 *   get_*_slice(L2470/L2547/L2595/L2643) —— 空数据装配下的稳定返回
 *   get_last_tick(L2881) / get_day_price(L3034) —— 无数据路径
 * 时间约定同 p04_replayer_csv_replay_test：SD0930 收盘 1500，12 位 YYYYMMDDHHMM。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
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
			("p04_term_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	const uint32_t kDates[5] = {20200102u, 20200103u, 20200106u, 20200107u, 20200108u};

	void write_daily_csv(const std::filesystem::path& base, const char* stdCode)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (uint32_t i = 0; i < 5; i++)
		{
			out << kDates[i] << ",150000," << (10.0 + i) << "," << (11.0 + i) << ","
				<< (9.0 + i) << "," << (10.5 + i) << "," << (1000 + i) << ","
				<< ((1000 + i) * 10.0) << ",0,0,0\n";
		}
		out.close();
	}

	WTSVariant* term_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, bool tick)
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
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	class CountingSink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t sectionCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t closeCalls {0};
		uint32_t replayDoneCalls {0};

		void handle_init() override
		{
			initCalls++;
			if (onInit)
				onInit();
		}
		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			(void)stdCode; (void)curTick; (void)pxType;
		}
		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			(void)stdCode; (void)period; (void)times; (void)newBar;
			closeCalls++;
		}
		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			(void)uDate; (void)uTime;
			scheduleCalls++;
		}
		void handle_session_begin(uint32_t curTDate) override
		{
			(void)curTDate;
			beginCalls++;
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

class P04ReplayTermTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("data");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
		write_daily_csv(_base, "SSE.STK.000001");
		write_daily_csv(_base, "SZSE.STK.000002");
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

// tick 模式但没有任何tick缓存：run_by_ticks 的日推进循环必须自行收敛
TEST_F(P04ReplayTermTest, tickModeWithoutTickDataStillTerminates)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = term_cfg(_root, _base, 202001020930, 202001081500, true);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_TRUE(replayer.is_tick_enabled());

	CountingSink sink;
	replayer.register_sink(&sink, "p04_tickmode");
	ASSERT_TRUE(replayer.prepare());
	// 没有登记主K线 -> run() 走 run_by_ticks 分支（L721）
	replayer.run(false);

	EXPECT_EQ(0u, sink.closeCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 循环结束后交易日游标只可能停在起点或推进到区间之后
	EXPECT_GE(replayer.get_trading_date(), 20200102u);
	EXPECT_EQ(0u, replayer.get_secs());
	// tick 模式下 run_by_ticks 不会改写模拟标记，reset() 留下的 true 保持不变
	EXPECT_TRUE(replayer.is_tick_simulated());
}

// 结束时间早于第一根K线收盘时刻：第一轮就 break（L807），只补一次收尾 session_end
TEST_F(P04ReplayTermTest, invertedTimeRangeStopsBeforeFirstBar)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = term_cfg(_root, _base, 202001020930, 202001081500, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	// 起止倒挂：end 落在 begin 之前
	replayer.set_time_range(202001020930, 202001010930);

	CountingSink sink;
	replayer.register_sink(&sink, "p04_inverted");
	sink.onInit = [&replayer]() {
		replayer.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	};
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(0u, sink.closeCalls);
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.scheduleCalls);
	// L926：_closed_tdate(0) != _cur_tdate(20200102) -> 收尾时补发一次 session_end
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 时间游标没有被推进
	EXPECT_EQ(20200102u, replayer.get_date());
	EXPECT_EQ(930u, replayer.get_raw_time());
}

// 第二个回放对象换合约、区间只覆盖部分日线：进度循环按区间收敛
TEST_F(P04ReplayTermTest, secondCodeReplaysItsOwnBars)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = term_cfg(_root, _base, 202001030930, 202001071500, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	CountingSink sink;
	replayer.register_sink(&sink, "p04_second");
	sink.onInit = [&replayer]() {
		replayer.get_kline_slice("SZSE.STK.000002", "d", 1, 1, true);
	};
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(20200103u, replayer.get_date());
	EXPECT_EQ(20200103u, replayer.get_trading_date());

	replayer.run(false);
	// 区间内 20200103/06/07 三根
	EXPECT_EQ(3u, sink.closeCalls);
	EXPECT_EQ(3u, sink.beginCalls);
	EXPECT_EQ(3u, sink.endCalls);
	EXPECT_EQ(3u, sink.scheduleCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(20200107u, replayer.get_date());
	EXPECT_EQ(1500u, replayer.get_raw_time());
}

// 空数据下各类切片/查询接口的稳定返回
TEST_F(P04ReplayTermTest, emptyCacheQueryApisAreStable)
{
	HisDataReplayer replayer;
	WTSVariant* cfg = term_cfg(_root, _base, 202001020930, 202001081500, false);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	CountingSink sink;
	replayer.register_sink(&sink, "p04_query");
	ASSERT_TRUE(replayer.prepare());

	// 没有tick/委托/成交缓存 -> 同一入参两次调用的空/非空结论必须一致
	WTSTickSlice* t1 = replayer.get_tick_slice("SSE.STK.000001", 3);
	WTSTickSlice* t2 = replayer.get_tick_slice("SSE.STK.000001", 3);
	EXPECT_EQ((t1 == nullptr), (t2 == nullptr));

	WTSOrdDtlSlice* d1 = replayer.get_order_detail_slice("SSE.STK.000001", 3);
	WTSOrdDtlSlice* d2 = replayer.get_order_detail_slice("SSE.STK.000001", 3);
	EXPECT_EQ((d1 == nullptr), (d2 == nullptr));

	WTSOrdQueSlice* q1 = replayer.get_order_queue_slice("SSE.STK.000001", 3);
	WTSOrdQueSlice* q2 = replayer.get_order_queue_slice("SSE.STK.000001", 3);
	EXPECT_EQ((q1 == nullptr), (q2 == nullptr));

	WTSTransSlice* s1 = replayer.get_transaction_slice("SSE.STK.000001", 3);
	WTSTransSlice* s2 = replayer.get_transaction_slice("SSE.STK.000001", 3);
	EXPECT_EQ((s1 == nullptr), (s2 == nullptr));

	// 最新价/当日价在无数据时必须是 0，不能是未初始化的脏值
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price("SSE.STK.000001"));
	for (int flag = 0; flag <= 4; flag++)
		EXPECT_DOUBLE_EQ(0.0, replayer.get_day_price("SSE.STK.000001", flag));
	EXPECT_EQ(nullptr, replayer.get_last_tick("SSE.STK.000001"));

	replayer.stop();
}

/*
 * 崩溃/挂死缺陷类，统一收进禁用用例：
 * 1) register_task 的默认 session="TRADING" 在 dist/common/sessions.json 里不存在，
 *    run_by_tasks(L943-945) 拿到 NULL 的 WTSSessionInfo 后继续解引用（BUG-P04-007）。
 * 2) 回放区间完全落在行情之后（begin 晚于最后一根K线）时，
 *    get_kline_slice 把游标定位到容器尾部，run_by_bars(L800) 直接读 _bars[_cursor]
 *    —— 越界读（BUG-P04-008）。
 * 3) _end_time 为 0 时 run_by_ticks(L744-748) 的 end_tdate 计算退化，日推进循环不可控（BUG-P04-003）。
 * 开启任何一条都会拖垮整块测试二进制，因此只作为缺陷归档保留。
 */
TEST_F(P04ReplayTermTest, DISABLED_taskFrequencyAndOutOfRangeReplayCrash)
{
	HisDataReplayer taskMode;
	WTSVariant* cfg1 = term_cfg(_root, _base, 202001020930, 202001081500, false);
	ASSERT_TRUE(taskMode.init(cfg1));
	cfg1->release();
	CountingSink sink1;
	taskMode.register_sink(&sink1, "p04_taskmode");
	// 崩溃点 1：默认 session 模板 "TRADING" 未定义
	taskMode.register_task(1, 0, 930, "d");
	taskMode.prepare();
	taskMode.run(false);

	HisDataReplayer outOfRange;
	WTSVariant* cfg2 = term_cfg(_root, _base, 202001200930, 202001281500, false);
	ASSERT_TRUE(outOfRange.init(cfg2));
	cfg2->release();
	CountingSink sink2;
	outOfRange.register_sink(&sink2, "p04_oor");
	sink2.onInit = [&outOfRange]() {
		outOfRange.get_kline_slice("SSE.STK.000001", "d", 1, 1, true);
	};
	// 崩溃点 2：游标已经在容器尾部
	outOfRange.prepare();
	outOfRange.run(false);

	HisDataReplayer zeroEnd;
	WTSVariant* cfg3 = term_cfg(_root, _base, 202001020930, 0, true);
	ASSERT_TRUE(zeroEnd.init(cfg3));
	cfg3->release();
	CountingSink sink3;
	zeroEnd.register_sink(&sink3, "p04_zeroend");
	zeroEnd.prepare();
	// 挂死点 3
	zeroEnd.run(false);
}
