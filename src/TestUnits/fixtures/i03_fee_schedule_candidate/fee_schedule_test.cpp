#include <gtest/gtest.h>
#include "Share/FeeSchedule.hpp"
#include <fstream>
#include <iterator>
#include <string>

using wt_fee::LookupStatus;
using wt_fee::Schedule;

namespace {
const char* const v2 = R"({
 "version":2,
 "contracts": {
  "SHFE.rb2510":{"open":1.25,"close":2,"closetoday":3,"byvolume":true},
  "SHFE.rb2601":{"open":0,"close":0,"closetoday":0,"byvolume":true}
 },
 "products":{"SHFE.rb":{"open":0.01,"close":0.02,"closetoday":0.03,"byvolume":false,"minfee":5}}
})";
}

TEST(FeeScheduleCandidate, contractOverridesProductAndZeroIsPresent)
{
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(v2, error)) << error;
	auto a = s.lookup("SHFE.rb2510", "SHFE.rb");
	EXPECT_EQ(a.status, LookupStatus::contract);
	EXPECT_DOUBLE_EQ(a.rates.open, 1.25);
	EXPECT_TRUE(a.rates.by_volume);
	auto b = s.lookup("SHFE.rb2601", "SHFE.rb");
	EXPECT_EQ(b.status, LookupStatus::contract);
	EXPECT_DOUBLE_EQ(b.rates.open, 0);
	auto c = s.lookup("SHFE.rb2605", "SHFE.rb");
	EXPECT_EQ(c.status, LookupStatus::product);
	EXPECT_DOUBLE_EQ(c.rates.open, 0.01);
	EXPECT_DOUBLE_EQ(c.rates.min_fee, 5);
	EXPECT_FALSE(c.rates.by_volume);
	EXPECT_EQ(s.lookup("SHFE.au2605", "SHFE.au").status, LookupStatus::missing);
}

TEST(FeeScheduleCandidate, legacyUnversionedEntriesAreOnlyProducts)
{
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(R"({"SHFE.rb":{"open":"0.02","close":0,"closetoday":0,"byvolume":false},"SHFE.rb2510":{"open":4,"close":0,"closetoday":0,"byvolume":false}})", error)) << error;
	auto a = s.lookup("SHFE.rb2510", "SHFE.rb");
	EXPECT_EQ(a.status, LookupStatus::product);
	EXPECT_DOUBLE_EQ(a.rates.open, 0.02);
	EXPECT_DOUBLE_EQ(a.rates.close, 0);
	EXPECT_EQ(s.lookup("SHFE.rb2510", "SHFE.au").status, LookupStatus::missing);
}

TEST(FeeScheduleCandidate, everyLegacyRateFieldMustBePresentEvenForZero)
{
	const char* incomplete[] = {
		R"({"SHFE.rb":{}})",
		R"({"SHFE.rb":{"close":0,"closetoday":0,"byvolume":false}})",
		R"({"SHFE.rb":{"open":0,"closetoday":0,"byvolume":false}})",
		R"({"SHFE.rb":{"open":0,"close":0,"byvolume":false}})",
		R"({"SHFE.rb":{"open":0,"close":0,"closetoday":0}})"
	};
	Schedule s; std::string error;
	for (const char* input : incomplete)
	{
		ASSERT_TRUE(s.loadJson(v2, error));
		EXPECT_FALSE(s.loadJson(input, error)) << input;
		EXPECT_EQ(s.lookup("SHFE.rb2510", "SHFE.rb").status, LookupStatus::invalid);
	}
	ASSERT_TRUE(s.loadJson(R"({"SHFE.rb":{"open":0,"close":0,"closetoday":0,"byvolume":false}})", error)) << error;
	auto zero = s.lookup("SHFE.rb2510", "SHFE.rb");
	EXPECT_EQ(zero.status, LookupStatus::product);
	EXPECT_DOUBLE_EQ(zero.rates.open, 0);
}

TEST(FeeScheduleCandidate, callerMustValidateContractProductRelationship)
{
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(v2, error));
	// Same-exchange syntax is not proof of a real instrument/product pair.
	// This deliberately demonstrates why production callers must validate
	// the pair against the actual trade's instrument metadata first.
	auto unverified = s.lookup("SHFE.au2605", "SHFE.rb");
	EXPECT_EQ(unverified.status, LookupStatus::product);
	EXPECT_DOUBLE_EQ(unverified.rates.open, 0.01);
}

TEST(FeeScheduleCandidate, readsExistingLegacyJsonFixture)
{
	std::ifstream input("src/TestUnits/fixtures/fees_minfee.json");
	ASSERT_TRUE(input.good());
	std::string document((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(document, error)) << error;
	auto contract = s.lookup("DCE.a2511", "DCE.a");
	EXPECT_EQ(contract.status, LookupStatus::product);
	EXPECT_DOUBLE_EQ(contract.rates.open, 1.0);
	EXPECT_DOUBLE_EQ(contract.rates.min_fee, 5.0);
	EXPECT_TRUE(contract.rates.by_volume);
}

TEST(FeeScheduleCandidate, requiresPhysicalIdentityAndSameExchange)
{
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(v2, error));
	for (const auto& identity : {"", "SHFE", "SHFE..rb2510", " SHFE.rb2510", "SHFE.rb2510\n"})
		EXPECT_EQ(s.lookup(identity, "SHFE.rb").status, LookupStatus::invalid) << identity;
	EXPECT_EQ(s.lookup("DCE.rb2510", "SHFE.rb").status, LookupStatus::invalid);
	EXPECT_EQ(s.lookup("SHFE.rb2510", "").status, LookupStatus::invalid);
}

TEST(FeeScheduleCandidate, invalidReloadCannotReusePreviousRates)
{
	Schedule s; std::string error;
	ASSERT_TRUE(s.loadJson(v2, error));
	EXPECT_FALSE(s.loadJson(R"({"version":3,"contracts":{},"products":{}})", error));
	EXPECT_FALSE(error.empty());
	EXPECT_EQ(s.lookup("SHFE.rb2510", "SHFE.rb").status, LookupStatus::invalid);
}

TEST(FeeScheduleCandidate, rejectsMalformedAmbiguousAndInvalidValues)
{
	const char* bad[] = {
		"[]", "{", R"({"version":2,"version":2,"contracts":{},"products":{}})",
		R"({"version":2,"contracts":{},"products":{},"other":{}})",
		R"({"version":2,"contracts":{}})",
		R"({"version":2,"contracts":{"SHFE.rb2510":{"open":1,"open":2,"close":0,"closetoday":0,"byvolume":true}},"products":{}})",
		R"({"version":2,"contracts":{"SHFE.rb2510":{"open":1,"close":0,"closetoday":0,"byvolume":true},"SHFE.rb2510":{"open":1,"close":0,"closetoday":0,"byvolume":true}},"products":{}})",
		R"({"version":2,"contracts":{"SHFE.rb2510":{"open":0,"close":0,"closetoday":0}},"products":{}})",
		R"({"version":2,"contracts":{"SHFE.rb2510":{"open":"1","close":0,"closetoday":0,"byvolume":true}},"products":{}})",
		R"({"version":2,"contracts":{"SHFE.rb2510":{"open":1e999,"close":0,"closetoday":0,"byvolume":true}},"products":{}})",
		R"({"SHFE.rb":{"open":"1junk"}})", R"({"SHFE.rb":{"minfee":-1}})",
		R"({"SHFE.rb":{"open":null}})", R"({"SHFE.rb":{"byvolume":"perhaps"}})",
		R"({"SHFE..rb":{"open":1}})", R"({"SHFE.rb":{"surprise":1}})"
	};
	for (const char* input : bad)
	{
		Schedule s; std::string error;
		EXPECT_FALSE(s.loadJson(input, error)) << input;
		EXPECT_FALSE(error.empty());
		EXPECT_EQ(s.lookup("SHFE.rb2510", "SHFE.rb").status, LookupStatus::invalid);
	}
	Schedule s; std::string error;
	EXPECT_FALSE(s.loadJson(std::string("{}\0{}", 5), error));
	EXPECT_FALSE(s.loadJson(R"({"SHFE.\uD800":{"open":0,"close":0,"closetoday":0,"byvolume":false}})", error));
	const std::string invalidUtf8 = std::string("{\"SHFE.") + char(0xff) + "\":{}}";
	EXPECT_FALSE(s.loadJson(invalidUtf8, error));
}
