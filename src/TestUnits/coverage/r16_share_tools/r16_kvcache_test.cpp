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
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include "Share/WtKVCache.hpp"
#include "Share/BoostFile.hpp"
#include "r16_tmp.h"

namespace wtp {
struct WtKVCacheTestAccess
{
	typedef WtKVCache::FileIdentity FileIdentity;
	struct ShortWriteCreateOps : WtKVCache::FileCreateOps
	{
		bool injected = false;
		bool write(bip::file_handle_t handle, const void* data, std::size_t size)
		{
			if (!injected && size < sizeof(WtKVCache::CacheItem) && size > 1)
			{
				injected = true;
				WtKVCache::FileCreateOps::write(handle, data, size / 2);
				return true; // Windows WriteFile can report success with a short byte count.
			}
			return WtKVCache::FileCreateOps::write(handle, data, size);
		}
	};

	template <typename Ops>
	static bool resize(WtKVCache& cache, uint32_t capacity, Ops& ops)
	{
		return cache.resize_with_ops(capacity, ops);
	}
	static bool double_capacity(uint32_t current, uint32_t& doubled)
	{
		return WtKVCache::calculate_doubled_capacity(current, doubled);
	}
	static bool get_file_identity(bip::file_handle_t handle, FileIdentity& identity)
	{
		return WtKVCache::get_file_identity(handle, identity);
	}
	static bool get_mapping_identity(const WtKVCache& cache, FileIdentity& identity)
	{
		return cache._cache._file != NULL && cache._cache._file->get_file_identity(identity.volume, identity.file)
			? (identity.valid = true) : false;
	}
	static bool create_initialized_file(const char* filename, uint32_t date, FileIdentity& identity)
	{
		return WtKVCache::create_initialized_file(filename, date, identity);
	}
	template <typename Ops>
	static bool create_initialized_file_with_ops(const char* filename, uint32_t date, FileIdentity& identity, Ops& ops)
	{
		return WtKVCache::create_initialized_file_with_ops(filename, date, identity, ops);
	}
	template <typename Ops>
	static bool init(WtKVCache& cache, const char* filename, uint32_t date, CacheLogger logger, Ops& ops)
	{
		return cache.init_with_ops(filename, date, std::move(logger), ops);
	}
	static bool has_mapping(const WtKVCache& cache) { return cache._cache._file != NULL; }
	static bool has_block(const WtKVCache& cache) { return cache._cache._block != NULL; }
	static void force_growth_state(WtKVCache& cache, uint32_t size, uint32_t capacity)
	{
		cache._cache._block->_size = size;
		cache._cache._block->_capacity = capacity;
	}
};
}

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

bool read_binary_file(const std::string& path, std::string& contents)
{
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input)
		return false;
	contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return !input.bad();
}

enum class ResizeFailure { open, seek, write, mapping, write_and_rollback, write_aligned_and_rollback, replace_path_before_open, replace_path_during_mapping };

struct FailingResizeOps
{
	FailingResizeOps(ResizeFailure failure, const std::string& replacementPath = std::string(), const std::string& backupPath = std::string())
		: failure(failure), replacementPath(replacementPath), backupPath(backupPath) {}
	~FailingResizeOps() { close_file(); }
	ResizeFailure failure;
	std::string filename;
	std::string replacementPath;
	std::string backupPath;
	bip::file_handle_t handle = bip::ipcdetail::invalid_file();
	std::size_t openCalls = 0;
	std::size_t truncateCalls = 0;
	bool lastTruncateResult = false;

	bool open_existing_file(const char* filename)
	{
		++openCalls;
		if (failure == ResizeFailure::open)
			return false;
		this->filename = filename;
		close_file();
		if (failure == ResizeFailure::replace_path_before_open)
		{
			if (replacementPath.empty() || backupPath.empty() || std::rename(filename, backupPath.c_str()) != 0)
				return false;
			if (std::rename(replacementPath.c_str(), filename) != 0)
			{
				std::rename(backupPath.c_str(), filename);
				return false;
			}
		}
		handle = bip::ipcdetail::open_existing_file(filename, bip::read_write);
		return handle != bip::ipcdetail::invalid_file();
	}
	bool get_file_identity(wtp::WtKVCacheTestAccess::FileIdentity& identity)
	{
		return wtp::WtKVCacheTestAccess::get_file_identity(handle, identity);
	}
	bool get_file_size(uint64_t& size)
	{
		bip::offset_t fileSize = 0;
		if (!bip::ipcdetail::get_file_size(handle, fileSize) || fileSize < 0)
			return false;
		size = (uint64_t)fileSize;
		return true;
	}
	bool seek_to_end()
	{
		if (failure == ResizeFailure::seek)
			return false;
		return bip::ipcdetail::set_file_pointer(handle, 0, bip::file_end);
	}
	bool get_file_pointer(uint64_t& pos)
	{
		bip::offset_t filePos = 0;
		if (!bip::ipcdetail::get_file_pointer(handle, filePos) || filePos < 0)
			return false;
		pos = (uint64_t)filePos;
		return true;
	}
	bool set_file_pointer(uint64_t pos)
	{
		return bip::ipcdetail::set_file_pointer(handle, (bip::offset_t)pos, bip::file_begin);
	}
	bool write_file(const void* data, std::size_t size)
	{
		if (failure == ResizeFailure::write || failure == ResizeFailure::write_and_rollback ||
			failure == ResizeFailure::write_aligned_and_rollback)
		{
			if (size > 1)
			{
				const std::size_t partialSize = failure == ResizeFailure::write_and_rollback ? size / 2 + 1 : size / 2;
				bip::ipcdetail::write_file(handle, data, partialSize); // real partial extension before the injected failure
			}
			return false;
		}
		return bip::ipcdetail::write_file(handle, data, size);
	}
	bool read_file(void* data, std::size_t size)
	{
#if defined(_WIN32)
		unsigned long bytesRead = 0;
		return bip::winapi::read_file(handle, data, (unsigned long)size, &bytesRead, 0) && bytesRead == size;
#else
		return ::read(handle, data, size) == (ssize_t)size;
#endif
	}
	bool truncate_file(uint64_t size)
	{
		++truncateCalls;
		if (failure == ResizeFailure::write_and_rollback || failure == ResizeFailure::write_aligned_and_rollback)
		{
			lastTruncateResult = false;
			return false;
		}
		lastTruncateResult = bip::ipcdetail::truncate_file(handle, (std::size_t)size);
		return lastTruncateResult;
	}
	bool close_file()
	{
		if (handle == bip::ipcdetail::invalid_file())
			return true;
		if (!bip::ipcdetail::close_file(handle))
			return false;
		handle = bip::ipcdetail::invalid_file();
		return true;
	}
	bool map(BoostMappingFile& mapping, const char* filename)
	{
		if (failure == ResizeFailure::mapping)
			return false;
		if (failure == ResizeFailure::replace_path_during_mapping)
		{
			if (replacementPath.empty() || backupPath.empty() || std::rename(filename, backupPath.c_str()) != 0)
				return false;
			if (std::rename(replacementPath.c_str(), filename) != 0)
			{
				std::rename(backupPath.c_str(), filename);
				return false;
			}
		}
		return mapping.map(filename);
	}
};

enum class InitFailure
{
	mapping,
	rebuilding_index,
	external_file_created_during_exclusive_create,
	short_header_write,
	map_replacement_file
};

struct FailingInitOps
{
	FailingInitOps(InitFailure failure, std::size_t failAt, const std::string& replacementPath = std::string())
		: failure(failure), failAt(failAt), replacementPath(replacementPath) {}
	InitFailure failure;
	std::size_t failAt;
	std::string replacementPath;
	std::size_t mapCalls = 0;
	std::size_t indexCalls = 0;

	bool create_initialized_file(const char* filename, uint32_t date, wtp::WtKVCacheTestAccess::FileIdentity& identity)
	{
		if (failure == InitFailure::external_file_created_during_exclusive_create)
		{
			// Model another process winning the create race after the caller's
			// exists() check. The exclusive-create operation must fail without
			// truncating or deleting that process's file.
			BoostFile external;
			if (!external.create_new_file(filename))
				return false;
			const char marker[] = "external-file";
			external.write_file(marker, sizeof(marker));
			external.close_file();
			return false;
		}
		if (failure == InitFailure::short_header_write)
		{
			wtp::WtKVCacheTestAccess::ShortWriteCreateOps ops;
			return wtp::WtKVCacheTestAccess::create_initialized_file_with_ops(filename, date, identity, ops);
		}
		return wtp::WtKVCacheTestAccess::create_initialized_file(filename, date, identity);
	}

	bool map(BoostMappingFile& mapping, const char* filename)
	{
		++mapCalls;
		if (failure == InitFailure::map_replacement_file && mapCalls == failAt)
		{
			// Model the mapping open resolving to a different, valid cache file
			// after the target was created.
			if (replacementPath.empty())
				return false;
			return mapping.map(replacementPath.c_str());
		}
		if (failure == InitFailure::mapping && mapCalls == failAt)
		{
			if (!mapping.map(filename))
				return false;
			throw std::runtime_error("injected exception after mapping completed");
		}
		return mapping.map(filename);
	}
	template <typename Index>
	void add_index(Index& index, const char* key, std::size_t keyLength, uint32_t value)
	{
		++indexCalls;
		if (failure == InitFailure::rebuilding_index && indexCalls == failAt)
			throw std::runtime_error("injected index rebuild exception");
		index[std::string(key, keyLength)] = value;
	}
};

void fill_cache(wtp::WtKVCache& cache)
{
	for (uint32_t i = 0; i < kInitStep; ++i)
	{
		const std::string key = "k" + std::to_string(i);
		const std::string value = "v" + std::to_string(i);
		cache.put(key.c_str(), value.c_str());
	}
}

}	// namespace

TEST(r16_kvcache, uninitialized_object_reports_empty)
{
	wtp::WtKVCache cache;
	EXPECT_EQ(0u, cache.size());
	EXPECT_EQ(0u, cache.capacity());
	EXPECT_FALSE(cache.has("anything"));
	EXPECT_FALSE(cache.try_put("anything", "value"));
	EXPECT_FALSE(cache.try_put_if_none("anything", "value"));
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
		EXPECT_TRUE(cache.try_put("k2", "v2"));
		EXPECT_TRUE(cache.try_put("k2", "v2b"));
		EXPECT_TRUE(cache.try_put_if_none("k2", "ignored"));
		EXPECT_TRUE(cache.try_put_if_none("k3", "v3"));
		EXPECT_EQ(4u, cache.size());
		EXPECT_TRUE(cache.has("k0"));
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v1", cache.get("k1"));
		EXPECT_STREQ("v2b", cache.get("k2"));
		EXPECT_STREQ("v3", cache.get("k3"));
		EXPECT_STREQ("", cache.get("k_not_there"));

		// 已存在的键：命中 _indice 分支，不增加 size
		cache.put("k0", "v0b");
		EXPECT_EQ(4u, cache.size());
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
		EXPECT_TRUE(cache.try_put_if_none("only", "ignored"));
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

TEST(r16_kvcache, oversized_keys_and_values_are_rejected_without_corrupting_records)
{
	const std::string path = r16tmp::file("kv4", "c1.cache");
	use_logger_capture();
	const std::string maxKey(63, 'k');
	const std::string maxValue(63, 'v');
	const std::string overOneValue(64, 'x');
	const std::string longValue(70, 'y');
	const std::string overOneKey(64, 'a');
	const std::string longKey(70, 'b');
	r16tmp::clean("kv4");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		cache.put("oversized", "initial");
		cache.put("guard", "guard-value");

		// Both overload paths accept the largest safe value (63 bytes plus NUL).
		cache.put("max_zero_len", maxValue.c_str());
		cache.put("max_explicit_len", maxValue.c_str(), maxValue.size());
		cache.put_if_none("max_if_none_zero_len", maxValue.c_str());
		cache.put_if_none("max_if_none_explicit_len", maxValue.c_str(), maxValue.size());
		cache.put(maxKey.c_str(), "max-key");
		ASSERT_EQ(7u, cache.size());

		// Reject over-limit updates and insertions before touching either record.
		cache.put("oversized", overOneValue.c_str(), overOneValue.size());	// explicit len=64
		cache.put("oversized", longValue.c_str());								// len=0, strlen=70
		cache.put_if_none("bad_if_none_value_64", overOneValue.c_str(), overOneValue.size());
		cache.put_if_none("bad_explicit_value", longValue.c_str(), longValue.size());
		cache.put_if_none("bad_zero_value", longValue.c_str());
		cache.put(overOneKey.c_str(), "bad-key-64");
		cache.put_if_none(longKey.c_str(), "bad-key-70");

		EXPECT_EQ(7u, cache.size());
		EXPECT_STREQ("initial", cache.get("oversized"));
		EXPECT_STREQ("guard-value", cache.get("guard"));
		EXPECT_FALSE(cache.has("bad_if_none_value_64"));
		EXPECT_FALSE(cache.has("bad_explicit_value"));
		EXPECT_FALSE(cache.has("bad_zero_value"));
		EXPECT_FALSE(cache.has(overOneKey.c_str()));
		EXPECT_FALSE(cache.has(longKey.c_str()));
		EXPECT_TRUE(log_has("key exceeds 63 bytes"));
		EXPECT_TRUE(log_has("value exceeds 63 bytes"));
		EXPECT_TRUE(log_has("value length exceeds 63 bytes"));
	}
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		// Reopening rebuilds the index from disk and proves all accepted entries survived.
		EXPECT_EQ(7u, cache.size());
		EXPECT_TRUE(cache.has("guard"));
		EXPECT_STREQ("guard-value", cache.get("guard"));
		EXPECT_STREQ("initial", cache.get("oversized"));
		EXPECT_STREQ(maxValue.c_str(), cache.get("max_zero_len"));
		EXPECT_STREQ(maxValue.c_str(), cache.get("max_explicit_len"));
		EXPECT_STREQ(maxValue.c_str(), cache.get("max_if_none_zero_len"));
		EXPECT_STREQ(maxValue.c_str(), cache.get("max_if_none_explicit_len"));
		EXPECT_STREQ("max-key", cache.get(maxKey.c_str()));
	}
	r16tmp::clean("kv4");
}

TEST(r16_kvcache, value_at_63_byte_limit_is_readable)
{
	const std::string path = r16tmp::file("kv4_max_value", "c1.cache");
	r16tmp::clean("kv4_max_value");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		const std::string value(63, 'v');
		cache.put("max", value.c_str(), value.size());
		EXPECT_EQ(1u, cache.size());
		EXPECT_STREQ(value.c_str(), cache.get("max"));
	}
	r16tmp::clean("kv4_max_value");
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

TEST(r16_kvcache, init_exceptions_clear_partial_mapping_and_index)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_init_exception_cleanup", "c1.cache");
	r16tmp::clean("kv_init_exception_cleanup");
	{
		wtp::WtKVCache seed;
		ASSERT_TRUE(seed.init(path.c_str(), kDate));
		seed.put("first", "one");
		seed.put("second", "two");
		seed.put("third", "three");
	}

	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::mapping, 1);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(1u, ops.mapCalls);
		EXPECT_EQ(0u, ops.indexCalls);
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
		EXPECT_FALSE(cache.has("first"));
		EXPECT_STREQ("", cache.get("first"));
	}

	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::rebuilding_index, 2);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(1u, ops.mapCalls);
		EXPECT_EQ(2u, ops.indexCalls);
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
		EXPECT_FALSE(cache.has("first"));
		EXPECT_STREQ("", cache.get("first"));
	}

	// Both exceptions leave the persisted cache intact and a fresh init can recover it.
	{
		wtp::WtKVCache recovered;
		ASSERT_TRUE(recovered.init(path.c_str(), kDate));
		EXPECT_EQ(3u, recovered.size());
		EXPECT_STREQ("one", recovered.get("first"));
		EXPECT_STREQ("two", recovered.get("second"));
		EXPECT_STREQ("three", recovered.get("third"));
	}
	EXPECT_TRUE(log_has("Cache initialization failed due to an exception"));
	r16tmp::clean("kv_init_exception_cleanup");
}

TEST(r16_kvcache, new_file_map_exception_leaves_valid_file_for_retry)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_new_file_map_exception", "c1.cache");
	r16tmp::clean("kv_new_file_map_exception");
	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::mapping, 1);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(1u, ops.mapCalls);
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
	}

	// Header and slots were initialized before mmap. A mapping exception does
	// not require deleting the path and a fresh cache can retry it safely.
	EXPECT_EQ(kBlockHead + kItemSize * kInitStep, BoostFile::get_file_size(path.c_str()));
	{
		wtp::WtKVCache recovered;
		ASSERT_TRUE(recovered.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(kInitStep, recovered.capacity());
		EXPECT_EQ(0u, recovered.size());
		EXPECT_FALSE(recovered.has("anything"));
	}
	r16tmp::clean("kv_new_file_map_exception");
}

TEST(r16_kvcache, short_header_write_is_rejected_without_deleting_created_file)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_new_file_short_header_write", "c1.cache");
	r16tmp::clean("kv_new_file_short_header_write");
	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::short_header_write, 1);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(0u, ops.mapCalls);
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
	}

	// The injected writer physically writes only half the header but reports
	// API success. Position/readback validation rejects it; the path is left in
	// place and subsequent recovery fails closed rather than claiming success.
	ASSERT_EQ(kBlockHead + kItemSize * kInitStep, BoostFile::get_file_size(path.c_str()));
	EXPECT_TRUE(log_has("exclusive initialization"));
	const uint64_t failedFileSize = BoostFile::get_file_size(path.c_str());
	{
		wtp::WtKVCache reopened;
		EXPECT_FALSE(reopened.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, reopened.size());
		EXPECT_EQ(0u, reopened.capacity());
	}
	EXPECT_EQ(failedFileSize, BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("kv_new_file_short_header_write");
}

TEST(r16_kvcache, mapped_replacement_file_is_rejected_by_creation_marker)
{
	use_logger_capture();
	const std::string tag = "kv_new_file_map_replacement";
	const std::string path = r16tmp::file(tag.c_str(), "created.cache");
	const std::string replacementPath = r16tmp::file(tag.c_str(), "replacement.cache");
	r16tmp::clean(tag.c_str());
	wtp::WtKVCacheTestAccess::FileIdentity replacementIdentity;
	ASSERT_TRUE(wtp::WtKVCacheTestAccess::create_initialized_file(replacementPath.c_str(), kDate, replacementIdentity));
	const uint64_t expectedFileSize = kBlockHead + kItemSize * kInitStep;

	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::map_replacement_file, 1, replacementPath);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(1u, ops.mapCalls);
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
	}
	EXPECT_TRUE(log_has("not the exclusively created file"));
	EXPECT_EQ(expectedFileSize, BoostFile::get_file_size(path.c_str()));
	EXPECT_EQ(expectedFileSize, BoostFile::get_file_size(replacementPath.c_str()));

	// The mapped replacement and the newly created target were both preserved;
	// the target's temporary marker permits a clean retry after fail-closed.
	{
		wtp::WtKVCache recovered;
		ASSERT_TRUE(recovered.init(path.c_str(), kDate));
		EXPECT_EQ(kInitStep, recovered.capacity());
		EXPECT_EQ(0u, recovered.size());
	}
	EXPECT_EQ(expectedFileSize, BoostFile::get_file_size(replacementPath.c_str()));
	r16tmp::clean(tag.c_str());
}

TEST(r16_kvcache, exclusive_create_race_does_not_remove_external_file)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_external_create_race", "c1.cache");
	r16tmp::clean("kv_external_create_race");
	{
		wtp::WtKVCache cache;
		FailingInitOps ops(InitFailure::external_file_created_during_exclusive_create, 1);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::init(cache, path.c_str(), kDate, make_logger(), ops));
		EXPECT_EQ(0u, ops.mapCalls);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_mapping(cache));
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::has_block(cache));
	}

	std::string contents;
	ASSERT_TRUE(read_binary_file(path, contents));
	const char expected[] = "external-file";
	EXPECT_EQ(std::string(expected, sizeof(expected)), contents);
	EXPECT_EQ(sizeof(expected), BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("kv_external_create_race");
}

TEST(r16_kvcache, init_rejects_nonterminated_disk_fields_without_mutating_file)
{
	const char* tags[] = { "kv_bad_disk_key", "kv_bad_disk_value" };
	use_logger_capture();
	for (std::size_t badField = 0; badField < 2; ++badField)
	{
		SCOPED_TRACE(tags[badField]);
		const std::string path = r16tmp::file(tags[badField], "c1.cache");
		r16tmp::clean(tags[badField]);
		{
			wtp::WtKVCache seed;
			ASSERT_TRUE(seed.init(path.c_str(), kDate));
			for (uint32_t i = 0; i < 10; ++i)
			{
				const std::string key = "k" + std::to_string(i);
				const std::string value = "v" + std::to_string(i);
				seed.put(key.c_str(), value.c_str());
			}
		}
		{
			BoostFile file;
			ASSERT_TRUE(file.open_existing_file(path.c_str()));
			ASSERT_TRUE(file.truncate_file((std::size_t)(kBlockHead + kItemSize * 10)));
		}
		{
			BoostMappingFile mapping;
			ASSERT_TRUE(mapping.map(path.c_str()));
			char* lastItem = static_cast<char*>(mapping.addr()) + kBlockHead + kItemSize * 9;
			if (badField == 0)
			{
				std::memset(lastItem, 'K', 64);
				std::memset(lastItem + 64, 'V', 64);
			}
			else
				std::memset(lastItem + 64, 'V', 64);
			mapping.sync();
		}

		std::string before;
		ASSERT_TRUE(read_binary_file(path, before));
		{
			wtp::WtKVCache rejected;
			EXPECT_FALSE(rejected.init(path.c_str(), kDate, make_logger()));
			EXPECT_EQ(0u, rejected.size());
			EXPECT_EQ(0u, rejected.capacity());
			EXPECT_FALSE(rejected.has("k0"));
			EXPECT_STREQ("", rejected.get("k0"));
		}
		std::string after;
		ASSERT_TRUE(read_binary_file(path, after));
		EXPECT_EQ(before, after);
		EXPECT_EQ(kBlockHead + kItemSize * 10, BoostFile::get_file_size(path.c_str()));
		EXPECT_TRUE(log_has("key or value without a terminator"));
		r16tmp::clean(tags[badField]);
	}
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

TEST(r16_kvcache, resize_file_and_mapping_failures_preserve_old_mapping)
{
	const char* tags[] = { "kv_resize_open_fail", "kv_resize_seek_fail", "kv_resize_write_fail", "kv_resize_map_fail" };
	const ResizeFailure failures[] = { ResizeFailure::open, ResizeFailure::seek, ResizeFailure::write, ResizeFailure::mapping };
	for (std::size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
	{
		SCOPED_TRACE(tags[i]);
		use_logger_capture();
		const std::string path = r16tmp::file(tags[i], "c1.cache");
		r16tmp::clean(tags[i]);
		const uint64_t oldFileSize = kBlockHead + kItemSize * kInitStep;
		uint64_t fileSizeAfterFailure = 0;
		bool completeSlotLayout = false;
		bool rollbackReportedFailed = false;
		{
			wtp::WtKVCache cache;
			ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
			fill_cache(cache);
			ASSERT_EQ(kInitStep, cache.size());
			ASSERT_EQ(kInitStep, cache.capacity());

			FailingResizeOps ops(failures[i]);
			EXPECT_FALSE(wtp::WtKVCacheTestAccess::resize(cache, kInitStep * 2, ops));
			EXPECT_EQ((i < 2) ? 0u : 1u, ops.truncateCalls);
			EXPECT_EQ(kInitStep, cache.size());
			EXPECT_EQ(kInitStep, cache.capacity());
			EXPECT_TRUE(cache.has("k0"));
			EXPECT_STREQ("v0", cache.get("k0"));
			EXPECT_TRUE(cache.has("k199"));
			EXPECT_STREQ("v199", cache.get("k199"));
			EXPECT_FALSE(cache.has("injected_failure"));
			fileSizeAfterFailure = BoostFile::get_file_size(path.c_str());
			completeSlotLayout = fileSizeAfterFailure >= kBlockHead &&
				(fileSizeAfterFailure - kBlockHead) % kItemSize == 0;
			// The implementation treats rollback as successful only when both
			// truncate_file succeeds and the old file length is verified afterward.
			rollbackReportedFailed = ops.truncateCalls != 0 &&
				(!ops.lastTruncateResult || fileSizeAfterFailure != oldFileSize);
			EXPECT_EQ(rollbackReportedFailed, log_has("rollback on original handle also failed"));
			if (fileSizeAfterFailure != oldFileSize)
			{
				if (failures[i] == ResizeFailure::write)
				{
					EXPECT_EQ(oldFileSize + (kItemSize * kInitStep) / 2, fileSizeAfterFailure);
					EXPECT_TRUE(completeSlotLayout);
				}
				else
					EXPECT_EQ(kBlockHead + kItemSize * kInitStep * 2, fileSizeAfterFailure);
			}
			else
				EXPECT_EQ(oldFileSize, fileSizeAfterFailure);
		}

		// Shrinking while the old mapping is alive can be rejected by Windows.
		// In that case the live object remains usable. Restart accepts a complete
		// slot layout (including a partial write that ended on a slot boundary) and
		// fails closed only for a non-slot tail.
		{
			wtp::WtKVCache reopened;
			if (!completeSlotLayout)
			{
				EXPECT_FALSE(reopened.init(path.c_str(), kDate, make_logger()));
				EXPECT_EQ(0u, reopened.size());
				EXPECT_EQ(0u, reopened.capacity());
				EXPECT_TRUE(log_has("complete header and slot layout"));
			}
			else
			{
				ASSERT_TRUE(reopened.init(path.c_str(), kDate, make_logger()));
				EXPECT_EQ((fileSizeAfterFailure - kBlockHead) / kItemSize, reopened.capacity());
				EXPECT_EQ(kInitStep, reopened.size());
				EXPECT_STREQ("v0", reopened.get("k0"));
				EXPECT_STREQ("v199", reopened.get("k199"));
			}
		}
		EXPECT_EQ(fileSizeAfterFailure, BoostFile::get_file_size(path.c_str()));
		r16tmp::clean(tags[i]);
	}
}

TEST(r16_kvcache, resize_rejects_replaced_path_before_reopen)
{
#if defined(_WIN32)
	GTEST_SKIP() << "Renaming an actively mapped file is not supported consistently on Windows";
#else
	use_logger_capture();
	const std::string tag = "kv_resize_replaced_before_open";
	const std::string path = r16tmp::file(tag.c_str(), "original.cache");
	const std::string replacementPath = r16tmp::file(tag.c_str(), "replacement.cache");
	const std::string backupPath = r16tmp::file(tag.c_str(), "original.backup");
	r16tmp::clean(tag.c_str());
	const uint64_t expectedSize = kBlockHead + kItemSize * kInitStep;
	{
		wtp::WtKVCache original;
		ASSERT_TRUE(original.init(path.c_str(), kDate, make_logger()));
		fill_cache(original);
		wtp::WtKVCache replacement;
		ASSERT_TRUE(replacement.init(replacementPath.c_str(), kDate));
		ASSERT_TRUE(replacement.try_put("replacement", "must remain unchanged"));
		const uint64_t replacementSize = BoostFile::get_file_size(replacementPath.c_str());

		FailingResizeOps ops(ResizeFailure::replace_path_before_open, replacementPath, backupPath);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::resize(original, kInitStep * 2, ops));
		EXPECT_EQ(1u, ops.openCalls);
		EXPECT_EQ(kInitStep, original.capacity());
		EXPECT_EQ(kInitStep, original.size());
		EXPECT_STREQ("v199", original.get("k199"));
		EXPECT_TRUE(log_has("reopened handle is not the original mapping file"));
		EXPECT_EQ(expectedSize, BoostFile::get_file_size(backupPath.c_str()));
		EXPECT_EQ(replacementSize, BoostFile::get_file_size(path.c_str()));
		EXPECT_TRUE(replacement.has("replacement"));
		EXPECT_STREQ("must remain unchanged", replacement.get("replacement"));
	}
	r16tmp::clean(tag.c_str());
#endif
}

TEST(r16_kvcache, resize_rollback_uses_original_handle_after_path_replaced)
{
#if defined(_WIN32)
	GTEST_SKIP() << "Renaming an actively mapped file is not supported consistently on Windows";
#else
	use_logger_capture();
	const std::string tag = "kv_resize_replaced_during_rollback";
	const std::string path = r16tmp::file(tag.c_str(), "original.cache");
	const std::string replacementPath = r16tmp::file(tag.c_str(), "third-party.cache");
	const std::string backupPath = r16tmp::file(tag.c_str(), "original.backup");
	r16tmp::clean(tag.c_str());
	const uint64_t oldSize = kBlockHead + kItemSize * kInitStep;
	const uint64_t expandedSize = kBlockHead + kItemSize * kInitStep * 2;
	{
		wtp::WtKVCache original;
		ASSERT_TRUE(original.init(path.c_str(), kDate, make_logger()));
		fill_cache(original);
		{
			BoostFile thirdParty;
			ASSERT_TRUE(thirdParty.create_new_file(replacementPath.c_str()));
			ASSERT_TRUE(thirdParty.truncate_file((std::size_t)expandedSize));
		}
		const uint64_t thirdPartySize = BoostFile::get_file_size(replacementPath.c_str());
		std::string thirdPartyBefore;
		ASSERT_TRUE(read_binary_file(replacementPath, thirdPartyBefore));

		FailingResizeOps ops(ResizeFailure::replace_path_during_mapping, replacementPath, backupPath);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::resize(original, kInitStep * 2, ops));
		EXPECT_EQ(kInitStep, original.capacity());
		EXPECT_EQ(kInitStep, original.size());
		EXPECT_STREQ("v0", original.get("k0"));
		EXPECT_EQ(oldSize, BoostFile::get_file_size(backupPath.c_str()));
		EXPECT_EQ(thirdPartySize, BoostFile::get_file_size(path.c_str()));
		EXPECT_EQ(expandedSize, BoostFile::get_file_size(path.c_str()));
		EXPECT_TRUE(log_has("Expanded mapping is not the original cache file"));
		std::string thirdPartyAfter;
		ASSERT_TRUE(read_binary_file(path, thirdPartyAfter));
		EXPECT_EQ(thirdPartyBefore, thirdPartyAfter);
	}
	r16tmp::clean(tag.c_str());
#endif
}

TEST(r16_kvcache, failed_rollback_keeps_old_mapping_but_restart_fails_closed)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_resize_rollback_fail", "c1.cache");
	r16tmp::clean("kv_resize_rollback_fail");
	const uint64_t oldFileSize = kBlockHead + kItemSize * kInitStep;
	const uint64_t requestedGrowth = kItemSize * kInitStep;
	const uint64_t partialWrite = requestedGrowth / 2 + 1; // one-byte non-slot tail
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		fill_cache(cache);
		ASSERT_EQ(kInitStep, cache.size());
		ASSERT_EQ(kInitStep, cache.capacity());

		FailingResizeOps ops(ResizeFailure::write_and_rollback);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::resize(cache, kInitStep * 2, ops));
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v199", cache.get("k199"));
		EXPECT_EQ(oldFileSize + partialWrite, BoostFile::get_file_size(path.c_str()));
		EXPECT_NE(0u, (BoostFile::get_file_size(path.c_str()) - kBlockHead) % kItemSize);
		EXPECT_TRUE(log_has("rollback on original handle also failed"));
	}

	// The failed rollback did not repair the persisted file. Reopening must reject
	// the partial slot layout instead of claiming recovery; the previous mapping
	// was usable only while the original cache object remained alive.
	const uint64_t damagedFileSize = BoostFile::get_file_size(path.c_str());
	{
		wtp::WtKVCache reopened;
		EXPECT_FALSE(reopened.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, reopened.size());
		EXPECT_EQ(0u, reopened.capacity());
		EXPECT_TRUE(log_has("complete header and slot layout"));
	}
	EXPECT_EQ(damagedFileSize, BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("kv_resize_rollback_fail");
}

TEST(r16_kvcache, failed_rollback_with_complete_slots_recovers_actual_capacity)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_resize_aligned_rollback_fail", "c1.cache");
	r16tmp::clean("kv_resize_aligned_rollback_fail");
	const uint64_t oldFileSize = kBlockHead + kItemSize * kInitStep;
	const uint64_t partialWrite = (kItemSize * kInitStep) / 2;
	const uint32_t recoveredCapacity = kInitStep + (uint32_t)(partialWrite / kItemSize);
	uint64_t expandedFileSize = 0;
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		fill_cache(cache);

		FailingResizeOps ops(ResizeFailure::write_aligned_and_rollback);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::resize(cache, kInitStep * 2, ops));
		EXPECT_EQ(1u, ops.truncateCalls);
		EXPECT_FALSE(ops.lastTruncateResult);
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v199", cache.get("k199"));
		expandedFileSize = BoostFile::get_file_size(path.c_str());
		EXPECT_EQ(oldFileSize + partialWrite, expandedFileSize);
		EXPECT_EQ(0u, (expandedFileSize - kBlockHead) % kItemSize);
		EXPECT_TRUE(log_has("rollback on original handle also failed"));
	}

	// The write stopped at a complete slot boundary. Although rollback failed,
	// the file remains structurally readable and init recovers its physical capacity.
	{
		wtp::WtKVCache reopened;
		ASSERT_TRUE(reopened.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(recoveredCapacity, reopened.capacity());
		EXPECT_EQ(kInitStep, reopened.size());
		EXPECT_STREQ("v0", reopened.get("k0"));
		EXPECT_STREQ("v199", reopened.get("k199"));
	}
	EXPECT_EQ(expandedFileSize, BoostFile::get_file_size(path.c_str()));
	r16tmp::clean("kv_resize_aligned_rollback_fail");
}

TEST(r16_kvcache, throwing_logger_does_not_escape_noexcept_insert)
{
#if defined(_WIN32)
	GTEST_SKIP() << "Renaming an actively mapped file is not supported consistently on Windows";
#else
	const std::string path = r16tmp::file("kv_resize_throwing_logger", "c1.cache");
	const std::string movedPath = path + ".moved";
	r16tmp::clean("kv_resize_throwing_logger");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, [](const char*) -> void { throw std::runtime_error("logger failure"); }));
		fill_cache(cache);
		ASSERT_EQ(0, std::rename(path.c_str(), movedPath.c_str()));
		EXPECT_FALSE(cache.try_put("throwing_log_put", "value"));
		EXPECT_FALSE(cache.try_put_if_none("throwing_log_put_if_none", "value"));
		cache.put("throwing_log_legacy_put", "value");
		cache.put_if_none("throwing_log_legacy_put_if_none", "value");
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_STREQ("v0", cache.get("k0"));
		ASSERT_EQ(0, std::rename(movedPath.c_str(), path.c_str()));
	}
	r16tmp::clean("kv_resize_throwing_logger");
#endif
}

TEST(r16_kvcache, put_calls_reject_new_record_when_resize_open_fails)
{
#if defined(_WIN32)
	GTEST_SKIP() << "Renaming an actively mapped file is not supported consistently on Windows";
#else
	use_logger_capture();
	const std::string path = r16tmp::file("kv_resize_caller_open_fail", "c1.cache");
	const std::string movedPath = path + ".moved";
	r16tmp::clean("kv_resize_caller_open_fail");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		fill_cache(cache);
		ASSERT_EQ(kInitStep, cache.size());
		ASSERT_EQ(kInitStep, cache.capacity());

		ASSERT_EQ(0, std::rename(path.c_str(), movedPath.c_str()));
		EXPECT_FALSE(cache.try_put("blocked_put", "value"));
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_FALSE(cache.has("blocked_put"));
		EXPECT_STREQ("v0", cache.get("k0"));
		ASSERT_EQ(0, std::rename(movedPath.c_str(), path.c_str()));

		ASSERT_EQ(0, std::rename(path.c_str(), movedPath.c_str()));
		EXPECT_FALSE(cache.try_put_if_none("blocked_put_if_none", "value"));
		EXPECT_EQ(kInitStep, cache.size());
		EXPECT_EQ(kInitStep, cache.capacity());
		EXPECT_FALSE(cache.has("blocked_put_if_none"));
		EXPECT_STREQ("v199", cache.get("k199"));
		ASSERT_EQ(0, std::rename(movedPath.c_str(), path.c_str()));
	}
	r16tmp::clean("kv_resize_caller_open_fail");
	EXPECT_TRUE(log_has("path no longer identifies original mapping"));
	EXPECT_TRUE(log_has("capacity expansion failed"));
#endif
}

TEST(r16_kvcache, capacity_doubling_overflow_is_rejected_before_insert)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_resize_overflow", "c1.cache");
	r16tmp::clean("kv_resize_overflow");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate, make_logger()));
		fill_cache(cache);
		uint32_t doubled = 0;
		EXPECT_TRUE(wtp::WtKVCacheTestAccess::double_capacity(kInitStep, doubled));
		EXPECT_EQ(kInitStep * 2, doubled);
		EXPECT_FALSE(wtp::WtKVCacheTestAccess::double_capacity((std::numeric_limits<uint32_t>::max)() / 2 + 1, doubled));

		const uint32_t overflowCapacity = (std::numeric_limits<uint32_t>::max)() / 2 + 1;
		wtp::WtKVCacheTestAccess::force_growth_state(cache, overflowCapacity, overflowCapacity);
		cache.put("overflow_rejected", "value");
		EXPECT_EQ(overflowCapacity, cache.size());
		EXPECT_FALSE(cache.has("overflow_rejected"));
		EXPECT_TRUE(log_has("capacity doubling would overflow uint32_t"));
		wtp::WtKVCacheTestAccess::force_growth_state(cache, kInitStep, kInitStep);
		EXPECT_EQ(kInitStep, cache.size());
	}
	r16tmp::clean("kv_resize_overflow");
}

TEST(r16_kvcache, rejects_file_smaller_than_cache_header)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_short_header", "c1.cache");
	r16tmp::clean("kv_short_header");
	{
		BoostFile file;
		ASSERT_TRUE(file.create_new_file(path.c_str()));
		ASSERT_TRUE(file.truncate_file(8));
	}
	{
		wtp::WtKVCache cache;
		EXPECT_FALSE(cache.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_TRUE(log_has("complete header and slot layout"));
	}
	r16tmp::clean("kv_short_header");
}

TEST(r16_kvcache, rejects_file_ending_inside_cache_item)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_partial_item", "c1.cache");
	r16tmp::clean("kv_partial_item");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
	}
	{
		BoostFile file;
		ASSERT_TRUE(file.open_existing_file(path.c_str()));
		ASSERT_TRUE(file.truncate_file((std::size_t)(kBlockHead + kItemSize + 1)));
	}
	{
		wtp::WtKVCache cache;
		EXPECT_FALSE(cache.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_TRUE(log_has("complete header and slot layout"));
	}
	r16tmp::clean("kv_partial_item");
}

TEST(r16_kvcache, rejects_file_with_invalid_header_flag)
{
	use_logger_capture();
	const std::string path = r16tmp::file("kv_bad_flag", "c1.cache");
	r16tmp::clean("kv_bad_flag");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
	}
	{
		BoostMappingFile mapping;
		ASSERT_TRUE(mapping.map(path.c_str()));
		static_cast<char*>(mapping.addr())[0] = 'X';
		mapping.sync();
	}
	{
		wtp::WtKVCache cache;
		EXPECT_FALSE(cache.init(path.c_str(), kDate, make_logger()));
		EXPECT_EQ(0u, cache.size());
		EXPECT_EQ(0u, cache.capacity());
		EXPECT_TRUE(log_has("header flag is invalid"));
	}
	r16tmp::clean("kv_bad_flag");
}

TEST(r16_kvcache, truncated_file_same_date_preserves_records_and_repairs_capacity)
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
	// 显式 flush 已关闭的缓存映射，确保文件中的头和三条记录在截断前已同步。
	{
		BoostMappingFile mapping;
		ASSERT_TRUE(mapping.map(path.c_str()));
		mapping.sync();
	}
	// 把文件截到 20 + 10*128 = 1300 字节（模拟 capacity 标记与实际扩容不一致）
	{
		BoostFile f;
		ASSERT_TRUE(f.open_existing_file(path.c_str()));
		ASSERT_TRUE(f.truncate_file((std::size_t)(kBlockHead + kItemSize * 10)));
	}
	ASSERT_EQ(kBlockHead + kItemSize * 10, BoostFile::get_file_size(path.c_str()));
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));	// 同日期 → 按真实文件大小恢复容量
		EXPECT_EQ(10u, cache.capacity());
		// 截断只改变可用容量，不应把空槽算作已写记录。
		EXPECT_EQ(3u, cache.size());
		EXPECT_TRUE(cache.has("k0"));
		EXPECT_TRUE(cache.has("k1"));
		EXPECT_TRUE(cache.has("k2"));
		EXPECT_STREQ("v0", cache.get("k0"));
		EXPECT_STREQ("v1", cache.get("k1"));
		EXPECT_STREQ("v2", cache.get("k2"));
		EXPECT_FALSE(cache.has(""));
	}
	r16tmp::clean("kv10");
}

TEST(r16_kvcache, truncated_file_other_date_resets_without_writing_past_mapping)
{
	// BUG-R16-08：日期切换必须先按实际映射大小恢复容量，再清零记录槽。
	const std::string path = r16tmp::file("kv11", "c1.cache");
	r16tmp::clean("kv11");
	{
		wtp::WtKVCache cache;
		ASSERT_TRUE(cache.init(path.c_str(), kDate));
		cache.put("k0", "v0");
		cache.put("k1", "v1");
		cache.put("k2", "v2");
	}
	{
		BoostMappingFile mapping;
		ASSERT_TRUE(mapping.map(path.c_str()));
		mapping.sync();
	}
	{
		BoostFile f;
		ASSERT_TRUE(f.open_existing_file(path.c_str()));
		ASSERT_TRUE(f.truncate_file((std::size_t)(kBlockHead + kItemSize * 10)));
	}
	ASSERT_EQ(kBlockHead + kItemSize * 10, BoostFile::get_file_size(path.c_str()));
	{
		wtp::WtKVCache cache;
		// 异日 → 只清理文件中实际存在的十个槽，旧记录不再可见。
		ASSERT_TRUE(cache.init(path.c_str(), 20200303));
		EXPECT_EQ(10u, cache.capacity());
		EXPECT_EQ(0u, cache.size());
		EXPECT_FALSE(cache.has("k0"));
		cache.put("next_day", "value");
		EXPECT_EQ(1u, cache.size());
		EXPECT_STREQ("value", cache.get("next_day"));
	}
	r16tmp::clean("kv11");
}
