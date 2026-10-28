/*!
 * \file r16_ini_boostfile_test.cpp
 * \brief R16 覆盖率用例：src/Share/IniHelper.hpp（基于 boost property_tree 的 INI 读写）
 *                         + src/Share/BoostFile.hpp（boost ipcdetail 文件封装）
 *
 * 只碰普通磁盘文件（临时目录，pid 隔离），不涉及共享内存/线程/socket。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include "Share/IniHelper.hpp"
#include "Share/BoostFile.hpp"
#include "r16_tmp.h"

namespace {

const char* SAMPLE_INI =
	"[sys]\n"
	"name=r16\n"
	"count=42\n"
	"neg=-7\n"
	"ratio=1.25\n"
	"flag=true\n"
	"off=0\n"
	"bad=abc\n"
	"[empty]\n";

std::string make_sample_ini(const char* tag, const char* name)
{
	std::string path = r16tmp::file(tag, name);
	std::string dir = r16tmp::dir(tag);
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << SAMPLE_INI;
	f.close();
	(void)dir;
	return path;
}

}	// namespace

//////////////////////////////////////////////////////////////////////////
// IniHelper：加载 + 各类型读取
TEST(r16_inihelper, load_sets_loaded_flag)
{
	const std::string path = make_sample_ini("ini1", "a.ini");
	IniHelper ini;
	EXPECT_FALSE(ini.isLoaded());	// 构造后未加载
	ini.load(path.c_str());
	EXPECT_TRUE(ini.isLoaded());
	r16tmp::clean("ini1");
}

TEST(r16_inihelper, read_string_int_double_bool_from_loaded_file)
{
	const std::string path = make_sample_ini("ini2", "a.ini");
	IniHelper ini;
	ini.load(path.c_str());
	EXPECT_EQ(std::string("r16"), ini.readString("sys", "name"));
	EXPECT_EQ(42, ini.readInt("sys", "count"));
	EXPECT_EQ(-7, ini.readInt("sys", "neg"));
	EXPECT_EQ(42u, ini.readUInt("sys", "count", 9u));
	EXPECT_TRUE(ini.readBool("sys", "flag", false));
	EXPECT_FALSE(ini.readBool("sys", "off", true));
	// 1.25 在 double 与 boost 默认 6 位有效数字下都能精确往返，所以用 EXPECT_EQ
	EXPECT_EQ(1.25, ini.readDouble("sys", "ratio", 0.0));
	r16tmp::clean("ini2");
}

TEST(r16_inihelper, read_returns_default_when_missing_or_unparsable)
{
	const std::string path = make_sample_ini("ini3", "a.ini");
	IniHelper ini;
	ini.load(path.c_str());
	// 键不存在
	EXPECT_EQ(std::string("dflt"), ini.readString("sys", "nokey", "dflt"));
	EXPECT_EQ(99, ini.readInt("sys", "nokey", 99));
	EXPECT_EQ(99u, ini.readUInt("sys", "nokey", 99u));
	EXPECT_TRUE(ini.readBool("sys", "nokey", true));
	EXPECT_EQ(9.5, ini.readDouble("sys", "nokey", 9.5));
	// 节不存在
	EXPECT_EQ(std::string("x"), ini.readString("nosuchsec", "name", "x"));
	// 值无法转换：boost 抛异常 → readValue 的 catch 分支返回默认值
	EXPECT_EQ(7, ini.readInt("sys", "bad", 7));
	r16tmp::clean("ini3");
}

TEST(r16_inihelper, read_sections_and_key_arrays)
{
	const std::string path = make_sample_ini("ini4", "a.ini");
	IniHelper ini;
	ini.load(path.c_str());
	FieldArray secs;
	int n = ini.readSections(secs);
	EXPECT_EQ(2, n);						// sys + empty
	ASSERT_EQ((size_t)2, secs.size());
	EXPECT_EQ(std::string("sys"), secs[0]);
	EXPECT_EQ(std::string("empty"), secs[1]);

	FieldArray keys;
	int kn = ini.readSecKeyArray("sys", keys);
	EXPECT_EQ(7, kn);
	ASSERT_EQ((size_t)7, keys.size());
	EXPECT_EQ(std::string("name"), keys[0]);
	EXPECT_EQ(std::string("bad"), keys[6]);

	FieldArray k2, v2;
	int kv = ini.readSecKeyValArray("sys", k2, v2);
	EXPECT_EQ(7, kv);
	ASSERT_EQ((size_t)7, v2.size());
	EXPECT_EQ(std::string("name"), k2[0]);
	EXPECT_EQ(std::string("r16"), v2[0]);
	EXPECT_EQ(std::string("42"), v2[1]);

	// 空节：0 个键，数组不被污染
	FieldArray ek;
	EXPECT_EQ(0, ini.readSecKeyArray("empty", ek));
	EXPECT_EQ((size_t)0, ek.size());

	// 不存在的节：走 catch 分支返回 0
	FieldArray nk;
	EXPECT_EQ(0, ini.readSecKeyArray("nosuchsec", nk));
	FieldArray nk2, nv2;
	EXPECT_EQ(0, ini.readSecKeyValArray("nosuchsec", nk2, nv2));
	r16tmp::clean("ini4");
}

TEST(r16_inihelper, write_then_save_then_reload_roundtrip)
{
	const std::string path = r16tmp::file("ini5", "w.ini");
	r16tmp::clean("ini5");
	{
		IniHelper ini;
		ini.writeString("cfg", "title", "hello");
		ini.writeInt("cfg", "iv", -1234);
		ini.writeUInt("cfg", "uv", 4000000000u);
		ini.writeBool("cfg", "yes", true);
		ini.writeBool("cfg", "no", false);
		ini.writeDouble("cfg", "dv", 2.25);
		ini.save(path.c_str());
	}
	ASSERT_TRUE(std::filesystem::exists(path));
	{
		IniHelper ini2;
		ini2.load(path.c_str());
		EXPECT_EQ(std::string("hello"), ini2.readString("cfg", "title"));
		EXPECT_EQ(-1234, ini2.readInt("cfg", "iv"));
		EXPECT_EQ(4000000000u, ini2.readUInt("cfg", "uv", 1u));
		EXPECT_TRUE(ini2.readBool("cfg", "yes", false));
		EXPECT_FALSE(ini2.readBool("cfg", "no", true));
		// 2.25 可被二进制精确表示，往返无损
		EXPECT_EQ(2.25, ini2.readDouble("cfg", "dv", 0.0));
	}
	r16tmp::clean("ini5");
}

TEST(r16_inihelper, save_without_filename_falls_back_to_load_path)
{
	const std::string path = r16tmp::file("ini6", "rw.ini");
	make_sample_ini("ini6", "rw.ini");
	IniHelper ini;
	ini.load(path.c_str());
	ini.writeString("sys", "added", "newval");
	ini.save();						// 走 _fname 分支（L100）

	IniHelper check;
	check.load(path.c_str());
	EXPECT_EQ(std::string("newval"), check.readString("sys", "added"));
	r16tmp::clean("ini6");
}

TEST(r16_inihelper, remove_value_and_remove_section)
{
	const std::string path = make_sample_ini("ini7", "a.ini");
	IniHelper ini;
	ini.load(path.c_str());

	ini.removeValue("sys", "name");
	EXPECT_EQ(std::string("gone"), ini.readString("sys", "name", "gone"));
	// 节/键不存在时走 catch 分支，不应抛出
	ini.removeValue("sys", "nosuchkey");
	ini.removeValue("nosuchsec", "whatever");

	ini.removeSection("sys");
	FieldArray keys;
	EXPECT_EQ(0, ini.readSecKeyArray("sys", keys));
	ini.removeSection("nosuchsec");
	FieldArray secs;
	EXPECT_EQ(1, ini.readSections(secs));
	EXPECT_EQ(std::string("empty"), secs[0]);
	r16tmp::clean("ini7");
}

TEST(r16_inihelper, write_overwrites_existing_value)
{
	const std::string path = make_sample_ini("ini8", "a.ini");
	IniHelper ini;
	ini.load(path.c_str());
	EXPECT_EQ(42, ini.readInt("sys", "count"));
	ini.writeInt("sys", "count", 43);
	EXPECT_EQ(43, ini.readInt("sys", "count"));
	r16tmp::clean("ini8");
}

TEST(r16_inihelper, load_missing_file_still_marks_loaded)
{
	// BUG-R16-05 的实际行为：load() 把 read_ini 的异常吞掉后仍然 _loaded = true，
	// isLoaded() 无法用来判断「文件到底读到了没有」。期望行为见 DISABLED_ 用例。
	const std::string path = r16tmp::file("ini9", "definitely_missing.ini");
	r16tmp::clean("ini9");
	IniHelper ini;
	ini.load(path.c_str());
	EXPECT_TRUE(ini.isLoaded());
	// 内容确实是空的
	FieldArray secs;
	EXPECT_EQ(0, ini.readSections(secs));
	r16tmp::clean("ini9");
}

TEST(r16_inihelper, DISABLED_load_missing_file_should_not_mark_loaded)
{
	// 触发条件：load() 一个不存在/非法的 INI。见 BUG-R16-05。
	const std::string path = r16tmp::file("ini10", "definitely_missing.ini");
	IniHelper ini;
	ini.load(path.c_str());
	EXPECT_FALSE(ini.isLoaded());
	r16tmp::clean("ini10");
}

TEST(r16_inihelper, long_section_and_key_are_silently_truncated_to_63_chars)
{
	// BUG-R16-06 的可观测后果：readXxx/writeXxx 用 static char path[64] + snprintf 组
	// "节.键"，超过 63 字符直接被截断。两条「前 63 字符相同、仅尾部不同」的键会撞成同一路径，
	// 后写覆盖先写。下面喂的输入：sec=30 字符，key=33 字符 → "sec.key" 正好 64 字符 → 截掉最后 1 个。
	const std::string path = r16tmp::file("ini11", "long.ini");
	r16tmp::clean("ini11");
	const std::string sec(30, 'a');
	const std::string keyBase(32, 'b');
	const std::string keyA = keyBase + "x";
	const std::string keyB = keyBase + "y";
	ASSERT_EQ((size_t)30, sec.size());
	ASSERT_EQ((size_t)33, keyA.size());
	ASSERT_NE(keyA, keyB);

	IniHelper ini;
	ini.writeString(sec.c_str(), keyA.c_str(), "valueA");
	ini.writeString(sec.c_str(), keyB.c_str(), "valueB");
	// 实际行为：两个不同键被折叠成一个，keyA 的值已经没了
	EXPECT_EQ(std::string("valueB"), ini.readString(sec.c_str(), keyA.c_str(), ""));
	EXPECT_EQ(std::string("valueB"), ini.readString(sec.c_str(), keyB.c_str(), ""));

	ini.save(path.c_str());
	r16tmp::clean("ini11");
}

TEST(r16_inihelper, DISABLED_long_section_and_key_should_not_collide)
{
	// 期望行为：不同键名保留各自主体；见 BUG-R16-06（MAX_KEY_LENGTH=64 静默截断）。
	const std::string sec(30, 'a');
	const std::string keyA = std::string(32, 'b') + "x";
	const std::string keyB = std::string(32, 'b') + "y";
	IniHelper ini;
	ini.writeString(sec.c_str(), keyA.c_str(), "valueA");
	ini.writeString(sec.c_str(), keyB.c_str(), "valueB");
	EXPECT_EQ(std::string("valueA"), ini.readString(sec.c_str(), keyA.c_str(), ""));
	r16tmp::clean("ini11");
}

//////////////////////////////////////////////////////////////////////////
// BoostFile（BoostFile.hpp:32-207）
TEST(r16_boostfile, fresh_handle_is_invalid_and_close_is_idempotent)
{
	BoostFile bf;
	EXPECT_TRUE(bf.is_invalid_file());
	EXPECT_FALSE(bf.valid());
	bf.close_file();						// 未打开也安全
	EXPECT_TRUE(bf.is_invalid_file());
}

TEST(r16_boostfile, create_new_file_truncates_to_zero)
{
	const std::string path = r16tmp::file("bf1", "a.bin");
	r16tmp::clean("bf1");
	BoostFile bf;
	ASSERT_TRUE(bf.create_new_file(path.c_str()));
	EXPECT_TRUE(bf.valid());
	bip::offset_t sz = -1;
	ASSERT_TRUE(bf.get_file_size(sz));
	EXPECT_EQ((long long)0, (long long)sz);
	EXPECT_EQ((unsigned long long)0, bf.get_file_size());
	bf.close_file();
	EXPECT_TRUE(bf.is_invalid_file());
	ASSERT_TRUE(std::filesystem::exists(path));
	r16tmp::clean("bf1");
}

TEST(r16_boostfile, create_new_file_on_existing_clears_content)
{
	const std::string path = r16tmp::file("bf2", "a.bin");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("1234567890", 10));
	}
	{
		BoostFile again;
		ASSERT_TRUE(again.create_new_file(path.c_str()));
		EXPECT_EQ((unsigned long long)0, again.get_file_size());
	}
	r16tmp::clean("bf2");
}

TEST(r16_boostfile, open_existing_missing_returns_false)
{
	const std::string path = r16tmp::file("bf3", "no_such.bin");
	r16tmp::clean("bf3");
	BoostFile bf;
	EXPECT_FALSE(bf.open_existing_file(path.c_str()));
	EXPECT_FALSE(bf.valid());
	// 静态尺寸助手对缺失文件返回 0
	EXPECT_EQ((unsigned long long)0, BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("bf3");
}

TEST(r16_boostfile, seek_begin_current_end_and_pointer)
{
	const std::string path = r16tmp::file("bf4", "a.bin");
	{
		BoostFile bf;
		ASSERT_TRUE(bf.create_new_file(path.c_str()));
		ASSERT_TRUE(bf.write_file("abcdefghij", 10));
		EXPECT_TRUE(bf.seek_to_begin());
		EXPECT_EQ((unsigned long long)0, bf.get_file_pointer());
		EXPECT_TRUE(bf.seek_current(3));
		EXPECT_EQ((unsigned long long)3, bf.get_file_pointer());
		EXPECT_TRUE(bf.seek_to_end());
		bip::offset_t off = -1;
		ASSERT_TRUE(bf.get_file_pointer(off));
		EXPECT_EQ((long long)10, (long long)off);
		EXPECT_TRUE(bf.seek_to_end(-2));
		EXPECT_EQ((unsigned long long)8, bf.get_file_pointer());
	}
	r16tmp::clean("bf4");
}

TEST(r16_boostfile, write_after_truncate_shrinks_file)
{
	const std::string path = r16tmp::file("bf5", "a.bin");
	{
		BoostFile bf;
		ASSERT_TRUE(bf.create_new_file(path.c_str()));
		ASSERT_TRUE(bf.write_file("abcdefghij", 10));
		EXPECT_EQ((unsigned long long)10, bf.get_file_size());
		ASSERT_TRUE(bf.truncate_file(4));
		EXPECT_EQ((unsigned long long)4, bf.get_file_size());
	}
	r16tmp::clean("bf5");
}

TEST(r16_boostfile, read_file_exact_length_true_short_read_false)
{
	const std::string path = r16tmp::file("bf6", "a.bin");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file(std::string("abcd")));
	}
	{
		BoostFile r;
		ASSERT_TRUE(r.open_existing_file(path.c_str(), bip::read_only));
		char buf[5] = {0};
		// 恰好 4 字节 → 成功
		EXPECT_TRUE(r.read_file(buf, 4));
		EXPECT_EQ(0, memcmp(buf, "abcd", 4));
		// 文件只剩 2 字节，请求 9 字节 → 部分读 → 返回 false
		EXPECT_TRUE(r.seek_to_begin(2));
		char big[9] = {0};
		EXPECT_FALSE(r.read_file(big, 9));
		// 同一个语义在 read_file_length 上返回实际字节数 2
		EXPECT_TRUE(r.seek_to_begin(2));
		char small[9] = {0};
		EXPECT_EQ(2, r.read_file_length(small, 9));
		EXPECT_EQ(0, memcmp(small, "cd", 2));
	}
	r16tmp::clean("bf6");
}

TEST(r16_boostfile, write_to_closed_handle_fails)
{
	const std::string path = r16tmp::file("bf7", "a.bin");
	BoostFile bf;
	ASSERT_TRUE(bf.create_new_file(path.c_str()));
	bf.close_file();
	EXPECT_FALSE(bf.write_file("x", 1));
	EXPECT_FALSE(bf.truncate_file(1));
	r16tmp::clean("bf7");
}

TEST(r16_boostfile, static_write_and_read_file_contents_roundtrip)
{
	const std::string path = r16tmp::file("bf8", "c.bin");
	r16tmp::clean("bf8");
	const std::string payload = "hello boost file";
	ASSERT_TRUE(BoostFile::write_file_contents(path.c_str(), payload.data(), (uint32_t)payload.size()));

	std::string out;
	ASSERT_TRUE(BoostFile::read_file_contents(path.c_str(), out));
	EXPECT_EQ(payload, out);
	r16tmp::clean("bf8");
}

TEST(r16_boostfile, read_file_contents_false_for_missing_or_empty)
{
	const std::string missing = r16tmp::file("bf9", "none.bin");
	const std::string emptyPath = r16tmp::file("bf9", "empty.bin");
	r16tmp::clean("bf9");
	(void)r16tmp::dir("bf9");
	{
		BoostFile e;
		ASSERT_TRUE(e.create_new_file(emptyPath.c_str()));
	}
	std::string out;
	EXPECT_FALSE(BoostFile::read_file_contents(missing.c_str(), out));
	EXPECT_FALSE(BoostFile::read_file_contents(emptyPath.c_str(), out));
	r16tmp::clean("bf9");
}

TEST(r16_boostfile, delete_file_true_then_false)
{
	const std::string path = r16tmp::file("bf10", "d.bin");
	{
		BoostFile bf;
		ASSERT_TRUE(bf.create_new_file(path.c_str()));
	}
	ASSERT_TRUE(std::filesystem::exists(path));
	EXPECT_TRUE(BoostFile::delete_file(path.c_str()));
	EXPECT_FALSE(std::filesystem::exists(path));
	EXPECT_FALSE(BoostFile::delete_file(path.c_str()));
	r16tmp::clean("bf10");
}

TEST(r16_boostfile, create_or_open_keeps_existing_size)
{
	const std::string path = r16tmp::file("bf11", "e.bin");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("abcdef", 6));
	}
	{
		BoostFile bf;
		ASSERT_TRUE(bf.create_or_open_file(path.c_str()));
		// create_or_open 不截断，尺寸仍是 6
		EXPECT_EQ((unsigned long long)6, bf.get_file_size());
	}
	r16tmp::clean("bf11");
}
