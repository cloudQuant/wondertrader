/**
 * I03-NEW-008 (card 13d): live fee consumers use the versioned fee schedule.
 *
 * Contract: contract-level rate first, product-level fallback; a legal zero
 * rate is valid; a missing fee is an observable error; a continuous code is
 * priced with its actual mapped contract; a fill is never dropped because its
 * fee is unavailable. Uses the R41 lane fakes (SHFE.rb / contract rb2310) and
 * writes only into the lane's temporary generate directory.
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/WtCtaEngine.h"

#include <string>

namespace {

using namespace r41;

// The lane's generate directory is set by the R41GenerateDirEnv registered in
// r41_engine_base_test.cpp (same executable).

const char* const kCode = "SHFE.rb.2310";

class S5LiveFeeSchedule : public R41EngineFixture
{
protected:
	std::string writeFeeFile(const char* name, const std::string& content)
	{
		_tmp.write(name, content);
		return _tmp.path(name);
	}

	void initEngine(WtCtaEngine& engine, const std::string& feePath)
	{
		WTSVariant* cfg = makeCfg(feePath);
		ASSERT_NE(nullptr, cfg);
		engine.set_adapter_mgr(&_adapter_mgr);
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		engine.set_date_time(20231010, 931, 0);
		engine.set_trading_date(20231010);
	}

	static std::string rates(double open, double close, double today)
	{
		return "{\"open\": " + std::to_string(open) + ", \"close\": " + std::to_string(close) +
			", \"closetoday\": " + std::to_string(today) + ", \"byvolume\": true}";
	}
};

TEST_F(S5LiveFeeSchedule, ContractRateWinsOverProductRate)
{
	const std::string path = writeFeeFile("v2_contract.json",
		"{\"version\": 2, \"contracts\": {\"SHFE.rb2310\": " + rates(1, 1, 1) +
		"}, \"products\": {\"SHFE.rb\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);
	ASSERT_TRUE(engine.fee_config_valid());

	double fee = -1;
	std::string error;
	ASSERT_TRUE(engine.calc_fee_checked(kCode, 3500.0, 3, 0, fee, error)) << error;
	EXPECT_DOUBLE_EQ(3.0, fee);
	EXPECT_DOUBLE_EQ(3.0, engine.calc_fee(kCode, 3500.0, 3, 0));
	EXPECT_EQ(0u, engine.fee_error_count());
}

TEST_F(S5LiveFeeSchedule, ProductRateIsTheFallback)
{
	const std::string path = writeFeeFile("v2_product.json",
		"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.rb\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);
	ASSERT_TRUE(engine.fee_config_valid());

	double fee = -1;
	std::string error;
	ASSERT_TRUE(engine.calc_fee_checked(kCode, 3500.0, 3, 2, fee, error)) << error;
	EXPECT_DOUBLE_EQ(9.0, fee);
}

TEST_F(S5LiveFeeSchedule, ExplicitZeroRateIsAValidFee)
{
	const std::string path = writeFeeFile("v2_zero.json",
		"{\"version\": 2, \"contracts\": {\"SHFE.rb2310\": " + rates(0, 0, 0) +
		"}, \"products\": {\"SHFE.rb\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);

	double fee = -1;
	std::string error;
	ASSERT_TRUE(engine.calc_fee_checked(kCode, 3500.0, 3, 0, fee, error)) << error;
	EXPECT_DOUBLE_EQ(0.0, fee);
	EXPECT_EQ(0u, engine.fee_error_count());
}

TEST_F(S5LiveFeeSchedule, MissingFeeIsAnObservableError)
{
	const std::string path = writeFeeFile("v2_missing.json",
		"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.ag\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);
	ASSERT_TRUE(engine.fee_config_valid());

	double fee = -1;
	std::string error;
	EXPECT_FALSE(engine.calc_fee_checked(kCode, 3500.0, 3, 0, fee, error));
	EXPECT_NE(std::string::npos, error.find("SHFE.rb2310")) << error;

	// The legacy signature cannot fail: it returns 0 but records the error.
	EXPECT_DOUBLE_EQ(0.0, engine.calc_fee(kCode, 3500.0, 3, 0));
	EXPECT_EQ(1u, engine.fee_error_count());
	EXPECT_NE(std::string::npos, engine.last_fee_error().find("SHFE.rb2310"));
}

TEST_F(S5LiveFeeSchedule, ContinuousCodeIsPricedWithItsMappedContract)
{
	// HotMgrFake maps every custom-rule code to rb2310.
	const std::string path = writeFeeFile("v2_rule.json",
		"{\"version\": 2, \"contracts\": {\"SHFE.rb2310\": " + rates(1.5, 1, 1) +
		"}, \"products\": {\"SHFE.rb\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);

	double fee = -1;
	std::string error;
	ASSERT_TRUE(engine.calc_fee_checked("SHFE.rb.R41", 3500.0, 2, 0, fee, error)) << error;
	EXPECT_DOUBLE_EQ(3.0, fee);
}

TEST_F(S5LiveFeeSchedule, FillIsKeptWhenItsFeeIsMissing)
{
	const std::string path = writeFeeFile("v2_fill.json",
		"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.ag\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);

	WTSTickData* tick = makeTick(kCode, 3500.0);
	engine.on_tick(kCode, tick);
	engine.handle_pos_change("s5_fee", kCode, 3.0);
	tick->release();

	const std::string saved = readPortfolio("datas.json");
	EXPECT_NE(std::string::npos, saved.find("\"code\": \"SHFE.rb.2310\"")) << saved;
	EXPECT_NE(std::string::npos, saved.find("\"volume\": 3")) << saved;
	EXPECT_GE(engine.fee_error_count(), 1u);
}

// The portfolio fee of a same-direction add is charged on the opened
// increment (the quantity written to trades.csv), not on the new target.
TEST_F(S5LiveFeeSchedule, PortfolioFeeIsChargedOnTheOpenedIncrement)
{
	const std::string path = writeFeeFile("v2_incr.json",
		"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.rb\": " + rates(2, 2, 3) + "}}");
	WtCtaEngine engine;
	initEngine(engine, path);

	WTSTickData* tick = makeTick(kCode, 3500.0);
	engine.on_tick(kCode, tick);
	tick->release();
	engine.handle_pos_change("s5_fee", kCode, 3.0);
	const double afterOpen = engine.getFundInfo()->fundInfo()._fees;
	EXPECT_DOUBLE_EQ(6.0, afterOpen);
	engine.handle_pos_change("s5_fee", kCode, 2.0);
	EXPECT_DOUBLE_EQ(10.0, engine.getFundInfo()->fundInfo()._fees) << "2 more lots at 2.0 each";
	EXPECT_EQ(0u, engine.fee_error_count());
}

TEST_F(S5LiveFeeSchedule, InvalidVersionedTemplateBlocksStart)
{
	const std::string path = writeFeeFile("v2_bad.json",
		"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.rb\": {\"open\": 1}}}");
	WtCtaEngine engine;
	initEngine(engine, path);
	EXPECT_FALSE(engine.fee_config_valid());
}

TEST_F(S5LiveFeeSchedule, VersionedYamlTemplateIsAccepted)
{
	const std::string path = writeFeeFile("v2.yaml",
		"version: 2\n"
		"contracts:\n"
		"  SHFE.rb2310: {open: 1, close: 1, closetoday: 1, byvolume: true, minfee: 5}\n"
		"products:\n"
		"  SHFE.rb: {open: 2, close: 2, closetoday: 3, byvolume: true}\n");
	WtCtaEngine engine;
	initEngine(engine, path);
	ASSERT_TRUE(engine.fee_config_valid());

	double fee = -1;
	std::string error;
	ASSERT_TRUE(engine.calc_fee_checked(kCode, 3500.0, 3, 0, fee, error)) << error;
	// minfee is parsed and validated but, as before, not applied by the live
	// engine (open product decision, see decision drafts).
	EXPECT_DOUBLE_EQ(3.0, fee);
}

} // namespace
