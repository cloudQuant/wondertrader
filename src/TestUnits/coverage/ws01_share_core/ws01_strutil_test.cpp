// WS01: StrUtil.hpp coverage.
#include <gtest/gtest.h>

#include "Share/StrUtil.hpp"

#include <string>
#include <vector>

TEST(ws01_strutil, trim_inplace_left_right)
{
	std::string both = " \t\r abc \t\r ";
	StrUtil::trim(both);
	EXPECT_EQ(both, "abc");

	std::string leftOnly = "  abc  ";
	StrUtil::trim(leftOnly, " \t\r", true, false);
	EXPECT_EQ(leftOnly, "abc  ");

	std::string rightOnly = "  abc  ";
	StrUtil::trim(rightOnly, " \t\r", false, true);
	EXPECT_EQ(rightOnly, "  abc");

	std::string allSpace = "   ";
	StrUtil::trim(allSpace);
	EXPECT_EQ(allSpace, "");

	std::string customDelims = "xxabcxx";
	StrUtil::trim(customDelims, "x");
	EXPECT_EQ(customDelims, "abc");

	// Empty string: find_last_not_of returns npos, erase(npos + 1) is erase(0).
	std::string empty;
	StrUtil::trim(empty);
	EXPECT_EQ(empty, "");
}

TEST(ws01_strutil, trim_cstr)
{
	EXPECT_EQ(StrUtil::trim("  abc  "), "abc");
	EXPECT_EQ(StrUtil::trim("--abc--", "-"), "abc");
	EXPECT_EQ(StrUtil::trim("  abc  ", " ", true, false), "abc  ");
	EXPECT_EQ(StrUtil::trim("  abc  ", " ", false, true), "  abc");
	EXPECT_EQ(StrUtil::trim(""), "");
}

TEST(ws01_strutil, trim_all)
{
	std::string s = "A [1, 2:x3]";
	EXPECT_EQ(StrUtil::trimAll(s, " :,[]x3"), "A12");
	EXPECT_EQ(s, "A12");

	std::string chain = " a\tb\nc\r";
	StrUtil::trimAll(chain);
	EXPECT_EQ(chain, "abc");

	std::string chain2 = "a b";
	StrUtil::trimAll(chain2).append("c");
	EXPECT_EQ(chain2, "abc");
}

TEST(ws01_strutil, find_first_last)
{
	EXPECT_EQ(StrUtil::findFirst("SHFE.au.2106", '.'), 4u);
	EXPECT_EQ(StrUtil::findFirst("SHFE.au.2106", 'z'), std::string::npos);
	EXPECT_EQ(StrUtil::findFirst("", '.'), std::string::npos);

	EXPECT_EQ(StrUtil::findLast("SHFE.au.2106", '.'), 7u);
	EXPECT_EQ(StrUtil::findLast("SHFE.au.2106", 'z'), std::string::npos);
	EXPECT_EQ(StrUtil::findLast("", '.'), std::string::npos);
	EXPECT_EQ(StrUtil::findLast("abc", 'a'), 0u);
}

TEST(ws01_strutil, split_vector)
{
	StringVector v = StrUtil::split("a b\tc\nd");
	ASSERT_EQ(v.size(), 4u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b");
	EXPECT_EQ(v[2], "c");
	EXPECT_EQ(v[3], "d");

	// Leading delimiter produces an empty field.
	v = StrUtil::split(" a");
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0], "");
	EXPECT_EQ(v[1], "a");

	// Empty input still yields one field.
	v = StrUtil::split("");
	ASSERT_EQ(v.size(), 1u);
	EXPECT_EQ(v[0], "");

	// maxSplits stops after the requested number of splits.
	v = StrUtil::split("a,b,c,d", ",", 2);
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b");
	EXPECT_EQ(v[2], "c,d");

	// Trailing delimiter.
	v = StrUtil::split("a,");
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "");
}

TEST(ws01_strutil, split_output_param)
{
	StringVector out;
	out.emplace_back("pre-existing");
	StrUtil::split("a-b-c", out, "-");
	ASSERT_EQ(out.size(), 4u);
	EXPECT_EQ(out[0], "pre-existing");
	EXPECT_EQ(out[1], "a");
	EXPECT_EQ(out[2], "b");
	EXPECT_EQ(out[3], "c");

	out.clear();
	StrUtil::split("a,b,c", out, ",", 1);
	ASSERT_EQ(out.size(), 2u);
	EXPECT_EQ(out[1], "b,c");

	out.clear();
	StrUtil::split("", out);
	ASSERT_EQ(out.size(), 1u);
	EXPECT_EQ(out[0], "");
}

TEST(ws01_strutil, split2)
{
	// split2 advances by delims.size(), so only single-char delimiters behave.
	StringVector v = StrUtil::split2("a,b,c", ",");
	ASSERT_EQ(v.size(), 3u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b");
	EXPECT_EQ(v[2], "c");

	v = StrUtil::split2(",a");
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0], "");
	EXPECT_EQ(v[1], "a");

	v = StrUtil::split2("a,b,c", ",", 1);
	ASSERT_EQ(v.size(), 2u);
	EXPECT_EQ(v[0], "a");
	EXPECT_EQ(v[1], "b,c");

	v = StrUtil::split2("");
	ASSERT_EQ(v.size(), 1u);
	EXPECT_EQ(v[0], "");
}

TEST(ws01_strutil, case_conversion)
{
	std::string lower = "AbC123";
	StrUtil::toLowerCase(lower);
	EXPECT_EQ(lower, "abc123");

	std::string upper = "AbC123";
	StrUtil::toUpperCase(upper);
	EXPECT_EQ(upper, "ABC123");

	EXPECT_EQ(StrUtil::makeLowerCase("AbC"), "abc");
	EXPECT_EQ(StrUtil::makeUpperCase("AbC"), "ABC");
	EXPECT_EQ(StrUtil::makeLowerCase(""), "");
	EXPECT_EQ(StrUtil::makeUpperCase(""), "");
}

TEST(ws01_strutil, starts_with)
{
	EXPECT_TRUE(StrUtil::startsWith("HelloWorld", "hello"));
	EXPECT_TRUE(StrUtil::startsWith("HelloWorld", "Hello"));
	EXPECT_FALSE(StrUtil::startsWith("HelloWorld", "hello", false));
	EXPECT_TRUE(StrUtil::startsWith("HelloWorld", "Hello", false));

	// Shorter source than pattern.
	EXPECT_FALSE(StrUtil::startsWith("ab", "abc"));
	// Empty pattern is never a match.
	EXPECT_FALSE(StrUtil::startsWith("abc", ""));
}

TEST(ws01_strutil, ends_with)
{
	EXPECT_TRUE(StrUtil::endsWith("HelloWorld", "WORLD"));
	EXPECT_TRUE(StrUtil::endsWith("HelloWorld", "World"));
	EXPECT_FALSE(StrUtil::endsWith("HelloWorld", "WORLD", false));
	EXPECT_TRUE(StrUtil::endsWith("HelloWorld", "World", false));

	EXPECT_FALSE(StrUtil::endsWith("ab", "abc"));
	EXPECT_FALSE(StrUtil::endsWith("abc", ""));
	EXPECT_TRUE(StrUtil::endsWith("abc", "abc", false));
}

TEST(ws01_strutil, standardise_path)
{
	EXPECT_EQ(StrUtil::standardisePath("C:\\data\\sub"), "C:/data/sub/");
	EXPECT_EQ(StrUtil::standardisePath("C:\\data\\sub", false), "C:/data/sub");
	EXPECT_EQ(StrUtil::standardisePath("data/sub/"), "data/sub/");
	EXPECT_EQ(StrUtil::standardisePath("", false), "");
}

TEST(ws01_strutil, replace)
{
	std::string s = "a.b.c";
	StrUtil::replace(s, ".", "-");
	EXPECT_EQ(s, "a-b-c");

	std::string noMatch = "abc";
	StrUtil::replace(noMatch, "x", "y");
	EXPECT_EQ(noMatch, "abc");

	std::string shrink = "a..b";
	StrUtil::replace(shrink, "..", ".");
	EXPECT_EQ(shrink, "a.b");

	std::string toEmpty = "a-b";
	StrUtil::replace(toEmpty, "-", "");
	EXPECT_EQ(toEmpty, "ab");
}

TEST(ws01_strutil, is_empty)
{
	EXPECT_TRUE(StrUtil::isEmpty(""));
	EXPECT_FALSE(StrUtil::isEmpty("a"));
}
