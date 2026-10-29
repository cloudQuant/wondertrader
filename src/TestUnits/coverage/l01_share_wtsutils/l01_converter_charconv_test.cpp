// L01 (lane Share/WTSUtils): Converter.hpp + charconv.hpp coverage.
// 疑似缺陷见 docs/迭代计划/迭代01/bugs/l01_share_wtsutils.md
#include <gtest/gtest.h>

#include "Share/Converter.hpp"
#include "Share/charconv.hpp"

#include <cstdint>
#include <cstring>
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

TEST(l01_convert, to_str_internal_static_buffer_max_width)
{
	// 20 个字符正好覆盖到缓冲区最后一个写入位，后面的字节永远是初始零
	const char* s = convert::to_str<int64_t>(-9223372036854775807LL);
	EXPECT_STREQ(s, "-9223372036854775807");
}

TEST(l01_convert, to_str_internal_static_buffer_terminates_each_result)
{
	// Use one T so both calls share the same thread_local buffer; the 64-byte bound
	// keeps the check safe even if the implementation omits the required terminator.
	const char* first = convert::to_str<unsigned long long>(9876543210ULL);
	EXPECT_EQ(10u, boundedCStringLength(first, 64));
	EXPECT_EQ('\0', first[10]);

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

// BUG-L01-011（Blocker）：convert::from_str<T>(std::string&) 这条重载在 libc++ 下
// 根本无法实例化 —— Converter.hpp:48 把 std::string::iterator（__wrap_iter<char*>）
// 直接传给要求 const char* 的 std::from_chars，编译期即报错：
//   no matching function for call to 'from_chars'
// 因此这里不能写任何调用该重载的用例，否则整个 TU 编译失败。
// 修复方向（不在本迭代实施）：改用 str.c_str() / &str[0] + str.size()，或改成
// 模板参数为 const std::string& 并用 std::from_chars(str.data(), str.data()+str.size(), ret)。
// 同时注意该重载里是 `T ret;`（未初始化），失败路径读取的是不确定值，而 char* 版本是 `T ret{}`。
TEST(l01_convert, const_char_pointer_overload_parses_signed_and_invalid_input)
{
	// 只验证 const char* 重载，不能据此证明 std::string 重载可以编译。
	EXPECT_EQ(convert::from_str<int32_t>("4567"), 4567);
	EXPECT_EQ(convert::from_str<int32_t>("-7"), -7);
	EXPECT_EQ(convert::from_str<int64_t>("1234567890123"), 1234567890123LL);
	EXPECT_EQ(convert::from_str<int32_t>("ff"), 0);
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

static_assert(convert::to_int32("2024") == 2024, "to_int32 must stay constexpr");
static_assert(convert::to_uint64("10") == 10ull, "to_uint64 must stay constexpr");

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

// BUG-L01-005：isGBK 在“高位字节但双字节校验失败”时既不返回 true 也不推进 i，
// 会死循环；同时 data[i+1] 在最后一个字节时越界读。修复前不能启用。
// 该缺陷类包含所有走失败分支的输入，不只是 0xFF —— 次字节 ==0xF7、<0x40 同样挂死
// （实测：启用态的 encoding_helper_is_gbk 卡在 {0xD6,0xF7} 上，CPU 空转 9 分钟）。
TEST(l01_charconv, DISABLED_encoding_helper_is_gbk_hangs)
{
	std::vector<unsigned char> bad{ 0xFF, 0xFF };
	EXPECT_FALSE(EncodingHelper::isGBK(bad.data(), bad.size()));

	std::vector<unsigned char> f7{ 0xD6, 0xF7 };
	EXPECT_FALSE(EncodingHelper::isGBK(f7.data(), f7.size()));

	std::vector<unsigned char> low{ 0xD6, 0x20 };
	EXPECT_FALSE(EncodingHelper::isGBK(low.data(), low.size()));
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
