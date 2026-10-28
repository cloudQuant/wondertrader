// R55（第十一波补测）：BoostMappingFile 全接口 + StdUtils 异常/长路径 + CpuHelper 绑定边界。
// 文件映射只做普通磁盘文件的 mmap（/tmp 下按 pid 隔离），不涉及共享内存/线程。
// 既有 r16 用例只通过 WtKVCache 间接碰过 map/addr/size，本文件补默认态/只读/同步/失败面。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include "Share/BoostFile.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/CpuHelper.hpp"
#include "Share/StdUtils.hpp"
#include "r16_tmp.h"

//////////////////////////////////////////////////////////////////////////
// 默认态与幂等 close（BoostMappingFile.hpp:33-71、81-102、114-119）
TEST(R55_mappingfile, default_state_and_idempotent_close)
{
	BoostMappingFile mf;
	EXPECT_FALSE(mf.valid());
	EXPECT_EQ(nullptr, mf.addr());
	EXPECT_EQ((size_t)0, mf.size());
	EXPECT_STREQ("", mf.filename());
	// 未映射时 sync 是 no-op，close 可重复调用
	mf.sync();
	mf.close();
	mf.close();
	EXPECT_FALSE(mf.valid());
}

// 映射不存在的文件：返回 false 且不改内部文件名（BoostMappingFile.hpp:142-150）
TEST(R55_mappingfile, map_missing_file_returns_false)
{
	const std::string path = r16tmp::file("r55map1", "missing.bin");
	r16tmp::clean("r55map1");
	BoostMappingFile mf;
	EXPECT_FALSE(mf.map(path.c_str()));
	EXPECT_FALSE(mf.valid());
	EXPECT_EQ(nullptr, mf.addr());
	EXPECT_STREQ("", mf.filename());
	r16tmp::clean("r55map1");
}

// 读写映射 + sync 落盘（BoostMappingFile.hpp:81-85、97-163）
TEST(R55_mappingfile, read_write_mapping_syncs_modification_to_disk)
{
	const std::string path = r16tmp::file("r55map2", "m.bin");
	r16tmp::clean("r55map2");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("abcd", 4));
	}
	{
		BoostMappingFile mf;
		ASSERT_TRUE(mf.map(path.c_str()));
		EXPECT_TRUE(mf.valid());
		EXPECT_EQ((size_t)4, mf.size());
		EXPECT_STREQ(path.c_str(), mf.filename());
		char* p = static_cast<char*>(mf.addr());
		ASSERT_NE(nullptr, p);
		EXPECT_EQ(0, std::memcmp(p, "abcd", 4));
		p[0] = 'Z';
		mf.sync();
		mf.close();
		EXPECT_FALSE(mf.valid());
	}
	std::string back;
	ASSERT_TRUE(BoostFile::read_file_contents(path.c_str(), back));
	EXPECT_EQ(std::string("Zbcd"), back);
	r16tmp::clean("r55map2");
}

// 只读映射：内容可读，mode 参数透传（BoostMappingFile.hpp:142-163）
TEST(R55_mappingfile, read_only_mapping_exposes_content)
{
	const std::string path = r16tmp::file("r55map3", "m.bin");
	r16tmp::clean("r55map3");
	{
		BoostFile w;
		ASSERT_TRUE(w.create_new_file(path.c_str()));
		ASSERT_TRUE(w.write_file("r55", 3));
	}
	BoostMappingFile mf;
	ASSERT_TRUE(mf.map(path.c_str(), bip::read_only, bip::read_only));
	ASSERT_TRUE(mf.valid());
	ASSERT_EQ((size_t)3, mf.size());
	const char* p = static_cast<const char*>(mf.addr());
	ASSERT_NE(nullptr, p);
	EXPECT_EQ(0, std::memcmp(p, "r55", 3));
	mf.close();
	r16tmp::clean("r55map3");
}

//////////////////////////////////////////////////////////////////////////
// StdFile：超长非法路径走 exists 的 catch，写目录触发 ofstream 失败（StdUtils.hpp:126-146）
TEST(R55_stdutils, exists_long_path_false_and_write_to_directory_throws)
{
	// 超过 PATH_MAX 的路径：filesystem 抛错时被 catch 成 false，不抛到调用方
	const std::string longPath = "/tmp/wondertrader_r55_" + std::string(5000, 'x');
	EXPECT_FALSE(StdFile::exists(longPath.c_str()));

	// 目标是一个已存在的目录：void* 版 write 打开失败 → runtime_error（L129-132）
	const std::string dir = r16tmp::dir("r55std1");
	EXPECT_THROW(StdFile::write_file_content(dir.c_str(), static_cast<const void*>("x"), 1),
		std::runtime_error);
	r16tmp::clean("r55std1");
}

//////////////////////////////////////////////////////////////////////////
// CpuHelper：核数缓存 + 越界绑定拒绝（CpuHelper.hpp:11-42）
TEST(R55_cpuhelper, core_count_and_out_of_range_binding)
{
	const uint32_t cores = CpuHelper::get_cpu_cores();
	EXPECT_EQ(std::thread::hardware_concurrency(), cores);
	ASSERT_GT(cores, 0u);

	// i >= cores 必须在调用系统 API 之前拒绝
	EXPECT_FALSE(CpuHelper::bind_core(cores));
	EXPECT_FALSE(CpuHelper::bind_core(0xFFFFFFFFu));

	// 0 号核：macOS 的 thread affinity 只是 hint，返回值随系统策略变化，
	// 这里只执行成功路径，不把 OS 策略写进断言。
	(void)CpuHelper::bind_core(0);
}
