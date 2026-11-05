// Actual TraderAdapter -> HftStraContext -> HftStraBaseCtx with an in-process
// ITraderApi. These red controls document unresolved behavior, not a fix.
#include "../../coverage/n01_wtcore_tradepaths/r71_hft_ctx_test.cpp"

namespace {

std::string tradeLog(const char* name)
{
    return r71::read_file(std::string(WtHelper::getOutputDir()) + name + "/trades.csv");
}

void pushFeeProbe(TraderAdapter& trader, WTSContractInfo* contract, const char* tradeId,
                  WTSDirectionType direction, WTSOffsetType offset,
                  const std::string& tag = "")
{
    WTSTradeInfo* trade = r71MakeHftTradeReport(contract, tradeId, direction, offset, 3500.0, 1.0);
    ASSERT_NE(nullptr, trade);
    trade->setUserTag(tag.c_str());
    trader.onPushTrade(trade);
    trade->release();
}

} // namespace

TEST_F(R71HftTest, FeeProbeActualFillMustUsePhysicalContractRate)
{
    auto ctx = makeCtx("fee_physical");
    R71HftSinkRegistration registration(trader, ctx.get());
    auto* contract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(nullptr, contract);
    contract->getCommInfo()->setFeeRates(1, 3, 9, true);
    contract->setFeeRates(7, 8, 10, true);
    pushFeeProbe(trader, contract, "actual-contract", WDT_LONG, WOT_OPEN);
    ASSERT_TRUE(registration.remove());
    ctx.reset();
    const std::string log = tradeLog("fee_physical");
    ASSERT_NE(std::string::npos, log.find("SHFE.ag.2512")); // fill was kept
    EXPECT_NE(std::string::npos, log.find(",7.00\n")) << log; // current code writes 1.00
}

TEST_F(R71HftTest, FeeProbeCloseMustUseProviderOffset)
{
    auto ctx = makeCtx("fee_offset");
    R71HftSinkRegistration registration(trader, ctx.get());
    auto* contract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(nullptr, contract);
    contract->getCommInfo()->setFeeRates(1, 3, 9, true);
    pushFeeProbe(trader, contract, "open", WDT_LONG, WOT_OPEN);
    pushFeeProbe(trader, contract, "close", WDT_LONG, WOT_CLOSE);
    ASSERT_TRUE(registration.remove());
    ctx.reset();
    const std::string log = tradeLog("fee_offset");
    ASSERT_NE(std::string::npos, log.find(",CLOSE,"));
    EXPECT_NE(std::string::npos, log.find(",3.00\n")) << log; // current code infers close-today (9.00)
}

TEST_F(R71HftTest, FeeProbeMissingMustRemainObservableAfterFill)
{
    auto* contract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(nullptr, contract);
    auto missing = makeCtx("fee_missing");
    R71HftSinkRegistration missingRegistration(trader, missing.get());
    pushFeeProbe(trader, contract, "fee-missing", WDT_LONG, WOT_OPEN);
    EXPECT_DOUBLE_EQ(1.0, trader.getPosition("SHFE.ag.2512", false));
    ASSERT_TRUE(missingRegistration.remove());
    missing.reset();
    const std::string missingLog = tradeLog("fee_missing");
    ASSERT_NE(std::string::npos, missingLog.find("SHFE.ag.2512")); // fill is recorded
    ASSERT_NE(std::string::npos, missingLog.find(",0.00\n"));

    contract->getCommInfo()->setFeeRates(0, 0, 0, true);
    auto zero = makeCtx("fee_zero");
    R71HftSinkRegistration zeroRegistration(trader, zero.get());
    pushFeeProbe(trader, contract, "fee-zero", WDT_LONG, WOT_OPEN);
    ASSERT_TRUE(zeroRegistration.remove());
    zero.reset();
    const std::string zeroLog = tradeLog("fee_zero");
    ASSERT_NE(std::string::npos, zeroLog.find(",0.00\n"));
    EXPECT_NE(missingLog, zeroLog) << "missing and explicit zero are indistinguishable in the recorded trade";
}

TEST_F(R71HftTest, FeeProbeFillMustReachOnlyItsOwner)
{
    auto owner = makeCtx("fee_owner");
    auto other = makeCtx("fee_other");
    R71HftSinkRegistration ownerRegistration(trader, owner.get());
    R71HftSinkRegistration otherRegistration(trader, other.get());
    const OrderIDs ids = owner->stra_buy("SHFE.ag.2512", 3500.0, 1.0, "owner-order");
    ASSERT_EQ(1u, ids.size());
    auto* contract = traderEnv.bd.getContract("ag2512", "SHFE");
    ASSERT_NE(nullptr, contract);
    const std::string tag = std::string("otp.r71_hft.") + std::to_string(ids.front());
    pushFeeProbe(trader, contract, "owner-trade", WDT_LONG, WOT_OPEN, tag);
    ASSERT_TRUE(ownerRegistration.remove());
    ASSERT_TRUE(otherRegistration.remove());
    owner.reset();
    other.reset();
    ASSERT_NE(std::string::npos, tradeLog("fee_owner").find("SHFE.ag.2512"));
    EXPECT_EQ(std::string::npos, tradeLog("fee_other").find("SHFE.ag.2512"));
}
