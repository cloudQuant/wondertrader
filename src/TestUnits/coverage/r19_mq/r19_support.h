/*!
 * \file r19_support.h
 * \brief lane R19 (mqshare / WtShareHelper + WtMsgQue) 用例公共设施
 *
 * 只服务本轮 r19_mq/ 目录下的用例，不触碰产品代码。
 * 约定：
 * - 所有映射都走「显式 path 的文件映射」（ShareBlocks.cpp L15-17/L93-95），
 *   每个用例一个独立临时目录，TearDown 里 remove_all 自清理；
 * - 不用 POSIX shm_open、不起线程、不 sleep、不碰真实网络；
 * - 日志走 shareblock::ShareBlocks::register_logger（ShareBlocks.h L119/L121），
 *   断言只看「有没有对应文本」，不锁死墙钟数值。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include <unistd.h>

#include "WtShareHelper/ShareBlocks.h"

namespace r19 {

// ---------------------------------------------------------------- 日志捕获
struct LogSink
{
	std::vector<uint32_t>	levels;
	std::vector<std::string> msgs;
};

inline LogSink& sink()
{
	static LogSink g_sink;
	return g_sink;
}

inline void sb_logger(uint32_t lvl, const char* msg)
{
	sink().levels.push_back(lvl);
	sink().msgs.push_back(msg == NULL ? std::string("<null>") : std::string(msg));
}

/// 注册本轮用例自己的 logger 并清空历史（同进程多 TU 顺序执行时互不污染）
inline void attach_logger()
{
	shareblock::ShareBlocks::one().register_logger(sb_logger);
	sink().levels.clear();
	sink().msgs.clear();
}

inline bool has_log(const std::string& needle)
{
	for (const std::string& m : sink().msgs)
	{
		if (m.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

inline size_t log_count()
{
	return sink().msgs.size();
}

// ---------------------------------------------------------------- 临时目录
inline std::string case_dir(const std::string& tag)
{
	static uint32_t seq = 0;
	std::string name = std::string("r19_") + tag + "_" + std::to_string(::getpid()) +
		"_" + std::to_string(++seq);
	auto p = std::filesystem::temp_directory_path() / name;
	std::error_code ec;
	std::filesystem::create_directories(p, ec);
	return p.string();
}

/// 在 dir 下拼一个映射文件路径（dir 由 TestFixture 管理，自动清理）
inline std::string in_dir(const std::string& dir, const char* name)
{
	return (std::filesystem::path(dir) / name).string();
}

// ---------------------------------------------------------------- 裸块构造
/**
 * @brief 按 ShareBlocks.h 的 pack(1) 布局造一份磁盘镜像，用于覆盖
 *        init_master 的「失效 section 压缩」分支（ShareBlocks.cpp L36-59）等
 *        正常 API 走不到的入口；只写自己创建的临时文件。
 */
inline bool write_raw_shm_block(const std::string& path, const void* data, std::size_t bytes)
{
	std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
	if (!ofs.is_open())
		return false;
	ofs.write((const char*)data, (std::streamsize)bytes);
	ofs.close();
	return true;
}

/// 分配一块全零的 sizeof(ShmBlock) 缓冲（放堆上，避免 480KB 栈对象）
inline std::vector<char> make_zero_block()
{
	std::vector<char> buf(sizeof(shareblock::ShmBlock), 0);
	return buf;
}

/// 往 SecInfo 里塞一个 int32 key（同时写 _keys 和 _data），返回 section 引用
inline shareblock::SecInfo& sec_at(char* block, uint32_t idx)
{
	return *reinterpret_cast<shareblock::SecInfo*>(
		block + offsetof(shareblock::ShmBlock, _sections) + idx * sizeof(shareblock::SecInfo));
}

inline void put_int32_key(shareblock::SecInfo& sec, uint16_t slot, const char* key,
	int32_t value, uint32_t offset)
{
	shareblock::KeyInfo& k = sec._keys[slot];
	std::memset(k._key, 0, sizeof(k._key));
	std::strncpy(k._key, key, sizeof(k._key) - 1);
	k._type = shareblock::SMVT_INT32;
	k._offset = offset;
	k._updatetime = 111000 + slot;
	*sec.get<int32_t>(offset) = value;
}

/// 域名单据（进程内 ShareBlocks::one() 是单例，_shm_blocks 只增不减，
/// 除 release_slave 外没有清理入口 —— 见本轮缺陷清单「接缝」条目）
inline std::string domain(const char* tag)
{
	static uint32_t seq = 0;
	return std::string("r19d_") + tag + "_" + std::to_string(++seq);
}

} // namespace r19

/**
 * @brief 每个用例独占临时目录，TearDown 必清理（ASSERT_* 提前 return 也能走到）
 */
class R19TempDir : public ::testing::Test
{
protected:
	void SetUp() override { dir_ = r19::case_dir(test_name()); }
	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(dir_, ec);
	}

	const std::string& dir() const { return dir_; }
	std::string file(const char* name) const { return r19::in_dir(dir_, name); }

private:
	std::string test_name() const
	{
		const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
		return info == NULL ? std::string("case") : std::string(info->test_case_name()) + "_" + info->name();
	}

	std::string dir_;
};
