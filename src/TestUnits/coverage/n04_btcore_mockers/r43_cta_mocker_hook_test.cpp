/**
 * r43_cta_mocker_hook_test.cpp
 * Lane R43：补 CtaMocker.{h,cpp} 中 trade/dump 主路径（p03/q02 已覆盖）之外的
 * 「调度—钩子—图表—用户数据」块：
 *   on_schedule(L1096-1212)        —— 主K线标签闭合前后返回值、_emit_times 推进
 *   install_hook/enable_hook/step_calc(L1050-1094) —— 非回测态下 step_calc 自终止
 *   handle_replay_done(L661-693)   —— _has_hook && _hook_valid 的通知分支 + 三份落盘
 *   add_chart_mark / set_index_value / register_index_line /
 *   add_index_baseline(L2143-2227)
 *   stra_save_user_data/stra_load_user_data(L1922-1936) 与 ud_*.json 落地
 * 落盘目录 temp_directory_path()/r43_cta_out_<pid>，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path r43_cta_out()
	{
		auto path = std::filesystem::temp_directory_path() /
			("r43_cta_out_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	std::string r43_cta_read(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	uint32_t r43_cta_lines(const std::string& content)
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
}

class R43CtaMockerHookTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_out = r43_cta_out();
		WtHelper::setOutputDir(_out.string().c_str());

		_root = std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();

		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", _out.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (_root / "dist/common/stk_comms.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		ASSERT_TRUE(_replayer.init(cfg));
		cfg->release();
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _out;
	HisDataReplayer _replayer;
};

// 主K线标签闭合后才允许调度；on_schedule 的返回值就是"本轮是否 emit"
TEST_F(R43CtaMockerHookTest, scheduleRequiresMainBarClosed)
{
	const char* code = "SSE.STK.000001";
	CtaMocker mocker(&_replayer, "r43_cta_sched");

	// 没有 csv 数据 -> 切片为空，但主K线 key 与标签已登记
	EXPECT_EQ(nullptr, mocker.stra_get_bars(code, "m1", 10, true));

	// 标签未闭合 -> 本轮不 emit
	EXPECT_FALSE(mocker.on_schedule(20200102u, 930u));

	WTSBarStruct bar;
	bar.date = 20200102u;
	bar.time = 3001020930ull;
	bar.open = 10.0;
	bar.high = 10.5;
	bar.low = 9.8;
	bar.close = 10.2;
	mocker.handle_bar_close(code, "m", 1, &bar);

	// 标签闭合后 -> emit 一次（on_calculate 空策略 + _emit_times++）
	EXPECT_TRUE(mocker.on_schedule(20200102u, 931u));

	// 同一次闭合只消费一次
	EXPECT_FALSE(mocker.on_schedule(20200102u, 932u));

	// 空K线指针的闭盘回调直接早退（L947-948）
	mocker.handle_bar_close(code, "m", 1, nullptr);
	EXPECT_FALSE(mocker.on_schedule(20200102u, 933u));
}

// 钩子生命周期：未进入回测时 step_calc 自终止；带钩子回放结束仍要落盘
TEST_F(R43CtaMockerHookTest, hookLifecycleAndReplayDone)
{
	CtaMocker mocker(&_replayer, "r43_cta_hook");
	mocker.install_hook();
	mocker.enable_hook(false);
	// _in_backtest 为 false -> 走 else 分支置 _hook_valid=false 并返回 false
	EXPECT_FALSE(mocker.step_calc());

	// 重新打开钩子，handle_replay_done 会进入通知分支（_wait_calc 为 false，不阻塞）
	mocker.enable_hook(true);
	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r43_cta_hook";
	EXPECT_TRUE(std::filesystem::exists(dir / "r43_cta_hook.json"));
	EXPECT_TRUE(std::filesystem::exists(dir / "btchart.json"));
	EXPECT_TRUE(std::filesystem::exists(dir / "trades.csv"));
	EXPECT_TRUE(std::filesystem::exists(dir / "positions.csv"));
	EXPECT_TRUE(std::filesystem::exists(dir / "ud_r43_cta_hook.json"));

	// 无持仓/成交 -> 落盘只有表头
	EXPECT_EQ(1u, r43_cta_lines(r43_cta_read(dir / "trades.csv")));
	const std::string json = r43_cta_read(dir / "r43_cta_hook.json");
	EXPECT_NE(std::string::npos, json.find("\"positions\""));
	EXPECT_NE(std::string::npos, json.find("\"fund\""));
}

// 图表指标：注册前的写入一律失败；注册后写入基线并落盘到 btchart.json
TEST_F(R43CtaMockerHookTest, chartIndexRegistrationPersists)
{
	const char* code = "SSE.STK.000001";
	CtaMocker mocker(&_replayer, "r43_cta_chart");

	// 不在调度期：打点与指标赋值都被拒绝
	mocker.add_chart_mark(10.0, "icon", "tag");
	EXPECT_FALSE(mocker.set_index_value("r43idx", "line", 1.0));
	// 未注册的指标/线
	EXPECT_FALSE(mocker.register_index_line("r43idx", "line", 1));
	EXPECT_FALSE(mocker.add_index_baseline("r43idx", "base", 0.5));

	mocker.register_index("r43idx", 1);
	EXPECT_TRUE(mocker.register_index_line("r43idx", "line", 1));
	EXPECT_TRUE(mocker.add_index_baseline("r43idx", "base", 0.5));
	mocker.set_chart_kline(code, "d1");

	mocker.handle_replay_done();

	const std::filesystem::path dir = _out / "r43_cta_chart";
	const std::string chart = r43_cta_read(dir / "btchart.json");
	ASSERT_FALSE(chart.empty());
	EXPECT_NE(std::string::npos, chart.find("r43idx"));
	EXPECT_NE(std::string::npos, chart.find("base"));
	// set_chart_kline 生效：kline 段写的是 d1 而不是主K线
	EXPECT_NE(std::string::npos, chart.find("\"d1\""));
	// 调度期之外没有 mark/index 值 -> 两份 csv 只有表头
	EXPECT_EQ(1u, r43_cta_lines(r43_cta_read(dir / "marks.csv")));
	EXPECT_EQ(1u, r43_cta_lines(r43_cta_read(dir / "indice.csv")));
}

// 用户数据写后读一致，并在回放结束时落成 ud_<name>.json
TEST_F(R43CtaMockerHookTest, userDataRoundTripsIntoDump)
{
	CtaMocker mocker(&_replayer, "r43_cta_user");

	EXPECT_EQ(std::string("def"), mocker.stra_load_user_data("r43_missing", "def"));
	mocker.stra_save_user_data("r43_key", "r43_value");
	EXPECT_EQ(std::string("r43_value"), mocker.stra_load_user_data("r43_key", "def"));

	// 无成交时资金口径全部为 0
	for (int flag = 0; flag <= 3; flag++)
		EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_fund_data(flag));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_fund_data(9));

	mocker.handle_replay_done();
	const std::string ud = r43_cta_read(_out / "r43_cta_user" / "ud_r43_cta_user.json");
	ASSERT_FALSE(ud.empty());
	EXPECT_NE(std::string::npos, ud.find("\"r43_key\""));
	EXPECT_NE(std::string::npos, ud.find("r43_value"));
}
