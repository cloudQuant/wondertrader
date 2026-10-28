/*!
 * \file r16_kvcache_test.cpp
 * \brief R16 覆盖率用例：src/Share/WtKVCache.hpp（基于 BoostMappingFile 的文件映射 KV 缓存）
 *
 * 用普通磁盘文件（临时目录 + pid 隔离）做 mmap，不涉及 System V / POSIX 共享内存段，
 * 不起线程、不读挂钟。
 */
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include "Share/WtKVCache.hpp"
#include "Share/BoostFile.hpp"
#include "r16_tmp.h"

namespace {

// CacheBlock 头：char[8] flag + volatile uint32 + uint32 + uint32 = 20 字节
// CacheItem：char _key[64] + char _val[64] = 128 字节
const uint64_t kBlockHead = 20;
const uint64_t kItemSize = 128;
const uint32_t kInitStep = 200;		// SIZE_STEP

const uint32_t kDate = 20200101;

std::vector<std::string> g_logs;

void use_logger_capture()
{
	g_logs.clear();
}

wtp::CacheLogger make_logger()
{
	return [](const char* msg) { g_logs.emplace_back(msg == NULL ? std::string("(null)") : msg); };
}

bool log_has(const std::string& needle)
{
	for (const std::string& s : g_logs)
		if (s.find(needle) != std::string::npos)
			return true;
	return false;
}

}	// namespace

TEST(r16_kvcache, uninitialized_object_reports_empty)
{
	wtp::WtKVCache cache;
	EXPECT_EQ(0u, cache.size());
	EXPECT_EQ(0u, cache.capacity());
	EXPECT_FALSE(cache.has("anything"));
	// 未命中时 get 直接返回 ""，不会去解引用空 block
	EXPECT_STREQ("", cache.get("anything"));
}

TEST(r16_kvcache, init_creates_file_with_initial_capacity)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv1", "c1.cache");
	r16tmp::clean("kv1");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_EQ(0u, cache.size());
	}
	// 20 + 200*128 = 25620
	EXPECT_EQ(kBlockHead + kItemSize * kInitStep, BoostFile::get_file_size(path.c_str()));
	// 新建文件既没有映射失败也不是「日期变更」，所以一条日志都不该有
	EXPECT_TRUE(g_logs.empty());
	r16tmp::clean("kv1");
}

TEST(r16_kvcache, put_get_has_and_overwrite)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv2", "c1.cache");
	r16tmp::clean("kv2");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
		cache.put("k1", "v1");
		EXPECT_EQ(2u, cache.size());
		EXPECT_TRUE(cache.has("k0"));
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v1", cache.get("k1"));
		EXPECT_STREQ("", cache.get("k_not_there"));

		// 已存在的键：命中 _indice 分支，不增加 size
		cache.put("k0", "v0b");
		EXPECT_EQ(2u, cache.size());
		EXPECT_STREQ("v0b", cache.get("k0"));
	}
	r16tmp::clean("kv2");
}

TEST(r16_kvcache, put_if_none_keeps_existing_value)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv3", "c1.cache");
	r16tmp::clean("kv3");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put_if_none("only", "first");
		EXPECT_STREQ("first", cache.get("only"));
		// 命中分支：直接 return，不覆盖
		cache.put_if_none("only", "second");
		EXPECT_STREQ("first", cache.get("only"));
		cache.put_if_none("other", "third");
		EXPECT_STREQ("third", cache.get("other"));
		EXPECT_EQ(2u, cache.size());
	}
	r16tmp::clean("kv3");
}

TEST(r16_kvcache, put_with_len_beyond_64_corrupts_next_item_key)
{
	// BUG-R16-07 的可观测后果：put(key,val,len) 把 len 原样交给 wt_strcpy，
	// 而 _val 只有 64 字节，wt_strcpy 还会在 des[len] 处补 '\0'。
	// 本用例喂 len=70 → 6 字节数据 + 1 个终止符溢出到下一条目的 _key[0..6]。
	const std::string path = r16tmp::file("kv4", "c1.cache");
	r16tmp::clean("kv4");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		const std::string longval(70, 'x');
		cache.put("k0", longval.c_str(), 70);
		// get 读的是 C 串，长度被溢出到下一项的终止符决定 → 70 个 x
		EXPECT_EQ(longval, std::string(cache.get("k0")));
		// 溢出写把 item[1] 的 _key 前 6 字节变成了 "xxxxxx"
		EXPECT_TRUE(cache.has("xxxxxx"));
	}
	r16tmp::clean("kv4");
}

TEST(r16_kvcache, DISABLED_put_with_len_beyond_64_should_be_rejected)
{
	// 期望行为：超出 64 字节的 val/len 应被拒绝或截断；见 BUG-R16-07。
	const std::string path = r16tmp::file("kv4", "c2.cache");
	wtp::WtKVCache cache;
	ASSERT_TRUE(cache.init(path.c_str(), kDate));
	cache.put("k0", std::string(70, 'x').c_str(), 70);
	EXPECT_FALSE(cache.has("xxxxxx"));
	r16tmp::clean("kv4");
}

TEST(r16_kvcache, put_fills_capacity_then_resizes_to_double)
{
	const std::string path = r16tmp::file("kv5", "c1.cache");
	r16tmp::clean("kv5");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		for (uint32_t i = 0; i < kInitStep; i++)
		{
			const std::string k = std::string("k") + std::to_string(i);
			const std::string v = std::string("v") + std::to_string(i);
			cache.put(k.c_str(), v.c_str());
		}
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());

		// 第 201 条触发 resize：capacity 200 -> 400
		cache.put("k200", "v200");
		EXPECT_EQ(400u, cache.capacity());
		EXPECT_EQ(201u, cache.size());
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v199", cache.get("k199"));
		EXPECT_STREQ("v200", cache.get("k200"));
	}
	// 20 + 400*128 = 51220
	EXPECT_EQ(kBlockHead + kItemSize * 400, BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("kv5");
}

TEST(r16_kvcache, clear_empties_entries_but_keeps_capacity)
{
	const std::string path = r16tmp::file("kv6", "c1.cache");
	r16tmp::clean("kv6");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("a", "1");
		cache.put("b", "2");
		ASSERT_EQ(2u, cache.size());
		cache.clear();
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_FALSE(cache.has("a"));
		EXPECT_STREQ("", cache.get("a"));
		// 清空后可以继续写
		cache.put("c", "3");
		EXPECT_EQ(1u, cache.size());
		EXPECT_STREQ("3", cache.get("c"));
	}
	r16tmp::clean("kv6");
}

TEST(r16_kvcache, reopen_with_same_date_restores_entries)
{
	const std::string path = r16tmp::file("kv7", "c1.cache");
	r16tmp::clean("kv7");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
		cache.put("k1", "v1");
	}
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));	// 同日期 → 不重置，索引从 block 重建
		EXPECT_EQ(2u, cache.size());
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v1", cache.get("k1"));
	}
	r16tmp::clean("kv7");
}

TEST(r16_kvcache, reopen_with_other_date_resets_and_logs)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv8", "c1.cache");
	r16tmp::clean("kv8");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
	}
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), 20200202, make_logger()));
		EXPECT_EQ(0u, cache.size());
		EXPECT_FALSE(cache.has("k0"));
		EXPECT_TRUE(log_has("Cache file reset due to a different date"));
	}
	r16tmp::clean("kv8");
}

TEST(r16_kvcache, init_on_unwritable_path_fails_with_log)
{
	use_logger_capture();
	// 父目录不存在：create_new_file 失败（返回值被忽略），随后 map 失败 → init 返回 false
	const std::string path = r16tmp::dir("kv9") + "/no_such_sub_dir_r16/c.cache";
	r16tmp::clean("kv9");
	{
		wtp::WtKVCache cache;
		EXPECT_FALSE(cache.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_TRUE(log_has("Mapping cache file failed"));
	}
	r16tmp::clean("kv9");
}

TEST(r16_kvcache, truncated_file_same_date_repairs_capacity_from_real_size)
{
	const std::string path = r16tmp::file("kv10", "c1.cache");
	r16tmp::clean("kv10");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
		cache.put("k1", "v1");
		cache.put("k2", "v2");
	}
	// 把文件截到 20 + 10*128 = 1300 字节（模拟 capacity 标记与实际扩容不一致）
	{
		BoostFile f;
		ASSERT_TRUE(f.open_existing_file(path.c_str()));
		ASSERT_TRUE(f.truncate_file((std::size_t)(kBlockHead + kItemSize * 10)));
	}
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));	// 同日期 → 走「自动恢复」分支
		// 容量按真实文件大小回填：(1300-20)/128 = 10
		EXPECT_EQ(10u, cache.capacity());
		// size 被写成 min(realCap, markedCap) = 10，而不是真实的 3 条
		EXPECT_EQ(10u, cache.size());
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v2", cache.get("k2"));
		// 第 4~10 槽是全零的 _key，索引里因此多出一个空串键（重建索引未过滤空键）
		EXPECT_TRUE(cache.has(""));
	}
	r16tmp::clean("kv10");
}

TEST(r16_kvcache, DISABLED_truncated_file_other_date_memset_out_of_mapping)
{
	// 触发条件（BUG-R16-08）：
	//   1) 先以 capacity=200 建好缓存文件（25620 字节）并写入若干条；
	//   2) 把文件截短到 1300 字节（映射区域随之只有 1300 字节）；
	//   3) 用一个「不同的 uDate」再 init()。
	// init() 在 L144-152 先用**旧的文件头 _capacity(=200)** memset 200*128=25600 字节，
	// 而「按真实文件大小回填 capacity」的自愈逻辑在 L161-179 才执行 → 越界写映射区 → SIGBUS/SIGSEGV。
	const std::string path = r16tmp::file("kv11", "c1.cache");
	r16tmp::clean("kv11");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
	}
	{
		BoostFile f;
		ASSERT_TRUE(f.open_existing_file(path.c_str()));
		ASSERT_TRUE(f.truncate_file((std::size_t)(kBlockHead + kItemSize * 10)));
	}
	{
		wtp::WtKVCache cache;
		// 期望：按真实尺寸先校正容量，再清零 → 不越界、返回 true
		ASSERT_TRUE(cache.init(path.c_str(), 20200303));
		EXPECT_EQ(10u, cache.capacity());
		EXPECT_EQ(0u, cache.size());
	}
	r16tmp::clean("kv11");
}
