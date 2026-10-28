/**
 * r2_mocker_tick_fill_test.cpp  (lane R2)
 *
 * 关键突破：上一轮 lane 认定「CtaMocker::proc_tick 是 private、WTSTickData 没有可安全构造
 * 的公开入口」，因此把成交回写整块留给主控裁定。实际上：
 *   - CtaMocker/SelMocker/HftMocker/UftMocker 的 handle_tick 都是 **IDataSink 公有重写**
 *     (CtaMocker.h:277 / SelMocker.h:205 / HftMocker.h:102+ / UftMocker.h:200+)
 *   - WTSTickData::create(WTSTickStruct&) 是 **public static 工厂** (WTSDataDef.hpp:884)，
 *     与 release() 同一个池，构造/回收都合法（p08 反对的是伪造对象后跨池 release）
 *   - 于是 proc_tick(L695) / do_set_position(L1603) / update_dyn_profit(L973)
 *     / log_trade / 结算落盘 这条撮合主链可以完全从公开接口驱动。
 * 另外 HftMocker::on_tick(L345) 与 UftMocker::on_tick(L228) 里的 procTask()
 * 在 _tasks 为空时直接 return（不起线程、不挂 cv），所以 tick 驱动是安全的。
 *
 * 落盘：temp_directory_path()/r2_tick_*；dist/ 只读。SelMocker::handle_replay_done
 * 有已知整数除零（q02 已整类 DISABLED），本文件不调用它。
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

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/UftMocker.h>
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
			("r2_tick_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	const char* const kCode = "SSE.STK.000001";
	const uint32_t kBarCnt = 5;
	const uint32_t kDates[kBarCnt] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };

	bool write_daily_csv(const std::filesystem::path& base)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(kCode) + "_d.csv"));
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

	WTSVariant* replay_cfg(const std::filesystem::path& root, const std::filesystem::path& base)
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
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	/*
	 * 用 public 工厂造一个真实 tick（池对象，release 回同一池）。
	 */
	WTSTickData* make_tick(const char* stdCode, uint32_t uDate, uint32_t uTimeMs, double px)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(WTSTickStruct));
		wt_strcpy(ts.code, stdCode);
		ts.action_date = uDate;
		ts.action_time = uTimeMs;
		ts.trading_date = uDate;
		ts.price = px;
		ts.open = px - 0.5;
		ts.high = px + 0.5;
		ts.low = px - 1.0;
		ts.volume = 100;
		ts.total_volume = 1000;
		ts.turn_over = px * 100;
		ts.total_turnover = px * 1000;
		return WTSTickData::create(ts);
	}

	uint32_t count_lines(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return 0;
		std::stringstream ss;
		ss << in.rdbuf();
		uint32_t n = 0;
		for (char c : ss.str())
			if (c == '\n') n++;
		return n;
	}

	std::string read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}
}

class R2TickTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("base");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
		ASSERT_TRUE(write_daily_csv(_base));

		WTSVariant* cfg = replay_cfg(_root, _base);
		ASSERT_TRUE(_replayer.init(cfg));
		cfg->release();
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
	HisDataReplayer _replayer;
};

/*
 * 市价信号 -> tick 撮合 -> 真实成交回写：
 * proc_tick 取到信号后用最新价 do_set_position，持仓表被真正建立，
 * _sig_map 被 erase（此后 stra_get_position 走持仓表而不是信号表）。
 */
TEST_F(R2TickTest, marketSignalFilledByTickAndDumpsTrade)
{
	CtaMocker mocker(&_replayer, "r2_mkt_fill", 0, true);
	_replayer.register_sink(&mocker, "r2_mkt_fill");
	WTSKlineSlice* bars = mocker.stra_get_bars(kCode, "d1", kBarCnt, true);
	ASSERT_NE(nullptr, bars);
	bars->release();
	ASSERT_TRUE(_replayer.prepare());

	mocker.handle_session_begin(_replayer.get_trading_date());

	// 下单前先确认最新价与持仓都是空的
	WTSTickData* px0 = make_tick(kCode, 20200102, 93000000, 10.6);
	mocker.handle_tick(kCode, px0, 0);
	px0->release();
	EXPECT_DOUBLE_EQ(10.6, mocker.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));

	mocker.stra_enter_long(kCode, 100, "tagA", 0.0, 0.0);
	// 未撮合时 stra_get_position 回落取信号量，但均价/明细必须为空
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kCode));
	EXPECT_EQ(0ull, mocker.stra_get_last_entertime(kCode));

	// 真实成交：11.0 的 tick 触发 proc_tick -> do_set_position
	WTSTickData* fill = make_tick(kCode, 20200102, 93100000, 11.0);
	mocker.handle_tick(kCode, fill, 1);
	fill->release();

	EXPECT_DOUBLE_EQ(11.0, mocker.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode));
	EXPECT_DOUBLE_EQ(11.0, mocker.stra_get_position_avgpx(kCode));
	EXPECT_NE(0ull, mocker.stra_get_last_entertime(kCode));
	EXPECT_DOUBLE_EQ(11.0, mocker.stra_get_last_enterprice(kCode));
	EXPECT_EQ(std::string("tagA"), std::string(mocker.stra_get_last_entertag(kCode)));
	EXPECT_NE(0ull, mocker.stra_get_first_entertime(kCode));
	EXPECT_NE(0ull, mocker.stra_get_detail_entertime(kCode, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_profit(kCode, "tagA", 1));

	// 价格上移到 13.0 -> 浮动盈亏必须为正（update_dyn_profit + stra_get_position_profit）
	WTSTickData* up = make_tick(kCode, 20200102, 93200000, 13.0);
	mocker.handle_tick(kCode, up, 1);
	up->release();
	EXPECT_GT(mocker.stra_get_position_profit(kCode), 0.0);
	EXPECT_GT(mocker.stra_get_detail_profit(kCode, "tagA", 1), 0.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_profit(kCode, "nosuch", 1));

	mocker.handle_session_end(_replayer.get_trading_date());
	mocker.handle_replay_done();

	const std::filesystem::path folder = _out / "r2_mkt_fill";
	// 一笔开仓成交必须落地（表头 + 1 行）
	EXPECT_EQ(2u, count_lines(folder / "trades.csv"));
	EXPECT_NE(std::string::npos, read_file(folder / "trades.csv").find("tagA"));
	// 持仓表不再为空
	EXPECT_EQ(2u, count_lines(folder / "positions.csv"));
	// 信号表：append_signal 一行
	EXPECT_EQ(2u, count_lines(folder / "signals.csv"));
	EXPECT_GE(count_lines(folder / "funds.csv"), 2u);
}

/*
 * 带目标价的信号（desprice != 0）：proc_tick 用**目标价**而不是最新价成交，
 * 这是与上一条用例唯一不同的分支，必须能证伪「成交价取错」。
 */
TEST_F(R2TickTest, signalWithDespriceFillsAtTargetNotLastPrice)
{
	CtaMocker mocker(&_replayer, "r2_des_px", 0, true);
	_replayer.register_sink(&mocker, "r2_des_px");
	WTSKlineSlice* bars = mocker.stra_get_bars(kCode, "d1", kBarCnt, true);
	ASSERT_NE(nullptr, bars);
	bars->release();
	ASSERT_TRUE(_replayer.prepare());

	WTSTickData* px0 = make_tick(kCode, 20200102, 93000000, 10.6);
	mocker.handle_tick(kCode, px0, 0);
	px0->release();

	mocker.stra_enter_long(kCode, 200, "tagL", 10.8, 0.0);
	WTSTickData* fill = make_tick(kCode, 20200102, 93100000, 11.5);
	mocker.handle_tick(kCode, fill, 1);
	fill->release();

	// 成交在目标价 10.8，而不是 tick 价 11.5
	EXPECT_DOUBLE_EQ(11.5, mocker.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(10.8, mocker.stra_get_position_avgpx(kCode));
	EXPECT_DOUBLE_EQ(200.0, mocker.stra_get_position(kCode));
	// 浮动盈亏按 11.5 - 10.8 计
	EXPECT_GT(mocker.stra_get_position_profit(kCode), 0.0);

	mocker.handle_replay_done();
	const std::filesystem::path folder = _out / "r2_des_px";
	std::string trades = read_file(folder / "trades.csv");
	EXPECT_NE(std::string::npos, trades.find("tagL"));
	EXPECT_NE(std::string::npos, trades.find("10.8"));
	EXPECT_EQ(2u, count_lines(folder / "trades.csv"));
}

/*
 * 平仓链路：先满仓成交，再 stra_exit_long 生成「目标持仓 = 现持仓 - qty」的信号，
 * 第二个 tick 撮合后走 do_set_position 的减仓分支（含 log_close）。
 */
TEST_F(R2TickTest, exitLongSignalClosesPartOfPosition)
{
	CtaMocker mocker(&_replayer, "r2_close_pos", 0, true);
	_replayer.register_sink(&mocker, "r2_close_pos");
	WTSKlineSlice* bars = mocker.stra_get_bars(kCode, "d1", kBarCnt, true);
	ASSERT_NE(nullptr, bars);
	bars->release();
	ASSERT_TRUE(_replayer.prepare());
	mocker.handle_session_begin(_replayer.get_trading_date());

	WTSTickData* t1 = make_tick(kCode, 20200102, 93000000, 11.0);
	mocker.handle_tick(kCode, t1, 0);
	t1->release();

	mocker.stra_enter_long(kCode, 100, "tagO", 0.0, 0.0);
	WTSTickData* t2 = make_tick(kCode, 20200102, 93100000, 12.0);
	mocker.handle_tick(kCode, t2, 1);
	t2->release();
	ASSERT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode));
	const uint64_t openTime = mocker.stra_get_detail_entertime(kCode, "tagO");
	ASSERT_NE(0ull, openTime);

	mocker.stra_exit_long(kCode, 40, "tagX", 0.0, 0.0);
	// 信号尚未成交：持仓仍是 100（信号表口径同样给 60，故只断言均价没变）
	EXPECT_DOUBLE_EQ(12.0, mocker.stra_get_position_avgpx(kCode));

	WTSTickData* t3 = make_tick(kCode, 20200102, 93200000, 12.5);
	mocker.handle_tick(kCode, t3, 1);
	t3->release();

	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kCode));
	// 剩余持仓的均价仍按原开仓价，明细开仓时间不变
	EXPECT_DOUBLE_EQ(12.0, mocker.stra_get_detail_cost(kCode, "tagO") / 60.0);
	EXPECT_EQ(openTime, mocker.stra_get_detail_entertime(kCode, "tagO"));
	// 已平掉的部分不该再被查成持仓
	EXPECT_DOUBLE_EQ(60.0, mocker.stra_get_position(kCode, false));

	mocker.handle_session_end(_replayer.get_trading_date());
	mocker.handle_replay_done();
	const std::filesystem::path folder = _out / "r2_close_pos";
	// 一开一平两笔成交
	EXPECT_EQ(3u, count_lines(folder / "trades.csv"));
	// 平仓明细落地（表头 + 1 行）
	EXPECT_EQ(2u, count_lines(folder / "closes.csv"));
}

/*
 * SelMocker / HftMocker / UftMocker 的 tick 入口：
 * 三者的 handle_tick 都会把最新价写进自己的 _price_map，
 * stra_get_price 是公开可观测点；无策略（_strategy==NULL）时
 * 各 on_xxx_updated / procTask 必须安全早退，不得起线程或挂死。
 */
TEST_F(R2TickTest, selHftUftTickUpdatesPriceMapWithoutStrategy)
{
	SelMocker sel(&_replayer, "r2_sel_tick");
	HftMocker hft(&_replayer, "r2_hft_tick");
	UftMocker uft(&_replayer, "r2_uft_tick");

	_replayer.register_sink(&sel, "r2_sel_tick");
	WTSKlineSlice* bars = sel.stra_get_bars(kCode, "d1", kBarCnt);
	ASSERT_NE(nullptr, bars);
	bars->release();
	ASSERT_TRUE(_replayer.prepare());

	const uint32_t tdate = _replayer.get_trading_date();
	ASSERT_EQ(20200102u, tdate);

	sel.handle_init();
	hft.handle_init();
	uft.handle_init();
	sel.handle_session_begin(tdate);
	hft.handle_session_begin(tdate);
	uft.handle_session_begin(tdate);

	// 未收到 tick 之前一律 0
	EXPECT_DOUBLE_EQ(0.0, sel.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(0.0, uft.stra_get_price(kCode));

	WTSTickData* t1 = make_tick(kCode, 20200102, 93000000, 11.25);
	sel.handle_tick(kCode, t1, 0);
	WTSTickData* t2 = make_tick(kCode, 20200102, 93000000, 11.25);
	hft.handle_tick(kCode, t2, 0);
	WTSTickData* t3 = make_tick(kCode, 20200102, 93000000, 11.25);
	uft.handle_tick(kCode, t3, 0);

	EXPECT_DOUBLE_EQ(11.25, sel.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(11.25, hft.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(11.25, uft.stra_get_price(kCode));

	// 第二笔 tick 覆盖最新价，旧 tick 各自 release 回池
	WTSTickData* t4 = make_tick(kCode, 20200102, 93100000, 12.75);
	sel.handle_tick(kCode, t4, 1);
	WTSTickData* t5 = make_tick(kCode, 20200102, 93100000, 12.75);
	uft.handle_tick(kCode, t5, 1);
	WTSTickData* t6 = make_tick(kCode, 20200102, 93100000, 12.75);
	hft.handle_tick(kCode, t6, 1);

	EXPECT_DOUBLE_EQ(12.75, sel.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(12.75, uft.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(12.75, hft.stra_get_price(kCode));
	// 未知合约仍然是 0，不会把上一次的价带过去
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_price("SSE.STK.999999"));

	t1->release();
	t2->release();
	t3->release();
	t4->release();
	t5->release();
	t6->release();

	// 分钟线闭合：三者都走 bar 通道（无策略时只做登记/时间更新）
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(WTSBarStruct));
	bar.date = 20200102;
	bar.time = 202001020931;
	bar.open = 11.0;
	bar.high = 12.0;
	bar.low = 10.5;
	bar.close = 11.5;
	bar.vol = 500;
	bar.money = 5750;
	sel.handle_bar_close(kCode, "m1", 1, &bar);
	hft.handle_bar_close(kCode, "m1", 1, &bar);
	uft.handle_bar_close(kCode, "m1", 1, &bar);
	// bar 通道不得把 tick 建立的最新价改掉
	EXPECT_DOUBLE_EQ(12.75, sel.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(12.75, hft.stra_get_price(kCode));
	EXPECT_DOUBLE_EQ(12.75, uft.stra_get_price(kCode));

	sel.handle_session_end(tdate);
	hft.handle_session_end(tdate);
	uft.handle_session_end(tdate);
	hft.handle_replay_done();
	uft.handle_replay_done();
}
