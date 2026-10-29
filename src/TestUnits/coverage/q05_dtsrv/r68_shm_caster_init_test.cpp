/*!
 * \file r68_shm_caster_init_test.cpp
 * \brief WtDtCore/ShmCaster::init 的失败形态补测（迭代01 第十三波 R68）
 *
 * 与既有 r18_shm_caster_test.cpp 的分工（只读复用，不修改）：
 *  - R18 已覆盖 DataItem/CastQueue 尺寸与默认值、cfg==NULL / active=false 两条拒绝、
 *    四个 broadcast 的未初始化守卫；
 *  - 本文件补 init(active=true) 在「path 是已存在目录」时的失败形态：BoostFile 的
 *    create/truncate 返回值被忽略、BoostMappingFile::map 抛出的 interprocess_exception
 *    没有被 init 捕获，函数没有按 bool 契约返回 false（BUG-R68-002）。
 *    该路径在 file_mapping 构造处即失败，不会建立任何映射、不创建 shm 文件。
 *
 * 不建立真实共享内存映射；目录路径在映射前就抛异常。
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

/*
 * ShmCaster.cpp:35-51 —— path 指向一个已存在的目录：
 *   1) BoostFile::create_or_open_file / truncate_file 均失败，但返回值被忽略（L39-43）；
 *   2) BoostMappingFile::map（L47）在 file_mapping 构造处抛 interprocess_exception。
 * init 没有 try/catch、也没有把失败转成 return false，异常直接穿透调用方。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R68.md BUG-R68-002。
 * 该路径未建立映射，也未创建/截断任何文件，因此不违反「不建真实 shm 映射」约束。
 */
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
