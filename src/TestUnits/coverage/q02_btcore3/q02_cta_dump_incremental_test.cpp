/**
 * q02_cta_dump_incremental_test.cpp
 *
 * 靶子：CtaMocker 的「结果落地 + 增量回测装载 + 落地后查询矩阵」，
 * p03 只做交易主路径（append_signal/proc_tick/do_set_position），p08 只做 Uft/Hft，
 * CtaMocker.cpp L88-L395（dump_stradata/dump_chartdata/dump_outputs）与
 * L454-L622（load_incremental_data）此前 0 覆盖。
 *
 * 驱动方式（全白盒-free，只用公开接口）：
 *   handle_session_end -> on_session_end   （写 _pos_logs/_fund_logs）
 *   handle_replay_done -> dump_outputs + dump_stradata + dump_chartdata
 *   load_incremental_data（public）
 *   stra_get_position / avgpx / profit / first_entertime / detail_* / enum_position
 * _strategy 默认 NULL 且 on_init/on_session_begin/on_session_end/on_bactest_end 都有判空，
 * _has_hook 默认 false，所以 handle_replay_done 不会进入 _cond_calc 阻塞分支。
 * _replayer 传 NULL 只在「不触碰行情」的落地链路里安全；所有需要 _replayer 的入口
 * 统一收进末尾的 DISABLED_NullReplayerDerefFamily，启用用例零残留。
 *
 * 落盘：temp_directory_path()/q02_cta_out_<pid>，SetUp/TearDown 双向 remove_all，
 * 仓库 dist/ 未读未写。
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

#include <rapidjson/document.h>

#include <WtBtCore/CtaMocker.h>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>

namespace
{
	std::filesystem::path make_out_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("q02_cta_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	void write_file(const std::filesystem::path& p, const std::string& content)
	{
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		std::ofstream out(p, std::ios::binary);
		out << content;
		out.close();
	}

	std::string read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	// rapidjson PrettyWriter 输出 "key" : value，去掉空白后可以做精确子串断言
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

	const char* const kBaseName = "q02_base";
	const char* const kStraName = "q02_cta";

	/*
	 * 上一次回测的产物（增量回测的输入）。
	 * 字段完全按 dump_stradata(L90-L226) 的写出格式对齐，
	 * 这样 load_incremental_data(L454-L622) 的每一个 GetXxx 都有对应成员。
	 */
	const char* const kBaseJson = R"JSON({
	"positions" : [
		{
			"code" : "SSE.STK.600000",
			"volume" : 100,
			"closeprofit" : 12.5,
			"dynprofit" : 3.25,
			"lastentertime" : 20200103145500000,
			"lastexittime" : 20200106093100000,
			"details" : [
				{
					"long" : true,
					"price" : 10.5,
					"maxprice" : 11.0,
					"minprice" : 10.0,
					"volume" : 60,
					"opentime" : 20200102093100000,
					"opentdate" : 20200102,
					"profit" : 1.5,
					"maxprofit" : 3.0,
					"maxloss" : -0.5,
					"opentag" : "entryA"
				},
				{
					"long" : true,
					"price" : 11.0,
					"maxprice" : 11.5,
					"minprice" : 10.8,
					"volume" : 40,
					"opentime" : 20200103145500000,
					"opentdate" : 20200103,
					"profit" : 0.5,
					"maxprofit" : 2.0,
					"maxloss" : -1.0,
					"opentag" : "entryB"
				}
			]
		},
		{
			"code" : "SZSE.STK.000001",
			"volume" : -50,
			"closeprofit" : -6.0,
			"dynprofit" : 0.0,
			"lastentertime" : 0,
			"lastexittime" : 0,
			"details" : []
		}
	],
	"fund" : {
		"total_profit" : 100.0,
		"total_dynprofit" : 20.0,
		"total_fees" : 5.0,
		"tdate" : 20200106
	},
	"signals" : {
		"SSE.STK.600001" : {
			"usertag" : "sigA",
			"volume" : 30,
			"sigprice" : 9.8,
			"gentime" : 20200106145500000
		}
	},
	"conditions" : {
		"settime" : 202001061455,
		"items" : {
			"SSE.STK.600002" : [
				{
					"usertag" : "cond1",
					"field" : 0,
					"alg" : 3,
					"target" : 12.34,
					"qty" : 20,
					"action" : 1
				}
			]
		}
	}
})JSON";

	/*
	 * 五个 csv 的「已加载部分」内容；dump 时 header + 这些行 + 本次新增行拼接。
	 * 顺序由插入顺序决定（各文件内部），逐行了。
	 */
	const char* const kBaseTrades =
		"code,time,direct,action,price,qty,tag,fee,barno\n"
		"SSE.STK.600000,20200102093100000,0,0,10.5,60,entryA,1.2,0\n"
		"SSE.STK.600000,20200103145500000,0,0,11.0,40,entryB,0.8,0\n";

	const char* const kBaseCloses =
		"code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag,openbarno,closebarno\n"
		"SZSE.STK.000001,1,20200102093100000,9.0,20200103093100000,9.2,10,2.0,2.5,-0.3,98.0,shortA,shortA,0,0\n";

	const char* const kBaseFunds =
		"date,closeprofit,positionprofit,dynbalance,fee\n"
		"20200103,50.00,0.00,50.00,2.00\n";

	const char* const kBasePositions =
		"date,code,volume,closeprofit,dynprofit\n"
		"20200103,SSE.STK.600000,60,5.00,1.00\n";

	const char* const kBaseSignals =
		"code,target,sigprice,gentime,usertag\n"
		"SSE.STK.600001,30,9.8,20200103145500000,sigOld\n";

	void seed_incremental_base(const std::filesystem::path& out)
	{
		const std::filesystem::path folder = out / kBaseName;
		write_file(folder / "trades.csv", kBaseTrades);
		write_file(folder / "closes.csv", kBaseCloses);
		write_file(folder / "funds.csv", kBaseFunds);
		write_file(folder / "positions.csv", kBasePositions);
		write_file(folder / "signals.csv", kBaseSignals);
		write_file(folder / fmtutil::format("{}.json", kBaseName), kBaseJson);
	}
}

class Q02CtaDumpTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_out = make_out_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
		seed_incremental_base(_out);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_out, ec);
		// 注意：不能在 TearDown 里 setOutputDir(nullptr)——
		// WtHelper::setOutputDir 内部是 std::string(out_dir)，传空指针会直接 UB。
		// 每个用例的 SetUp 都会重设自己的输出目录，无需回退。
	}

	std::filesystem::path _out;
};

/*
 * load_incremental_data 的五个 csv 分支 + json 全量恢复，
 * 恢复结果用公开查询接口做确定值断言（不依赖私有成员）。
 */
TEST_F(Q02CtaDumpTest, incrementalLoadRestoresPositionQueries)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.load_incremental_data(kBaseName);

	// positions[0]：volume 100，dynprofit 3.25
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position("SSE.STK.600000", true));
	EXPECT_DOUBLE_EQ(3.25, mocker.stra_get_position_profit("SSE.STK.600000"));
	// 加权均价 = (10.5*60 + 11.0*40) / 100 = 1070/100
	EXPECT_DOUBLE_EQ(10.7, mocker.stra_get_position_avgpx("SSE.STK.600000"));
	// 首笔明细的开仓时间来自 details[0]
	EXPECT_EQ(20200102093100000ull, mocker.stra_get_first_entertime("SSE.STK.600000"));
	// 明细按 usertag 命中
	EXPECT_EQ(20200102093100000ull, mocker.stra_get_detail_entertime("SSE.STK.600000", "entryA"));
	EXPECT_EQ(20200103145500000ull, mocker.stra_get_detail_entertime("SSE.STK.600000", "entryB"));
	EXPECT_DOUBLE_EQ(10.5, mocker.stra_get_detail_cost("SSE.STK.600000", "entryA"));
	EXPECT_DOUBLE_EQ(11.0, mocker.stra_get_detail_cost("SSE.STK.600000", "entryB"));

	// positions[1]：空头、无明细
	EXPECT_DOUBLE_EQ(-50.0, mocker.stra_get_position("SZSE.STK.000001"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx("SZSE.STK.000001"));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime("SZSE.STK.000001"));

	// signals：只有信号没有持仓的合约，stra_get_position 回落取信号量
	EXPECT_DOUBLE_EQ(30.0, mocker.stra_get_position("SSE.STK.600001"));

	// 没有恢复的合约 -> 0
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("SSE.STK.999999"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_profit("SSE.STK.999999"));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime("SSE.STK.999999"));
}

/*
 * enum_position(L1241)：_pos_map 先入表、_sig_map 后覆盖，
 * 结果经 std::map 归一化后逐键断言（容器本身是哈希表，顺序不确定）。
 */
TEST_F(Q02CtaDumpTest, enumPositionMergesSignalsOverPositions)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.load_incremental_data(kBaseName);

	std::map<std::string, double> got;
	mocker.enum_position([&got](const char* stdCode, double qty) {
		got[stdCode] = qty;
	}, false);

	std::map<std::string, double> expect = {
		{"SSE.STK.600000", 100.0},	// 持仓
		{"SZSE.STK.000001", -50.0},	// 持仓
		{"SSE.STK.600001", 30.0}	// 仅信号
	};
	EXPECT_EQ(expect, got);
}

/*
 * on_session_end(L1264) 写 _pos_logs/_fund_logs，
 * 与 load_incremental_data 读进来的历史行一起被 dump_outputs(L341) 落地。
 * 这里对 funds.csv / positions.csv / trades.csv / closes.csv / signals.csv
 * 做整文件精确比对（含表头、历史行、本次新增行）。
 */
TEST_F(Q02CtaDumpTest, sessionEndFeedsDumpedCsvArtifacts)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.load_incremental_data(kBaseName);

	// 资金已由 json 恢复为 100 / 20 / 5；交易日 20200106 收尾
	mocker.handle_session_end(20200106u);
	mocker.handle_replay_done();

	const std::filesystem::path folder = _out / kStraName;
	const std::string funds = read_file(folder / "funds.csv");
	EXPECT_EQ(std::string("date,closeprofit,positionprofit,dynbalance,fee\n")
		+ "20200103,50.00,0.00,50.00,2.00\n"
		+ "20200106,100.00,20.00,115.00,5.00\n", funds);

	// volume==0 的持仓被 continue 跳过（本例两笔都非零）
	const std::string positions = read_file(folder / "positions.csv");
	EXPECT_EQ(std::string("date,code,volume,closeprofit,dynprofit\n")
		+ "20200103,SSE.STK.600000,60,5.00,1.00\n"
		+ "20200106,SSE.STK.600000,100,12.50,3.25\n", positions);
	// 空头那行也必须在（_volume=-50 非零）
	EXPECT_NE(std::string::npos, positions.find("20200106,SZSE.STK.000001,-50,-6.00,0.00\n"));

	EXPECT_EQ(std::string("code,time,direct,action,price,qty,tag,fee,barno\n")
		+ "SSE.STK.600000,20200102093100000,0,0,10.5,60,entryA,1.2,0\n"
		+ "SSE.STK.600000,20200103145500000,0,0,11.0,40,entryB,0.8,0\n",
		read_file(folder / "trades.csv"));

	EXPECT_EQ(std::string("code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag,openbarno,closebarno\n")
		+ "SZSE.STK.000001,1,20200102093100000,9.0,20200103093100000,9.2,10,2.0,2.5,-0.3,98.0,shortA,shortA,0,0\n",
		read_file(folder / "closes.csv"));

	EXPECT_EQ(std::string("code,target,sigprice,gentime,usertag\n")
		+ "SSE.STK.600001,30,9.8,20200103145500000,sigOld\n",
		read_file(folder / "signals.csv"));
}

/*
 * dump_stradata(L88)：持仓/资金/信号/条件单四段 JSON 必须能再被
 * load_incremental_data 读回来，形成闭环（用第二个实例做二次装载）。
 */
TEST_F(Q02CtaDumpTest, strategyDumpRoundTripsIntoSecondMocker)
{
	{
		CtaMocker mocker(NULL, kStraName);
		mocker.load_incremental_data(kBaseName);
		mocker.handle_replay_done();
	}

	const std::filesystem::path dumpFile = _out / kStraName / fmtutil::format("{}.json", kStraName);
	ASSERT_TRUE(std::filesystem::exists(dumpFile));

	const std::string rawJson = read_file(dumpFile);
	const std::string json = squeeze(rawJson);
	// fund 段三个可恢复字段
	EXPECT_NE(std::string::npos, json.find("\"total_profit\":100"));
	EXPECT_NE(std::string::npos, json.find("\"total_dynprofit\":20"));
	EXPECT_NE(std::string::npos, json.find("\"total_fees\":5"));
	// 持仓/明细/信号/条件单四段
	EXPECT_NE(std::string::npos, json.find("\"positions\":["));
	EXPECT_NE(std::string::npos, json.find("\"opentag\":\"entryA\""));
	EXPECT_NE(std::string::npos, json.find("\"long\":true"));
	EXPECT_NE(std::string::npos, json.find("\"lastexittime\":20200106093100000"));
	EXPECT_NE(std::string::npos, json.find("\"signals\":{\"SSE.STK.600001\""));
	EXPECT_NE(std::string::npos, json.find("\"usertag\":\"sigA\""));
	rapidjson::Document dump;
	dump.Parse(rawJson.c_str());
	ASSERT_FALSE(dump.HasParseError());
	ASSERT_TRUE(dump.IsObject());
	auto conditions = dump.FindMember("conditions");
	ASSERT_NE(dump.MemberEnd(), conditions);
	ASSERT_TRUE(conditions->value.IsObject());
	auto items = conditions->value.FindMember("items");
	ASSERT_NE(conditions->value.MemberEnd(), items);
	ASSERT_TRUE(items->value.IsObject());
	auto codeItems = items->value.FindMember("SSE.STK.600002");
	ASSERT_NE(items->value.MemberEnd(), codeItems);
	ASSERT_TRUE(codeItems->value.IsArray());
	ASSERT_GE(codeItems->value.Size(), 1u);
	const rapidjson::Value& condition = codeItems->value[0];
	ASSERT_TRUE(condition.IsObject());
	ASSERT_TRUE(conditions->value.HasMember("settime"));
	auto usertag = condition.FindMember("usertag");
	ASSERT_NE(condition.MemberEnd(), usertag);
	ASSERT_TRUE(usertag->value.IsString());
	EXPECT_STREQ("cond1", usertag->value.GetString());
	EXPECT_NE(std::string::npos, json.find("\"target\":12.34"));

	CtaMocker second(NULL, kStraName);
	second.load_incremental_data(kStraName);
	// 二次装载后查询口径必须与第一次一致
	EXPECT_DOUBLE_EQ(100.0, second.stra_get_position("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(10.7, second.stra_get_position_avgpx("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(3.25, second.stra_get_position_profit("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(30.0, second.stra_get_position("SSE.STK.600001"));
	EXPECT_DOUBLE_EQ(-50.0, second.stra_get_position("SZSE.STK.000001"));
	// 明细数也原样恢复（2 条 + 0 条）
	EXPECT_EQ(20200102093100000ull, second.stra_get_first_entertime("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(11.0, second.stra_get_detail_cost("SSE.STK.600000", "entryB"));
}

/*
 * dump_chartdata(L228)：未设置主K线时用 _main_code/_chart_code 的空串落地，
 * 指标为空时不写 index 段；btchart.json + indice.csv + marks.csv 三个产物。
 */
TEST_F(Q02CtaDumpTest, chartDumpWritesEmptyKlineAndLogHeaders)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.handle_replay_done();

	const std::filesystem::path folder = _out / kStraName;
	const std::string chart = squeeze(read_file(folder / "btchart.json"));
	EXPECT_EQ(std::string("{\"kline\":{\"code\":\"\",\"period\":\"\"}}"), chart);

	EXPECT_EQ("bartime,index_name,line_name,value\n", read_file(folder / "indice.csv"));
	EXPECT_EQ("bartime,price,icon,tag\n", read_file(folder / "marks.csv"));
}

/*
 * _persist_data=false 时 dump_outputs(L343) 直接 return，
 * dump_stradata/dump_chartdata 的写盘分支同样被 _persist_data 守住 -> 目录不产生。
 */
TEST_F(Q02CtaDumpTest, persistDisabledProducesNoFolder)
{
	CtaMocker mocker(NULL, "q02_cta_np", 0, false);
	mocker.load_incremental_data(kBaseName);
	mocker.handle_session_end(20200106u);
	mocker.handle_replay_done();

	std::error_code ec;
	EXPECT_FALSE(std::filesystem::exists(_out / "q02_cta_np", ec));
	// 数据仍然装载进内存了，与落地开关无关
	EXPECT_DOUBLE_EQ(100.0, mocker.stra_get_position("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(10.7, mocker.stra_get_position_avgpx("SSE.STK.600000"));
}

/*
 * 缺失目录 / 只有部分文件时的分支：
 * csv 五段全部走 fs::exists 假分支，json 段走 else 的 warn 分支，
 * 不应有任何状态变化，也不应抛异常。
 */
TEST_F(Q02CtaDumpTest, missingBaseFolderLeavesStateEmpty)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.load_incremental_data("q02_not_exist");

	std::map<std::string, double> got;
	mocker.enum_position([&got](const char* stdCode, double qty) {
		got[stdCode] = qty;
	}, false);
	EXPECT_TRUE(got.empty());

	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx("SSE.STK.600000"));

	mocker.handle_replay_done();
	const std::string funds = read_file(_out / kStraName / "funds.csv");
	EXPECT_EQ("date,closeprofit,positionprofit,dynbalance,fee\n", funds);
}

/*
 * 只有 json、没有 csv 时：csv 五段跳过、json 四段照常恢复。
 * 顺带覆盖 load_incremental_data 的 details 缺失分支（SZSE.STK.000001 details:[] 已在用例1，
 * 这里用不带 details 键的报文覆盖 HasMember("details")==false）。
 */
TEST_F(Q02CtaDumpTest, jsonWithoutDetailsKeyStillLoads)
{
	const std::filesystem::path folder = _out / "q02_nodetail";
	write_file(folder / "q02_nodetail.json",
		"{\"positions\":[{\"code\":\"SSE.STK.600000\",\"volume\":20,"
		"\"closeprofit\":1.5,\"dynprofit\":0.75,"
		"\"lastentertime\":20200102093100000,\"lastexittime\":0}],"
		"\"fund\":{\"total_profit\":2.25,\"total_dynprofit\":0.75,\"total_fees\":0.5,\"tdate\":20200102}}");

	CtaMocker mocker(NULL, kStraName);
	mocker.load_incremental_data("q02_nodetail");

	EXPECT_DOUBLE_EQ(20.0, mocker.stra_get_position("SSE.STK.600000"));
	// 没有 details -> 均价 0（_volume 非零但明细为空）
	EXPECT_DOUBLE_EQ(0.0, mocker.stra_get_position_avgpx("SSE.STK.600000"));
	EXPECT_EQ(0ull, mocker.stra_get_first_entertime("SSE.STK.600000"));
	EXPECT_DOUBLE_EQ(0.75, mocker.stra_get_position_profit("SSE.STK.600000"));
}

/*
 * 需要 _replayer 的入口整族收在这里（本用例禁用，启用集合零残留）：
 *   stra_enter_long/stra_enter_short/stra_set_position(L1305+) 第一行就是
 *     _replayer->get_commodity_info(stdCode)
 *   append_signal(L1590) 里 _replayer->get_date()/get_raw_time()/get_secs()
 *   stra_get_bars/stra_get_ticks/stra_get_last_tick/stra_sub_ticks/stra_get_price
 *   on_bar/on_tick/handle_tick/on_tick_updated -> _replayer 或策略指针
 *   on_calculate / step_calc / install_hook / enable_hook(true) -> 条件变量与线程
 * 以上在 _replayer==NULL 时是空指针解引用（段错误），带 hook 时会阻塞，
 * 因此统一禁用；驱动它们需要真实 HisDataReplayer + 商品底座，属 p03/n04 的夹具范围。
 */
TEST_F(Q02CtaDumpTest, DISABLED_NullReplayerDerefAndBlockingHookFamily)
{
	CtaMocker mocker(NULL, kStraName);
	mocker.stra_enter_long("SSE.STK.600000", 10, "tag", 0.0, 0.0);
	mocker.stra_enter_short("SSE.STK.600000", 10, "tag", 0.0, 0.0);
	mocker.stra_set_position("SSE.STK.600000", 10, "tag", 0.0, 0.0);
	mocker.stra_get_price("SSE.STK.600000");
	mocker.stra_get_bars("SSE.STK.600000", "d", 10);
	mocker.stra_get_ticks("SSE.STK.600000", 10);
	mocker.stra_get_last_tick("SSE.STK.600000");
	mocker.stra_sub_ticks("SSE.STK.600000");
	mocker.install_hook();
	mocker.enable_hook(true);
	mocker.step_calc();
}
