#include <gtest/gtest.h>
#include "Share/FeeSchedule.hpp"
#include <clocale>
#include <cstdio>
#include <limits>
#include <string>

using namespace wt_fee;
namespace {
const char* v2 = R"({"version":2,"contracts":{"SHFE.rb2510":{"open":1.25,"close":0,"closetoday":-0.015,"byvolume":true}},"products":{"SHFE.rb":{"open":0.001,"close":0.002,"closetoday":0.003,"byvolume":false,"minfee":5}}})";
Context ctx(Rates rates = {}) { return {rates, 10, "SHFE.rb2510", "SHFE.rb"}; }
void expectOk(CalculationResult r, double expected) { EXPECT_EQ(CalculationStatus::ok, r.status); EXPECT_DOUBLE_EQ(expected, r.value); }
void expectFailure(CalculationResult r, CalculationStatus status) { EXPECT_EQ(status, r.status); EXPECT_DOUBLE_EQ(0, r.value); }
}

TEST(FeeContext, ContractPriorityFallbackAndIndependentSnapshots)
{
    Schedule s; std::string error;
    EXPECT_FALSE(s.valid()); EXPECT_TRUE(s.contractRates().empty()); EXPECT_TRUE(s.productRates().empty());
    ASSERT_TRUE(s.loadJson(v2, error)) << error;
    EXPECT_TRUE(s.valid()); EXPECT_EQ(1u, s.contractRates().size()); EXPECT_EQ(1u, s.productRates().size());
    const auto direct = s.lookup("SHFE.rb2510", "SHFE.rb"); ASSERT_EQ(LookupStatus::contract, direct.status);
    const Context contract = ctx(direct.rates);
    expectOk(calculate(contract, 100, 2, 0), 2.5);
    expectOk(calculate(contract, 100, 2, 1), 0);
    const auto fallback = s.lookup("SHFE.rb2605", "SHFE.rb"); ASSERT_EQ(LookupStatus::product, fallback.status);
    Context product{fallback.rates, 10, "SHFE.rb2605", "SHFE.rb"};
    expectOk(calculate(product, 100, 2, 0), 5);
    EXPECT_EQ(LookupStatus::missing, s.lookup("SHFE.au2605", "SHFE.au").status);
    EXPECT_FALSE(s.loadJson(R"({"version":3})", error));
    EXPECT_FALSE(s.valid()); EXPECT_TRUE(s.contractRates().empty()); EXPECT_TRUE(s.productRates().empty());
    expectOk(calculate(contract, 100, 2, 0), 2.5);
    EXPECT_EQ(LookupStatus::invalid, s.lookup("SHFE.rb2510", "SHFE.rb").status);
}

TEST(FeeContext, AmountModesOffsetsMinimumZeroAndSignedRounding)
{
    Rates r; r.open = 0.001; r.close = 0.002; r.close_today = 0.003; r.min_fee = 5;
    Context c = ctx(r);
    expectOk(calculate(c, 100, 2, 0), 5); expectOk(calculate(c, 1000, 2, 0), 20);
    expectOk(calculate(c, 1000, 2, 1), 40); expectOk(calculate(c, 1000, 2, 2), 60);
    expectOk(calculate(c, 100, 0, 0), 0);
    c.rates.open = 0; expectOk(calculate(c, 100, 2, 0), 0);
    c.rates.by_volume = true; c.rates.open = -0.015; expectOk(calculate(c, 100, 1, 0), -0.02);
    c.rates.open = -0.014; expectOk(calculate(c, 100, 1, 0), -0.01);
    c.rates.open = 0.015; c.rates.min_fee = 0; expectOk(calculate(c, 100, 1, 0), 0.02);
    c.rates.open = 0.014; expectOk(calculate(c, 100, 1, 0), 0.01);
}

TEST(FeeContext, RejectsInvalidInputsAndContexts)
{
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    Context c = ctx();
    for (double x : {-1.0, nan, inf}) { expectFailure(calculate(c, x, 1, 0), CalculationStatus::invalid_input); expectFailure(calculate(c, 1, x, 0), CalculationStatus::invalid_input); }
    expectFailure(calculate(c, 1, 1, 3), CalculationStatus::invalid_input);
    for (double x : {0.0, -1.0, nan, inf}) { Context a = c; a.volume_scale = x; expectFailure(calculate(a, 1, 1, 0), CalculationStatus::invalid_context); }
    for (int i = 0; i < 5; ++i) {
        Context a = c; double* fields[] = {&a.rates.open, &a.rates.close, &a.rates.close_today, &a.rates.min_fee, &a.rates.margin}; *fields[i] = nan;
        expectFailure(calculate(a, 1, 1, 0), CalculationStatus::invalid_context);
    }
    c.rates.min_fee = -1; expectFailure(calculate(c, 1, 1, 0), CalculationStatus::invalid_context);
    c = ctx(); c.physical_contract.clear(); expectFailure(calculate(c, 1, 1, 0), CalculationStatus::invalid_context);
    c = ctx(); c.product = "DCE.rb"; expectFailure(calculate(c, 1, 1, 0), CalculationStatus::invalid_context);
}

TEST(FeeContext, DetectsIntermediateAndCentsOverflow)
{
    const double max = std::numeric_limits<double>::max();
    Rates r; r.by_volume = true; r.open = max; Context c = ctx(r);
    expectFailure(calculate(c, 1, 2, 0), CalculationStatus::overflow);
    expectFailure(calculate(c, 1, 1, 0), CalculationStatus::overflow);
    expectOk(calculate(c, 1, 0, 0), 0);
    r.by_volume = false; r.open = max; c = ctx(r);
    expectFailure(calculate(c, 2, 1, 0), CalculationStatus::overflow);
    r.open = 1; c = ctx(r); c.volume_scale = 2;
    expectFailure(calculate(c, max / 2, 2, 0), CalculationStatus::overflow);
}

TEST(FeeContext, LegacyStringsUseClassicLocale)
{
    struct LocaleGuard { std::string old; LocaleGuard() : old(std::setlocale(LC_NUMERIC, nullptr)) {} ~LocaleGuard() { std::setlocale(LC_NUMERIC, old.c_str()); } } guard;
    const char* selected = nullptr;
    for (auto candidate : {"de_DE.UTF-8", "fr_FR.UTF-8", "nl_NL.UTF-8"})
        if (std::setlocale(LC_NUMERIC, candidate) && std::string(std::localeconv()->decimal_point) == ",") { selected = candidate; break; }
    if (!selected) GTEST_SKIP() << "No decimal-comma locale available";
    std::printf("LC_NUMERIC locale: %s; decimal point: %s\n", selected, std::localeconv()->decimal_point);
    Schedule s; std::string error;
    ASSERT_TRUE(s.loadJson(R"({"SHFE.rb":{"open":"0.125","close":0.25,"closetoday":"-0.015","minfee":"0.5","margin":"0.75","byvolume":true}})", error)) << error;
    auto found = s.lookup("SHFE.rb2510", "SHFE.rb"); ASSERT_EQ(LookupStatus::product, found.status);
    EXPECT_DOUBLE_EQ(0.125, found.rates.open); EXPECT_DOUBLE_EQ(0.25, found.rates.close);
    EXPECT_DOUBLE_EQ(-0.015, found.rates.close_today); EXPECT_DOUBLE_EQ(0.5, found.rates.min_fee);
    EXPECT_DOUBLE_EQ(0.75, found.rates.margin); expectOk(calculate(ctx(found.rates), 100, 1, 2), -0.02);
    EXPECT_FALSE(s.loadJson(R"({"SHFE.rb":{"open":"0.1junk","close":0,"closetoday":0,"byvolume":true}})", error));
    EXPECT_FALSE(s.valid()); EXPECT_TRUE(s.productRates().empty());
}
