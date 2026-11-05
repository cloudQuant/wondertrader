#pragma once

#include "../Includes/WTSSessionInfo.hpp"
#include "rapidjson/document.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wtp
{
namespace vwap_profile
{

struct SessionSignature
{
	std::string session_id;
	int32_t offset_minutes = 0;
	std::vector<std::pair<uint32_t, uint32_t>> sections;
	uint32_t trading_minutes = 0;
};

inline bool fail(std::string& diagnostic, const char* message)
{
	diagnostic = message;
	return false;
}

inline bool validHHMM(uint32_t value);
inline bool validateTradingSections(WTSSessionInfo& session, std::string& diagnostic);

inline bool makeSessionSignature(WTSSessionInfo& session, SessionSignature& result,
	std::string& diagnostic)
{
	if (session.id() == nullptr || session.id()[0] == '\0')
		return fail(diagnostic, "VWAP profile requires a non-empty session id");
	if (!validateTradingSections(session, diagnostic))
		return false;

	SessionSignature candidate;
	candidate.session_id = session.id();
	candidate.offset_minutes = session.getOffsetMins();
	const WTSSessionInfo::TradingTimes& raw_sections = session.getTradingSections();
	candidate.sections.reserve(raw_sections.size());
	for (const WTSSessionInfo::TradingSection& section : raw_sections)
		candidate.sections.emplace_back(section.first_raw, section.second_raw);
	candidate.trading_minutes = session.getTradingMins();

	result = std::move(candidate);
	diagnostic.clear();
	return true;
}

inline bool sameSession(const SessionSignature& left, const SessionSignature& right)
{
	return left.session_id == right.session_id
		&& left.offset_minutes == right.offset_minutes
		&& left.sections == right.sections
		&& left.trading_minutes == right.trading_minutes;
}

inline bool sameProfileKey(const SessionSignature& left, const SessionSignature& right)
{
	return sameSession(left, right);
}

struct SessionReleaser
{
	void operator()(WTSSessionInfo* session) const
	{
		if (session != nullptr)
			session->release();
	}
};

inline bool validHHMM(uint32_t value)
{
	if (value > 2400)
		return false;
	const uint32_t hour = value / 100;
	const uint32_t minute = value % 100;
	return hour <= 24 && minute < 60 && (hour < 24 || minute == 0);
}

inline bool validateTradingSections(WTSSessionInfo& session, std::string& diagnostic)
{
	if (session.getOffsetMins() < -1440 || session.getOffsetMins() > 1440)
		return fail(diagnostic, "VWAP profile session offset must be within one day");

	const WTSSessionInfo::TradingTimes& sections = session.getTradingSections();
	if (sections.empty() || session.getSectionCount() == 0)
		return fail(diagnostic, "VWAP profile requires at least one trading section");

	bool have_previous = false;
	uint32_t previous_end = 0;
	for (const WTSSessionInfo::TradingSection& section : sections)
	{
		if (!validHHMM(section.first_raw) || !validHHMM(section.second_raw)
			|| !validHHMM(section.first) || !validHHMM(section.second))
			return fail(diagnostic, "VWAP trading section contains an invalid HHMM endpoint");

		const uint32_t expected_start = session.offsetTime(section.first_raw, true);
		const uint32_t expected_end = session.offsetTime(section.second_raw, false);
		if (section.first != expected_start || section.second != expected_end)
			return fail(diagnostic, "VWAP trading section endpoints are stale for the session offset");
		if (section.first >= section.second)
			return fail(diagnostic, "VWAP trading section must have a positive offset-adjusted duration");
		if (have_previous && section.first < previous_end)
			return fail(diagnostic, "VWAP trading sections must be ordered and non-overlapping");

		if (section.second_raw % 2400 == 0
			&& section.second != session.offsetTime(0, true))
			return fail(diagnostic, "VWAP 24:00 close cannot be mapped by the session time API for this offset");

		previous_end = section.second;
		have_previous = true;
	}

	diagnostic.clear();
	return true;
}

inline bool hasExactMembers(const rapidjson::Value& value,
	std::initializer_list<const char*> expected)
{
	if (!value.IsObject() || value.MemberCount() != expected.size())
		return false;

	std::set<std::string> expected_names;
	for (const char* name : expected)
		expected_names.insert(name);

	std::set<std::string> actual_names;
	for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member)
	{
		if (!member->name.IsString())
			return false;
		const std::string name(member->name.GetString(), member->name.GetStringLength());
		if (expected_names.find(name) == expected_names.end() || !actual_names.insert(name).second)
			return false;
	}
	return actual_names.size() == expected_names.size();
}

inline bool readProfileSignature(const rapidjson::Value& profile,
	SessionSignature& signature, const rapidjson::Value*& values,
	std::string& diagnostic)
{
	if (!hasExactMembers(profile, { "session_id", "offset_minutes", "sections",
		"bucket_unit", "value_mode", "close_inclusive", "values" }))
		return fail(diagnostic, "VWAP profile entry has missing, duplicate, or unexpected fields");

	const rapidjson::Value& id = profile["session_id"];
	const rapidjson::Value& offset = profile["offset_minutes"];
	const rapidjson::Value& sections = profile["sections"];
	const rapidjson::Value& bucket_unit = profile["bucket_unit"];
	const rapidjson::Value& value_mode = profile["value_mode"];
	const rapidjson::Value& close_inclusive = profile["close_inclusive"];
	values = &profile["values"];

	if (!id.IsString() || id.GetStringLength() == 0)
		return fail(diagnostic, "VWAP profile session_id must be a non-empty string");
	if (!offset.IsInt())
		return fail(diagnostic, "VWAP profile offset_minutes must be a 32-bit integer");
	if (offset.GetInt() < -1440 || offset.GetInt() > 1440)
		return fail(diagnostic, "VWAP profile offset_minutes must be within one day");
	if (!bucket_unit.IsString() || std::string(bucket_unit.GetString(), bucket_unit.GetStringLength()) != "trading_minute")
		return fail(diagnostic, "VWAP profile bucket_unit must be trading_minute");
	if (!value_mode.IsString() || std::string(value_mode.GetString(), value_mode.GetStringLength()) != "cumulative")
		return fail(diagnostic, "VWAP profile value_mode must be cumulative");
	if (!close_inclusive.IsBool() || !close_inclusive.GetBool())
		return fail(diagnostic, "VWAP profile close_inclusive must be true");
	if (!sections.IsArray() || sections.Empty())
		return fail(diagnostic, "VWAP profile sections must be a non-empty array");
	if (values == nullptr || !values->IsArray())
		return fail(diagnostic, "VWAP profile values must be an array");

	signature.session_id.assign(id.GetString(), id.GetStringLength());
	signature.offset_minutes = offset.GetInt();
	signature.sections.reserve(sections.Size());
	for (rapidjson::SizeType i = 0; i < sections.Size(); ++i)
	{
		const rapidjson::Value& section = sections[i];
		if (!hasExactMembers(section, { "from", "to" }))
			return fail(diagnostic, "VWAP profile section must contain exactly from and to");
		const rapidjson::Value& from = section["from"];
		const rapidjson::Value& to = section["to"];
		if (!from.IsUint() || !to.IsUint() || !validHHMM(from.GetUint()) || !validHHMM(to.GetUint()))
			return fail(diagnostic, "VWAP profile section endpoints must be valid HHMM integers");
		signature.sections.emplace_back(from.GetUint(), to.GetUint());
	}

	std::unique_ptr<WTSSessionInfo, SessionReleaser> profile_session(
		WTSSessionInfo::create(signature.session_id.c_str(), signature.session_id.c_str(), signature.offset_minutes));
	if (!profile_session)
		return fail(diagnostic, "VWAP profile session could not be constructed");
	// I03-NEW-096: card 2/K30 make addTradingSection drop zero-length or overlapping
	// sections silently; reject them here with the section diagnostic instead of a
	// misleading later failure (e.g. value count) on the reduced session.
	for (const auto& section : signature.sections)
	{
		if (!profile_session->tryAddTradingSection(section.first, section.second))
			return fail(diagnostic, section.first % 2400 == section.second % 2400
				? "VWAP trading section must have a positive offset-adjusted duration"
				: "VWAP trading sections must be ordered and non-overlapping");
	}
	if (!validateTradingSections(*profile_session, diagnostic))
		return false;
	signature.trading_minutes = profile_session->getTradingMins();
	const size_t expected_values = static_cast<size_t>(signature.trading_minutes) + 1;
	if (values->Size() != expected_values)
		return fail(diagnostic, "VWAP profile value count must equal trading minutes plus one");

	for (rapidjson::SizeType i = 0; i < values->Size(); ++i)
	{
		if (!(*values)[i].IsNumber() || !std::isfinite((*values)[i].GetDouble()))
			return fail(diagnostic, "VWAP profile values must contain only finite numbers");
	}
	diagnostic.clear();
	return true;
}

inline bool parseV2(const std::string& text, const SessionSignature& wanted,
	std::vector<double>& output, std::string& diagnostic)
{
	rapidjson::Document document;
	document.Parse(text.c_str());
	if (document.HasParseError())
		return fail(diagnostic, "VWAP V2 profile contains malformed JSON");
	if (!hasExactMembers(document, { "version", "profiles" }))
		return fail(diagnostic, "VWAP V2 document must contain exactly version and profiles");
	if (!document["version"].IsInt() || document["version"].GetInt() != 2)
		return fail(diagnostic, "VWAP profile version must be integer 2");
	const rapidjson::Value& profiles = document["profiles"];
	if (!profiles.IsArray() || profiles.Empty())
		return fail(diagnostic, "VWAP V2 profiles must be a non-empty array");

	std::vector<SessionSignature> seen_keys;
	std::vector<double> selected_values;
	uint32_t matching_profiles = 0;
	for (rapidjson::SizeType i = 0; i < profiles.Size(); ++i)
	{
		SessionSignature current;
		const rapidjson::Value* values = nullptr;
		if (!readProfileSignature(profiles[i], current, values, diagnostic))
			return false;

		for (const SessionSignature& earlier : seen_keys)
		{
			if (sameProfileKey(earlier, current))
				return fail(diagnostic, "VWAP V2 document contains duplicate session profiles");
		}
		seen_keys.emplace_back(current);

		if (sameProfileKey(current, wanted))
		{
			++matching_profiles;
			selected_values.reserve(values->Size());
			for (rapidjson::SizeType j = 0; j < values->Size(); ++j)
				selected_values.push_back((*values)[j].GetDouble());
		}
	}

	if (matching_profiles != 1)
		return fail(diagnostic, matching_profiles == 0
			? "VWAP V2 document has no profile matching this session"
			: "VWAP V2 document has duplicate matching profiles");
	output.swap(selected_values);
	diagnostic.clear();
	return true;
}

inline bool isCanonicalLegacySession(const SessionSignature& signature)
{
	return signature.offset_minutes == 0
		&& signature.sections.size() == 2
		&& signature.sections[0] == std::make_pair(930u, 1130u)
		&& signature.sections[1] == std::make_pair(1300u, 1500u)
		&& signature.trading_minutes == 240;
}

inline bool parseDecimal(const std::string& text, double& result)
{
	if (text.empty())
		return false;

	size_t i = 0;
	if (text[i] == '+' || text[i] == '-')
		++i;
	size_t digits = 0;
	while (i < text.size() && text[i] >= '0' && text[i] <= '9')
	{
		++i;
		++digits;
	}
	if (i < text.size() && text[i] == '.')
	{
		++i;
		while (i < text.size() && text[i] >= '0' && text[i] <= '9')
		{
			++i;
			++digits;
		}
	}
	if (digits == 0)
		return false;
	if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
	{
		++i;
		if (i < text.size() && (text[i] == '+' || text[i] == '-'))
			++i;
		size_t exponent_digits = 0;
		while (i < text.size() && text[i] >= '0' && text[i] <= '9')
		{
			++i;
			++exponent_digits;
		}
		if (exponent_digits == 0)
			return false;
	}
	if (i != text.size())
		return false;

	errno = 0;
	char* end = nullptr;
	result = std::strtod(text.c_str(), &end);
	return end == text.c_str() + text.size() && errno != ERANGE && std::isfinite(result);
}

inline std::string trim(const std::string& value)
{
	const size_t first = value.find_first_not_of(" \t\r\n\f\v");
	if (first == std::string::npos)
		return std::string();
	const size_t last = value.find_last_not_of(" \t\r\n\f\v");
	return value.substr(first, last - first + 1);
}

inline bool parseLegacyCsv(const std::string& text, const SessionSignature& session,
	std::vector<double>& output, std::string& diagnostic)
{
	std::vector<double> candidate;
	size_t begin = 0;
	while (begin <= text.size())
	{
		const size_t comma = text.find(',', begin);
		const size_t end = comma == std::string::npos ? text.size() : comma;
		const std::string field = trim(text.substr(begin, end - begin));
		double value = 0.0;
		if (!parseDecimal(field, value))
			return fail(diagnostic, "legacy VWAP CSV contains an invalid or non-finite number");
		candidate.push_back(value);
		if (comma == std::string::npos)
			break;
		begin = comma + 1;
	}

	if (candidate.size() != 241)
		return fail(diagnostic, "legacy VWAP CSV is accepted only with exactly 241 values");
	if (!isCanonicalLegacySession(session))
		return fail(diagnostic, "legacy VWAP CSV is supported only for the canonical 09:30-11:30,13:00-15:00 session");
	output.swap(candidate);
	diagnostic.clear();
	return true;
}

inline bool loadProfile(const std::string& text, WTSSessionInfo& session,
	std::vector<double>& output, std::string& diagnostic)
{
	SessionSignature signature;
	if (!makeSessionSignature(session, signature, diagnostic))
		return false;

	const std::string normalized = trim(text);
	if (normalized.empty())
		return fail(diagnostic, "VWAP profile input is empty");

	try
	{
		if (normalized[0] == '{' || normalized[0] == '[')
			return parseV2(normalized, signature, output, diagnostic);
		return parseLegacyCsv(normalized, signature, output, diagnostic);
	}
	catch (const std::exception&)
	{
		return fail(diagnostic, "VWAP profile parsing failed due to an exception");
	}
	catch (...)
	{
		return fail(diagnostic, "VWAP profile parsing failed due to an unknown exception");
	}
}

// This mapping intentionally accepts only HHMMSSmmm; it has no action/trading
// date input, so callers remain responsible for validating the tick's date pair.
inline bool mapActionTime(WTSSessionInfo& session, uint32_t action_time,
	size_t value_count, size_t& output_index, std::string& diagnostic)
{
	if (session.id() == nullptr || session.id()[0] == '\0')
		return fail(diagnostic, "VWAP time mapping requires a non-empty session id");
	if (!validateTradingSections(session, diagnostic))
		return false;

	const uint32_t millisecond = action_time % 1000;
	const uint32_t hhmmss = action_time / 1000;
	const uint32_t second = hhmmss % 100;
	const uint32_t hhmm = hhmmss / 100;
	const uint32_t minute = hhmm % 100;
	const uint32_t hour = hhmm / 100;
	if (second >= 60 || minute >= 60 || hour >= 24)
		return fail(diagnostic, "VWAP tick action_time is not a valid HHMMSSmmm value");

	if (session.isInAuctionTime(hhmm))
		return fail(diagnostic, "VWAP tick time falls in an unsupported auction interval");

	const uint32_t offset_time = session.offsetTime(hhmm, true);
	bool is_section_start = false;
	bool is_section_end = false;
	for (const WTSSessionInfo::TradingSection& section : session.getTradingSections())
	{
		is_section_start = is_section_start || section.first == offset_time;
		is_section_end = is_section_end || section.second == offset_time;
	}
	if (is_section_end && !is_section_start && (second != 0 || millisecond != 0))
		return fail(diagnostic, "VWAP section close includes only the exact HH:MM:00.000 endpoint");

	const uint32_t bucket = session.timeToMinutes(hhmm, false);
	if (bucket == INVALID_UINT32)
	{
		// For a zero-offset section ending at raw 2400, WTSSessionInfo stores
		// 2400 but timeToMinutes(0000) does not recognize the midnight alias.
		// action_time has no date, so do not guess whether 00:00 is that close.
		return fail(diagnostic, "VWAP tick time is outside the trading sections or is an ambiguous 24:00 alias");
	}
	if (bucket > session.getTradingMins() || static_cast<size_t>(bucket) >= value_count)
		return fail(diagnostic, "VWAP time bucket is outside the profile value range");

	output_index = static_cast<size_t>(bucket);
	diagnostic.clear();
	return true;
}
} // namespace vwap_profile
} // namespace wtp
