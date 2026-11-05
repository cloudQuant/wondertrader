/**
 * s5_sel_hft_frozen_tdate_test.cpp  (BUG-Q02-003, decision card 3 sub-item 3d=C)
 *
 * Card 3d=C: frozen volume is persisted and session-begin clears it only when
 * the trading date differs from the trading date the frozen volume belongs to.
 * CtaMocker implements this in load/dump/on_session_begin (fixture
 * i03_card3_dayboundary_20261104). This file applies the same contract to the
 * other mockers where it is applicable:
 *   - SelMocker: dump_stradata persists frozen next to fund.tdate; a repeated
 *     session-begin for the same trading date keeps the frozen volume.
 *     SelMocker has no snapshot loader, so there is no resume path to test.
 *   - HftMocker: no snapshot is dumped or loaded; only the session-begin rule
 *     applies (same trading date keeps, a different one clears).
 * A frozen volume created before any session-begin has no owning trading date
 * and is cleared by the first session-begin (unchanged legacy behaviour).
 */
#include <queue>
#include <sstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include <Includes/FasterDefs.h>
#include <Includes/ISelStraCtx.h>
#include <Includes/IHftStraCtx.h>
#include <Includes/SelStrategyDefs.h>
#include <Includes/HftStrategyDefs.h>
#include <Share/StdUtils.hpp>
#include <Share/DLLHelper.hpp>
#include <Share/fmtlib.h>

#define private public
#include <WtBtCore/HftMocker.h>
#undef private
#include <WtBtCore/SelMocker.h>

namespace
{
	const char* const kStk = "SSE.STK.000001";

	// SelMocker keeps _pos_map protected; expose it to the test only.
	class SelProbe : public SelMocker
	{
	public:
		using SelMocker::SelMocker;
		void set_frozen(const char* code, double volume, double frozen)
		{
			PosInfo& pInfo = _pos_map[code];
			pInfo._volume = volume;
			pInfo._frozen = frozen;
		}
	};

	void set_hft_frozen(HftMocker& mocker, const char* code, double volume, double frozen)
	{
		auto& pInfo = mocker._pos_map[code];
		pInfo._volume = volume;
		pInfo._frozen = frozen;
	}
}

// Self-contained replayer fixture (base data only, no fee schedule: session
// begin and the snapshot dump do not fill orders).
class S5Q02FrozenTDate : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
		_base = make_dir("base");
		_out = make_dir("out");
		WtHelper::setOutputDir((_out.string() + "/").c_str());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", _base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001060930));
		cfg->append("etime", static_cast<uint64_t>(202001071500));
		cfg->append("tick", false);
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (_root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (_root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (_root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		ASSERT_TRUE(_rp.init(cfg));
		cfg->release();
	}

	void TearDown() override
	{
		WtHelper::setOutputDir("");
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	static std::filesystem::path make_dir(const char* tag)
	{
		const auto path = std::filesystem::temp_directory_path() /
			(std::string("s5_q02_frozen_") + tag + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	static std::string read_file(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	HisDataReplayer _rp;
	std::filesystem::path _root, _base, _out;
};

// SEL: frozen created on 20200106 after its session-begin is kept by another
// session-begin of 20200106 and released by the next trading date.
TEST_F(S5Q02FrozenTDate, SelSameTradingDaySessionBeginKeepsFrozen)
{
	SelProbe sel(&_rp, "s5_sel_same_day");
	sel.handle_session_begin(20200106u);
	sel.set_frozen(kStk, 100.0, 40.0);
	ASSERT_DOUBLE_EQ(60.0, sel.stra_get_position(kStk, true));

	sel.handle_session_begin(20200106u);
	EXPECT_DOUBLE_EQ(60.0, sel.stra_get_position(kStk, true)) << "same-day session-begin must keep frozen";

	sel.handle_session_begin(20200107u);
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, true)) << "next trading date must release frozen";
}

// SEL: the snapshot written at replay end carries frozen and its trading date.
TEST_F(S5Q02FrozenTDate, SelDumpPersistsFrozenAndTradingDate)
{
	SelProbe sel(&_rp, "s5_sel_dump");
	sel.handle_session_begin(20200106u);
	sel.set_frozen(kStk, 100.0, 25.0);
	sel.handle_replay_done();

	const std::string dumped = read_file(_out / "s5_sel_dump" / "s5_sel_dump.json");
	rapidjson::Document d;
	d.Parse(dumped.c_str());
	ASSERT_FALSE(d.HasParseError()) << dumped;
	ASSERT_TRUE(d.IsObject()) << dumped;
	ASSERT_TRUE(d.HasMember("positions") && d["positions"].IsArray()) << dumped;
	ASSERT_EQ(1u, d["positions"].Size()) << dumped;
	const rapidjson::Value& pos = d["positions"][0];
	ASSERT_TRUE(pos.HasMember("frozen")) << dumped;
	ASSERT_TRUE(pos["frozen"].IsNumber()) << dumped;
	EXPECT_DOUBLE_EQ(25.0, pos["frozen"].GetDouble());
	ASSERT_TRUE(d.HasMember("fund") && d["fund"].IsObject() && d["fund"].HasMember("tdate")) << dumped;
	EXPECT_EQ(20200106u, d["fund"]["tdate"].GetUint());
}

// SEL positive control: frozen without an owning session is cleared by the
// first session-begin, and a new trading date clears as before.
TEST_F(S5Q02FrozenTDate, SelFrozenWithoutOwningSessionIsClearedByFirstSessionBegin)
{
	SelProbe sel(&_rp, "s5_sel_first");
	sel.set_frozen(kStk, 100.0, 40.0);
	sel.handle_session_begin(20200106u);
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, true));

	sel.set_frozen(kStk, 100.0, 30.0);
	sel.handle_session_begin(20200107u);
	EXPECT_DOUBLE_EQ(100.0, sel.stra_get_position(kStk, true));
}

// HFT: same session-begin rule; no snapshot is involved.
TEST_F(S5Q02FrozenTDate, HftSameTradingDaySessionBeginKeepsFrozen)
{
	HftMocker hft(&_rp, "s5_hft_same_day");
	hft.handle_session_begin(20200106u);
	set_hft_frozen(hft, kStk, 100.0, 40.0);
	ASSERT_DOUBLE_EQ(60.0, hft.stra_get_position(kStk, true));

	hft.handle_session_begin(20200106u);
	EXPECT_DOUBLE_EQ(60.0, hft.stra_get_position(kStk, true)) << "same-day session-begin must keep frozen";

	hft.handle_session_begin(20200107u);
	EXPECT_DOUBLE_EQ(100.0, hft.stra_get_position(kStk, true)) << "next trading date must release frozen";
}

// HFT positive control, mirroring the SEL one.
TEST_F(S5Q02FrozenTDate, HftFrozenWithoutOwningSessionIsClearedByFirstSessionBegin)
{
	HftMocker hft(&_rp, "s5_hft_first");
	set_hft_frozen(hft, kStk, 100.0, 40.0);
	hft.handle_session_begin(20200106u);
	EXPECT_DOUBLE_EQ(100.0, hft.stra_get_position(kStk, true));

	set_hft_frozen(hft, kStk, 100.0, 30.0);
	hft.handle_session_begin(20200107u);
	EXPECT_DOUBLE_EQ(100.0, hft.stra_get_position(kStk, true));
}
