// L01 (lane Share/WTSUtils): Converter.hpp + charconv.hpp coverage.
// 疑似缺陷见 docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md
#include <unistd.h>
#include <gtest/gtest.h>

#include "Share/Converter.hpp"
#include "Share/charconv.hpp"
#include "WTSTools/CsvHelper.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::size_t boundedCStringLength(const char* text, std::size_t capacity)
{
	for (std::size_t i = 0; i < capacity; ++i)
	{
		if (text[i] == '\0')
			return i;
	}
	return capacity;
}

} // namespace

// ------------------------------------------------------------------ convert::to_str
TEST(l01_convert, to_str_into_caller_buffer)
{
	char buf[32];
	std::memset(buf, 0, sizeof(buf));
	const char* ret = convert::to_str(buf, sizeof(buf), 12345);
	EXPECT_EQ(ret, buf);
	// 零填充让数值内容检查有界；复用缓冲区时的终止符契约由下一用例检查。
	EXPECT_STREQ(buf, "12345");

	std::memset(buf, 0, sizeof(buf));
	EXPECT_STREQ(convert::to_str(buf, sizeof(buf), -42), "-42");

	std::memset(buf, 0, sizeof(buf));
	EXPECT_STREQ(convert::to_str(buf, sizeof(buf), 0), "0");

	std::memset(buf, 0, sizeof(buf));
	EXPECT_STREQ(convert::to_str(buf, sizeof(buf), 9223372036854775807LL), "9223372036854775807");
}

// Caller buffer overload returns a C string and must replace the previous value.
TEST(l01_convert, to_str_into_caller_buffer_terminates_each_result)
{
	char buf[32];
	std::memset(buf, 0xA5, sizeof(buf));
	buf[sizeof(buf) - 1] = '\0'; // bounded sentinel prevents any unterminated read
	convert::to_str(buf, sizeof(buf) - 1, 123456);
	EXPECT_EQ(6u, boundedCStringLength(buf, sizeof(buf)));
	EXPECT_EQ('\0', buf[6]);

	convert::to_str(buf, sizeof(buf) - 1, 7);
	EXPECT_EQ(1u, boundedCStringLength(buf, sizeof(buf)));
	EXPECT_EQ('7', buf[0]);
	EXPECT_EQ('\0', buf[1]);
	EXPECT_STREQ(buf, "7"); // the reserved final byte bounds the C-string read
}

TEST(l01_convert, to_str_caller_buffer_capacity_failure_is_empty)
{
	char oneByte[1] = { 'x' };
	EXPECT_EQ(convert::to_str(oneByte, sizeof(oneByte), 42), oneByte);
	EXPECT_EQ(oneByte[0], '\0');

	char tooSmall[4] = { 'x', 'y', 'z', '!' };
	EXPECT_EQ(convert::to_str(tooSmall, 3, 12345), tooSmall);
	EXPECT_EQ(boundedCStringLength(tooSmall, 3), 0u);
	EXPECT_EQ(tooSmall[0], '\0');
	EXPECT_EQ(tooSmall[3], '!'); // byte beyond the supplied capacity remains untouched

	char untouched[1] = { 'x' };
	EXPECT_EQ(convert::to_str(untouched, 0, 42), untouched);
	EXPECT_EQ(untouched[0], 'x');
	EXPECT_EQ(convert::to_str<int>(nullptr, 4, 42), nullptr);
}

TEST(l01_convert, to_str_internal_static_buffer_max_width)
{
	// 20 个字符正好覆盖到缓冲区最后一个写入位，后面的字节永远是初始零
	const char* s = convert::to_str<int64_t>(-9223372036854775807LL);
	EXPECT_STREQ(s, "-9223372036854775807");
}

TEST(l01_convert, to_str_internal_static_buffer_terminates_each_result)
{
	// Use one T so both calls share the same thread_local buffer.
	const char* first = convert::to_str<unsigned long long>(18446744073709551615ULL);
	EXPECT_EQ(20u, boundedCStringLength(first, 64));
	EXPECT_EQ('\0', first[20]);

	const char* second = convert::to_str<unsigned long long>(1ULL);
	EXPECT_EQ(1u, boundedCStringLength(second, 64));
	EXPECT_EQ('1', second[0]);
	EXPECT_EQ('\0', second[1]);
}

// ------------------------------------------------------------------ convert::from_str
TEST(l01_convert, from_str_cstr)
{
	EXPECT_EQ(convert::from_str<int32_t>("123"), 123);
	EXPECT_EQ(convert::from_str<int32_t>("-45"), -45);
	EXPECT_EQ(convert::from_str<int32_t>("0"), 0);
	// 遇到非法字符就停止，前面的部分仍然被解析
	EXPECT_EQ(convert::from_str<int32_t>("12abc"), 12);
	// 完全解析失败时 noexcept 吞掉错误码，返回零初始化的值
	EXPECT_EQ(convert::from_str<int32_t>("abc"), 0);
	EXPECT_EQ(convert::from_str<int32_t>(""), 0);
	EXPECT_EQ(convert::from_str<int32_t>(" 42"), 0);
	EXPECT_EQ(convert::from_str<uint32_t>("-1"), 0u);
	EXPECT_EQ(convert::from_str<int64_t>("9223372036854775807"), 9223372036854775807LL);
	// 溢出：from_chars 返回 result_out_of_range，ret 保持零初始化
	EXPECT_EQ(convert::from_str<int32_t>("99999999999999999999"), 0);
}

// Both overloads preserve std::from_chars prefix parsing: a valid numeric prefix
// is returned even when the input has a trailing non-numeric suffix.
TEST(l01_convert, from_str_std_string)
{
	const std::string positive = "4567";
	const std::string negative = "-7";
	const std::string prefix = "12abc";
	const std::string invalid = "ff";
	const std::string empty;
	const std::string overflow = "99999999999999999999";

	EXPECT_EQ(convert::from_str<int32_t>(positive), 4567);
	EXPECT_EQ(convert::from_str<int32_t>(negative), -7);
	EXPECT_EQ(convert::from_str<int32_t>(prefix), 12);
	EXPECT_EQ(convert::from_str<int32_t>(invalid), 0);
	EXPECT_EQ(convert::from_str<int32_t>(empty), 0);
	EXPECT_EQ(convert::from_str<int32_t>(overflow), 0);
	EXPECT_EQ(convert::from_str<int64_t>(std::string("1234567890123")), 1234567890123LL);
}

// ------------------------------------------------------------------ convert::to_intXXX
TEST(l01_convert, to_int_helpers)
{
	EXPECT_EQ(convert::to_int32("123"), 123);
	EXPECT_EQ(convert::to_int32(""), 0);
	EXPECT_EQ(convert::to_int32("0012"), 12);
	EXPECT_EQ(convert::to_uint32("4294967295"), 4294967295u);
	EXPECT_EQ(convert::to_int64("922337203685"), 922337203685LL);
	EXPECT_EQ(convert::to_uint64("1844674407370955161"), 1844674407370955161ull);
}

TEST(l01_convert, to_int32_parses_negative_decimal)
{
	// CsvReader::get_int32 is documented as a signed 32-bit integer reader and
	// delegates to this helper; a valid negative decimal must preserve its sign.
	EXPECT_EQ(convert::to_int32("-5"), -5);
}

TEST(l01_convert, to_int_helpers_validate_signs_and_boundaries)
{
	int32_t i32 = 123;
	EXPECT_TRUE(convert::try_to_int32("-5", i32));
	EXPECT_EQ(-5, i32);
	EXPECT_TRUE(convert::try_to_int32("+5", i32));
	EXPECT_EQ(5, i32);
	EXPECT_TRUE(convert::try_to_int32("2147483647", i32));
	EXPECT_EQ(INT32_MAX, i32);
	EXPECT_TRUE(convert::try_to_int32("-2147483648", i32));
	EXPECT_EQ(INT32_MIN, i32);
	EXPECT_FALSE(convert::try_to_int32("2147483648", i32));
	EXPECT_EQ(0, i32);
	EXPECT_FALSE(convert::try_to_int32("-2147483649", i32));
	EXPECT_FALSE(convert::try_to_int32("12x", i32));
	EXPECT_FALSE(convert::try_to_int32("+", i32));
	EXPECT_FALSE(convert::try_to_int32("", i32));
	EXPECT_FALSE(convert::try_to_int32(nullptr, i32));
	EXPECT_EQ(0, i32);
	EXPECT_EQ(0, convert::to_int32("12x"));
	EXPECT_EQ(0, convert::to_int32(nullptr));

	uint32_t u32 = 1;
	EXPECT_TRUE(convert::try_to_uint32("+42", u32));
	EXPECT_EQ(42u, u32);
	EXPECT_TRUE(convert::try_to_uint32("4294967295", u32));
	EXPECT_EQ(UINT32_MAX, u32);
	EXPECT_FALSE(convert::try_to_uint32("4294967296", u32));
	EXPECT_EQ(0u, u32);
	EXPECT_FALSE(convert::try_to_uint32("-1", u32));
	EXPECT_FALSE(convert::try_to_uint32("1a", u32));
	EXPECT_EQ(0u, convert::to_uint32("4294967296"));

	int64_t i64 = 0;
	EXPECT_TRUE(convert::try_to_int64("9223372036854775807", i64));
	EXPECT_EQ(INT64_MAX, i64);
	EXPECT_TRUE(convert::try_to_int64("-9223372036854775808", i64));
	EXPECT_EQ(INT64_MIN, i64);
	EXPECT_FALSE(convert::try_to_int64("9223372036854775808", i64));
	EXPECT_FALSE(convert::try_to_int64("-9223372036854775809", i64));
	EXPECT_EQ(0, i64);

	uint64_t u64 = 0;
	EXPECT_TRUE(convert::try_to_uint64("18446744073709551615", u64));
	EXPECT_EQ(UINT64_MAX, u64);
	EXPECT_FALSE(convert::try_to_uint64("18446744073709551616", u64));
	EXPECT_FALSE(convert::try_to_uint64("-0", u64));
	EXPECT_EQ(0u, u64);
	EXPECT_EQ(0u, convert::to_uint64("18446744073709551616"));
}

TEST(l01_convert, csv_reader_preserves_negative_int32)
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path() /
		(std::string("wondertrader_converter_negative_") + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(stamp) + ".csv");
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(output.good());
		output << "amount\n-123\n";
	}

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.string().c_str()));
	ASSERT_TRUE(reader.next_row());
	EXPECT_EQ(-123, reader.get_int32("amount"));

	std::error_code ec;
	std::filesystem::remove(path, ec);
}

static_assert(convert::to_int32("2024") == 2024, "to_int32 must stay constexpr");
static_assert(convert::to_int32("-2024") == -2024, "signed to_int32 must stay constexpr");
static_assert(convert::to_int64("-9223372036854775808") == INT64_MIN, "signed to_int64 must handle its minimum value constexpr");
static_assert(convert::to_uint64("10") == 10ull, "to_uint64 must stay constexpr");
static_assert(convert::to_int32("23", 1) == 123, "legacy accumulator overload must stay constexpr");

// ------------------------------------------------------------------ URLEncode
TEST(l01_charconv, url_encode_ascii)
{
	URLEncode plain("abc123");
	EXPECT_STREQ(static_cast<const char*>(plain), "abc123");

	URLEncode space("a b");
	EXPECT_STREQ(static_cast<const char*>(space), "a%20b");

	// '%' 本身不会被转义（见 bug 清单）
	URLEncode percent("%41");
	EXPECT_STREQ(static_cast<const char*>(percent), "%41");
}

TEST(l01_charconv, url_encode_non_ascii)
{
	const std::string utf8 = "\xE4\xB8\xAD";
	URLEncode enc(utf8.c_str());
	EXPECT_STREQ(static_cast<const char*>(enc), "%E4%B8%AD");

	URLEncode empty("");
	EXPECT_STREQ(static_cast<const char*>(empty), "");
}

// ------------------------------------------------------------------ URLDecode
TEST(l01_charconv, url_decode_basic)
{
	URLEncode enc("a b");
	URLDecode back(static_cast<const char*>(enc));
	EXPECT_STREQ(static_cast<const char*>(back), "a b");

	URLDecode plus("a+b");
	EXPECT_STREQ(static_cast<const char*>(plus), "a b");

	const std::string utf8 = "\xE4\xB8\xAD";
	URLDecode nonAscii("%E4%B8%AD");
	EXPECT_STREQ(static_cast<const char*>(nonAscii), utf8.c_str());
}

TEST(l01_charconv, url_decode_keeps_percent_for_safe_chars)
{
	// 落在“可不编码”白名单里的字符不会被还原（BUG-L01-004）
	URLDecode letter("%41");
	EXPECT_STREQ(static_cast<const char*>(letter), "%41");

	URLDecode slash("%2f");
	EXPECT_STREQ(static_cast<const char*>(slash), "%2f");

	// 不在白名单里的（如 %25='%'、%23='#'）会被还原
	URLDecode percent("%25");
	EXPECT_STREQ(static_cast<const char*>(percent), "%");

	URLDecode sharp("%23");
	EXPECT_STREQ(static_cast<const char*>(sharp), "#");

	// 非法/残缺的百分号序列原样保留
	URLDecode broken("%zz");
	EXPECT_STREQ(static_cast<const char*>(broken), "%zz");

	URLDecode trailing("abc%");
	EXPECT_STREQ(static_cast<const char*>(trailing), "abc%");

	URLDecode shortSeq("a%2");
	EXPECT_STREQ(static_cast<const char*>(shortSeq), "a%2");
}

// ------------------------------------------------------------------ EncodingHelper
TEST(l01_charconv, encoding_helper_pre_num)
{
	EXPECT_EQ(EncodingHelper::preNUm(0x00), 0);
	EXPECT_EQ(EncodingHelper::preNUm(0x7F), 0);
	EXPECT_EQ(EncodingHelper::preNUm(0x80), 1);
	EXPECT_EQ(EncodingHelper::preNUm(0xBF), 1);
	EXPECT_EQ(EncodingHelper::preNUm(0xC0), 2);
	EXPECT_EQ(EncodingHelper::preNUm(0xE4), 3);
	EXPECT_EQ(EncodingHelper::preNUm(0xF0), 4);
	EXPECT_EQ(EncodingHelper::preNUm(0xF8), 5);
	EXPECT_EQ(EncodingHelper::preNUm(0xFC), 6);
	// 0xFE -> 7、0xFF -> 8，超出 UTF-8 合法首字节范围但没有校验
	EXPECT_EQ(EncodingHelper::preNUm(0xFE), 7);
	EXPECT_EQ(EncodingHelper::preNUm(0xFF), 8);
}

TEST(l01_charconv, encoding_helper_is_utf8)
{
	std::vector<unsigned char> ascii{ 'a', 'b', 'c', '1' };
	EXPECT_TRUE(EncodingHelper::isUtf8(ascii.data(), ascii.size()));

	std::vector<unsigned char> empty;
	EXPECT_TRUE(EncodingHelper::isUtf8(empty.data(), empty.size()));

	// “中” UTF-8 = E4 B8 AD
	std::vector<unsigned char> zh{ 0xE4, 0xB8, 0xAD };
	EXPECT_TRUE(EncodingHelper::isUtf8(zh.data(), zh.size()));

	// 4 字节 emoji U+1F600 = F0 9F 98 80
	std::vector<unsigned char> emoji{ 0xF0, 0x9F, 0x98, 0x80 };
	EXPECT_TRUE(EncodingHelper::isUtf8(emoji.data(), emoji.size()));

	// GBK 的“中” = D6 D0，不是合法 UTF-8
	std::vector<unsigned char> gbk{ 0xD6, 0xD0 };
	EXPECT_FALSE(EncodingHelper::isUtf8(gbk.data(), gbk.size()));

	// 单独的后续字节
	std::vector<unsigned char> stray{ 0x80 };
	EXPECT_FALSE(EncodingHelper::isUtf8(stray.data(), stray.size()));

	// 后续字节不符合 10xxxxxx
	std::vector<unsigned char> broken{ 0xE4, 0xB8, 0x41 };
	EXPECT_FALSE(EncodingHelper::isUtf8(broken.data(), broken.size()));
}

TEST(l01_charconv, encoding_helper_is_gbk)
{
	std::vector<unsigned char> ascii{ 'a', 'b', 'c' };
	EXPECT_FALSE(EncodingHelper::isGBK(ascii.data(), ascii.size()));

	std::vector<unsigned char> empty;
	EXPECT_FALSE(EncodingHelper::isGBK(empty.data(), empty.size()));

	// GBK 双字节：首字节 0x81~0xFE，次字节 0x40~0xFE 且 != 0xF7
	std::vector<unsigned char> zh{ 0xD6, 0xD0 };
	EXPECT_TRUE(EncodingHelper::isGBK(zh.data(), zh.size()));

	std::vector<unsigned char> mixed{ 'x', 0xC4, 0xE3 };
	EXPECT_TRUE(EncodingHelper::isGBK(mixed.data(), mixed.size()));
}

// Invalid GBK pairs must terminate, and a trailing lead byte must not read
// beyond the supplied length.
TEST(l01_charconv, encoding_helper_is_gbk_rejects_malformed_without_stalling)
{
	std::vector<unsigned char> bad{ 0xFF, 0xFF };
	EXPECT_FALSE(EncodingHelper::isGBK(bad.data(), bad.size()));

	std::vector<unsigned char> f7{ 0xD6, 0xF7 };
	EXPECT_FALSE(EncodingHelper::isGBK(f7.data(), f7.size()));

	std::vector<unsigned char> low{ 0xD6, 0x20 };
	EXPECT_FALSE(EncodingHelper::isGBK(low.data(), low.size()));

	std::vector<unsigned char> loneLead{ 0xD6 };
	EXPECT_FALSE(EncodingHelper::isGBK(loneLead.data(), loneLead.size()));
	EXPECT_FALSE(EncodingHelper::isGBK(nullptr, 1));
	EXPECT_FALSE(EncodingHelper::isGBK(nullptr, 0));
}

// ------------------------------------------------------------------ UTF8toChar / ChartoUTF8
TEST(l01_charconv, utf8_to_char_pure_ascii)
{
	UTF8toChar conv("hello");
	EXPECT_STREQ(conv.c_str(), "hello");

	const std::string s = "world";
	UTF8toChar fromStd(s);
	EXPECT_STREQ(fromStd.c_str(), "world");
}

TEST(l01_charconv, char_to_utf8_pure_ascii)
{
	ChartoUTF8 conv("hello");
	EXPECT_STREQ(conv.c_str(), "hello");

	const std::string s = "abc-123";
	ChartoUTF8 fromStd(s);
	EXPECT_STREQ(fromStd.c_str(), "abc-123");
}
