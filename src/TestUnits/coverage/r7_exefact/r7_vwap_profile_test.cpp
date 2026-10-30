#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/VwapProfile.hpp"

namespace
{
using wtp::WTSSessionInfo;
using wtp::vwap_profile::loadProfile;
using wtp::vwap_profile::mapActionTime;

struct SessionReleaser
{
	void operator()(WTSSessionInfo* session) const
	{
		if (session != nullptr)
			session->release();
	}
};

using SessionPtr = std::unique_ptr<WTSSessionInfo, SessionReleaser>;

SessionPtr makeSession(const char* id, int32_t offset,
	const std::vector<std::pair<uint32_t, uint32_t>>& sections)
{
	SessionPtr session(WTSSessionInfo::create(id, id, offset));
	for (const auto& section : sections)
		session->addTradingSection(section.first, section.second);
	return session;
}

std::string sectionJson(const std::vector<std::pair<uint32_t, uint32_t>>& sections)
{
	std::ostringstream out;
	out << '[';
	for (size_t i = 0; i < sections.size(); ++i)
	{
		if (i != 0)
			out << ',';
		out << "{\"from\":" << sections[i].first << ",\"to\":" << sections[i].second << '}';
	}
	out << ']';
	return out.str();
}

std::string valuesJson(size_t count)
{
	std::ostringstream out;
	out << '[';
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			out << ',';
		out << i;
	}
	out << ']';
	return out.str();
}

std::string valuesJsonWithStringElement(size_t count)
{
	std::ostringstream out;
	out << '[';
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			out << ',';
		if (i == 1)
			out << "\"1\"";
		else
			out << i;
	}
	out << ']';
	return out.str();
}

std::string valuesJsonWithOverflowElement(size_t count)
{
	std::ostringstream out;
	out << '[';
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			out << ',';
		if (i == 1)
			out << "1e999";
		else
			out << i;
	}
	out << ']';
	return out.str();
}

std::string profileJson(const std::string& id, int32_t offset,
	const std::vector<std::pair<uint32_t, uint32_t>>& sections,
	const std::string& values, const std::string& extra_fields = std::string())
{
	std::ostringstream out;
	out << "{\"session_id\":\"" << id << "\",\"offset_minutes\":" << offset
		<< ",\"sections\":" << sectionJson(sections)
		<< ",\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
		<< "\"close_inclusive\":true,\"values\":" << values << extra_fields << '}';
	return out.str();
}

std::string rootJson(const std::string& profiles)
{
	return "{\"version\":2,\"profiles\":[" + profiles + "]}";
}

std::string validProfile(const char* id, int32_t offset,
	const std::vector<std::pair<uint32_t, uint32_t>>& sections)
{
	SessionPtr session = makeSession(id, offset, sections);
	return profileJson(id, offset, sections,
		valuesJson(static_cast<size_t>(session->getTradingMins()) + 1));
}

std::string legacyCsv(size_t count)
{
	std::ostringstream out;
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			out << ',';
		out << i;
	}
	out << "\r\n";
	return out.str();
}

std::string legacyCsvWithOverflowElement(size_t count)
{
	std::ostringstream out;
	for (size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			out << ',';
		if (i == 1)
			out << "1e999";
		else
			out << i;
	}
	return out.str();
}

void expectRejectedPreservingOutput(const std::string& text, WTSSessionInfo& session)
{
	std::vector<double> output{ 71.0, 72.0 };
	const std::vector<double> before = output;
	std::string diagnostic;
	EXPECT_FALSE(loadProfile(text, session, output, diagnostic));
	EXPECT_EQ(output, before);
	EXPECT_FALSE(diagnostic.empty());
}

void expectRejectedWithDiagnostic(const std::string& text, WTSSessionInfo& session,
	const std::string& expected_diagnostic)
{
	std::vector<double> output{ 81.0 };
	std::string diagnostic;
	EXPECT_FALSE(loadProfile(text, session, output, diagnostic));
	EXPECT_EQ(output, std::vector<double>{ 81.0 });
	EXPECT_NE(diagnostic.find(expected_diagnostic), std::string::npos);
}

const std::vector<std::pair<uint32_t, uint32_t>> kDaySections{ { 930, 1130 }, { 1300, 1500 } };

TEST(r7_vwap_profile, dynamic_v2_profiles_match_distinct_sessions)
{
	const std::vector<std::pair<uint32_t, uint32_t>> alternate{ { 900, 1130 }, { 1300, 1600 } };
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	SessionPtr other = makeSession("DAY_B", 0, alternate);
	const std::string json = rootJson(validProfile("DAY_A", 0, kDaySections) + ","
		+ validProfile("DAY_B", 0, alternate));

	std::vector<double> day_values;
	std::string diagnostic;
	ASSERT_TRUE(loadProfile(json, *day, day_values, diagnostic)) << diagnostic;
	ASSERT_EQ(day_values.size(), 241u);
	EXPECT_DOUBLE_EQ(day_values.front(), 0.0);
	EXPECT_DOUBLE_EQ(day_values.back(), 240.0);

	std::vector<double> other_values;
	ASSERT_TRUE(loadProfile(json, *other, other_values, diagnostic)) << diagnostic;
	ASSERT_EQ(other_values.size(), 331u);
	EXPECT_DOUBLE_EQ(other_values.back(), 330.0);
}

TEST(r7_vwap_profile, v2_requires_exact_session_id_offset_and_raw_sections)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	const std::string wrong_id = rootJson(validProfile("OTHER", 0, kDaySections));
	const std::string wrong_offset = rootJson(validProfile("DAY_A", -60, kDaySections));
	const std::string wrong_sections = rootJson(validProfile("DAY_A", 0, { { 930, 1145 }, { 1300, 1500 } }));

	expectRejectedPreservingOutput(wrong_id, *day);
	expectRejectedPreservingOutput(wrong_offset, *day);
	expectRejectedPreservingOutput(wrong_sections, *day);
}

TEST(r7_vwap_profile, v2_rejects_duplicate_matching_profile_and_wrong_value_count)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	const std::string matching = validProfile("DAY_A", 0, kDaySections);
	expectRejectedPreservingOutput(rootJson(matching + ',' + matching), *day);

	const std::string too_short = profileJson("DAY_A", 0, kDaySections, valuesJson(240));
	expectRejectedPreservingOutput(rootJson(too_short), *day);
}

TEST(r7_vwap_profile, v2_rejects_malformed_json_types_and_non_finite_values)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	expectRejectedPreservingOutput("{\"version\":2,\"profiles\":[", *day);
	expectRejectedPreservingOutput("{\"version\":\"2\",\"profiles\":[]}", *day);
	expectRejectedPreservingOutput("{\"version\":2,\"version\":2,\"profiles\":[]}", *day);

	const std::string bad_type = profileJson("DAY_A", 0, kDaySections,
		valuesJsonWithStringElement(241));
	expectRejectedPreservingOutput(rootJson(bad_type), *day);

	const std::string bad_endpoint_type =
		"{\"session_id\":\"DAY_A\",\"offset_minutes\":0,"
		"\"sections\":[{\"from\":\"930\",\"to\":1130},{\"from\":1300,\"to\":1500}],"
		"\"bucket_unit\":\"trading_minute\",\"value_mode\":\"cumulative\","
		"\"close_inclusive\":true,\"values\":" + valuesJson(241) + '}';
	expectRejectedPreservingOutput(rootJson(bad_endpoint_type), *day);

	const std::string non_finite = profileJson("DAY_A", 0, kDaySections,
		valuesJsonWithOverflowElement(241));
	expectRejectedPreservingOutput(rootJson(non_finite), *day);

	const std::string duplicate_field = profileJson("DAY_A", 0, kDaySections,
		valuesJson(241), ",\"close_inclusive\":true");
	expectRejectedPreservingOutput(rootJson(duplicate_field), *day);
}

TEST(r7_vwap_profile, legacy_csv_is_limited_to_exact_canonical_241_point_day_session)
{
	SessionPtr canonical = makeSession("ANY_DAY_ID", 0, kDaySections);
	std::vector<double> values;
	std::string diagnostic;
	ASSERT_TRUE(loadProfile(legacyCsv(241), *canonical, values, diagnostic)) << diagnostic;
	ASSERT_EQ(values.size(), 241u);
	EXPECT_DOUBLE_EQ(values.front(), 0.0);
	EXPECT_DOUBLE_EQ(values.back(), 240.0);

	expectRejectedPreservingOutput(legacyCsv(240), *canonical);

	SessionPtr alternate = makeSession("DAY_ALT", 0, { { 900, 1130 }, { 1300, 1600 } });
	expectRejectedPreservingOutput(legacyCsv(241), *alternate);
	expectRejectedPreservingOutput("0,nan,2", *canonical);
	expectRejectedPreservingOutput(legacyCsvWithOverflowElement(241), *canonical);
}

TEST(r7_vwap_profile, empty_session_is_rejected_without_changing_output)
{
	SessionPtr empty = makeSession("EMPTY", 0, {});
	expectRejectedPreservingOutput(validProfile("EMPTY", 0, { { 930, 1500 } }), *empty);
}

TEST(r7_vwap_profile, invalid_or_unordered_sections_are_rejected_before_minute_counting)
{
	SessionPtr reversed = makeSession("REVERSED", 0, { { 2100, 230 } });
	expectRejectedPreservingOutput("0", *reversed);

	SessionPtr out_of_order = makeSession("OUT_OF_ORDER", 0, { { 1300, 1500 }, { 900, 1100 } });
	expectRejectedPreservingOutput("0", *out_of_order);

	SessionPtr overlap = makeSession("OVERLAP", 0, { { 900, 1100 }, { 1000, 1200 } });
	expectRejectedPreservingOutput("0", *overlap);

	SessionPtr unshifted_midnight_sections = makeSession("UNSHIFTED_NIGHT", 0,
		{ { 2100, 2300 }, { 0, 230 } });
	expectRejectedPreservingOutput("0", *unshifted_midnight_sections);

	SessionPtr zero_length = makeSession("ZERO_LENGTH", 0, { { 930, 930 } });
	expectRejectedPreservingOutput("0", *zero_length);

}

TEST(r7_vwap_profile, legal_minute_borrow_sections_keep_api_minute_counts)
{
	const std::vector<std::pair<uint32_t, uint32_t>> day_section{ { 930, 1100 } };
	SessionPtr day = makeSession("DAY_BORROW", 0, day_section);
	std::vector<double> values;
	std::string diagnostic;
	ASSERT_TRUE(loadProfile(rootJson(validProfile("DAY_BORROW", 0, day_section)),
		*day, values, diagnostic)) << diagnostic;
	ASSERT_EQ(values.size(), 91u);
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*day, 93000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
	ASSERT_TRUE(mapActionTime(*day, 110000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 90u);

	const std::vector<std::pair<uint32_t, uint32_t>> overnight{ { 2130, 200 } };
	SessionPtr night = makeSession("NIGHT_BORROW", 300, overnight);
	values.clear();
	ASSERT_TRUE(loadProfile(rootJson(validProfile("NIGHT_BORROW", 300, overnight)),
		*night, values, diagnostic)) << diagnostic;
	ASSERT_EQ(values.size(), 271u);
	ASSERT_TRUE(mapActionTime(*night, 213000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
	ASSERT_TRUE(mapActionTime(*night, 20000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 270u);
}

TEST(r7_vwap_profile, v2_rejects_invalid_unselected_sections_before_counting_values)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	const std::string bad_midnight = rootJson(profileJson("BAD_MIDNIGHT", 0,
		{ { 2100, 230 } }, "[]"));
	expectRejectedWithDiagnostic(bad_midnight, *day, "positive offset-adjusted duration");

	const std::string overlap = rootJson(profileJson("BAD_OVERLAP", 0,
		{ { 900, 1100 }, { 1000, 1200 } }, "[]"));
	expectRejectedWithDiagnostic(overlap, *day, "ordered and non-overlapping");

	const std::string zero_length = rootJson(profileJson("BAD_ZERO", 0,
		{ { 930, 930 } }, "[]"));
	expectRejectedWithDiagnostic(zero_length, *day, "positive offset-adjusted duration");
}

TEST(r7_vwap_profile, offset_makes_cross_midnight_sections_ordered_and_usable)
{
	const std::vector<std::pair<uint32_t, uint32_t>> night_day{ { 2100, 2300 }, { 0, 230 } };
	SessionPtr session = makeSession("NIGHT_DAY", 300, night_day);
	std::string diagnostic;
	std::vector<double> values;
	ASSERT_TRUE(loadProfile(rootJson(validProfile("NIGHT_DAY", 300, night_day)),
		*session, values, diagnostic)) << diagnostic;
	ASSERT_EQ(values.size(), 271u);

	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*session, 213000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 30u);
	ASSERT_TRUE(mapActionTime(*session, 0u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 120u);
	ASSERT_TRUE(mapActionTime(*session, 23000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 270u);
}

TEST(r7_vwap_profile, day_session_boundaries_and_short_numeric_open_time_map_correctly)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	std::string diagnostic;
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*day, 93000000u, 241, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u); // numeric HHMMSSmmm naturally represents 09:30 with eight digits.
	ASSERT_TRUE(mapActionTime(*day, 113000000u, 241, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 120u);
	EXPECT_FALSE(mapActionTime(*day, 113000001u, 241, index, diagnostic));
	EXPECT_FALSE(mapActionTime(*day, 113000500u, 241, index, diagnostic));
	ASSERT_TRUE(mapActionTime(*day, 130000000u, 241, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 120u);
	ASSERT_TRUE(mapActionTime(*day, 150000000u, 241, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 240u);

	EXPECT_FALSE(mapActionTime(*day, 92959999u, 241, index, diagnostic));
	EXPECT_FALSE(mapActionTime(*day, 115959999u, 241, index, diagnostic));
	EXPECT_FALSE(mapActionTime(*day, 125959999u, 241, index, diagnostic));
	EXPECT_FALSE(mapActionTime(*day, 150100000u, 241, index, diagnostic));
	EXPECT_FALSE(mapActionTime(*day, 150000001u, 241, index, diagnostic));

	// Opening minutes remain valid after the exact HH:MM:00.000 open.
	ASSERT_TRUE(mapActionTime(*day, 93000001u, 241, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
}

TEST(r7_vwap_profile, adjacent_close_and_open_timestamp_is_accepted_as_new_section_start)
{
	SessionPtr adjacent = makeSession("ADJACENT", 0, { { 900, 1000 }, { 1000, 1100 } });
	std::string diagnostic;
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*adjacent, 100000001u, 121, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 60u);
}

TEST(r7_vwap_profile, action_time_validation_auction_and_bucket_bounds_are_fail_closed)
{
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	day->addAuctionTime(915, 930);
	std::string diagnostic;
	size_t index = 77;
	EXPECT_FALSE(mapActionTime(*day, 91500000u, 241, index, diagnostic));
	EXPECT_EQ(index, 77u);
	EXPECT_FALSE(mapActionTime(*day, 126099999u, 241, index, diagnostic));
	EXPECT_EQ(index, 77u);
	EXPECT_FALSE(mapActionTime(*day, 150000000u, 240, index, diagnostic));
	EXPECT_EQ(index, 77u);
}

TEST(r7_vwap_profile, offset_cross_midnight_session_maps_using_session_api)
{
	SessionPtr night = makeSession("NIGHT", 300, { { 2100, 230 } });
	ASSERT_EQ(night->getTradingMins(), 330u);
	std::string diagnostic;
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*night, 210000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
	ASSERT_TRUE(mapActionTime(*night, 213000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 30u);
	ASSERT_TRUE(mapActionTime(*night, 0u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 180u);
	ASSERT_TRUE(mapActionTime(*night, 3000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 210u);
	ASSERT_TRUE(mapActionTime(*night, 23000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 330u);
	EXPECT_FALSE(mapActionTime(*night, 23000001u, 331, index, diagnostic));
}

TEST(r7_vwap_profile, zero_offset_2400_alias_is_not_guessed_from_time_only)
{
	SessionPtr midnight = makeSession("MIDNIGHT", 0, { { 2100, 2400 } });
	SessionPtr day = makeSession("DAY_A", 0, kDaySections);
	std::string diagnostic;
	size_t index = 999;
	const std::string profile = rootJson(profileJson("MIDNIGHT", 0,
		{ { 2100, 2400 } }, valuesJson(181)));
	expectRejectedWithDiagnostic(profile, *midnight, "24:00 close");
	expectRejectedWithDiagnostic(profile, *day, "24:00 close");
	EXPECT_FALSE(mapActionTime(*midnight, 0u, 181, index, diagnostic));
	EXPECT_EQ(index, 999u);
	EXPECT_NE(diagnostic.find("24:00 close"), std::string::npos);
}

TEST(r7_vwap_profile, offset_2400_close_and_2400_start_follow_api_endpoint_mapping)
{
	const std::vector<std::pair<uint32_t, uint32_t>> midnight_close{ { 2100, 2400 } };
	SessionPtr shifted_close = makeSession("SHIFTED_CLOSE", 300, midnight_close);
	std::string diagnostic;
	std::vector<double> values;
	ASSERT_TRUE(loadProfile(rootJson(validProfile("SHIFTED_CLOSE", 300, midnight_close)),
		*shifted_close, values, diagnostic)) << diagnostic;
	ASSERT_EQ(values.size(), 181u);
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*shifted_close, 0u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 180u);
	EXPECT_FALSE(mapActionTime(*shifted_close, 1u, values.size(), index, diagnostic));

	// In this session the API left-aligns raw 2400 to 0000, which is representable
	// by action_time and maps as the trading section start.
	const std::vector<std::pair<uint32_t, uint32_t>> midnight_start{ { 2400, 300 } };
	SessionPtr shifted_start = makeSession("MIDNIGHT_START", 0, midnight_start);
	ASSERT_TRUE(loadProfile(rootJson(validProfile("MIDNIGHT_START", 0, midnight_start)),
		*shifted_start, values, diagnostic)) << diagnostic;
	ASSERT_TRUE(mapActionTime(*shifted_start, 0u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
	ASSERT_TRUE(mapActionTime(*shifted_start, 30000000u, values.size(), index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 180u);
}

TEST(r7_vwap_profile, alternate_day_session_maps_lunch_and_close_from_its_own_minutes)
{
	SessionPtr alternate = makeSession("DAY_ALT", 0, { { 900, 1130 }, { 1300, 1600 } });
	std::string diagnostic;
	size_t index = 999;
	ASSERT_TRUE(mapActionTime(*alternate, 90000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 0u);
	ASSERT_TRUE(mapActionTime(*alternate, 130000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 150u);
	ASSERT_TRUE(mapActionTime(*alternate, 160000000u, 331, index, diagnostic)) << diagnostic;
	EXPECT_EQ(index, 330u);
}

} // namespace
