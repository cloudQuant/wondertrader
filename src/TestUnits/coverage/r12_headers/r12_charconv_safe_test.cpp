// Lane R12 - src/Share/charconv.hpp 编码转换（安全子集）
// 覆盖：UTF8toChar / ChartoUTF8 的空指针、空串、纯 ASCII 直通分支与不可转换输入，
//       EncodingHelper::preNUm 首码点计数、isUtf8 的合法/非法字节序列。
// 明确回避：isGBK（charconv.hpp:560-582 有死循环 + 越界读，已知缺陷），
//           以及任何需要本机 libiconv 支持 gb2312 别名才能预测输出的期望值（放 DISABLED_）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Share/charconv.hpp"

TEST(r12_charconv_utf8tochar, ascii_passthrough_and_empty_inputs)
{
	// 纯 ASCII：不分配内存，直接复用输入指针内容
	UTF8toChar plain("SHFE.rb2405");
	EXPECT_STREQ(plain.c_str(), "SHFE.rb2405");
	EXPECT_STREQ(static_cast<const char*>(plain), "SHFE.rb2405");

	UTF8toChar digits("1234567890");
	EXPECT_STREQ(digits.c_str(), "1234567890");

	// 空串：走空字符串常量分支
	UTF8toChar empty("");
	EXPECT_STREQ(empty.c_str(), "");

	// 空指针：输出同样是空指针
	UTF8toChar nullIn((const char*)NULL);
	EXPECT_EQ(nullIn.c_str(), nullptr);

	// std::string 构造体重载
	std::string s = "abc-XYZ_.";
	UTF8toChar fromStr(s);
	EXPECT_STREQ(fromStr.c_str(), s.c_str());
}

TEST(r12_charconv_utf8tochar, unconvertible_input_yields_empty_buffer)
{
	// 0xFF/0xFE 不是合法 UTF-8 首码点，因此转换结果是空串。
	unsigned char bad[] = { 0xFF, 0xFE, 0x00 };
	UTF8toChar conv((const char*)bad);
	ASSERT_NE(conv.c_str(), nullptr);
	EXPECT_STREQ(conv.c_str(), "");

	// 单字节 0x80 同样非法
	unsigned char one[] = { 0x80, 0x00 };
	UTF8toChar conv2((const char*)one);
	EXPECT_STREQ(conv2.c_str(), "");
}

TEST(r12_charconv_chartoutf8, ascii_passthrough_and_empty_inputs)
{
	ChartoUTF8 plain("DCE.m2409");
	EXPECT_STREQ(plain.c_str(), "DCE.m2409");
	ChartoUTF8 empty("");
	EXPECT_STREQ(empty.c_str(), "");
	ChartoUTF8 nullIn((const char*)NULL);
	EXPECT_EQ(nullIn.c_str(), nullptr);
	std::string s = "wondertrader";
	ChartoUTF8 fromStr(s);
	EXPECT_STREQ(fromStr.c_str(), s.c_str());
}

TEST(r12_charconv_chartoutf8, unconvertible_input_yields_empty_buffer)
{
	// 0xFF 不是合法 GB2312 首字节，因此转换结果是空串。
	unsigned char bad[] = { 0xFF, 0x41, 0x00 };
	ChartoUTF8 conv((const char*)bad);
	ASSERT_NE(conv.c_str(), nullptr);
	EXPECT_STREQ(conv.c_str(), "");
}

#ifndef _MSC_VER
TEST(r12_charconv_iconv, empty_and_valid_utf8_gb2312_round_trip)
{
	char emptyOutput[1] = { 'x' };
	EXPECT_TRUE(wt_charconv_detail::convert("UTF-8", "UTF-8", "", 0,
		emptyOutput, sizeof(emptyOutput)));
	EXPECT_EQ('\0', emptyOutput[0]);

	const char* utf8 = "\xe4\xb8\xad"; // 中
	wt_charconv_detail::IconvDescriptor probe(iconv_open("gb2312", "utf-8"));
	if (!probe.valid())
		GTEST_SKIP() << "host iconv does not provide the gb2312 encoding alias";
	wt_charconv_detail::IconvDescriptor reverseProbe(iconv_open("utf-8", "gb2312"));
	if (!reverseProbe.valid())
		GTEST_SKIP() << "host iconv does not provide the reverse gb2312 encoding alias";

	char gb2312[8] = { 0 };
	ASSERT_TRUE(wt_charconv_detail::convert("gb2312", "utf-8", utf8,
		std::strlen(utf8), gb2312, sizeof(gb2312)));
	const char expectedGb2312[] = { static_cast<char>(0xD6), static_cast<char>(0xD0), '\0' };
	EXPECT_EQ(0, std::memcmp(gb2312, expectedGb2312, sizeof(expectedGb2312)));

	char roundTrip[8] = { 0 };
	ASSERT_TRUE(wt_charconv_detail::convert("utf-8", "gb2312", gb2312,
		2, roundTrip, sizeof(roundTrip)));
	EXPECT_STREQ(utf8, roundTrip);

	UTF8toChar local(utf8);
	ASSERT_NE(local.c_str(), nullptr);
	EXPECT_EQ(0, std::memcmp(local.c_str(), expectedGb2312, sizeof(expectedGb2312)));
	ChartoUTF8 back(gb2312);
	ASSERT_NE(back.c_str(), nullptr);
	EXPECT_STREQ(utf8, back.c_str());
}

TEST(r12_charconv_iconv, invalid_incomplete_and_embedded_nul_inputs_are_terminated)
{
	const char* invalid = "\xff";
	char invalidOutput[8];
	std::memset(invalidOutput, 'x', sizeof(invalidOutput));
	EXPECT_FALSE(wt_charconv_detail::convert("UTF-8", "UTF-8", invalid, 1,
		invalidOutput, sizeof(invalidOutput)));
	EXPECT_STREQ("", invalidOutput);
	const char validPrefixThenInvalid[] = { 'A', static_cast<char>(0xFF) };
	char prefixOutput[8];
	std::memset(prefixOutput, 'x', sizeof(prefixOutput));
	EXPECT_FALSE(wt_charconv_detail::convert("UTF-8", "UTF-8", validPrefixThenInvalid,
		sizeof(validPrefixThenInvalid), prefixOutput, sizeof(prefixOutput)));
	EXPECT_STREQ("A", prefixOutput);

	const char* incomplete = "\xe2\x82";
	char incompleteOutput[8];
	std::memset(incompleteOutput, 'x', sizeof(incompleteOutput));
	EXPECT_FALSE(wt_charconv_detail::convert("UTF-8", "UTF-8", incomplete, 2,
		incompleteOutput, sizeof(incompleteOutput)));
	EXPECT_STREQ("", incompleteOutput);
	const char validPrefixThenIncomplete[] = {
		'A', static_cast<char>(0xE2), static_cast<char>(0x82) };
	char prefixIncompleteOutput[8];
	std::memset(prefixIncompleteOutput, 'x', sizeof(prefixIncompleteOutput));
	EXPECT_FALSE(wt_charconv_detail::convert("UTF-8", "UTF-8", validPrefixThenIncomplete,
		sizeof(validPrefixThenIncomplete), prefixIncompleteOutput, sizeof(prefixIncompleteOutput)));
	EXPECT_STREQ("A", prefixIncompleteOutput);

	const char withNul[] = { 'A', '\0', 'B' };
	char nulOutput[sizeof(withNul) + 1] = { 0 };
	ASSERT_TRUE(wt_charconv_detail::convert("UTF-8", "UTF-8", withNul,
		sizeof(withNul), nulOutput, sizeof(nulOutput)));
	const char expected[] = { 'A', '\0', 'B', '\0' };
	EXPECT_EQ(0, std::memcmp(nulOutput, expected, sizeof(expected)));
}

TEST(r12_charconv_iconv, output_too_small_stays_inside_destination_capacity)
{
	// “中” requires three bytes in UTF-8. The supplied destination has room
	// for one data byte plus NUL, so iconv must report E2BIG without overrunning.
	char guarded[] = { 'L', 'x', 'x', 'R' };
	const char* utf8 = "\xe4\xb8\xad";
	EXPECT_FALSE(wt_charconv_detail::convert("UTF-8", "UTF-8", utf8, 3,
		guarded + 1, 2));
	EXPECT_EQ('L', guarded[0]);
	EXPECT_EQ('\0', guarded[1]);
	EXPECT_EQ('x', guarded[2]);
	EXPECT_EQ('R', guarded[3]);
}

TEST(r12_charconv_iconv, unavailable_encoding_returns_empty_without_closing_invalid_descriptor)
{
	char guarded[] = { 'L', 'x', 'x', 'R' };
	EXPECT_FALSE(wt_charconv_detail::convert("wt-invalid-encoding-name",
		"UTF-8", "hello", 5, guarded + 1, 2));
	EXPECT_EQ('L', guarded[0]);
	EXPECT_EQ('\0', guarded[1]);
	EXPECT_EQ('R', guarded[3]);
}
#endif

TEST(r12_charconv_encoding_helper, prenum_counts_leading_ones)
{
	// 0xxxxxxx → 0
	EXPECT_EQ(EncodingHelper::preNUm(0x00), 0);
	EXPECT_EQ(EncodingHelper::preNUm(0x7F), 0);
	// 10xxxxxx → 1（续字节）
	EXPECT_EQ(EncodingHelper::preNUm(0x80), 1);
	EXPECT_EQ(EncodingHelper::preNUm(0xBF), 1);
	// 110xxxxx → 2
	EXPECT_EQ(EncodingHelper::preNUm(0xC0), 2);
	// 1110xxxx → 3
	EXPECT_EQ(EncodingHelper::preNUm(0xE0), 3);
	// 11110xxx → 4
	EXPECT_EQ(EncodingHelper::preNUm(0xF0), 4);
	// 111110xx → 5
	EXPECT_EQ(EncodingHelper::preNUm(0xF8), 5);
	// 1111110x → 6
	EXPECT_EQ(EncodingHelper::preNUm(0xFC), 6);
	// 11111110 → 7
	EXPECT_EQ(EncodingHelper::preNUm(0xFE), 7);
	// 全 1 → 8
	EXPECT_EQ(EncodingHelper::preNUm(0xFF), 8);
}

TEST(r12_charconv_encoding_helper, isutf8_valid_sequences)
{
	// 空数据（长度为 0）视为合法
	unsigned char emptyBuf[1] = { 0x00 };
	EXPECT_TRUE(EncodingHelper::isUtf8(emptyBuf, 0));
	// 纯 ASCII
	unsigned char ascii[] = { 'a', 'b', 'C', '9', '-', '.' };
	EXPECT_TRUE(EncodingHelper::isUtf8(ascii, sizeof(ascii)));
	// 2 字节：C3 A9
	unsigned char two[] = { 0xC3, 0xA9 };
	EXPECT_TRUE(EncodingHelper::isUtf8(two, sizeof(two)));
	// 3 字节：E4 B8 AD（“中”）
	unsigned char three[] = { 0xE4, 0xB8, 0xAD };
	EXPECT_TRUE(EncodingHelper::isUtf8(three, sizeof(three)));
	// 4 字节：F0 9F 98 80
	unsigned char four[] = { 0xF0, 0x9F, 0x98, 0x80 };
	EXPECT_TRUE(EncodingHelper::isUtf8(four, sizeof(four)));
	// ASCII 与多字节混排
	unsigned char mix[] = { 'A', 0xE4, 0xB8, 0xAD, 'B' };
	EXPECT_TRUE(EncodingHelper::isUtf8(mix, sizeof(mix)));
}

TEST(r12_charconv_encoding_helper, isutf8_rejects_broken_sequences)
{
	// 续字节缺失：E4 后面跟 ASCII
	unsigned char missing[] = { 0xE4, 0x41, 0xAD };
	EXPECT_FALSE(EncodingHelper::isUtf8(missing, sizeof(missing)));
	// 续字节高位不对（A8 是 10101000 合法，换成 C0 就非法）
	unsigned char badTail[] = { 0xE4, 0xB8, 0xC0 };
	EXPECT_FALSE(EncodingHelper::isUtf8(badTail, sizeof(badTail)));
	// 单独的续字节 0x80：preNUm==1，落到“其他情况”分支
	unsigned char stray[] = { 0x80 };
	EXPECT_FALSE(EncodingHelper::isUtf8(stray, sizeof(stray)));
	// 0xFE/0xFF 首字节非法
	unsigned char fe[] = { 0xFE, 0x80 };
	EXPECT_FALSE(EncodingHelper::isUtf8(fe, sizeof(fe)));
}

TEST(r12_charconv_encoding_helper, isutf8_rejects_sequence_truncated_by_logical_length)
{
	// The physical arrays contain valid continuation bytes past the logical
	// length, so only the supplied length determines whether a sequence fits.
	unsigned char twoByteWithCanary[] = { 0xC2, 0xA2 };
	unsigned char threeByteWithCanary[] = { 0xE4, 0xB8, 0xAD };
	unsigned char fourByteWithCanary[] = { 0xF0, 0x9F, 0x98, 0x80 };
	EXPECT_FALSE(EncodingHelper::isUtf8(twoByteWithCanary, 1));
	EXPECT_FALSE(EncodingHelper::isUtf8(threeByteWithCanary, 1));
	EXPECT_FALSE(EncodingHelper::isUtf8(threeByteWithCanary, 2));
	EXPECT_FALSE(EncodingHelper::isUtf8(fourByteWithCanary, 1));
	EXPECT_FALSE(EncodingHelper::isUtf8(fourByteWithCanary, 2));
	EXPECT_FALSE(EncodingHelper::isUtf8(fourByteWithCanary, 3));

	// These arrays end exactly at the logical boundary. ASan can detect any
	// implementation that reads a missing continuation byte.
	unsigned char exactTwoByteTruncation[] = { 0xC2 };
	unsigned char exactThreeByteTruncation[] = { 0xE4, 0xB8 };
	unsigned char exactFourByteTruncation[] = { 0xF0, 0x9F, 0x98 };
	EXPECT_FALSE(EncodingHelper::isUtf8(exactTwoByteTruncation, sizeof(exactTwoByteTruncation)));
	EXPECT_FALSE(EncodingHelper::isUtf8(exactThreeByteTruncation, sizeof(exactThreeByteTruncation)));
	EXPECT_FALSE(EncodingHelper::isUtf8(exactFourByteTruncation, sizeof(exactFourByteTruncation)));
}

TEST(r12_charconv_encoding_helper, isutf8_enforces_unicode_scalar_boundaries)
{
	// The boundary code points of the valid 2, 3 and 4 byte ranges.
	unsigned char twoByteLow[] = { 0xC2, 0x80 };
	unsigned char twoByteHigh[] = { 0xDF, 0xBF };
	unsigned char threeByteLow[] = { 0xE0, 0xA0, 0x80 };
	unsigned char beforeSurrogates[] = { 0xED, 0x9F, 0xBF };
	unsigned char afterSurrogates[] = { 0xEE, 0x80, 0x80 };
	unsigned char fourByteLow[] = { 0xF0, 0x90, 0x80, 0x80 };
	unsigned char fourByteHigh[] = { 0xF4, 0x8F, 0xBF, 0xBF };
	EXPECT_TRUE(EncodingHelper::isUtf8(twoByteLow, sizeof(twoByteLow)));
	EXPECT_TRUE(EncodingHelper::isUtf8(twoByteHigh, sizeof(twoByteHigh)));
	EXPECT_TRUE(EncodingHelper::isUtf8(threeByteLow, sizeof(threeByteLow)));
	EXPECT_TRUE(EncodingHelper::isUtf8(beforeSurrogates, sizeof(beforeSurrogates)));
	EXPECT_TRUE(EncodingHelper::isUtf8(afterSurrogates, sizeof(afterSurrogates)));
	EXPECT_TRUE(EncodingHelper::isUtf8(fourByteLow, sizeof(fourByteLow)));
	EXPECT_TRUE(EncodingHelper::isUtf8(fourByteHigh, sizeof(fourByteHigh)));

	// Overlong forms, UTF-16 surrogates and values above U+10FFFF are invalid.
	unsigned char overlongTwo[] = { 0xC0, 0x80 };
	unsigned char overlongThree[] = { 0xE0, 0x80, 0x80 };
	unsigned char overlongFour[] = { 0xF0, 0x80, 0x80, 0x80 };
	unsigned char surrogate[] = { 0xED, 0xA0, 0x80 };
	unsigned char aboveUnicode[] = { 0xF4, 0x90, 0x80, 0x80 };
	unsigned char invalidLead[] = { 0xF5, 0x80, 0x80, 0x80 };
	EXPECT_FALSE(EncodingHelper::isUtf8(overlongTwo, sizeof(overlongTwo)));
	EXPECT_FALSE(EncodingHelper::isUtf8(overlongThree, sizeof(overlongThree)));
	EXPECT_FALSE(EncodingHelper::isUtf8(overlongFour, sizeof(overlongFour)));
	EXPECT_FALSE(EncodingHelper::isUtf8(surrogate, sizeof(surrogate)));
	EXPECT_FALSE(EncodingHelper::isUtf8(aboveUnicode, sizeof(aboveUnicode)));
	EXPECT_FALSE(EncodingHelper::isUtf8(invalidLead, sizeof(invalidLead)));

	unsigned char emptyStorage[] = { 0 };
	EXPECT_TRUE(EncodingHelper::isUtf8(NULL, 0));
	EXPECT_FALSE(EncodingHelper::isUtf8(NULL, 1));
	EXPECT_TRUE(EncodingHelper::isUtf8(emptyStorage, 0));
}

TEST(r12_charconv_url, encode_decode_round_trip_on_ascii_subset)
{
	// 空格编码为 %20，解码回来是空格
	URLEncode enc("a b");
	EXPECT_STREQ(static_cast<const char*>(enc), "a%20b");
	URLDecode back("a%20b");
	EXPECT_STREQ(static_cast<const char*>(back), "a b");
	// '+' 解码成空格
	URLDecode plus("a+b");
	EXPECT_STREQ(static_cast<const char*>(plus), "a b");
	// 字母数字的 %XX 形式保持原样不解码（URL 规范里的免编码字符）
	URLDecode keep("%41");
	EXPECT_STREQ(static_cast<const char*>(keep), "%41");
	// 需要解码的符号
	URLDecode sharp("%23");
	EXPECT_STREQ(static_cast<const char*>(sharp), "#");
	// 断链的百分号序列原样保留
	URLDecode broken("%zz");
	EXPECT_STREQ(static_cast<const char*>(broken), "%zz");
}
