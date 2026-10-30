/**
 * r43_sel_mocker_dump_test.cpp
 * Lane R43：补 SelMocker.{h,cpp} 中 p03（交易主路径）与 q02（tick 更新）没有驱动到的
 * 「落盘 + 调度 + 会话 + 查询」块：
 *   dump_outputs(L161-213)        —— trades/closes/funds/signals/positions/ud_*.json
 *   dump_stradata(L59-158)        —— 持仓/资金/信号 json（含 _cur_tdate 透传）
 *   on_schedule(L481-512)         —— _emit_times/_schedule_times 推进（handle_replay_done 依赖）
 *   on_session_begin/end(L514-579) —— _cur_tdate 与资金日志
 *   handle_tick(L316-354) / proc_tick(L356-377) —— 无信号、无持仓时的价格缓存推进
 *   stra_* 查询族(L913-1013)      —— fund_data/day_price/sessinfo/user_data/enum_position
 * 落盘目录为 temp_directory_path()/r43_sel_out_<pid>，TearDown 清理。
 *
 * handle_replay_done 在零次调度时将平均耗时记为 0；零次与正常调度结束路径均有覆盖。
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
#include <WtBtCore/SelMocker.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path r43_sel_out()
	{
		auto path = std::filesystem::temp_directory_path() /
			("r43_sel_out_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	std::filesystem::path r43_repo_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::string r43_read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	uint32_t r43_line_count(const std::string& content)
	{
		if (content.empty())
			return 0;
		uint32_t lines = 0;
		for (char c : content)
		{
			if (c == '\n')
				lines++;
		}
		if (content.back() != '\n')
			lines++;
		return lines;
	}

	WTSTickData* r43_tick(const char* exchg, const char* code, uint32_t uDate,
		uint32_t actionTime, double price, double volume)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, exchg);
		strcpy(ts.code, code);
		ts.action_date = uDate;
		ts.trading_date = uDate;
		ts.action_time = actionTime;
		ts.price = price;
		ts.open = price;
		ts.high = price;
		ts.low = price;
		ts.volume = volume;
		ts.total_volume = volume;
		ts.bid_prices[0] = price - 0.01;
		ts.ask_prices[0] = price + 0.01;
		ts.bid_qty[0] = 100;
		ts.ask_qty[0] = 100;
		return WTSTickData::create(ts);
	}
}

class R43CompletionTrackingSelMocker : public SelMocker
{
public:
	using SelMocker::SelMocker;

	uint32_t replayDoneCalls {0};

	void on_bactest_end() override { replayDoneCalls++; }
};

class R43SelMockerDumpTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_out = r43_sel_out();
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _out;
};

// 调度推进 + 回放结束落盘：六个 csv 与两个 json 都必须生成，且 json 含三段结构
TEST_F(R43SelMockerDumpTest, scheduleThenReplayDoneWritesAllArtifacts)
{
	HisDataReplayer replayer;
	SelMocker mocker(&replayer, "r43_sel_dump");
	const char* code = "SSE.STK.000001";

	// 没有K线标签时 on_schedule 仍会推进 _schedule_times/_emit_times
	mocker.handle_schedule(20200106u, 930u);
	mocker.handle_schedule(20200106u, 931u);

	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r43_sel_dump";
	ASSERT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "closes.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "funds.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "signals.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "positions.csv"));
	ASSERT_TRUE(std::filesystem::exists(dir / "ud_r43_sel_dump.json"));

	const std::string json = r43_read_file(dir / "r43_sel_dump.json");
	ASSERT_FALSE(json.empty());
	EXPECT_NE(std::string::npos, json.find("\"positions\""));
	EXPECT_NE(std::string::npos, json.find("\"fund\""));
	EXPECT_NE(std::string::npos, json.find("\"signals\""));
	EXPECT_NE(std::string::npos, json.find("\"tdate\""));

	// 没有成交/平仓/信号时，csv 只留表头（不能出现脏行）
	EXPECT_EQ(1u, r43_line_count(r43_read_file(dir / "trades.csv")));
	EXPECT_EQ(1u, r43_line_count(r43_read_file(dir / "closes.csv")));
	EXPECT_EQ(1u, r43_line_count(r43_read_file(dir / "signals.csv")));
	(void)code;
}

// 会话开始/结束 + tick 回调：资金日志落一行，json 里的 tdate 取会话交易日
TEST_F(R43SelMockerDumpTest, sessionAndTickFeedDumpRows)
{
	HisDataReplayer replayer;
	SelMocker mocker(&replayer, "r43_sel_sess");
	const char* code = "SSE.STK.000001";

	mocker.handle_schedule(20200106u, 930u);
	mocker.handle_session_begin(20200106u);

	WTSTickData* t1 = r43_tick("SSE", "000001", 20200106u, 93100000u, 10.5, 100);
	mocker.handle_tick(code, t1, 0);
	t1->release();
	// pxType=3 是模拟tick的收盘那笔，handle_tick 内部不再重复撮合
	WTSTickData* t2 = r43_tick("SSE", "000001", 20200106u, 93200000u, 10.8, 200);
	mocker.handle_tick(code, t2, 3);
	t2->release();

	mocker.handle_session_end(20200106u);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r43_sel_sess";
	const std::string funds = r43_read_file(dir / "funds.csv");
	// 表头 + 一个交易日
	EXPECT_EQ(2u, r43_line_count(funds));
	EXPECT_NE(std::string::npos, funds.find("20200106"));

	const std::string json = r43_read_file(dir / "r43_sel_sess.json");
	EXPECT_NE(std::string::npos, json.find("20200106"));
	// 没有信号/持仓，两段仍然必须落成结构而不是缺字段
	EXPECT_NE(std::string::npos, json.find("\"positions\""));
	EXPECT_NE(std::string::npos, json.find("\"signals\""));
}

// 无行情数据时的查询族：返回稳定默认值，不因空 map/空回放器崩溃
TEST_F(R43SelMockerDumpTest, emptyStateQueriesAreStable)
{
	HisDataReplayer replayer;
	SelMocker mocker(&replayer, "r43_sel_query");
	const char* code = "SSE.STK.000001";

	for (int flag = 0; flag <= 4; flag++)
		EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_fund_data(flag));

	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_day_price(code, 0));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_price(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx(code));
	EXPECT_EQ(0u, mocker.stra_get_first_entertime(code));
	EXPECT_EQ(0u, mocker.stra_get_last_entertime(code));
	EXPECT_EQ(std::string(""), mocker.stra_get_last_entertag(code));
	EXPECT_EQ(0u, mocker.stra_get_last_exittime(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_last_enterprice(code));

	// 回放器未加载基础数据 -> 时段/品种查询为空
	EXPECT_EQ(nullptr, mocker.stra_get_sessinfo(code));
	EXPECT_EQ(nullptr, mocker.stra_get_comminfo(code));
	EXPECT_EQ(std::string(""), mocker.stra_get_rawcode(code));

	uint32_t enumCnt = 0;
	mocker.enum_position([&enumCnt](const char*, double) { enumCnt++; });
	EXPECT_EQ(0u, enumCnt);

	// 用户数据是纯内存键值对，默认值与写后读都必须一致
	EXPECT_EQ(std::string("def"), mocker.stra_load_user_data("r43_key", "def"));
	mocker.stra_save_user_data("r43_key", "r43_value");
	EXPECT_EQ(std::string("r43_value"), mocker.stra_load_user_data("r43_key"));

	mocker.handle_schedule(20200106u, 930u);
	mocker.handle_replay_done();
	const std::string ud = r43_read_file(_out / "r43_sel_query" / "ud_r43_sel_query.json");
	EXPECT_NE(std::string::npos, ud.find("\"r43_key\""));
	EXPECT_NE(std::string::npos, ud.find("r43_value"));
}

// 日志族不得改状态；handle_tick/proc_tick 在无信号时也是空操作
TEST_F(R43SelMockerDumpTest, loggingAndTickWithoutSignalsKeepState)
{
	HisDataReplayer replayer;
	SelMocker mocker(&replayer, "r43_sel_log");
	const char* code = "SSE.STK.000001";

	mocker.stra_log_info("r43 info");
	mocker.stra_log_debug("r43 debug");
	mocker.stra_log_warn("r43 warn");
	mocker.stra_log_error("r43 error");

	WTSTickData* t = r43_tick("SSE", "000001", 20200106u, 93100000u, 9.9, 300);
	mocker.handle_tick(code, t, 0);
	t->release();

	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit(code));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_fund_data(2));

	// 空K线指针直接早退（L382-385）
	mocker.handle_bar_close(code, "m", 1, nullptr);
	mocker.handle_schedule(20200106u, 931u);
	mocker.handle_replay_done();
	EXPECT_TRUE(std::filesystem::exists(_out / "r43_sel_log" / "funds.csv"));
}

TEST_F(R43SelMockerDumpTest, replayDoneWithoutScheduleWritesArtifacts)
{
	HisDataReplayer replayer;
	R43CompletionTrackingSelMocker mocker(&replayer, "r43_sel_div0");

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "csv");
	cfg->append("path", _out.string().c_str());
	cfg->append("stime", static_cast<uint64_t>(202001020930));
	cfg->append("etime", static_cast<uint64_t>(202001021500));
	cfg->append("tick", false);
	cfg->append("adjust_flag", static_cast<uint32_t>(0));
	cfg->append("align_by_section", false);
	cfg->append("dont_simtick_if_notrade", false);
	cfg->append("cache_clear_days", static_cast<uint32_t>(10));
	cfg->append("fees", "");

	const std::filesystem::path root = r43_repo_root();
	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
	basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();

	const bool initialized = replayer.init(cfg);
	cfg->release();
	ASSERT_TRUE(initialized);
	replayer.enable_tick(false);
	replayer.register_sink(&mocker, "r43_sel_div0");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(0u, mocker.replayDoneCalls);
	replayer.run(false);
	EXPECT_EQ(1u, mocker.replayDoneCalls);

	const std::filesystem::path dir = _out / "r43_sel_div0";
	EXPECT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	EXPECT_TRUE(std::filesystem::exists(dir / "ud_r43_sel_div0.json"));
}
