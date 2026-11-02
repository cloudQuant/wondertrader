#include "WtDataStorageAD/LMDBBarKeyV2.hpp"
#include "WtDataStorageAD/LMDBKeys.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace keyv2 = wt_ad_bar_key_v2;

#define REQUIRE(expression) do { \
	if (!(expression)) { std::cerr << __func__ << ':' << __LINE__ << ": " #expression " failed\n"; return false; } \
} while (false)

using V1Key = std::array<std::uint8_t, keyv2::v1_size>;

static void putInteger(std::uint8_t* output, std::size_t width,
	std::uint64_t value, bool little)
{
	for (std::size_t i = 0; i < width; ++i)
		output[i] = static_cast<std::uint8_t>(value >> ((little ? i : width - 1 - i) * 8));
}

static V1Key oldKey(std::string_view exchange, std::string_view code,
	std::uint64_t time, keyv2::LegacySourceHost host)
{
	V1Key key{};
	std::memcpy(key.data(), exchange.data(), exchange.size());
	std::memcpy(key.data() + keyv2::exchange_width, code.data(), code.size());
	// V1 applied reverseEndian before placing the integer in native storage.
	putInteger(key.data() + 48, 4, static_cast<std::uint32_t>(time),
		host == keyv2::LegacySourceHost::BigEndian);
	return key;
}

static std::vector<std::uint8_t> oldValue(std::uint64_t time,
	keyv2::LegacySourceHost host, keyv2::LegacyValueLayout layout)
{
	const bool packed = layout == keyv2::LegacyValueLayout::Packed64;
	std::vector<std::uint8_t> bytes(packed ? 84 : 88, 0);
	putInteger(bytes.data() + (packed ? 4 : 8), 8, time,
		host == keyv2::LegacySourceHost::LittleEndian);
	return bytes;
}

static keyv2::LegacyEvidence evidenceFor(std::uint64_t time,
	keyv2::LegacySourceHost host = keyv2::LegacySourceHost::LittleEndian,
	keyv2::LegacyValueLayout layout = keyv2::LegacyValueLayout::Aligned64)
{
	return {host, layout, keyv2::TimeEvidence::IndependentlyVerified, time};
}

static bool roundTripBoundaryIdentities()
{
	const std::string exchange(15, 'E'), code(31, 'C');
	keyv2::Key key{};
	REQUIRE(keyv2::encode(exchange, code, std::numeric_limits<std::uint64_t>::max(), key) == keyv2::Status::Ok);
	keyv2::DecodedKey decoded;
	REQUIRE(keyv2::decode(key.data(), key.size(), decoded) == keyv2::Status::Ok);
	REQUIRE(decoded.exchange == exchange);
	REQUIRE(decoded.code == code);
	REQUIRE(decoded.time == std::numeric_limits<std::uint64_t>::max());
	REQUIRE(key.size() == 56);
	return true;
}

static bool encodeRejectsInvalidAndPreservesOutput()
{
	keyv2::Key key; key.fill(0xA5); const auto before = key;
	REQUIRE(keyv2::encode("", "rb", 1, key) == keyv2::Status::InvalidIdentity);
	REQUIRE(key == before);
	REQUIRE(keyv2::encode(std::string(16, 'E'), "rb", 1, key) == keyv2::Status::InvalidIdentity);
	REQUIRE(key == before);
	REQUIRE(keyv2::encode("SHFE", std::string(32, 'C'), 1, key) == keyv2::Status::InvalidIdentity);
	REQUIRE(key == before);
	const std::string invalid("A\0B", 3);
	REQUIRE(keyv2::encode(invalid, "rb", 1, key) == keyv2::Status::InvalidIdentity);
	REQUIRE(key == before);
	REQUIRE(keyv2::encode("SHFE", invalid, 1, key) == keyv2::Status::InvalidIdentity);
	REQUIRE(key == before);
	return true;
}

static bool byteOrderingAndInclusiveEndpoint()
{
	const std::uint64_t t2032 = (20321231ULL - 19900000ULL) * 10000ULL + 2359;
	const std::uint64_t t2033 = (20330101ULL - 19900000ULL) * 10000ULL + 1000;
	REQUIRE(t2032 < 0xffffffffULL && t2033 > 0xffffffffULL);
	const std::array<std::uint64_t, 8> times = {0, 1, t2032, 0xfffffffeULL,
		0xffffffffULL, 0x100000000ULL, t2033, std::numeric_limits<std::uint64_t>::max()};
	keyv2::Key previous{};
	for (std::size_t i = 0; i < times.size(); ++i)
	{
		keyv2::Key key{};
		REQUIRE(keyv2::encode("SHFE", "rb2409", times[i], key) == keyv2::Status::Ok);
		if (i != 0) REQUIRE(std::memcmp(previous.data(), key.data(), key.size()) < 0);
		keyv2::DecodedKey decoded;
		REQUIRE(keyv2::decode(key.data(), key.size(), decoded) == keyv2::Status::Ok);
		REQUIRE(decoded.time == times[i]);
		previous = key;
	}
	keyv2::Key sentinel{}, next{};
	REQUIRE(keyv2::encode("SHFE", "rb2409", 0xffffffffULL, sentinel) == keyv2::Status::Ok);
	REQUIRE(keyv2::encode("SHFE", "rb2409", 0x100000000ULL, next) == keyv2::Status::Ok);
	REQUIRE(std::memcmp(sentinel.data(), next.data(), sentinel.size()) < 0);
	REQUIRE(sentinel[52] == 0xff && sentinel[55] == 0xff);
	return true;
}

static bool distinctIdentityOrdering()
{
	keyv2::Key one{}, two{}, three{};
	REQUIRE(keyv2::encode("A", "X", UINT64_MAX, one) == keyv2::Status::Ok);
	REQUIRE(keyv2::encode("A", "XY", 0, two) == keyv2::Status::Ok);
	REQUIRE(keyv2::encode("B", "A", 0, three) == keyv2::Status::Ok);
	REQUIRE(std::memcmp(one.data(), two.data(), one.size()) < 0);
	REQUIRE(std::memcmp(two.data(), three.data(), one.size()) < 0);
	return true;
}

static bool decodeRejectsCorruptAndPreservesOutput()
{
	keyv2::DecodedKey out{"original-exchange", "original-code", 123};
	auto untouched = [&] { return out.exchange == "original-exchange" && out.code == "original-code" && out.time == 123; };
	keyv2::Key key{};
	REQUIRE(keyv2::encode("A", "B", 1, key) == keyv2::Status::Ok);
	REQUIRE(keyv2::decode(nullptr, key.size(), out) == keyv2::Status::InvalidKeySize && untouched());
	REQUIRE(keyv2::decode(key.data(), key.size() - 1, out) == keyv2::Status::InvalidKeySize && untouched());
	key[5] = 'X';
	REQUIRE(keyv2::decode(key.data(), key.size(), out) == keyv2::Status::NonCanonicalIdentity && untouched());
	key[5] = 0; std::fill(key.begin(), key.begin() + 16, 'A');
	REQUIRE(keyv2::decode(key.data(), key.size(), out) == keyv2::Status::NonCanonicalIdentity && untouched());
	return true;
}

static bool migratesVerifiedTimesInBothSourceOrders()
{
	const std::uint64_t t2032 = (20321231ULL - 19900000ULL) * 10000ULL + 2359;
	const std::uint64_t t2033 = (20330101ULL - 19900000ULL) * 10000ULL + 1000;
	for (const auto host : {keyv2::LegacySourceHost::LittleEndian, keyv2::LegacySourceHost::BigEndian})
		for (const auto layout : {keyv2::LegacyValueLayout::Aligned64, keyv2::LegacyValueLayout::Packed64})
			for (const auto time : {t2032, 0xffffffffULL, t2033})
			{
				auto v1 = oldKey("SHFE", "rb2409", time, host);
				auto value = oldValue(time, host, layout);
				auto evidence = evidenceFor(time, host, layout);
				keyv2::Key output{}; output.fill(0xA5);
				REQUIRE(keyv2::convertLegacyCandidate(v1.data(), v1.size(), value.data(), value.size(),
					"SHFE", "rb2409", evidence, output) == keyv2::Status::Ok);
				keyv2::DecodedKey decoded;
				REQUIRE(keyv2::decode(output.data(), output.size(), decoded) == keyv2::Status::Ok);
				REQUIRE(decoded.exchange == "SHFE" && decoded.code == "rb2409" && decoded.time == time);
			}
	return true;
}

static bool migrationRejectsUntrustedOrMismatched()
{
	const std::uint64_t time = 4301011000ULL;
	auto host = keyv2::LegacySourceHost::LittleEndian;
	auto v1 = oldKey("SHFE", "rb2409", time, host);
	auto value = oldValue(time, host, keyv2::LegacyValueLayout::Aligned64);
	auto evidence = evidenceFor(time);
	keyv2::Key out; out.fill(0xE3); const auto before = out;
	auto run = [&](const void* k, std::size_t ks, const void* v, std::size_t vs,
		std::string_view e, std::string_view c, const keyv2::LegacyEvidence& ev) {
		return keyv2::convertLegacyCandidate(k, ks, v, vs, e, c, ev, out);
	};
	REQUIRE(run(v1.data(), v1.size() - 1, value.data(), value.size(), "SHFE", "rb2409", evidence) == keyv2::Status::InvalidKeySize && out == before);
	REQUIRE(run(nullptr, v1.size(), value.data(), value.size(), "SHFE", "rb2409", evidence) == keyv2::Status::InvalidKeySize && out == before);
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "", "rb2409", evidence) == keyv2::Status::InvalidIdentity && out == before);
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", std::string(32, 'X'), evidence) == keyv2::Status::InvalidIdentity && out == before);
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "DCE", "rb2409", evidence) == keyv2::Status::IdentityMismatch && out == before);
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2410", evidence) == keyv2::Status::IdentityMismatch && out == before);
	REQUIRE(run(v1.data(), v1.size(), nullptr, value.size(), "SHFE", "rb2409", evidence) == keyv2::Status::InvalidValueSize && out == before);
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size() - 1, "SHFE", "rb2409", evidence) == keyv2::Status::InvalidValueSize && out == before);
	auto bad = evidence; bad.sourceHost = keyv2::LegacySourceHost::Unknown;
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", bad) == keyv2::Status::UnsupportedSourceByteOrder && out == before);
	bad = evidence; bad.valueLayout = keyv2::LegacyValueLayout::Old32;
	REQUIRE(run(v1.data(), v1.size(), value.data(), 68, "SHFE", "rb2409", bad) == keyv2::Status::UnsupportedValueLayout && out == before);
	bad = evidence; bad.valueLayout = keyv2::LegacyValueLayout::Unknown;
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", bad) == keyv2::Status::UnsupportedValueLayout && out == before);
	bad = evidence; bad.timeEvidence = keyv2::TimeEvidence::Missing;
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", bad) == keyv2::Status::MissingIndependentTimeEvidence && out == before);
	bad = evidence; bad.independentlyVerifiedTime++;
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", bad) == keyv2::Status::ValueTimeMismatch && out == before);
	++v1[51];
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", evidence) == keyv2::Status::LegacyKeyTimeMismatch && out == before);
	v1[5] = 'X';
	REQUIRE(run(v1.data(), v1.size(), value.data(), value.size(), "SHFE", "rb2409", evidence) == keyv2::Status::NonCanonicalIdentity && out == before);
	return true;
}

static bool wrappedCollisionRequiresIndependentEvidence()
{
	const std::uint64_t newer = 4301011000ULL;
	const std::uint64_t earlier = newer - 0x100000000ULL;
	auto host = keyv2::LegacySourceHost::LittleEndian;
	auto a = oldKey("DCE", "m2409", earlier, host);
	auto b = oldKey("DCE", "m2409", newer, host);
	REQUIRE(a == b); // An overwritten V1 value cannot be reconstructed here.
	auto value = oldValue(newer, host, keyv2::LegacyValueLayout::Aligned64);
	keyv2::Key out; out.fill(0x71); const auto before = out;
	auto missing = evidenceFor(newer); missing.timeEvidence = keyv2::TimeEvidence::Missing;
	REQUIRE(keyv2::convertLegacyCandidate(b.data(), b.size(), value.data(), value.size(), "DCE", "m2409", missing, out)
		== keyv2::Status::MissingIndependentTimeEvidence && out == before);
	auto verified = evidenceFor(newer);
	REQUIRE(keyv2::convertLegacyCandidate(b.data(), b.size(), value.data(), value.size(), "DCE", "m2409", verified, out)
		== keyv2::Status::Ok);
	keyv2::DecodedKey decoded;
	REQUIRE(keyv2::decode(out.data(), out.size(), decoded) == keyv2::Status::Ok && decoded.time == newer);
	return true;
}

static bool actualLegacyStructsAreHandledExplicitly()
{
	static_assert(sizeof(LMDBBarKey) == keyv2::v1_size, "legacy key size mismatch");
	const std::uint64_t time = (20330101ULL - 19900000ULL) * 10000ULL + 1000;
	const std::uint16_t endian = 1;
	const bool isLittle = *reinterpret_cast<const std::uint8_t*>(&endian) == 1;
	auto host = isLittle ? keyv2::LegacySourceHost::LittleEndian : keyv2::LegacySourceHost::BigEndian;
	LMDBBarKey v1("SHFE", "rb2409", static_cast<std::uint32_t>(time));
	auto expectedBytes = oldKey("SHFE", "rb2409", time, host);
	REQUIRE(std::memcmp(&v1, expectedBytes.data(), expectedBytes.size()) == 0);
	wtp::WTSBarStruct value;
	value.date = 20330101;
	value.time = time;
	auto evidence = evidenceFor(time, host);
	keyv2::Key output{};
	REQUIRE(keyv2::convertLegacyCandidate(&v1, sizeof(v1), &value, sizeof(value),
		"SHFE", "rb2409", evidence, output) == keyv2::Status::Ok);
	keyv2::DecodedKey decoded;
	REQUIRE(keyv2::decode(output.data(), output.size(), decoded) == keyv2::Status::Ok && decoded.time == time);
	wtp::WTSBarStructOld oldValue32;
	oldValue32.date = 20330101;
	oldValue32.time = static_cast<std::uint32_t>(time);
	const auto before = output;
	evidence.valueLayout = keyv2::LegacyValueLayout::Old32;
	REQUIRE(keyv2::convertLegacyCandidate(&v1, sizeof(v1), &oldValue32, sizeof(oldValue32),
		"SHFE", "rb2409", evidence, output) == keyv2::Status::UnsupportedValueLayout && output == before);
	return true;
}

int main()
{
	using Test = bool (*)();
	const std::array<std::pair<const char*, Test>, 9> tests = {{
		{"roundTripBoundaryIdentities", roundTripBoundaryIdentities},
		{"encodeRejectsInvalidAndPreservesOutput", encodeRejectsInvalidAndPreservesOutput},
		{"byteOrderingAndInclusiveEndpoint", byteOrderingAndInclusiveEndpoint},
		{"distinctIdentityOrdering", distinctIdentityOrdering},
		{"decodeRejectsCorruptAndPreservesOutput", decodeRejectsCorruptAndPreservesOutput},
		{"migratesVerifiedTimesInBothSourceOrders", migratesVerifiedTimesInBothSourceOrders},
		{"migrationRejectsUntrustedOrMismatched", migrationRejectsUntrustedOrMismatched},
		{"wrappedCollisionRequiresIndependentEvidence", wrappedCollisionRequiresIndependentEvidence},
		{"actualLegacyStructsAreHandledExplicitly", actualLegacyStructsAreHandledExplicitly}
	}};
	for (const auto& test : tests)
	{
		if (!test.second()) return 1;
		std::cout << "PASS " << test.first << '\n';
	}
	return 0;
}
