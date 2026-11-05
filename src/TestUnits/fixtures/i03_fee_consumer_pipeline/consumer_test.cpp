#include <gtest/gtest.h>
#include <WtBtCore/HisDataReplayer.h>
#include <Includes/WTSVariant.hpp>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

// I03-NEW-008 / card 13d=A (contract rate first, product fallback, a missing fee is an
// error, an explicit zero rate is valid, fees belong to the actual fill contract).
// The exported-*.json files are the real normal-valid callback output of the
// t26_fees_atomic CTPLoader probe (fees, commodities and contracts of one export run).
namespace {
std::string fixture(const char* name) { return std::string(std::getenv("WT_FEE_FIXTURE")) + "/" + name; }
WTSVariant* config(const std::string& file, bool commodity, bool contract = true) {
    WTSVariant* c = WTSVariant::createObject();
    c->append("mode", "csv"); c->append("path", fixture("run").c_str());
    c->append("stime", static_cast<uint64_t>(202001020930));
    c->append("etime", static_cast<uint64_t>(202001081500));
    c->append("tick", false); c->append("adjust_flag", static_cast<uint32_t>(0));
    c->append("align_by_section", false); c->append("dont_simtick_if_notrade", false);
    c->append("fees", file.c_str());
    WTSVariant* b = WTSVariant::createObject();
    b->append("session", fixture("sessions.json").c_str());
    if (commodity) b->append("commodity", fixture("commodities.json").c_str());
    if (contract) b->append("contract", fixture("contracts.json").c_str());
    c->append("basefiles", b); b->release(); return c;
}
}

// The CTPLoader export has not been migrated: it is still an unversioned object keyed
// by "exchange.contract". An unversioned file is a legacy product table, so those keys
// are not contract rules. The exported nonzero SHFE.au2512 rate is therefore not
// applied, and the gap must stay an observable fee error (NaN, failed context) instead
// of the former silent zero.
TEST(FeeConsumerPipeline, UnmigratedExportedContractKeysAreAnObservableMissingFee) {
    HisDataReplayer r; WTSVariant* c = config(fixture("fees-exported.json"), true);
    const bool ok = r.init(c); c->release(); ASSERT_TRUE(ok);
    EXPECT_TRUE(std::isnan(r.calc_fee("SHFE.au.2512", 100.0, 1.0, 0)));
    wt_fee::Context context; context.product = "unchanged"; std::string error;
    EXPECT_FALSE(r.prepare_fee_context("SHFE.au.2512", nullptr, context, error));
    EXPECT_EQ("unchanged", context.product);
    EXPECT_FALSE(error.empty());
}

// The same exported rates expressed as version-2 contract entries are applied to the
// physical contract; the exported explicit zero rate of SHFE.rb2510 is a valid fee.
TEST(FeeConsumerPipeline, VersionedContractVolumeFeeIsUsedForThePhysicalContract) {
    HisDataReplayer r; WTSVariant* c = config(fixture("fees-v2-contract-volume.json"), true);
    const bool ok = r.init(c); c->release(); ASSERT_TRUE(ok);
    wt_fee::Context context; std::string error;
    ASSERT_TRUE(r.prepare_fee_context("SHFE.au.2512", nullptr, context, error)) << error;
    EXPECT_EQ("SHFE.au2512", context.physical_contract);
    EXPECT_DOUBLE_EQ(-0.5, r.calc_fee("SHFE.au.2512", 100.0, 2.0, 0));
    EXPECT_DOUBLE_EQ(0.0, r.calc_fee("SHFE.rb.2510", 3500.0, 1.0, 0));
}

// A contract-level amount fee needs the physical contract metadata at initialization:
// without the exported contracts it is rejected, with them it uses the contract.
TEST(FeeConsumerPipeline, ContractAmountFeeRequiresPhysicalContractMetadata) {
    {
        HisDataReplayer r; WTSVariant* c = config(fixture("fees-amount-contract.json"), true, false);
        const bool ok = r.init(c); c->release(); EXPECT_FALSE(ok);
    }
    HisDataReplayer r; WTSVariant* c = config(fixture("fees-amount-contract.json"), true);
    const bool ok = r.init(c); c->release(); ASSERT_TRUE(ok);
    wt_fee::Context context; std::string error;
    ASSERT_TRUE(r.prepare_fee_context("SHFE.au.2512", nullptr, context, error)) << error;
    const double expected = 100.0 * 1.0 * context.volume_scale * 0.01;
    EXPECT_GT(expected, 0.0);
    EXPECT_DOUBLE_EQ(expected, r.calc_fee("SHFE.au.2512", 100.0, 1.0, 0));
}

// A complete legacy product-keyed amount fee keeps working through the fallback.
TEST(FeeConsumerPipeline, LegacyProductAmountFeeStillWorks) {
    HisDataReplayer r; WTSVariant* c = config(fixture("fees-amount-product.json"), true);
    const bool ok = r.init(c); c->release(); ASSERT_TRUE(ok);
    wt_fee::Context context; std::string error;
    ASSERT_TRUE(r.prepare_fee_context("SHFE.au.2512", nullptr, context, error)) << error;
    EXPECT_EQ("SHFE.au", context.product);
    const double expected = 100.0 * 1.0 * context.volume_scale * 0.01;
    EXPECT_GT(expected, 0.0);
    EXPECT_DOUBLE_EQ(expected, r.calc_fee("SHFE.au.2512", 100.0, 1.0, 0));
}
