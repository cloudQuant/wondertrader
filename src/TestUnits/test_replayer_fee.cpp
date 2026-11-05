#include "gtest/gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "../WtBtCore/HisDataReplayer.h"
#include "../Includes/WTSVariant.hpp"

namespace
{
std::filesystem::path fee_fixture(const char* name)
{
	return std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
}

std::filesystem::path repository_file(const char* relative_path)
{
	return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
		relative_path;
}
}

TEST(test_replayer_fee, applies_minimum_only_to_nonzero_rate_fills)
{
	EXPECT_DOUBLE_EQ(
		5.0,
		HisDataReplayer::apply_fee_minimum(0.01, 100.0, 0.0001, 5.0));
	EXPECT_DOUBLE_EQ(
		10.0,
		HisDataReplayer::apply_fee_minimum(10.0, 100000.0, 0.0001, 5.0));
	EXPECT_DOUBLE_EQ(
		5.0,
		HisDataReplayer::apply_fee_minimum(0.5, 1.0, 0.5, 5.0));
	EXPECT_DOUBLE_EQ(
		-0.1,
		HisDataReplayer::apply_fee_minimum(-0.1, 100.0, -0.001, 5.0));
}

TEST(test_replayer_fee, zero_quantity_zero_rate_and_default_are_unchanged)
{
	EXPECT_DOUBLE_EQ(
		0.0,
		HisDataReplayer::apply_fee_minimum(0.0, 0.0, 0.0001, 5.0));
	EXPECT_DOUBLE_EQ(
		0.0,
		HisDataReplayer::apply_fee_minimum(0.0, 100.0, 0.0, 5.0));
	EXPECT_DOUBLE_EQ(
		0.01,
		HisDataReplayer::apply_fee_minimum(0.01, 100.0, 0.0001, 0.0));
}

TEST(test_replayer_fee, loads_optional_minimum_from_json_template)
{
	const auto fixture = fee_fixture("fees_minfee.json");
	ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture;

	// Iteration 03: init validates the replay time range (0..0 is rejected), and card 13d
	// prices a fill only for a physical contract present in the base data. Supply a valid
	// range and a contracts file for the codes priced below (inputs only; rates unchanged).
	const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
		("test_replayer_fee_" + std::to_string(static_cast<long>(::getpid())));
	std::filesystem::create_directories(scratch);
	const std::filesystem::path contracts = scratch / "contracts.json";
	{
		std::ofstream out(contracts, std::ios::binary | std::ios::trunc);
		out << R"({"SSE":{"000001":{"name":"000001","code":"000001","exchg":"SSE","product":"STK","opendate":19900101,"expiredate":20991231},)"
			<< R"("000300":{"name":"000300","code":"000300","exchg":"SSE","product":"IDX","opendate":19900101,"expiredate":20991231}},)"
			<< R"("SZSE":{"000001":{"name":"000001","code":"000001","exchg":"SZSE","product":"STK","opendate":19900101,"expiredate":20991231}},)"
			<< R"("DCE":{"a2405":{"name":"a2405","code":"a2405","exchg":"DCE","product":"a","opendate":19900101,"expiredate":20991231}}})";
	}

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "csv");
	cfg->append("path", "/tmp");
	cfg->append("stime", static_cast<uint64_t>(202001020930ull));
	cfg->append("etime", static_cast<uint64_t>(202001021500ull));
	cfg->append("tick", false);
	cfg->append("adjust_flag", static_cast<uint32_t>(0));
	cfg->append("align_by_section", false);
	cfg->append("dont_simtick_if_notrade", false);
	cfg->append("fees", fixture.string().c_str());

	WTSVariant* basefiles = WTSVariant::createObject();
	const auto sessions = repository_file("dist/common/sessions.json");
	basefiles->append("session", sessions.string().c_str());
	WTSVariant* commodities = WTSVariant::createArray();
	commodities->append(repository_file("dist/common/commodities.json").string().c_str());
	commodities->append(repository_file("dist/common/stk_comms.json").string().c_str());
	basefiles->append("commodity", commodities);
	commodities->release();
	basefiles->append("contract", contracts.string().c_str());
	cfg->append("basefiles", basefiles);
	basefiles->release();

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	EXPECT_DOUBLE_EQ(5.0,
		replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 0));
	EXPECT_DOUBLE_EQ(10000.0,
		replayer.calc_fee("SSE.STK.000001", 100000.0, 100.0, 0));
	EXPECT_DOUBLE_EQ(12000.0,
		replayer.calc_fee("SSE.STK.000001", 100000.0, 100.0, 1));
	EXPECT_DOUBLE_EQ(5.0,
		replayer.calc_fee("DCE.a.2405", 1.0, 1.0, 0));
	EXPECT_DOUBLE_EQ(7.0,
		replayer.calc_fee("DCE.a.2405", 1.0, 1.0, 1));
	const double negative_fee =
		replayer.calc_fee("SZSE.STK.000001", 1.0, 100.0, 0);
	// Card 13d unified fee calculation rounds signed amounts to cents symmetrically; the
	// legacy -0.09 came from (int)(fee * 100 + 0.5) truncating -9.5 toward zero.
	EXPECT_DOUBLE_EQ(-0.1, negative_fee);
	EXPECT_DOUBLE_EQ(0.1,
		replayer.calc_fee("SSE.IDX.000300", 1.0, 100.0, 0));
	EXPECT_DOUBLE_EQ(0.0,
		replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 2));
	EXPECT_DOUBLE_EQ(0.0,
		replayer.calc_fee("SSE.STK.000001", 1.0, 0.0, 0));
	std::error_code ec;
	std::filesystem::remove_all(scratch, ec);
}
