/*!
 * \file r154_shm_caster_active_test.cpp
 * \brief ShmCaster active 装配与四个写入体（迭代01 第二十三波 R154）
 *
 * r18/r68/r145 只覆盖 cfg==NULL / active=false / 目录或空路径的拒绝形态；
 * ShmCaster.cpp:72-173 的四个写入体（_writable/_readable 推进、_type 置位、
 * memcpy 载荷）需要一份真正映射成功的队列。本用例把队列落到系统临时目录的
 * 普通文件（非 System V/POSIX shm 段、不跨进程），写入后用文件字节回读校验，
 * 用例结束由 ShmCaster 析构 unmap、fixture 删除临时目录。
 *
 * 覆盖：ShmCaster.cpp:24-65（active=true 成功段）、72-92、99-119、126-146、153-173。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/ShmCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include "r154_test_util.h"

namespace fs = std::filesystem;

namespace {

class R154ShmCasterActiveTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r154::make_tmp_dir("shm"); }

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path _dir;
};

TEST_F(R154ShmCasterActiveTest, ActiveInitWritesAllFourTypesIntoMappedQueue)
{
	const fs::path queuePath = _dir / "cast_queue.bin";
	r154::write_file(_dir / "active.json",
		std::string(R"JSON({"active": true, "path": ")JSON")
		+ queuePath.string() + R"JSON("})JSON");

	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "active.json").string().c_str());
	ASSERT_NE(cfg, nullptr);

	WTSTickStruct tickStruct;
	memset(&tickStruct, 0, sizeof(tickStruct));
	strcpy(tickStruct.code, "ag2512");
	strcpy(tickStruct.exchg, "SHFE");
	tickStruct.action_date = 20261028;
	tickStruct.trading_date = 20261028;
	tickStruct.price = 7250.5;

	WTSOrdQueStruct queStruct;
	memset(&queStruct, 0, sizeof(queStruct));
	strcpy(queStruct.code, "ag2512");
	strcpy(queStruct.exchg, "SHFE");

	WTSOrdDtlStruct dtlStruct;
	memset(&dtlStruct, 0, sizeof(dtlStruct));
	strcpy(dtlStruct.code, "ag2512");
	strcpy(dtlStruct.exchg, "SHFE");

	WTSTransStruct transStruct;
	memset(&transStruct, 0, sizeof(transStruct));
	strcpy(transStruct.code, "ag2512");
	strcpy(transStruct.exchg, "SHFE");

	{
		ShmCaster caster;
		// ShmCaster.cpp:24-65：active=true 时创建/截断临时文件、映射、placement new
		ASSERT_TRUE(caster.init(cfg));
		cfg->release();

		WTSTickData* tick = WTSTickData::create(tickStruct);
		WTSOrdQueData* que = WTSOrdQueData::create(queStruct);
		WTSOrdDtlData* dtl = WTSOrdDtlData::create(dtlStruct);
		WTSTransData* trans = WTSTransData::create(transStruct);
		ASSERT_NE(tick, nullptr);
		ASSERT_NE(que, nullptr);
		ASSERT_NE(dtl, nullptr);
		ASSERT_NE(trans, nullptr);

		// ShmCaster.cpp:72-173：四个写入体各执行一次
		caster.broadcast(tick);
		caster.broadcast(que);
		caster.broadcast(dtl);
		caster.broadcast(trans);

		tick->release();
		que->release();
		dtl->release();
		trans->release();
	}

	ASSERT_TRUE(fs::exists(queuePath));
	EXPECT_EQ(fs::file_size(queuePath), sizeof(ShmCaster::CastQueue));

	std::unique_ptr<void, void (*)(void*)> raw(::operator new(sizeof(ShmCaster::CastQueue)),
		[](void* p) { ::operator delete(p); });
	{
		std::ifstream ifs(queuePath.string(), std::ios::in | std::ios::binary);
		ASSERT_TRUE(ifs.good());
		ifs.read(static_cast<char*>(raw.get()), sizeof(ShmCaster::CastQueue));
		ASSERT_EQ(ifs.gcount(), static_cast<std::streamsize>(sizeof(ShmCaster::CastQueue)));
	}

	const ShmCaster::CastQueue* queue = static_cast<const ShmCaster::CastQueue*>(raw.get());
	EXPECT_EQ(queue->_capacity, 8192u);
	EXPECT_EQ(queue->_writable, 4u);
	EXPECT_EQ(queue->_readable, 3u);
	EXPECT_EQ(queue->_pid, static_cast<uint32_t>(::getpid()));

	EXPECT_EQ(queue->_items[0]._type, 0u);
	EXPECT_DOUBLE_EQ(queue->_items[0]._tick.price, 7250.5);
	EXPECT_STREQ(queue->_items[0]._tick.code, "ag2512");

	EXPECT_EQ(queue->_items[1]._type, 1u);
	EXPECT_STREQ(queue->_items[1]._queue.code, "ag2512");

	EXPECT_EQ(queue->_items[2]._type, 2u);
	EXPECT_STREQ(queue->_items[2]._order.code, "ag2512");

	EXPECT_EQ(queue->_items[3]._type, 3u);
	EXPECT_STREQ(queue->_items[3]._trans.code, "ag2512");
}

/*
 * 触发条件（禁用原因）：ShmCaster::init 在 active=true 时先
 * `_mapfile.reset(new BoostMappingFile)`（ShmCaster.cpp:46）销毁旧映射，再 map 新路径。
 * 若第一次 init 成功、第二次 path 指向目录，L47 抛异常，但 `_queue` 仍指向已释放的旧
 * 映射、`_inited` 仍为 true；随后 broadcast 的 `_queue==NULL || !_inited` 守卫全部放行，
 * L83/L110/L137/L164 写入已 unmap 的地址。缺陷登记：bugs/R154.md BUG-R154-002。
 */
TEST_F(R154ShmCasterActiveTest, DISABLED_ReinitWithDirectoryPathLeavesDanglingQueue)
{
	const fs::path queuePath = _dir / "reinit_queue.bin";
	r154::write_file(_dir / "reinit_good.json",
		std::string(R"JSON({"active": true, "path": ")JSON")
		+ queuePath.string() + R"JSON("})JSON");
	WTSVariant* good = WTSCfgLoader::load_from_file((_dir / "reinit_good.json").string().c_str());
	ASSERT_NE(good, nullptr);

	ShmCaster caster;
	ASSERT_TRUE(caster.init(good));
	good->release();

	WTSTickData* tick = WTSTickData::create("ag2512");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);

	const fs::path dirPath = _dir / "as_dir";
	fs::create_directories(dirPath);
	r154::write_file(_dir / "reinit_bad.json",
		std::string(R"JSON({"active": true, "path": ")JSON")
		+ dirPath.string() + R"JSON("})JSON");
	WTSVariant* bad = WTSCfgLoader::load_from_file((_dir / "reinit_bad.json").string().c_str());
	ASSERT_NE(bad, nullptr);
	EXPECT_THROW(caster.init(bad), std::exception);
	bad->release();

	caster.broadcast(tick);	// 悬垂 _queue：写入已 unmap 的内存
	tick->release();
}

}	// namespace
