// R55（第十一波补测）：IniHelper 注释/直接路径/显式另存 + BoostFile 失败句柄/扩容
//                    + WtKVCache 的 put_if_none 扩容与“文件变大”自愈分支。
// 只碰普通磁盘文件（/tmp 下按 pid + tag 隔离），不涉及共享内存/线程/挂钟。
// 既有 r16_ini_boostfile / r16_kvcache 已覆盖的主路径不再重复。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Share/BoostFile.hpp"
#include "Share/IniHelper.hpp"
#include "Share/WtKVCache.hpp"
#include "r16_tmp.h"

namespace {

const uint32_t kR55Date = 20200101;
const uint64_t kR55ItemSize = 128;	// CacheItem: _key[64] + _val[64]

void writeText(const std::string& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f << text;
	f.close();
}

}	// namespace

//////////////////////////////////////////////////////////////////////////
// IniHelper：整行注释、键值空白、空值、尾部空节（IniHelper.hpp:73-86、260-268、278-295）
TEST(R55_inihelper, comments_whitespace_and_empty_section_are_ignored)
{
	const std::string path = r16tmp::file("r55ini1", "a.ini");
	r16tmp::clean("r55ini1");
	writeText(path,
		"; semicolon comment\n"
		"# hash comment\n"
		"[sys]\n"
		"name = alpha beta\n"
		"empty =\n"
		"[tail]\n"
		"; tail has no key, parser drops the section\n");

	IniHelper ini;
	ini.load(path.c_str());
	EXPECT_TRUE(ini.isLoaded());
	EXPECT_EQ(std::string("alpha beta"), ini.readString("sys", "name"));
	// 键存在但值为空：返回空串而不是 fallback，说明键被解析出来了
	EXPECT_EQ(std::string(""), ini.readString("sys", "empty", "fallback"));

	FieldArray secs;
	EXPECT_EQ(1, ini.readSections(secs));
	ASSERT_EQ((size_t)1, secs.size());
	EXPECT_EQ(std::string("sys"), secs[0]);

	FieldArray keys;
	EXPECT_EQ(2, ini.readSecKeyArray("sys", keys));
	r16tmp::clean("r55ini1");
}

// IniHelper 直接读写带点路径（IniHelper.hpp:159-170、334-338）
TEST(R55_inihelper, direct_read_write_value_with_dotted_path)
{
	IniHelper ini;
	ini.writeValue<std::string>("alpha.beta", "x");
	ini.writeValue<int>("alpha.num", 7);
	ini.writeValue<uint32_t>("alpha.u", 9u);

	EXPECT_EQ(std::string("x"), ini.readValue<std::string>("alpha.beta", "d"));
	EXPECT_EQ(7, ini.readValue<int>("alpha.num", 0));
	EXPECT_EQ(9u, ini.readValue<uint32_t>("alpha.u", 0u));
	EXPECT_EQ(1.5, ini.readValue<double>("alpha.missing", 1.5));
}

// save(filename) 只写目标文件，不回头改已加载文件（IniHelper.hpp:95-101）
TEST(R55_inihelper, save_to_explicit_path_does_not_touch_loaded_file)
{
	const std::string src = r16tmp::file("r55ini3", "src.ini");
	const std::string dst = r16tmp::file("r55ini3", "dst.ini");
	r16tmp::clean("r55ini3");
	writeText(src, "[sys]\nname=old\n");

	{
		IniHelper ini;
		ini.load(src.c_str());
		ini.writeString("sys", "name", "new");
		ini.save(dst.c_str());
	}

	IniHelper srcCheck;
	srcCheck.load(src.c_str());
	EXPECT_EQ(std::string("old"), srcCheck.readString("sys", "name"));

	IniHelper dstCheck;
	dstCheck.load(dst.c_str());
	EXPECT_EQ(std::string("new"), dstCheck.readString("sys", "name"));
	r16tmp::clean("r55ini3");
}

//////////////////////////////////////////////////////////////////////////
// BoostFile：静态尺寸成功路径 + 无效句柄的指针/尺寸查询（BoostFile.hpp:90-145）
TEST(R55_boostfile, static_size_and_invalid_handle_pointer_queries)
{
	const std::string path = r16tmp::file("r55bf1", "a.bin");
	r16tmp::clean("r55bf1");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("abcde", 5));
	}
	EXPECT_EQ((unsigned long long)5, BoostFile::get_file_size(path.c_str()));

	BoostFile invalid;
	bip::offset_t size = 123;
	EXPECT_FALSE(invalid.get_file_size(size));
	EXPECT_EQ((unsigned long long)0, invalid.get_file_size());

	bip::offset_t off = 123;
	EXPECT_FALSE(invalid.get_file_pointer(off));
	EXPECT_EQ((unsigned long long)0, invalid.get_file_pointer());
	r16tmp::clean("r55bf1");
}

// truncate 扩大文件：新区域补零，可整段读回（BoostFile.hpp:85-88、157-166）
TEST(R55_boostfile, truncate_growth_zero_fills_and_reads_back)
{
	const std::string path = r16tmp::file("r55bf2", "a.bin");
	r16tmp::clean("r55bf2");
	BoostFile bf;
	ASSERT_TRUE(bf.create_new_file(path.c_str()));
	ASSERT_TRUE(bf.write_file("abcd", 4));
	ASSERT_TRUE(bf.truncate_file(16));
	EXPECT_EQ((unsigned long long)16, bf.get_file_size());

	ASSERT_TRUE(bf.seek_to_begin());
	char buf[16];
	std::memset(buf, 0x7F, sizeof(buf));
	ASSERT_TRUE(bf.read_file(buf, sizeof(buf)));
	EXPECT_EQ(0, std::memcmp(buf, "abcd", 4));
	for (std::size_t i = 4; i < sizeof(buf); i++)
		EXPECT_EQ(0, buf[i]);
	r16tmp::clean("r55bf2");
}

// 只读句柄：读成功、写/截断被拒绝（BoostFile.hpp:60-64、85-88、147-150）
TEST(R55_boostfile, read_only_handle_reads_but_rejects_write_and_truncate)
{
	const std::string path = r16tmp::file("r55bf3", "a.bin");
	r16tmp::clean("r55bf3");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("abcd", 4));
	}

	BoostFile ro;
	ASSERT_TRUE(ro.open_existing_file(path.c_str(), bip::read_only));
	ASSERT_TRUE(ro.seek_to_begin());
	char buf[4] = { 0 };
	EXPECT_TRUE(ro.read_file(buf, sizeof(buf)));
	EXPECT_EQ(0, std::memcmp(buf, "abcd", 4));
	EXPECT_FALSE(ro.write_file("x", 1));
	EXPECT_FALSE(ro.truncate_file(2));
	r16tmp::clean("r55bf3");
}

//////////////////////////////////////////////////////////////////////////
// WtKVCache::put_if_none 写满后的扩容分支（WtKVCache.hpp:234-253，241-246）
TEST(R55_kvcache, put_if_none_resizes_when_capacity_is_full)
{
	const std::string path = r16tmp::file("r55kv1", "c.cache");
	r16tmp::clean("r55kv1");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kR55Date));
		for (uint32_t i = 0; i < 200; i++)
		{
			const std::string k = std::string("k") + std::to_string(i);
			cache.put_if_none(k.c_str(), "v");
		}
		EXPECT_EQ(200u, cache.size());
		EXPECT_EQ(200u, cache.capacity());

		// 第 201 条且键不存在：走 put_if_none 自己的 resize 分支
		cache.put_if_none("k200", "v200");
		EXPECT_EQ(400u, cache.capacity());
		EXPECT_EQ(201u, cache.size());
		EXPECT_STREQ("v", cache.get("k0"));
		EXPECT_STREQ("v", cache.get("k199"));
		EXPECT_STREQ("v200", cache.get("k200"));
	}
	r16tmp::clean("r55kv1");
}

// 文件比文件头标记的 capacity 更大：按真实尺寸回填并取 min 作为 size（WtKVCache.hpp:161-179）
TEST(R55_kvcache, reopen_with_enlarged_file_caps_size_at_marked_capacity)
{
	const std::string path = r16tmp::file("r55kv2", "c.cache");
	r16tmp::clean("r55kv2");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kR55Date));
		cache.put("k0", "v0");
	}
	{
		// 文件头仍是 capacity=200/size=1，把文件扩到 400 条容量
		BoostFile f;
		ASSERT_TRUE(f.open_existing_file(path.c_str()));
		ASSERT_TRUE(f.seek_to_end());
		std::vector<char> zeros((std::size_t)(kR55ItemSize * 200), 0);
		ASSERT_TRUE(f.write_file(zeros.data(), zeros.size()));
	}
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kR55Date));
		EXPECT_EQ(400u, cache.capacity());
		// size 取 min(realCap=400, markedCap=200)
		EXPECT_EQ(200u, cache.size());
		EXPECT_STREQ("v0", cache.get("k0"));
		// 200 个槽位里只有第 0 个有键，其余空键都被重建索引收进 map
		EXPECT_TRUE(cache.has(""));
	}
	r16tmp::clean("r55kv2");
}
