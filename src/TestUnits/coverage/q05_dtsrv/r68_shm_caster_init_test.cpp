/*!
 * \file r68_shm_caster_init_test.cpp
 * \brief WtDtCore/ShmCaster::init 的失败形态补测（迭代01 第十三波 R68）
 *
 * 与既有 r18_shm_caster_test.cpp 的分工：
 *  - R18 已覆盖 DataItem/CastQueue 尺寸与默认值、cfg==NULL / active=false 两条拒绝、
 *    四个 broadcast 的未初始化守卫；
 *  - 本文件验证 init(active=true) 在「path 是已存在目录」和空 path 时安全返回 false；
 *    对应 BUG-R68-002，init 必须检查文件准备/映射结果并捕获映射异常。
 *
 * 不建立真实共享内存映射；目录和空路径均是本地文件系统拒绝场景。
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include "r68_test_util.h"

namespace fs = std::filesystem;

namespace {

class R68ShmCasterInitTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r68::make_tmp_dir("shm"); }

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		r68::write_file(p, body);
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	fs::path _dir;
};

TEST_F(R68ShmCasterInitTest, InitWithDirectoryPathReturnsFalseWithoutThrowing)
{
	fs::path dirAsPath = _dir / "shm_dir";
	fs::create_directories(dirAsPath);

	// 注意：不能用 R"JSON(...)" + 字符串拼接，前一段 raw literal 少了 JSON 定界符
	// 会把整段拼接文本吞进字面量，rapidjson 解析失败导致 cfg==NULL。
	WTSVariant* cfg = loadCfg("dir_path.json",
		std::string("{\"active\": true, \"path\": \"") + dirAsPath.string() + "\"}");
	ASSERT_NE(cfg, nullptr);

	ShmCaster caster;
	bool initialized = false;
	EXPECT_NO_THROW(initialized = caster.init(cfg));
	EXPECT_FALSE(initialized);
	cfg->release();
}

TEST_F(R68ShmCasterInitTest, InitWithEmptyPathReturnsFalseWithoutTouchingFilesystem)
{
	WTSVariant* cfg = loadCfg("empty_path.json", R"JSON({"active": true, "path": ""})JSON");
	ASSERT_NE(cfg, nullptr);

	ShmCaster caster;
	bool initialized = true;
	EXPECT_NO_THROW(initialized = caster.init(cfg));
	EXPECT_FALSE(initialized);
	cfg->release();

	EXPECT_EQ(std::distance(fs::directory_iterator(_dir), fs::directory_iterator{}), 1);
}

// ShmCaster.cpp:31-32 —— active=false 时即使 path 指向一个已有文件，也必须原样保留
// 文件内容/大小（拒绝发生在 L38 的 create_or_open/truncate 之前）。
TEST_F(R68ShmCasterInitTest, InactiveInitLeavesExistingPathUntouched)
{
	fs::path dataFile = _dir / "cast_queue.bin";
	r68::write_file(dataFile, "R68SENTINEL");
	const std::uintmax_t before = fs::file_size(dataFile);

	WTSVariant* cfg = loadCfg("inactive3.json",
		std::string("{\"active\": false, \"path\": \"") + dataFile.string() + "\"}");
	ASSERT_NE(cfg, nullptr);

	ShmCaster caster;
	EXPECT_FALSE(caster.init(cfg));
	cfg->release();

	EXPECT_TRUE(fs::exists(dataFile));
	EXPECT_EQ(fs::file_size(dataFile), before);
	std::ifstream ifs(dataFile.string());
	std::string content((std::istreambuf_iterator<char>(ifs)),
		std::istreambuf_iterator<char>());
	EXPECT_EQ(content, "R68SENTINEL");
}

// ShmCaster.h:65-89 —— pack(8) 下队列头的四个标量偏移固定为 0/8/16/24，
// items 紧跟在 32 字节之后；这是共享内存跨进程 ABI 的一部分。
TEST_F(R68ShmCasterInitTest, CastQueueHeaderOffsetsArePackEightAligned)
{
	using CastQueue = ShmCaster::CastQueue;
	static CastQueue q;	// 4MB 级对象放静态区

	const volatile char* base = reinterpret_cast<const volatile char*>(&q);
	EXPECT_EQ(reinterpret_cast<const volatile char*>(&q._capacity) - base, 0);
	EXPECT_EQ(reinterpret_cast<const volatile char*>(&q._readable) - base, 8);
	EXPECT_EQ(reinterpret_cast<const volatile char*>(&q._writable) - base, 16);
	EXPECT_EQ(reinterpret_cast<const volatile char*>(&q._pid) - base, 24);
	EXPECT_EQ(reinterpret_cast<const volatile char*>(&q._items[0]) - base, 32);
	EXPECT_EQ(sizeof(CastQueue), 32u + 8192u * sizeof(ShmCaster::DataItem));
}

}	// namespace
