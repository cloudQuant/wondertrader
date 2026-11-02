#pragma once

// Standalone codec for a future, separately versioned minute-bar key space.
// No existing LMDB database uses this format yet. Never mix these keys with
// 52-byte V1 keys in the same ordered key space.

#include "../Includes/WTSStruct.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace wt_ad_bar_key_v2
{
constexpr std::size_t exchange_width = 16;
constexpr std::size_t code_width = 32;
constexpr std::size_t v1_size = exchange_width + code_width + 4;
constexpr std::size_t v2_size = exchange_width + code_width + 8;

static_assert(MAX_EXCHANGE_LENGTH == exchange_width, "V1 exchange width changed");
static_assert(MAX_INSTRUMENT_LENGTH == code_width, "V1 instrument width changed");
static_assert(sizeof(wtp::WTSBarStruct) == 88, "current bar value ABI changed");
static_assert(offsetof(wtp::WTSBarStruct, time) == 8, "current bar time offset changed");
static_assert(sizeof(wtp::WTSBarStructOld) == 68, "old 32-bit bar value ABI changed");

using Key = std::array<std::uint8_t, v2_size>;

struct DecodedKey
{
	std::string exchange;
	std::string code;
	std::uint64_t time = 0;
};

enum class Status
{
	Ok,
	InvalidIdentity,
	InvalidKeySize,
	NonCanonicalIdentity,
	IdentityMismatch,
	UnsupportedSourceByteOrder,
	UnsupportedValueLayout,
	InvalidValueSize,
	MissingIndependentTimeEvidence,
	ValueTimeMismatch,
	LegacyKeyTimeMismatch,
	OutOfMemory
};

// V1 reversed the integer bytes unconditionally before storing a packed
// struct. The byte order of its keys and raw values therefore depends on the
// source host, not on the host that is doing a later migration.
enum class LegacySourceHost { Unknown, LittleEndian, BigEndian };
enum class LegacyValueLayout { Unknown, Old32, Packed64, Aligned64 };
enum class TimeEvidence { Missing, IndependentlyVerified };

struct LegacyEvidence
{
	LegacySourceHost sourceHost = LegacySourceHost::Unknown;
	LegacyValueLayout valueLayout = LegacyValueLayout::Unknown;
	TimeEvidence timeEvidence = TimeEvidence::Missing;
	std::uint64_t independentlyVerifiedTime = 0;
};

namespace detail
{
inline bool validIdentity(std::string_view text, std::size_t width) noexcept
{
	return !text.empty() && text.size() < width && text.find('\0') == std::string_view::npos;
}

inline bool canonicalField(const std::uint8_t* bytes, std::size_t width,
	std::size_t& length) noexcept
{
	length = 0;
	while (length < width && bytes[length] != 0)
		++length;
	if (length == 0 || length == width)
		return false;
	for (std::size_t i = length + 1; i < width; ++i)
		if (bytes[i] != 0)
			return false;
	return true;
}

inline std::uint64_t readBigEndian(const std::uint8_t* data, std::size_t size) noexcept
{
	std::uint64_t result = 0;
	for (std::size_t i = 0; i < size; ++i)
		result = (result << 8) | data[i];
	return result;
}

inline std::uint64_t readLittleEndian(const std::uint8_t* data, std::size_t size) noexcept
{
	std::uint64_t result = 0;
	for (std::size_t i = size; i > 0; --i)
		result = (result << 8) | data[i - 1];
	return result;
}
} // namespace detail

// Failure leaves output unchanged. Text fields are nonempty, NUL-free and
// leave room for a terminator; their remaining bytes are canonically zeroed.
inline Status encode(std::string_view exchange, std::string_view code,
	std::uint64_t time, Key& output) noexcept
{
	if (!detail::validIdentity(exchange, exchange_width) ||
		!detail::validIdentity(code, code_width))
		return Status::InvalidIdentity;
	Key candidate{};
	std::memcpy(candidate.data(), exchange.data(), exchange.size());
	std::memcpy(candidate.data() + exchange_width, code.data(), code.size());
	for (std::size_t i = 0; i < 8; ++i)
		candidate[exchange_width + code_width + i] =
			static_cast<std::uint8_t>(time >> ((7 - i) * 8));
	output = candidate;
	return Status::Ok;
}

inline Status decode(const void* bytes, std::size_t size, DecodedKey& output) noexcept
{
	if (bytes == nullptr || size != v2_size)
		return Status::InvalidKeySize;
	const auto* key = static_cast<const std::uint8_t*>(bytes);
	std::size_t exchangeSize = 0, codeSize = 0;
	if (!detail::canonicalField(key, exchange_width, exchangeSize) ||
		!detail::canonicalField(key + exchange_width, code_width, codeSize))
		return Status::NonCanonicalIdentity;
	try
	{
		DecodedKey candidate;
		candidate.exchange.assign(reinterpret_cast<const char*>(key), exchangeSize);
		candidate.code.assign(reinterpret_cast<const char*>(key + exchange_width), codeSize);
		candidate.time = detail::readBigEndian(key + exchange_width + code_width, 8);
		static_assert(std::is_nothrow_move_assignable<DecodedKey>::value,
			"output publication must not partially mutate on failure");
		output = std::move(candidate);
		return Status::Ok;
	}
	catch (const std::bad_alloc&)
	{
		return Status::OutOfMemory;
	}
}

// Converts one candidate record only; it does not perform, approve or commit a
// database migration. The independent time must come from evidence other than
// the V1 key and value being converted. Matching these bytes cannot establish
// that a previous same-key record was not overwritten. Unsupported/unknown
// historical layouts, source byte order or missing evidence fail closed.
// The output is untouched on every non-Ok return.
inline Status convertLegacyCandidate(const void* keyBytes, std::size_t keySize,
	const void* valueBytes, std::size_t valueSize,
	std::string_view expectedExchange, std::string_view expectedCode,
	const LegacyEvidence& evidence, Key& output) noexcept
{
	if (!detail::validIdentity(expectedExchange, exchange_width) ||
		!detail::validIdentity(expectedCode, code_width))
		return Status::InvalidIdentity;
	if (keyBytes == nullptr || keySize != v1_size)
		return Status::InvalidKeySize;
	const auto* key = static_cast<const std::uint8_t*>(keyBytes);
	std::size_t exchangeSize = 0, codeSize = 0;
	if (!detail::canonicalField(key, exchange_width, exchangeSize) ||
		!detail::canonicalField(key + exchange_width, code_width, codeSize))
		return Status::NonCanonicalIdentity;
	if (expectedExchange.size() != exchangeSize || expectedCode.size() != codeSize ||
		std::memcmp(key, expectedExchange.data(), exchangeSize) != 0 ||
		std::memcmp(key + exchange_width, expectedCode.data(), codeSize) != 0)
		return Status::IdentityMismatch;
	if (evidence.sourceHost != LegacySourceHost::LittleEndian &&
		evidence.sourceHost != LegacySourceHost::BigEndian)
		return Status::UnsupportedSourceByteOrder;
	std::size_t valueTimeOffset = 0, expectedValueSize = 0;
	switch (evidence.valueLayout)
	{
	case LegacyValueLayout::Aligned64: valueTimeOffset = 8; expectedValueSize = 88; break;
	case LegacyValueLayout::Packed64: valueTimeOffset = 4; expectedValueSize = 84; break;
	default: return Status::UnsupportedValueLayout;
	}
	if (valueBytes == nullptr || valueSize != expectedValueSize)
		return Status::InvalidValueSize;
	if (evidence.timeEvidence != TimeEvidence::IndependentlyVerified)
		return Status::MissingIndependentTimeEvidence;
	const auto* value = static_cast<const std::uint8_t*>(valueBytes);
	const bool sourceLittle = evidence.sourceHost == LegacySourceHost::LittleEndian;
	const std::uint64_t valueTime = sourceLittle
		? detail::readLittleEndian(value + valueTimeOffset, 8)
		: detail::readBigEndian(value + valueTimeOffset, 8);
	if (valueTime != evidence.independentlyVerifiedTime)
		return Status::ValueTimeMismatch;
	const auto* tail = key + exchange_width + code_width;
	const std::uint64_t legacyTail = sourceLittle
		? detail::readBigEndian(tail, 4)
		: detail::readLittleEndian(tail, 4);
	if (legacyTail != static_cast<std::uint32_t>(valueTime))
		return Status::LegacyKeyTimeMismatch;
	return encode(expectedExchange, expectedCode, valueTime, output);
}
} // namespace wt_ad_bar_key_v2
