/**
 * q02_cta_position_query_matrix_test.cpp
 *
 * 靶子：CtaMocker::stra_get_position(L1999-L2043) 的 **userTag 过滤分支**
 * 与 onlyValid 分支，以及 dump_stradata/load_incremental_data 之后
 * 「已落地明细 -> 再查询」的闭环。p03 走的是手工塞 _pos_map 的成交路径，
 * p04/p07/p08 都不碰 CtaMocker 的仓位查询；本波前两篇文件覆盖了装载与落地，
 * 这一篇专门吃 stra_get_position 里 `strlen(userTag) != 0` 的那段循环。
 *
 * 夹具：真实 HisDataReplayer（csv 日线底座，同 q02_cta_replayer_integration_test），
 * 这样任何 `_replayer->` 解引用都落在合法对象上；仓位明细由
 * load_incremental_data 从自建 JSON 恢复（明细 _frozen 不被恢复，恒为 0，
 * 因此 onlyValid 分支结果可推导，不受 BUG-Q02-003 影响的口径见断言旁注释）。
 *
 * 落盘：temp_directory_path()/q02_ctapm_*，TearDown remove_all；dist/ 只读。
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
			("q02_ctapm_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	const char* const kCode = "SSE.STK.000001";
	const char* const kCode2 = "SZSE.STK.000002";

	bool write_daily_csv(const std::filesystem::path& base)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (std::string(kCode) + "_d.csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		const uint32_t dates[] = {20200102u, 20200103u, 20200106u};
		for (uint32_t i = 0; i < 3; i++)
		{
			out << dates[i] << ",150000,"
				<< (9.5 + i) << "," << (11.0 + i) << "," << (9.4 + i) << ","
				<< (10.5 + i) << "," << (1000 + i) << "," << (1000 + i) * 10.0 << ",0,0,0\n";
		}
		out.close();

		std::ofstream out2(base / "csv" / (std::string(kCode2) + "_d.csv"));
		if (!out2.is_open())
			return false;
		out2 << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (uint32_t i = 0; i < 3; i++)
		{
			out2 << dates[i] << ",150000,"
				<< (5.0 + i) << "," << (5.5 + i) << "," << (4.9 + i) << ","
				<< (5.2 + i) << "," << (2000 + i) << "," << (2000 + i) * 5.0 << ",0,0,0\n";
		}
		out2.close();
		return true;
	}

	WTSVariant* replay_cfg(const std::filesystem::path& root, const std::filesystem::path& base)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001061500));
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
	 * 两个合约、四条明细：
	 *   kCode  -> 多 60@10.5(entryA) + 多 40@11.0(entryB) = 100
	 *   kCode2 -> 空 -30@5.5(shortA) + 多 20@5.2(longC)   = -10
	 */
	void seed_state_dir(const std::filesystem::path& out)
	{
		const std::filesystem::path folder = out / "q02_state";
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		std::ofstream j(folder / "q02_state.json", std::ios::binary);
		j << R"JSON({
	"positions" : [
		{
			"code" : "SSE.STK.000001", "volume" : 100, "closeprofit" : 8.0,
			"dynprofit" : 2.5, "lastentertime" : 20200103145500000,
			"lastexittime" : 20200106093100000,
			"details" : [
				{"long":true,"price":10.5,"maxprice":11.0,"minprice":10.0,"volume":60,
				 "opentime":20200102093100000,"opentdate":20200102,"profit":1.5,
				 "maxprofit":3.0,"maxloss":-0.5,"opentag":"entryA"},
				{"long":true,"price":11.0,"maxprice":11.5,"minprice":10.8,"volume":40,
				 "opentime":20200103145500000,"opentdate":20200103,"profit":0.5,
				 "maxprofit":2.0,"maxloss":-1.0,"opentag":"entryB"}
			]
		},
		{
			"code" : "SZSE.STK.000002", "volume" : -10, "closeprofit" : -1.0,
			"dynprofit" : 0.0, "lastentertime" : 20200106093100000,
			"lastexittime" : 0,
			"details" : [
				{"long":false,"price":5.5,"maxprice":5.6,"minprice":5.2,"volume":-30,
				 "opentime":20200102093100000,"opentdate":20200102,"profit":0.6,
				 "maxprofit":1.2,"maxloss":-0.2,"opentag":"shortA"},
				{"long":true,"price":5.2,"maxprice":5.4,"minprice":5.0,"volume":20,
				 "opentime":20200106093100000,"opentdate":20200106,"profit":0.0,
				 "maxprofit":0.4,"maxloss":-0.4,"opentag":"longC"}
			]
		}
	],
	"fund" : {"total_profit" : 7.0, "total_dynprofit" : 2.5, "total_fees" : 1.5, "tdate" : 20200106},
	"signals" : {},
	"conditions" : {"settime" : 0, "items" : {}}
})JSON";
		j.close();
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
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out.push_back(c);
		return out;
	}
}

class Q02CtaPosMatrixTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		_base = make_temp_dir("base");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
		ASSERT_TRUE(write_daily_csv(_base));
		seed_state_dir(_out);

		WTSVariant* cfg = replay_cfg(_root, _base);
		ASSERT_TRUE(_replayer.init(cfg));
		cfg->release();
		ASSERT_TRUE(_replayer.prepare());

		_mocker = new CtaMocker(&_replayer, "q02_pm");
		_replayer.register_sink(_mocker, "q02_pm");
		_mocker->load_incremental_data("q02_state");
	}

	void TearDown() override
	{
		delete _mocker;
		_mocker = nullptr;
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;
	HisDataReplayer _replayer;
	CtaMocker* _mocker {nullptr};
};

/*
 * userTag 非空时走 stra_get_position(L2017+) 的明细过滤循环：
 * 只累加 _opentag 完全相等的明细量；不存在的 tag 得到 0。
 */
TEST_F(Q02CtaPosMatrixTest, userTagFiltersPositionByDetail)
{
	EXPECT_DOUBLE_EQ(60.0, _mocker->stra_get_position(kCode, false, "entryA"));
	EXPECT_DOUBLE_EQ(40.0, _mocker->stra_get_position(kCode, false, "entryB"));
	// 两个 tag 都不匹配 -> 0，而不是回落取总量
	EXPECT_DOUBLE_EQ(0.0, _mocker->stra_get_position(kCode, false, "nosuch"));
	// 空串 tag -> 全量
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, false, ""));

	// 多空混仓：按 tag 精确取符号
	EXPECT_DOUBLE_EQ(-30.0, _mocker->stra_get_position(kCode2, false, "shortA"));
	EXPECT_DOUBLE_EQ(20.0, _mocker->stra_get_position(kCode2, false, "longC"));
	EXPECT_DOUBLE_EQ(-10.0, _mocker->stra_get_position(kCode2, false, ""));
}

/*
 * 明细级查询：同一 tag 的入场时间与成本价必须成对命中；
 * 空明细合约（未装载）返回 0。
 */
TEST_F(Q02CtaPosMatrixTest, detailTimeAndCostPairUp)
{
	EXPECT_EQ(20200102093100000ull, _mocker->stra_get_detail_entertime(kCode, "entryA"));
	EXPECT_EQ(20200103145500000ull, _mocker->stra_get_detail_entertime(kCode, "entryB"));
	EXPECT_DOUBLE_EQ(10.5, _mocker->stra_get_detail_cost(kCode, "entryA"));
	EXPECT_DOUBLE_EQ(11.0, _mocker->stra_get_detail_cost(kCode, "entryB"));
	EXPECT_EQ(20200102093100000ull, _mocker->stra_get_detail_entertime(kCode2, "shortA"));
	EXPECT_DOUBLE_EQ(5.5, _mocker->stra_get_detail_cost(kCode2, "shortA"));

	// 首次/末次开仓时间：details[0] 与 _last_entertime 两个来源
	EXPECT_EQ(20200102093100000ull, _mocker->stra_get_first_entertime(kCode));
	EXPECT_EQ(20200106093100000ull, _mocker->stra_get_first_entertime(kCode2));
	EXPECT_EQ(20200103145500000ull, _mocker->stra_get_last_entertime(kCode));
	EXPECT_EQ(20200106093100000ull, _mocker->stra_get_last_exittime(kCode));
	EXPECT_EQ(0ull, _mocker->stra_get_last_exittime(kCode2));
	EXPECT_EQ(0ull, _mocker->stra_get_first_entertime("SSE.STK.999999"));

	// 加权均价：60*10.5 + 40*11.0 = 1070 / 100
	EXPECT_DOUBLE_EQ(10.7, _mocker->stra_get_position_avgpx(kCode));
	// -30*5.5 + 20*5.2 = -165 + 104 = -61 / -10
	EXPECT_DOUBLE_EQ(6.1, _mocker->stra_get_position_avgpx(kCode2));
	EXPECT_DOUBLE_EQ(2.5, _mocker->stra_get_position_profit(kCode));
	EXPECT_DOUBLE_EQ(0.0, _mocker->stra_get_position_profit(kCode2));
}

/*
 * onlyValid 分支：_frozen 不由 load_incremental_data 恢复（BUG-Q02-003），
 * 恒为 0，因此 onlyValid 与非 onlyValid 必须一致。
 * 这里把「装载后冻结量为 0」当作可推导事实固化，修复 BUG-Q02-003 后需要同步收紧。
 */
TEST_F(Q02CtaPosMatrixTest, onlyValidMatchesTotalBecauseFrozenNotPersisted)
{
	EXPECT_DOUBLE_EQ(_mocker->stra_get_position(kCode, false),
		_mocker->stra_get_position(kCode, true));
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, true));
	EXPECT_DOUBLE_EQ(-10.0, _mocker->stra_get_position(kCode2, true));
	// tag 过滤路径不受 onlyValid 影响（源码里 userTag 非空时直接按明细累加）
	EXPECT_DOUBLE_EQ(60.0, _mocker->stra_get_position(kCode, true, "entryA"));
}

/*
 * 信号表优先级的两个方向：
 *   _sig_map 命中且 _pos_map 也命中 -> 取 _pos_map（L2010-L2015 覆盖 totalPos）
 *   只有 _sig_map -> 取信号量
 * 这里在已装载持仓的合约上再下一个新信号，验证装载持仓仍然优先。
 */
TEST_F(Q02CtaPosMatrixTest, positionOverridesSignalWhenBothExist)
{
	_mocker->stra_enter_long(kCode, 500, "tagNew", 0.0, 0.0);
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode));

	// 只在下一次 tick 才会成交，但条件单形式不会进 _sig_map
	_mocker->stra_enter_long(kCode, 500, "tagCond", 0.0, 12.0);
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode));

	std::map<std::string, double> got;
	_mocker->enum_position([&got](const char* stdCode, double qty) { got[stdCode] = qty; }, false);
	ASSERT_EQ(2u, got.size());
	// stra_enter_long(L1317-L1321) 在已有净持仓时按 curQty+qty 追加目标量：
	// 100(装载持仓) + 500 = 600，enum_position 里 _sig_map 后写入 -> 显示目标量
	EXPECT_DOUBLE_EQ(600.0, got[kCode]);
	EXPECT_DOUBLE_EQ(-10.0, got[kCode2]);
}

/*
 * 落地后的仓位/资金行：装载态直接结算一次，
 * positions.csv 两行（两个合约都非零）、funds.csv 一行（装载来的资金）。
 */
TEST_F(Q02CtaPosMatrixTest, loadedStateSettlesIntoCsv)
{
	_mocker->handle_session_end(_replayer.get_trading_date());
	_mocker->handle_replay_done();

	const std::string funds = read_file(_out / "q02_pm" / "funds.csv");
	const std::string day = std::to_string(_replayer.get_trading_date());
	EXPECT_EQ(std::string("date,closeprofit,positionprofit,dynbalance,fee\n")
		+ day + ",7.00,2.50,8.00,1.50\n", funds);

	const std::string positions = read_file(_out / "q02_pm" / "positions.csv");
	EXPECT_NE(std::string::npos, positions.find(day + ",SSE.STK.000001,100,8.00,2.50\n"));
	EXPECT_NE(std::string::npos, positions.find(day + ",SZSE.STK.000002,-10,-1.00,0.00\n"));

	const std::string sdata = squeeze(read_file(_out / "q02_pm" / "q02_pm.json"));
	EXPECT_NE(std::string::npos, sdata.find("\"opentag\":\"shortA\""));
	EXPECT_NE(std::string::npos, sdata.find("\"volume\":-30"));
	EXPECT_NE(std::string::npos, sdata.find("\"total_fees\":1.5"));
	// stra_enter_long 在只读用例里没有调用 -> signals 段为空
	EXPECT_NE(std::string::npos, sdata.find("\"signals\":{}"));
}
