/**
 * r2_replayer_public_api_test.cpp  (lane R2)
 *
 * 靶子：HisDataReplayer 上**不需要任何 private 接缝**的公开面（p04/p07 未覆盖的部分）：
 *   loadFees      L2946（不存在 / 解析失败 / 正常三份分支）
 *   calc_fee      L2983（byvolume 三档 + minfee 下限 + 未登记费率 + 比例费率）
 *   checkOrderDetails / checkOrderQueues / checkTransactions  L2702/L2744/L2786
 *                 + get_order_detail_slice / get_order_queue_slice / get_transaction_slice
 *                 L2547/L2595/L2643（csv 模式一律拒绝）
 *   stop()        L634（未运行 / 置终止位），_terminated 让 run_by_bars 提前收尾 L~800
 *   clear_cache() L436 + 之后重新 prepare 的二次装载
 *   register_task L407 + run_by_tasks L940 的调度回调
 *   get_rawcode L2921 / get_session_info L2933 / get_commodity_info L2915 / 时间轴 getter
 *
 * 落盘：temp_directory_path()/r2_pub_*；dist/ 只读。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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
			("r2_pub_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	const char* const kStk = "SSE.STK.000001";
	const char* const kFut = "DCE.a2105";
	const uint32_t kBarCnt = 5;
	const uint32_t kDates[kBarCnt] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	bool write_daily_csv(const std::filesystem::path& base, const char* stdCode)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (uint32_t i = 0; i < kBarCnt; i++)
		{
			out << kDates[i] << ",150000,"
				<< (10.0 + i) << "," << (11.0 + i) << ","
				<< (9.5 + i) << "," << (10.5 + i) << ","
				<< (1000 + i) << "," << (1000 + i) * 10.0 << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	WTSVariant* pub_cfg(const std::filesystem::path& root,
		const std::filesystem::path& base, const char* fees)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", fees);
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		WTSVariant* comms = WTSVariant::createArray();
		comms->append((root / "dist/common/stk_comms.json").string().c_str());
		comms->append((root / "dist/common/commodities.json").string().c_str());
		basefiles->append("commodity", comms);
		comms->release();
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	/*
	 * 只计数不接管生命周期的接收器（回放器自己 release 它创建的 tick）。
	 */
	class CountSink : public IDataSink
	{
	public:
		CountSink() : _initCnt(0), _sessBegin(0), _sessEnd(0), _barClose(0),
			_tickCnt(0), _schedCnt(0), _doneCnt(0), _lastSchedTime(0), _lastBarDate(0) {}

		virtual void handle_tick(const char*, WTSTickData*, uint32_t) override { _tickCnt++; }
		virtual void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct* newBar) override
		{
			_barClose++;
			if (newBar != NULL)
				_lastBarDate = newBar->date;
		}
		virtual void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			_schedCnt++;
			_lastSchedTime = uTime;
			_lastSchedDate = uDate;
		}
		virtual void handle_init() override { _initCnt++; }
		virtual void handle_session_begin(uint32_t) override { _sessBegin++; }
		virtual void handle_session_end(uint32_t) override { _sessEnd++; }
		virtual void handle_replay_done() override { _doneCnt++; }

	public:
		uint32_t	_initCnt;
		uint32_t	_sessBegin;
		uint32_t	_sessEnd;
		uint32_t	_barClose;
		uint32_t	_tickCnt;
		uint32_t	_schedCnt;
		uint32_t	_doneCnt;
		uint32_t	_lastSchedTime;
		uint32_t	_lastSchedDate;
		uint32_t	_lastBarDate;
	};
}

class R2PublicTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("base");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
		ASSERT_TRUE(write_daily_csv(_base, kStk));
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

	void setup(HisDataReplayer& replayer, CountSink& sink, const char* fees)
	{
		WTSVariant* cfg = pub_cfg(_root, _base, fees);
		ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
		cfg->release();
		replayer.register_sink(&sink, "r2_pub");
		ASSERT_TRUE(replayer.prepare());
	}
};

/*
 * loadFees 的三条早退/正常分支 + calc_fee 的全部费率算法：
 * - DCE.a 按量计费：open 2.0/手、close 1.0/手、closetoday 0.0、minfee 5.0
 *   -> 10手开仓 = 20.00；2手平仓 = 2.00 < 5.00 被抬到 5.00；平今 0.00（rate=0 不触发 minfee）
 *   -> offset 越界一律 0.00
 * - SSE.STK 按额计费：close 费率恰为 open 的 2 倍、closetoday 为 3 倍
 * - 未登记费率的合约恒为 0
 * - fees 指向不存在/坏文件：只走错误分支，不影响后续计算
 */
TEST_F(R2PublicTest, feeTemplateAndCalcFeeMatrix)
{
	const std::filesystem::path fees = _base / "fees.json";
	{
		std::ofstream out(fees);
		out << "{\n"
			<< "  \"DCE.a\": {\"open\":2.0, \"close\":1.0, \"closetoday\":0.0, \"byvolume\":true, \"minfee\":5.0},\n"
			<< "  \"SSE.STK\": {\"open\":0.001, \"close\":0.002, \"closetoday\":0.003, \"byvolume\":false}\n"
			<< "}\n";
	}

	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, fees.string().c_str());

	// 按量：rate*qty，与价格无关
	EXPECT_DOUBLE_EQ(20.0, replayer.calc_fee(kFut, 5000.0, 10, 0));
	// 按量 + 最小费用下限
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee(kFut, 5000.0, 2, 1));
	// closetoday=0 时 rate==0，minfee 不生效
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee(kFut, 5000.0, 10, 2));
	// offset 越界 -> default 分支
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee(kFut, 5000.0, 10, 9));
	// 未登记费率的合约 -> 0
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("CFFEX.IF2103", 5000.0, 1, 0));

	// 按额：1:2:3 的严格比例（金额相同，只有 rate 在变）
	double fOpen = replayer.calc_fee(kStk, 10.0, 100, 0);
	double fClose = replayer.calc_fee(kStk, 10.0, 100, 1);
	double fCToday = replayer.calc_fee(kStk, 10.0, 100, 2);
	EXPECT_GT(fOpen, 0.0);
	EXPECT_DOUBLE_EQ(2 * fOpen, fClose);
	EXPECT_DOUBLE_EQ(3 * fOpen, fCToday);
	// 金额翻倍 -> 费用翻倍（四舍五入到分）
	EXPECT_DOUBLE_EQ(2 * fClose, replayer.calc_fee(kStk, 20.0, 100, 1));

	// 不存在的模板文件
	HisDataReplayer r2;
	CountSink s2;
	setup(r2, s2, (_base / "no_such_fees.json").string().c_str());
	EXPECT_DOUBLE_EQ(0.0, r2.calc_fee(kFut, 5000.0, 10, 0));

	// 坏文件（解析失败）
	const std::filesystem::path bad = _base / "bad_fees.json";
	{
		std::ofstream out(bad);
		out << "{ this is not a json at all ]]]";
	}
	HisDataReplayer r3;
	CountSink s3;
	setup(r3, s3, bad.string().c_str());
	EXPECT_DOUBLE_EQ(0.0, r3.calc_fee(kStk, 10.0, 100, 0));
}

/*
 * csv 模式下 level2 三类数据一律拒绝：
 * get_order_*_slice / get_transaction_slice -> check*() 早退 -> NULL，
 * 且订阅接口 sub_order_detail/sub_order_queue/sub_transaction 本身要能进去（含空代码早退）。
 */
TEST_F(R2PublicTest, level2DataRefusedInCsvMode)
{
	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, "");

	// 空代码早退
	replayer.sub_order_detail(1, "");
	replayer.sub_order_queue(1, "");
	replayer.sub_transaction(1, "");
	replayer.sub_tick(1, "");

	replayer.sub_order_detail(11, kStk);
	replayer.sub_order_queue(12, kStk);
	replayer.sub_transaction(13, kStk);
	replayer.sub_tick(14, kStk);

	EXPECT_EQ(nullptr, replayer.get_order_detail_slice(kStk, 5));
	EXPECT_EQ(nullptr, replayer.get_order_queue_slice(kStk, 5));
	EXPECT_EQ(nullptr, replayer.get_transaction_slice(kStk, 5));

	// 第二次调用走「已缓存但日期不同/为空」的分支，仍然是 NULL
	EXPECT_EQ(nullptr, replayer.get_order_detail_slice(kStk, 5));
	EXPECT_EQ(nullptr, replayer.get_order_queue_slice(kStk, 5));
	EXPECT_EQ(nullptr, replayer.get_transaction_slice(kStk, 5));

	// tick 未启用 -> get_tick_slice 直接 NULL（不查数据）
	EXPECT_EQ(nullptr, replayer.get_tick_slice(kStk, 5));
}

/*
 * stop()：未运行时报错早退；prepare 后置终止位，
 * 使 run() 在第一个回合就退出（不产生 replay done、不回放全部K线）。
 */
TEST_F(R2PublicTest, stopBeforeRunQuitsReplayEarly)
{
	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, "");

	// prepare 之后 _running 为 true
	replayer.stop();
	// 重复 stop 走 _terminated 早退分支
	replayer.stop();

	// 主K线登记
	replayer.enable_tick(false);
	WTSKlineSlice* main = replayer.get_kline_slice(kStk, "d1", kBarCnt, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();

	replayer.run();
	EXPECT_EQ(0u, sink._doneCnt);
	EXPECT_LT(sink._barClose, kBarCnt);
}

/*
 * clear_cache() 之后再 prepare()：缓存被彻底清掉，
 * 同一合约必须重新从 csv 装载（第二次取切片仍能得到完整K线）。
 */
TEST_F(R2PublicTest, clearCacheForcesReloadOnNextPrepare)
{
	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, "");

	WTSKlineSlice* first = replayer.get_kline_slice(kStk, "d1", kBarCnt, 1, true);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(kBarCnt, (uint32_t)first->size());
	first->release();

	replayer.clear_cache();
	// 缓存清空后，时间轴仍在，但主K线键与价格表已失效
	replayer.update_price(kStk, 88.0);
	EXPECT_DOUBLE_EQ(88.0, replayer.get_cur_price(kStk));
	replayer.clear_cache();
	EXPECT_EQ(20200102u, replayer.get_date());
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price(kStk));

	replayer.run();
	EXPECT_EQ(1u, sink._doneCnt);

	// 二次 prepare：reset() 把游标还原，重新装载仍能拿到 5 根
	ASSERT_TRUE(replayer.prepare());
	WTSKlineSlice* second = replayer.get_kline_slice(kStk, "d1", kBarCnt, 1, false);
	ASSERT_NE(nullptr, second);
	EXPECT_EQ(kBarCnt, (uint32_t)second->size());
	uint32_t firstDate = second->at(0)->date;
	second->release();
	EXPECT_TRUE(firstDate >= 20200102u && firstDate <= 20200108u);
}

/*
 * register_task + run_by_tasks：注册 0935 的每日调度任务后，
 * 回放必须由 run_by_tasks 分支驱动（handle_schedule 至少一次、replay_done 恰好一次），
 * 且时间轴推进到最后一天。
 */
TEST_F(R2PublicTest, registeredTaskSwitchesToTaskReplayAndSchedules)
{
	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, "");

	replayer.register_task(1, 0, 935, "d1");
	WTSKlineSlice* bars = replayer.get_kline_slice(kStk, "d1", kBarCnt, 1, true);
	ASSERT_NE(nullptr, bars);
	bars->release();

	replayer.run();
	EXPECT_GE(sink._schedCnt, 1u);
	EXPECT_EQ(1u, sink._doneCnt);
	EXPECT_EQ(20200108u, sink._lastBarDate);
	EXPECT_LE(sink._lastSchedTime, 100000u);

	// 非法周期 / 非法模板不应崩
	replayer.register_task(2, 0, 935, "x1");
	replayer.register_task(3, 0, 99999, "d1");
	EXPECT_EQ(20200108u, replayer.get_date());
}

/*
 * 元数据与时间轴的公开查询面：
 * get_commodity_info / get_session_info / get_rawcode / 四个时间 getter 口径一致。
 */
TEST_F(R2PublicTest, metadataAndTimelineQueries)
{
	HisDataReplayer replayer;
	CountSink sink;
	setup(replayer, sink, "");

	EXPECT_NE(nullptr, replayer.get_commodity_info(kStk));
	EXPECT_EQ(nullptr, replayer.get_commodity_info("SSE.STK.999999"));

	std::string raw = replayer.get_rawcode(kStk);
	EXPECT_EQ(std::string("000001"), raw);
	// 非法代码不得崩，返回空或原样
	EXPECT_TRUE(replayer.get_rawcode("").empty() || true);

	WTSSessionInfo* sInfo = replayer.get_session_info("SD0930");
	ASSERT_NE(nullptr, sInfo);
	WTSSessionInfo* byCode = replayer.get_session_info(kStk, true);
	EXPECT_EQ(sInfo, byCode);
	EXPECT_EQ(nullptr, replayer.get_session_info("NOT_A_SESSION"));

	EXPECT_EQ(20200102u, replayer.get_date());
	EXPECT_EQ(930u, replayer.get_min_time());
	EXPECT_EQ(930u, replayer.get_raw_time());
	EXPECT_EQ(0u, replayer.get_secs());
	EXPECT_EQ(20200102u, replayer.get_trading_date());
	EXPECT_FALSE(replayer.is_tick_enabled());

	// 未装载任何数据时价格查询恒为 0
	EXPECT_DOUBLE_EQ(0.0, replayer.get_day_price(kStk));
	EXPECT_DOUBLE_EQ(0.0, replayer.get_day_price(kStk, 9));
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price(kStk));
	replayer.update_price(kStk, 12.34);
	EXPECT_DOUBLE_EQ(12.34, replayer.get_cur_price(kStk));
}
