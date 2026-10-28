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
	// 0xFF/0xFE 不是合法 UTF-8 首码点：iconv 不会写出任何字节，
	// 而目标缓冲在转换前被整段 memset(0)，因此结果必定是空串。
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
	// 0xFF 不是合法 GB2312 首字节 → 目标缓冲保持 memset 后的全零
	unsigned char bad[] = { 0xFF, 0x41, 0x00 };
	ChartoUTF8 conv((const char*)bad);
	ASSERT_NE(conv.c_str(), nullptr);
	EXPECT_STREQ(conv.c_str(), "");
}

// 触发条件：期望值依赖本机 libiconv 是否提供 "gb2312" 别名。
// macOS 上 iconv_open("gb2312","utf-8") 可能成功（得到 D6 D0）也可能失败
// （失败时 charconv 仍走进 if(cd != 0) 分支，因为失败返回的是 (iconv_t)-1 而不是 0，
// 随后 iconv() 报 EBADF、缓冲保持全零 → 结果为空串）。
// 两种结果都可能，断言无法在当前平台唯一确定，故整类 DISABLED 只做记录。
TEST(DISABLED_r12_charconv_utf8tochar, chinese_conversion_is_platform_dependent)
{
	UTF8toChar conv("\xe4\xb8\xad");
	const unsigned char expectGb[] = { 0xD6, 0xD0, 0x00 };
	EXPECT_EQ(memcmp(conv.c_str(), expectGb, 3), 0);
}

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
	// 截断的多字节序列：长度不够，末尾读到 '\0' 之外的越界位置由 len 限制，
	// 循环按 len 结束前的续字节判定为不合法
	unsigned char truncated[] = { 0xF0, 0x9F, 0x98 };
	EXPECT_FALSE(EncodingHelper::isUtf8(truncated, sizeof(truncated)));
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
