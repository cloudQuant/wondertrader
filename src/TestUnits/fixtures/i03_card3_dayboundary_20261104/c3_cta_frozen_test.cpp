// 迭代03 卡3 3d=C（BUG-Q02-003）：CtaMocker 保存 frozen，由 session-begin 按快照 fund.tdate 条件清零。
//  - 同日续跑（session-begin 交易日 == 快照 tdate）：冻结量保留，可用量 = 总量 - 冻结量
//  - 跨日续跑（交易日 != 快照 tdate）：冻结量清零
//  - 非零 frozen 而无 fund.tdate：无法判定归属交易日，整份快照拒绝（不部分恢复）
//  - frozen 超出 [0, max(volume,0)]：拒绝
//  - 旧快照无 frozen 字段：冻结量 0（与旧行为一致）
// 夹具同 q02_cta_position_query_matrix_test：真实 HisDataReplayer（csv 日线），dist/common 只读。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include <rapidjson/document.h>

#include <WtBtCore/HisDataReplayer.h>
#define private public
#include <WtBtCore/CtaMocker.h>
#undef private
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>

namespace
{
	const char* const kCode = "SSE.STK.000001";

	std::filesystem::path repo_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("c3_frozen_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

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
			out << dates[i] << ",150000," << (9.5 + i) << "," << (11.0 + i) << "," << (9.4 + i) << ","
				<< (10.5 + i) << "," << (1000 + i) << "," << (1000 + i) * 10.0 << ",0,0,0\n";
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

	// frozenField: 例如 "\"frozen\" : 40," 或空串；fund 末尾可选 tdate
	void seed_state(const std::filesystem::path& out, const char* name, const std::string& frozenField, const std::string& fundTail)
	{
		const std::filesystem::path folder = out / name;
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		std::ofstream j(folder / (std::string(name) + ".json"), std::ios::binary);
		j << "{\"positions\":[{\"code\":\"SSE.STK.000001\",\"volume\":100,\"closeprofit\":0.0,\"dynprofit\":0.0,"
			"\"lastentertime\":20200106093100000,\"lastexittime\":0," << frozenField <<
			"\"details\":[{\"long\":true,\"price\":10.5,\"maxprice\":11.0,\"minprice\":10.0,\"volume\":100,"
			"\"opentime\":20200106093100000,\"opentdate\":20200106,\"profit\":0.0,\"maxprofit\":0.0,\"maxloss\":0.0,"
			"\"opentag\":\"entryA\"}]}],"
			"\"fund\":{\"total_profit\":0.0,\"total_dynprofit\":0.0,\"total_fees\":0.0" << fundTail << "},"
			"\"signals\":{},\"conditions\":{\"settime\":0,\"items\":{}}}";
	}

	std::string read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}
}

class C3CtaFrozen : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = repo_root();
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
		delete _mocker;
		_mocker = nullptr;
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	void make_mocker(const char* name)
	{
		_mocker = new CtaMocker(&_replayer, name);
		_replayer.register_sink(_mocker, name);
		ASSERT_TRUE(_replayer.prepare());
	}

	std::filesystem::path _root, _base, _out;
	HisDataReplayer _replayer;
	CtaMocker* _mocker = nullptr;
};

// 同日续跑：快照 tdate=20200106，session-begin(20200106) 后冻结 40 保留
TEST_F(C3CtaFrozen, SameTradingDayResumeKeepsFrozen)
{
	seed_state(_out, "c3_same", "\"frozen\":40,", ",\"tdate\":20200106");
	make_mocker("c3_same");
	_mocker->load_incremental_data("c3_same");
	ASSERT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, false));
	EXPECT_DOUBLE_EQ(60.0, _mocker->stra_get_position(kCode, true)) << "frozen not restored";
	_mocker->on_session_begin(20200106);
	EXPECT_DOUBLE_EQ(60.0, _mocker->stra_get_position(kCode, true)) << "same-day session-begin must keep frozen";
}

// 跨日续跑：session-begin(20200107) 清零
TEST_F(C3CtaFrozen, NextTradingDayResumeClearsFrozen)
{
	seed_state(_out, "c3_next", "\"frozen\":40,", ",\"tdate\":20200106");
	make_mocker("c3_next");
	_mocker->load_incremental_data("c3_next");
	_mocker->on_session_begin(20200107);
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, true));
	// 之后的同一交易日再次 session-begin 不再改变（已为 0）
	_mocker->on_session_begin(20200107);
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, true));
}

// dump 对称：保存 frozen 与 fund.tdate，第二实例按同日续跑恢复
TEST_F(C3CtaFrozen, DumpPersistsFrozenAndTradingDate)
{
	seed_state(_out, "c3_dump", "\"frozen\":25,", ",\"tdate\":20200106");
	make_mocker("c3_dump");
	_mocker->load_incremental_data("c3_dump");
	_mocker->on_session_begin(20200106);
	_mocker->dump_stradata();
	const std::string dumped = read_file(_out / "c3_dump" / "c3_dump.json");
	rapidjson::Document d;
	d.Parse(dumped.c_str());
	ASSERT_FALSE(d.HasParseError()) << dumped;
	ASSERT_TRUE(d.HasMember("positions") && d["positions"].IsArray() && d["positions"].Size() == 1u) << dumped;
	const rapidjson::Value& pos = d["positions"][0];
	ASSERT_TRUE(pos.HasMember("frozen")) << dumped;
	EXPECT_DOUBLE_EQ(25.0, pos["frozen"].GetDouble());
	ASSERT_TRUE(d["fund"].HasMember("tdate"));
	EXPECT_EQ(20200106u, d["fund"]["tdate"].GetUint());
}

// 非零 frozen 但缺 fund.tdate：拒绝整份快照（持仓不恢复）
TEST_F(C3CtaFrozen, FrozenWithoutTradingDateRejectsSnapshot)
{
	seed_state(_out, "c3_notd", "\"frozen\":40,", "");
	make_mocker("c3_notd");
	_mocker->load_incremental_data("c3_notd");
	EXPECT_DOUBLE_EQ(0.0, _mocker->stra_get_position(kCode, false));
}

// frozen 超过持仓量：拒绝
TEST_F(C3CtaFrozen, FrozenAboveVolumeRejectsSnapshot)
{
	seed_state(_out, "c3_over", "\"frozen\":140,", ",\"tdate\":20200106");
	make_mocker("c3_over");
	_mocker->load_incremental_data("c3_over");
	EXPECT_DOUBLE_EQ(0.0, _mocker->stra_get_position(kCode, false));
}

// 正控：旧快照无 frozen 字段，冻结量为 0，同日 session-begin 后仍为全部可用
TEST_F(C3CtaFrozen, LegacySnapshotWithoutFrozenHasNoFrozen)
{
	seed_state(_out, "c3_legacy", "", ",\"tdate\":20200106");
	make_mocker("c3_legacy");
	_mocker->load_incremental_data("c3_legacy");
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, true));
	_mocker->on_session_begin(20200106);
	EXPECT_DOUBLE_EQ(100.0, _mocker->stra_get_position(kCode, true));
}
