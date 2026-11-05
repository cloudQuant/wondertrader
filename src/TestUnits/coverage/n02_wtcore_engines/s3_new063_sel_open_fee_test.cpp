/**
 * I03-NEW-063: the live SEL strategy context charges the open fee of a
 * same-direction add on the opened increment, not on the new target.
 *
 * Contract (card 13d fee schedule, I03-NEW-008 review; same as the CTA context
 * and the SEL backtest mocker): opening 3 lots and then raising the target to
 * 5 lots opens 2 more lots, so the second open fee is 2 * rate. Before the fix
 * SelStraBaseCtx::do_set_position charged abs(target) (3 + 5 lots).
 *
 * Uses the R41 lane fakes (SHFE.rb / contract rb2310, day session 09:00-15:00,
 * versioned product rate 2.0 per lot) and the lane generate directory.
 */
#include "r41_wtcore_fixture.h"

#include "WtCore/SelStraBaseCtx.h"
#include "WtCore/WtSelEngine.h"

#include <memory>
#include <string>

namespace {

using namespace r41;

const char* const kSelFeeCode = "SHFE.rb.2310";

class S3New063SelCtx final : public SelStraBaseCtx
{
public:
	S3New063SelCtx(WtSelEngine* engine, const char* name) : SelStraBaseCtx(engine, name, 0) {}
	void on_bar_close(const char*, const char*, WTSBarStruct*) override {}
};

class S3New063SelOpenFee : public R41EngineFixture
{
protected:
	void initEngine(WtSelEngine& engine)
	{
		_tmp.write("s3_new063_fees.json",
			"{\"version\": 2, \"contracts\": {}, \"products\": {\"SHFE.rb\": "
			"{\"open\": 2.0, \"close\": 2.0, \"closetoday\": 3.0, \"byvolume\": true}}}");
		WTSVariant* cfg = makeCfg(_tmp.path("s3_new063_fees.json"));
		ASSERT_NE(nullptr, cfg);
		engine.init(cfg, _bd.get(), _dt.get(), _hot.get(), NULL);
		engine.set_date_time(20231010, 931, 0);
		engine.set_trading_date(20231010);
		ASSERT_TRUE(engine.fee_config_valid());
	}

	// stra_set_position only records the signal; the next tick in trading time
	// applies it through do_set_position at the tick price.
	void applyTarget(S3New063SelCtx& ctx, double target, const char* tag)
	{
		ctx.stra_set_position(kSelFeeCode, target, tag);
		WTSTickData* tick = makeTick(kSelFeeCode, 3500.0);
		ctx.on_tick(kSelFeeCode, tick, false);
		tick->release();
	}
};

TEST_F(S3New063SelOpenFee, SameDirectionAddIsChargedOnTheOpenedIncrement)
{
	WtSelEngine engine;
	initEngine(engine);
	std::shared_ptr<S3New063SelCtx> ctx(new S3New063SelCtx(&engine, "s3_new063_sel"));

	applyTarget(*ctx, 3.0, "open3");
	ASSERT_DOUBLE_EQ(3.0, ctx->stra_get_position(kSelFeeCode));
	EXPECT_DOUBLE_EQ(6.0, ctx->stra_get_fund_data(3)) << "3 lots opened at 2.0 each";

	applyTarget(*ctx, 5.0, "add2");
	ASSERT_DOUBLE_EQ(5.0, ctx->stra_get_position(kSelFeeCode));
	EXPECT_DOUBLE_EQ(10.0, ctx->stra_get_fund_data(3)) << "only the 2 added lots are charged";
	EXPECT_EQ(0u, engine.fee_error_count());
	engine.release();
}

} // namespace
