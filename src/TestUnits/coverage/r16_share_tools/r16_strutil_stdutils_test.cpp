/*!
 * \file r16_strutil_stdutils_test.cpp
 * \brief R16 覆盖率用例：src/Share/StrUtil.hpp 边角分支 + src/Share/StdUtils.hpp（StdLocker/StdFile）
 *
 * ws01_share_core/ws01_strutil_test.cpp 已覆盖 trim/trim(cstr)/trimAll/findFirst/findLast/
 * split/split(out)/split2/大小写/startsWith/endsWith/standardisePath(常规)/replace(常规)/isEmpty，
 * 本文件只补它们没走到的分支（自定义分隔符、maxSplits、多字符 delims、负下标、异常路径）。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <mutex>
#include <fstream>
#include "Share/StrUtil.hpp"
#include "Share/StdUtils.hpp"
#include "r16_tmp.h"

//////////////////////////////////////////////////////////////////////////
// StrUtil::trim（StrUtil.hpp:33-50）
TEST(r16_strutil, trim_inplace_left_only_keeps_right_ws)
{
	std::string s = "  abc  ";
	StrUtil::trim(s, " \t\r", true, false);
	EXPECT_EQ(std::string("abc  "), s);
}

TEST(r16_strutil, trim_inplace_right_only_keeps_left_ws)
{
	std::string s = "  abc  ";
	StrUtil::trim(s, " \t\r", false, true);
	EXPECT_EQ(std::string("  abc"), s);
}

TEST(r16_strutil, trim_inplace_custom_delims)
{
	std::string s = "##hello##";
	StrUtil::trim(s, "#");
	EXPECT_EQ(std::string("hello"), s);
}

TEST(r16_strutil, trim_cstr_only_right)
{
	std::string s = StrUtil::trim("  x y  ", " ", true, false);
	EXPECT_EQ(std::string("x y  "), s);
}

TEST(r16_strutil, trim_all_chars_removed_yields_empty)
{
	std::string s = "   ";
	StrUtil::trim(s, " ");
	EXPECT_EQ(std::string(""), s);
}

TEST(r16_strutil, trimAll_chained_filter_matches_doc)
{
	std::string s = "A [1, 2:x3]";
	std::string& r = StrUtil::trimAll(s, " :,[]x3");
	EXPECT_EQ(std::string("A12"), s);
	// 返回的就是入参引用
	EXPECT_EQ(&s, &r);
}

TEST(r16_strutil, trimAll_default_only_whitespace_family)
{
	std::string s = " a\tb\nc\r";
	StrUtil::trimAll(s);
	EXPECT_EQ(std::string("abc"), s);
}

//////////////////////////////////////////////////////////////////////////
// findFirst / findLast（StrUtil.hpp:64-92）
TEST(r16_strutil, find_first_of_nul_returns_length)
{
	// 查 '\0' 时循环走到终止符即命中，返回下标 = 长度
	EXPECT_EQ((size_t)3, StrUtil::findFirst("abc", '\0'));
	EXPECT_EQ((size_t)0, StrUtil::findFirst("abc", 'a'));
}

TEST(r16_strutil, find_first_not_found_is_npos)
{
	EXPECT_EQ(std::string::npos, StrUtil::findFirst("abc", 'z'));
}

TEST(r16_strutil, find_last_at_end_and_absent)
{
	EXPECT_EQ((size_t)3, StrUtil::findLast("a/b/c", '/'));
	EXPECT_EQ(std::string::npos, StrUtil::findLast("abc", '/'));
	EXPECT_EQ((size_t)0, StrUtil::findLast("x", 'x'));
}

//////////////////////////////////////////////////////////////////////////
// split / split2 的 maxSplits 与多字符分隔符
TEST(r16_strutil, split_respects_max_splits)
{
	// maxSplits=1：只切一刀，其余整段落最后一项
	StringVector v = StrUtil::split("a b c d", " ", 1);
	ASSERT_EQ((size_t)2, v.size());
	EXPECT_EQ(std::string("a"), v[0]);
	EXPECT_EQ(std::string("b c d"), v[1]);
}

TEST(r16_strutil, split2_respects_max_splits)
{
	StringVector v = StrUtil::split2("a::b::c", "::", 1);
	ASSERT_EQ((size_t)2, v.size());
	EXPECT_EQ(std::string("a"), v[0]);
	EXPECT_EQ(std::string("b::c"), v[1]);
}

TEST(r16_strutil, split2_advances_by_delim_length)
{
	// split2 用 start = pos + delims.size()，双字符分隔符要跳过 2 格
	StringVector v = StrUtil::split2("a::b", "::");
	ASSERT_EQ((size_t)2, v.size());
	EXPECT_EQ(std::string("a"), v[0]);
	EXPECT_EQ(std::string("b"), v[1]);
}

TEST(r16_strutil, split_leading_delim_yields_empty_first)
{
	StringVector v = StrUtil::split("  a", " ");
	ASSERT_EQ((size_t)2, v.size());
	EXPECT_EQ(std::string(""), v[0]);
	EXPECT_EQ(std::string(""), v[1]);
}

TEST(r16_strutil, split_output_version_appends_to_existing_vector)
{
	StringVector v;
	v.emplace_back("seed");
	StrUtil::split("x,y", v, ",");
	ASSERT_EQ((size_t)3, v.size());
	EXPECT_EQ(std::string("seed"), v[0]);
	EXPECT_EQ(std::string("x"), v[1]);
	EXPECT_EQ(std::string("y"), v[2]);
}

TEST(r16_strutil, split_empty_string_yields_one_empty_item)
{
	StringVector v = StrUtil::split("", " ");
	ASSERT_EQ((size_t)1, v.size());
	EXPECT_EQ(std::string(""), v[0]);
}

//////////////////////////////////////////////////////////////////////////
// replace（StrUtil.hpp:317-334）
TEST(r16_strutil, replace_all_occurrences_and_longer_target)
{
	std::string s = "aXbXc";
	StrUtil::replace(s, "X", "<<");
	EXPECT_EQ(std::string("a<<b<<c"), s);
}

TEST(r16_strutil, replace_no_match_keeps_string)
{
	std::string s = "hello";
	StrUtil::replace(s, "z", "Y");
	EXPECT_EQ(std::string("hello"), s);
}

TEST(r16_strutil, replace_shrinks_target)
{
	std::string s = "C:/tmp//dir";
	StrUtil::replace(s, "//", "/");
	EXPECT_EQ(std::string("C:/tmp/dir"), s);
}

TEST(r16_strutil, DISABLED_replace_with_empty_source_hangs)
{
	// BUG-R16-03：src 为空串时 find("") 恒为 0，循环不推进 → 死循环 + 内存无限增长。
	// 触发条件：StrUtil::replace(s, "", "x")。整条用例隔离，不参与门禁以外的执行。
	std::string s = "abc";
	StrUtil::replace(s, "", "x");
	EXPECT_EQ(std::string("abc"), s);
}

//////////////////////////////////////////////////////////////////////////
// standardisePath 空串（StrUtil.hpp:306-315）
TEST(r16_strutil, standardise_path_non_dir_no_trailing_slash)
{
	EXPECT_EQ(std::string("a/b/c.txt"), StrUtil::standardisePath("a\\b\\c.txt", false));
}

TEST(r16_strutil, DISABLED_standardise_path_empty_string_reads_before_begin)
{
	// BUG-R16-04：path[path.length()-1] 在空串时是 path[(size_t)-1]，越界读（libc++ 断言/崩溃）。
	// 触发条件：StrUtil::standardisePath("")。整条用例隔离。
	std::string s = StrUtil::standardisePath("");
	EXPECT_EQ(std::string("/"), s);
}

//////////////////////////////////////////////////////////////////////////
// StdLocker（StdUtils.hpp:34-50）
TEST(r16_stdutils, stdlocker_locks_and_releases_on_scope_exit)
{
	std::mutex mtx;
	{
		StdLocker<std::mutex> lk(mtx);
		EXPECT_FALSE(mtx.try_lock());	// 已被持有
	}
	// 出作用域后析构必须解锁，否则 try_lock 失败
	EXPECT_TRUE(mtx.try_lock());
	mtx.unlock();
}

namespace {
	// StdLocker 是模板，用自计数的假锁就能把「构造加锁 / 析构解锁」写成可证伪断言
	struct CountingMutex
	{
		int locks{0};
		int unlocks{0};
		void lock() { ++locks; }
		void unlock() { ++unlocks; }
	};
}

TEST(r16_stdutils, stdlocker_locks_once_and_unlocks_once_on_destroy)
{
	CountingMutex m;
	{
		StdLocker<CountingMutex> lk(m);
		EXPECT_EQ(1, m.locks);
		EXPECT_EQ(0, m.unlocks);
	}
	EXPECT_EQ(1, m.locks);
	EXPECT_EQ(1, m.unlocks);
}

TEST(r16_stdutils, stdlocker_nested_scopes_lock_twice_unlock_twice)
{
	CountingMutex m;
	{
		StdLocker<CountingMutex> outer(m);
		{
			StdLocker<CountingMutex> inner(m);
			EXPECT_EQ(2, m.locks);
		}
		EXPECT_EQ(1, m.unlocks);
	}
	EXPECT_EQ(2, m.locks);
	EXPECT_EQ(2, m.unlocks);
}

//////////////////////////////////////////////////////////////////////////
// StdFile（StdUtils.hpp:54-147）
TEST(r16_stdutils, stdfile_write_then_read_whole_file)
{
	const std::string path = r16tmp::file("stdfile1", "a.bin");
	const std::string payload = "0123456789";
	StdFile::write_file_content(path.c_str(), payload);

	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content);
	EXPECT_EQ((uint64_t)10, got);
	EXPECT_EQ(payload, content);
	r16tmp::clean("stdfile1");
}

TEST(r16_stdutils, stdfile_read_missing_file_throws)
{
	const std::string path = r16tmp::file("stdfile2", "missing.bin");
	r16tmp::clean("stdfile2");
	std::string content;
	EXPECT_THROW(StdFile::read_file_content(path.c_str(), content), std::runtime_error);
}

TEST(r16_stdutils, stdfile_read_offset_beyond_size_throws)
{
	const std::string path = r16tmp::file("stdfile3", "a.bin");
	StdFile::write_file_content(path.c_str(), std::string("abcd"));	// 4 字节
	std::string content;
	EXPECT_THROW(StdFile::read_file_content(path.c_str(), content, 5), std::runtime_error);
	r16tmp::clean("stdfile3");
}

TEST(r16_stdutils, stdfile_read_offset_equal_size_returns_zero)
{
	const std::string path = r16tmp::file("stdfile4", "a.bin");
	StdFile::write_file_content(path.c_str(), std::string("abcd"));
	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content, 4);
	EXPECT_EQ((uint64_t)0, got);
	EXPECT_EQ(std::string(""), content);
	r16tmp::clean("stdfile4");
}

TEST(r16_stdutils, stdfile_read_clamps_length_to_file_end)
{
	const std::string path = r16tmp::file("stdfile5", "a.bin");
	StdFile::write_file_content(path.c_str(), std::string("abcdefghij"));	// 10 字节
	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content, 6, 100);
	// offset+length 越界时被夹回 file_size-offset
	EXPECT_EQ((uint64_t)4, got);
	EXPECT_EQ(std::string("ghij"), content);
	r16tmp::clean("stdfile5");
}

TEST(r16_stdutils, stdfile_read_partial_window)
{
	const std::string path = r16tmp::file("stdfile6", "a.bin");
	StdFile::write_file_content(path.c_str(), std::string("abcdefghij"));
	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content, 2, 3);
	EXPECT_EQ((uint64_t)3, got);
	EXPECT_EQ(std::string("cde"), content);
	r16tmp::clean("stdfile6");
}

TEST(r16_stdutils, stdfile_write_raw_buffer_overwrites_and_reads_back)
{
	const std::string path = r16tmp::file("stdfile7", "a.bin");
	const char buf[] = { 1, 2, 3, 4, 5 };
	StdFile::write_file_content(path.c_str(), (const void*)buf, sizeof(buf));
	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content);
	EXPECT_EQ((uint64_t)5, got);
	ASSERT_EQ(content.size(), (size_t)5);
	EXPECT_EQ(0, memcmp(content.data(), buf, 5));
	r16tmp::clean("stdfile7");
}

TEST(r16_stdutils, stdfile_write_to_missing_directory_throws)
{
	const std::string path = r16tmp::dir("stdfile8") + "/no_such_dir_deeper/a.bin";
	EXPECT_THROW(StdFile::write_file_content(path.c_str(), std::string("x")), std::runtime_error);
	r16tmp::clean("stdfile8");
}

TEST(r16_stdutils, stdfile_exists_true_for_file_false_for_missing)
{
	const std::string path = r16tmp::file("stdfile9", "a.bin");
	EXPECT_FALSE(StdFile::exists(path.c_str()));
	StdFile::write_file_content(path.c_str(), std::string("x"));
	EXPECT_TRUE(StdFile::exists(path.c_str()));
	r16tmp::clean("stdfile9");
}

TEST(r16_stdutils, stdfile_empty_file_read_returns_zero)
{
	const std::string path = r16tmp::file("stdfile10", "empty.bin");
	std::ofstream f(path, std::ios::binary);
	ASSERT_TRUE(f.is_open());
	f.close();
	std::string content;
	uint64_t got = StdFile::read_file_content(path.c_str(), content);
	EXPECT_EQ((uint64_t)0, got);
	EXPECT_EQ(std::string(""), content);
	r16tmp::clean("stdfile10");
}
