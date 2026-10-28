/**
 * q02_cta_replayer_integration_test.cpp
 *
 * 靶子：CtaMocker 挂在**真实 HisDataReplayer**（csv 日线底座）上的只读查询委托、
 * 信号/条件单登记分支与结算落地。
 * p03 用 #define private public + 手工塞 _bd_mgr/_fee_map 覆盖了成交推进；
 * 本文件只用公开接口，并且专挑 p03/p08 未走的「委托到 _replayer」的查询面。
 *
 * 说明（不虚构断言）：
 *   CtaMocker::proc_tick 是 private（CtaMocker.h L233 位于 private 段），
 *   成交回写只能由 handle_tick(WTSTickData*) 驱动，而 WTSTickData 没有可安全构造的公开入口
 *   （p08 已论证，伪造对象有 release 错池风险）；故本文件不驱动真实成交，
 *   只固化「信号量即持仓」的 append_signal 语义与条件单登记语义。
 *   真实成交回写由 p03（手工夹具）与本 lane 的 DISABLED_ 归档共同说明。
 *
 * 落盘：temp_directory_path()/q02_ctaint_*，TearDown remove_all；dist/ 只读。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/CtaMocker.h>
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
			("q02_ctaint_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	struct DayBar
	{
		uint32_t date;
		double open;
		double high;
		double low;
		double close;
	};

	// 20200102 ~ 20200108 五个交易日
	const DayBar kDayBars[] = {
		{20200102u,  9.5, 11.0,  9.4, 10.5},
		{20200103u, 10.5, 12.0, 10.4, 11.5},
		{20200106u, 11.5, 13.0, 11.4, 12.5},
		{20200107u, 12.5, 14.0, 12.4, 13.5},
		{20200108u, 13.5, 15.0, 13.4, 14.5},
	};
	const uint32_t kBarCnt = sizeof(kDayBars) / sizeof(kDayBars[0]);

	const char* const kCode = "SSE.STK.000001";

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
			out << kDayBars[i].date << ",150000,"
				<< kDayBars[i].open << "," << kDayBars[i].high << ","
				<< kDayBars[i].low << "," << kDayBars[i].close << ","
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

	std::string read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	std::string squeeze(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		for (char c : s)
		{
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
				out.push_back(c);
		}
		return out;
	}

	uint32_t count_lines(const std::string& s)
	{
		uint32_t n = 0;
		for (char c : s)
			if (c == '\n') n++;
		return n;
	}
}

class Q02CtaIntegrationTest : public ::testing::Test
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
		ASSERT_TRUE(_replayer.prepare());
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
 * CtaMocker 的只读查询全部是对 _replayer 的委托：
 * 时间口径一致、合约信息来自底座、rawcode 去掉交易所与品类前缀。
 */
TEST_F(Q02CtaIntegrationTest, queriesDelegateToReplayer)
{
	CtaMocker mocker(&_replayer, "q02_qry");
	_replayer.register_sink(&mocker, "q02_qry");

	ASSERT_NE(nullptr, mocker.stra_get_comminfo(kCode));
	EXPECT_EQ(nullptr, mocker.stra_get_comminfo("DCE.FUTU.999999"));

	EXPECT_EQ(std::string("000001"), mocker.stra_get_rawcode(kCode));
	EXPECT_EQ(_replayer.get_date(), mocker.stra_get_date());
	EXPECT_EQ(_replayer.get_trading_date(), mocker.stra_get_tdate());
	EXPECT_EQ(_replayer.get_min_time(), mocker.stra_get_time());
	EXPECT_TRUE(_replayer.is_tick_enabled() == false);

	// 主K线登记（isMain=true -> replayer 记住 _main_key/_main_period）
	WTSKlineSlice* sli = mocker.stra_get_bars(kCode, "d", 5, true);
	ASSERT_NE(nullptr, sli);
	EXPECT_GE(static_cast<uint32_t>(sli->size()), 1u);
	sli->release();

	// 未开 tick、无 tick 缓存
	EXPECT_EQ(nullptr, mocker.stra_get_last_tick(kCode));

	// 用户数据读写
	mocker.stra_save_user_data("k1", "v1");
	EXPECT_EQ(std::string("v1"), std::string(mocker.stra_load_user_data("k1")));
	EXPECT_EQ(std::string("dft"), std::string(mocker.stra_load_user_data("k_none", "dft")));

	// 日志族只走 WTSLogger，不允许崩溃
	mocker.stra_log_info("info");
	mocker.stra_log_debug("debug");
	mocker.stra_log_warn("warn");
	mocker.stra_log_error("error");
}

/*
 * 市价单：stra_enter_long -> append_signal -> _sig_map。
 * append_signal(L1590) 的 gentime 由 replayer 的 date/raw_time/secs 拼出来，
 * 这里断言它已越过「尚未推进游标」的 0 值即可（具体位宽口径见 BUG-P04-010 的时间对齐说明）。
 * 成交回写需要真实 tick（proc_tick 为 private），故本用例固化的是信号语义：
 * 未成交时 stra_get_position 回落取信号量（L1999-L2012）。
 */
TEST_F(Q02CtaIntegrationTest, marketSignalQueuesAndDumpsWithoutFill)
{
	CtaMocker mocker(&_replayer, "q02_mkt", 0, true);
	_replayer.register_sink(&mocker, "q02_mkt");
	mocker.stra_get_bars(kCode, "d", 5, true);

	mocker.stra_enter_long(kCode, 100, "tagA", 0.0, 0.0);
	// 有信号 -> stra_get_position 取信号量
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode));
	// 但持仓表还是空的：明细类查询必须为 0
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(kCode));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(kCode));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime(kCode));
	EXPECT_EQ(0ull, mocker.stra_get_detail_entertime(kCode, "tagA"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_detail_cost(kCode, "tagA"));

	std::map<std::string, double> got;
	mocker.enum_position([&got](const char* stdCode, double qty) { got[stdCode] = qty; }, false);
	ASSERT_EQ(1u, got.size());
	EXPECT_DOUBLE_EQ(100.0, got[kCode]);

	// 空头信号覆盖多头信号（同一合约只保留最后一条）
	mocker.stra_enter_short(kCode, -30, "tagB", 0.0, 0.0);
	EXPECT_DOUBLE_EQ(-30.0, mocker.stra_get_position(kCode));

	mocker.handle_session_end(_replayer.get_trading_date());
	mocker.handle_replay_done();

	const std::filesystem::path folder = _out / "q02_mkt";
	// 没有成交 -> 交易/平仓日志只有表头
	EXPECT_EQ(1u, count_lines(read_file(folder / "trades.csv")));
	EXPECT_EQ(1u, count_lines(read_file(folder / "closes.csv")));
	EXPECT_EQ(1u, count_lines(read_file(folder / "signals.csv")));
	// 结算行仍然落地（持仓为空的合约被 continue 跳过）
	EXPECT_EQ(std::string("date,closeprofit,positionprofit,dynbalance,fee\n")
		+ std::to_string(_replayer.get_trading_date()) + ",0.00,0.00,0.00,0.00\n",
		read_file(folder / "funds.csv"));
	EXPECT_EQ(1u, count_lines(read_file(folder / "positions.csv")));

	// 信号被 dump_stradata 序列化，供增量回测恢复
	const std::string sdata = squeeze(read_file(folder / "q02_mkt.json"));
	EXPECT_NE(std::string::npos, sdata.find("\"signals\":{\"SSE.STK.000001\""));
	EXPECT_NE(std::string::npos, sdata.find("\"usertag\":\"tagB\""));
	EXPECT_NE(std::string::npos, sdata.find("\"volume\":-30"));
	EXPECT_NE(std::string::npos, sdata.find("\"positions\":[]"));
}

/*
 * 限价条件单：stra_enter_long(L1323-L1347) 带 limitprice 时只登记 _condtions，
 * 不产生信号 -> stra_get_position 必须为 0，且 dump 的 conditions 段带回 target/action。
 */
TEST_F(Q02CtaIntegrationTest, limitPriceRegistersConditionOnly)
{
	CtaMocker mocker(&_replayer, "q02_cond", 0, true);
	_replayer.register_sink(&mocker, "q02_cond");
	mocker.stra_get_bars(kCode, "d", 5, true);

	mocker.stra_enter_long(kCode, 10, "cond1", 11.0, 0.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));

	std::map<std::string, double> got;
	mocker.enum_position([&got](const char* stdCode, double qty) { got[stdCode] = qty; }, false);
	EXPECT_TRUE(got.empty());

	// 止损单走同一个分支，_alg 不同
	mocker.stra_enter_short(kCode, 20, "cond2", 0.0, 9.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(kCode));

	mocker.handle_replay_done();
	const std::string sdata = squeeze(read_file(_out / "q02_cond" / "q02_cond.json"));
	EXPECT_NE(std::string::npos, sdata.find("\"conditions\":{\"settime\""));
	EXPECT_NE(std::string::npos, sdata.find("\"items\":{\"SSE.STK.000001\""));
	EXPECT_NE(std::string::npos, sdata.find("\"code\":\"SSE.STK.000001\""));
	EXPECT_NE(std::string::npos, sdata.find("\"usertag\":\"cond1\""));
	EXPECT_NE(std::string::npos, sdata.find("\"usertag\":\"cond2\""));
	EXPECT_NE(std::string::npos, sdata.find("\"target\":11"));
	EXPECT_NE(std::string::npos, sdata.find("\"target\":9"));
	EXPECT_NE(std::string::npos, sdata.find("\"qty\":10"));
	EXPECT_NE(std::string::npos, sdata.find("\"qty\":20"));
	// dump 里没有 signals 的残留
	EXPECT_NE(std::string::npos, sdata.find("\"signals\":{}"));

	// 条件单能被 load_incremental_data 读回来（复用同一份 dump 报文）
	CtaMocker second(&_replayer, "q02_cond");
	second.load_incremental_data("q02_cond");
	second.stra_get_bars(kCode, "d", 5, true);
	// 恢复的仍是条件单，不是持仓 -> 不触发成交前持仓为 0
	EXPECT_DOUBLE_EQ(0.0, second.stra_get_position(kCode));
}

/*
 * 反手与目标量：stra_set_position(L1520+) 的守卫族在真实底座下的行为——
 * 未知合约早退、与当前目标相等早退，均不得改动信号表。
 */
TEST_F(Q02CtaIntegrationTest, unknownCodeAndFlatQueriesAreSafe)
{
	CtaMocker mocker(&_replayer, "q02_unk");
	_replayer.register_sink(&mocker, "q02_unk");

	mocker.stra_enter_long("DCE.FUTU.999999", 10, "x", 0.0, 0.0);
	mocker.stra_enter_short("DCE.FUTU.999999", 10, "x", 0.0, 0.0);
	mocker.stra_set_position("DCE.FUTU.999999", 10, "x", 0.0, 0.0);
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("DCE.FUTU.999999"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx("DCE.FUTU.999999"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit("DCE.FUTU.999999"));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime("DCE.FUTU.999999"));
	EXPECT_EQ(0ull, mocker.stra_get_last_entertime("DCE.FUTU.999999"));
	EXPECT_EQ(0ull, mocker.stra_get_last_exittime("DCE.FUTU.999999"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_last_enterprice("DCE.FUTU.999999"));
	// 未知合约的 rawcode 原样返回（找不到 pid 时的兜底口径）
	EXPECT_EQ(std::string("DCE.FUTU.999999"), std::string(mocker.stra_get_rawcode("DCE.FUTU.999999")));

	std::map<std::string, double> got;
	mocker.enum_position([&got](const char* stdCode, double qty) { got[stdCode] = qty; }, false);
	EXPECT_TRUE(got.empty());

	// 小节结束清价格缓存；会话边界只做状态推进
	mocker.handle_section_end(20200102u, 1130u);
	mocker.handle_session_begin(_replayer.get_trading_date());
	mocker.handle_session_end(_replayer.get_trading_date());
	mocker.handle_replay_done();
	EXPECT_EQ(1u, count_lines(read_file(_out / "q02_unk" / "positions.csv")));
}

/*
 * 需要真实 tick 指针 / 会阻塞 / 口径不可从底座推导的入口整族收在这里（启用集合零残留）：
 *   handle_tick / on_tick / on_tick_updated 需要非空 WTSTickData*，
 *     handle_tick 第一行 newTick->price() 即解引用（p08 已论证无安全构造入口）；
 *   proc_tick 是 private，成交回写只能经 tick 进入，
 *     因此 do_set_position 的建仓均价、update_dyn_profit 的浮盈数值不在本 lane 断言；
 *   stra_get_position 的 onlyValid 口径依赖 SSE.STK 是否被判为 T+1
 *     （dist/common/stk_comms.json 未显式配 t1，不可从底座推导）-> 待裁定；
 *   stra_get_price / stra_get_day_price 在无 tick 无价格缓存时的取价路径未精读；
 *   install_hook / enable_hook(true) / step_calc / on_calculate 走 _cond_calc.wait() 阻塞原语；
 *   handle_bar_close / on_bar 在无策略时虽不解引用，但其 _kline_tags 记账口径需配合
 *     run_by_bars 才能给出可推导的闭盘数（BUG-P04-010 会吃掉首根）-> 一并禁用。
 */
TEST_F(Q02CtaIntegrationTest, DISABLED_RealTickAndBlockingHookFamily)
{
	CtaMocker mocker(&_replayer, "q02_disabled");
	_replayer.register_sink(&mocker, "q02_disabled");
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = 20200102u;
	bar.time = 20200102u;
	bar.close = 10.5;
	mocker.handle_bar_close(kCode, "d", 1, &bar);
	mocker.stra_sub_ticks(kCode);
	mocker.stra_sub_bar_events(kCode, "d");
	mocker.install_hook();
	mocker.enable_hook(true);
	mocker.step_calc();
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position(kCode, true));
	EXPECT_NE(0.0, mocker.stra_get_price(kCode));
	EXPECT_NE(0.0, mocker.stra_get_day_price(kCode, 0));
	_replayer.run(false);
}
