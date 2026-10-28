// p10 / WtDataStorage：WtDataWriter 的实时数据文件块（.dmb / 分钟 .dmb）装配与写入
//
// 覆盖对象（全部是普通文件 + 文件映射，路径都在 temp 目录内；不触碰命名共享内存、不启线程）：
//  - getTickBlock：新建块头、容量/日期/标志、日期切换清零、文件大小与 capacity 不符的自愈分支
//  - pipeToTicks：写入 RT tick 文件块（写回磁盘后逐字段读回比对）
//  - getKlineBlock：min1/min5 两个分支、按交易分钟数预分配容量、非法周期与 NULL 合约拒绝
//  - getTransBlock / getOrdDtlBlock / getOrdQueBlock：三个逐笔文件块的新建与拒绝分支
//  - releaseBlock / release：块指针回收
//
// 落盘：std::filesystem::temp_directory_path()/p10_rt_<pid>_<seq>，TearDown 清理；不碰 dist/
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#include "p10_writer_prefix.h"   // 第三方头需先于访问宏 include，见该文件注释

#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"

using namespace wtp;

namespace {

static const uint32_t kHftStep = 2500;      // WtDataWriter.cpp: HFT_SIZE_STEP
static const uint32_t kTickDate = 20240108;
static const uint32_t kNextDate = 20240219;

static std::string read_file_bytes(const std::string& path)
{
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return std::string();
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

static WTSTickStruct make_tick(uint32_t seq)
{
	WTSTickStruct ts;
	std::memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "%s", "SHFE");
	snprintf(ts.code, sizeof(ts.code), "%s", "rb2210");
	ts.price = 3600.0 + seq;
	ts.open = 3560.0 + seq;
	ts.high = 3620.0 + seq;
	ts.low = 3550.0 + seq;
	ts.settle_price = 3590.0 + seq;
	ts.total_volume = 10000 + seq;
	ts.volume = 20 + seq;
	ts.open_interest = 1234567 + seq;
	ts.pre_interest = 1230000 + seq;
	ts.trading_date = kTickDate;
	ts.action_date = kTickDate;
	ts.action_time = 93000000 + seq * 500;
	return ts;
}

class P10RtBlockTest : public ::testing::Test
{
protected:
	std::filesystem::path _dir;
	std::string _base;
	WTSSessionInfo* _session;
	WTSCommodityInfo* _comm;
	WTSContractInfo* _ct;
	std::vector<WtDataWriter*> _writers;

	void SetUp() override
	{
		static std::atomic<uint32_t> seq(0);
		_dir = std::filesystem::temp_directory_path()
			/ ("p10_rt_" + std::to_string(getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();
		_base = _dir.string() + "/";

		_session = WTSSessionInfo::create("P10_RT_SESS", "p10 实时", 0);
		_session->addTradingSection(900, 1130);
		_session->addTradingSection(1330, 1500);
		_comm = WTSCommodityInfo::create("p10rb", "p10 螺纹钢", "SHFE", "P10_RT_SESS", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
		_ct = WTSContractInfo::create("rb2210", "p10 rb2210", "SHFE", "p10rb");
		_ct->setCommInfo(_comm);
	}

	WtDataWriter* new_writer()
	{
		WtDataWriter* w = new WtDataWriter();
		w->_base_dir = _base;
		w->_save_tick_log = false;
		_writers.push_back(w);
		return w;
	}

	void TearDown() override
	{
		for (size_t i = 0; i < _writers.size(); i++)
		{
			_writers[i]->release();
			delete _writers[i];
		}
		_writers.clear();

		if (_ct != NULL) { _ct->release(); _ct = NULL; }
		if (_comm != NULL) { _comm->release(); _comm = NULL; }
		if (_session != NULL) { _session->release(); _session = NULL; }

		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}
};

TEST_F(P10RtBlockTest, TickBlockCreatesDmbWithV2Header)
{
	WtDataWriter* w = new_writer();
	WtDataWriter::TickBlockPair* pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair != NULL);
	ASSERT_TRUE(pair->_block != NULL);

	const std::string file = _base + "rt/ticks/SHFE/rb2210.dmb";
	ASSERT_TRUE(std::filesystem::exists(file));

	RTTickBlock* blk = pair->_block;
	EXPECT_EQ(std::memcmp(blk->_blk_flag, BLK_FLAG, sizeof(blk->_blk_flag)), 0);
	EXPECT_EQ(blk->_type, (uint16_t)BT_RT_Ticks);
	EXPECT_EQ(blk->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(blk->_capacity, kHftStep);
	EXPECT_EQ(blk->_size, 0u);
	EXPECT_EQ(blk->_date, kTickDate);

	// 第二次调用必须命中缓存表，返回同一个块指针（不重复建文件）
	WtDataWriter::TickBlockPair* again = w->getTickBlock(_ct, kTickDate, true);
	EXPECT_EQ(again, pair);

	const std::string content = read_file_bytes(file);
	EXPECT_EQ(content.size(), sizeof(RTTickBlock) + (size_t)kHftStep * sizeof(WTSTickStruct));
	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._type, (uint16_t)BT_RT_Ticks);
	EXPECT_FALSE(hdr.is_compressed());
	EXPECT_FALSE(hdr.is_old_version());
}

TEST_F(P10RtBlockTest, PipeToTicksWritesBlockAndDiskRoundTrip)
{
	WtDataWriter* w = new_writer();
	WtDataWriter::TickBlockPair* pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair != NULL);

	const uint32_t nt = 5;
	WTSTickStruct sent[nt];
	for (uint32_t i = 0; i < nt; i++)
	{
		sent[i] = make_tick(i);
		WTSTickData* tick = WTSTickData::create(sent[i]);
		ASSERT_TRUE(tick != NULL);
		w->pipeToTicks(_ct, tick);
	}

	RTTickBlock* blk = pair->_block;
	ASSERT_TRUE(blk != NULL);
	EXPECT_EQ(blk->_size, nt);
	for (uint32_t i = 0; i < nt; i++)
	{
		EXPECT_DOUBLE_EQ(blk->_ticks[i].price, 3600.0 + i);
		EXPECT_EQ(blk->_ticks[i].trading_date, kTickDate);
	}

	// 落盘后按文件字节读回，逐字段比对
	pair->_file->sync();
	const std::string content = read_file_bytes(_base + "rt/ticks/SHFE/rb2210.dmb");
	ASSERT_GT(content.size(), sizeof(RTTickBlock));
	RTTickBlock disk;
	std::memcpy(&disk, content.data(), sizeof(disk));
	EXPECT_EQ(disk._size, nt);
	EXPECT_EQ(disk._capacity, kHftStep);
	EXPECT_EQ(disk._date, kTickDate);

	const WTSTickStruct* onDisk = (const WTSTickStruct*)(content.data() + sizeof(RTTickBlock));
	for (uint32_t i = 0; i < nt; i++)
	{
		EXPECT_EQ(std::memcmp(&onDisk[i], &sent[i], sizeof(WTSTickStruct)), 0);
		EXPECT_STREQ(onDisk[i].code, "rb2210");
		EXPECT_DOUBLE_EQ(onDisk[i].settle_price, 3590.0 + i);
		EXPECT_EQ(onDisk[i].total_volume, 10000u + i);
		EXPECT_EQ(onDisk[i].action_time, 93000000u + i * 500);
	}
}

TEST_F(P10RtBlockTest, TickBlockDateSwitchClearsBlock)
{
	{
		WtDataWriter* w1 = new_writer();
		WtDataWriter::TickBlockPair* p1 = w1->getTickBlock(_ct, kTickDate, true);
		ASSERT_TRUE(p1 != NULL);
		WTSTickStruct ts = make_tick(0);
		WTSTickData* tick = WTSTickData::create(ts);
		w1->pipeToTicks(_ct, tick);
		ASSERT_EQ(p1->_block->_size, 1u);
		p1->_file->sync();
		w1->release();
		delete w1;
		for (size_t i = 0; i < _writers.size(); i++)
		{
			if (_writers[i] == w1)
			{
				_writers.erase(_writers.begin() + i);
				break;
			}
		}
	}

	// 新 writer（块表为空）重新映射同一个 .dmb：日期不同 -> 归零并改写日期
	WtDataWriter* w2 = new_writer();
	WtDataWriter::TickBlockPair* p2 = w2->getTickBlock(_ct, kNextDate, true);
	ASSERT_TRUE(p2 != NULL);
	ASSERT_TRUE(p2->_block != NULL);
	EXPECT_EQ(p2->_block->_date, kNextDate);
	EXPECT_EQ(p2->_block->_size, 0u);
	EXPECT_EQ(p2->_block->_capacity, kHftStep);
	EXPECT_EQ(p2->_block->_type, (uint16_t)BT_RT_Ticks);

	p2->_file->sync();
	const std::string content = read_file_bytes(_base + "rt/ticks/SHFE/rb2210.dmb");
	WTSTickStruct first;
	std::memcpy(&first, content.data() + sizeof(RTTickBlock), sizeof(first));
	EXPECT_DOUBLE_EQ(first.price, 0.0);
	EXPECT_EQ(first.trading_date, 0u);
}

TEST_F(P10RtBlockTest, TickBlockCapacityRepairedFromRealFileSize)
{
	std::string file;
	{
		WtDataWriter* w1 = new_writer();
		WtDataWriter::TickBlockPair* p1 = w1->getTickBlock(_ct, kTickDate, true);
		ASSERT_TRUE(p1 != NULL);
		p1->_file->sync();
		file = _base + "rt/ticks/SHFE/rb2210.dmb";
		w1->release();
		delete w1;
		_writers.clear();
	}

	// 人为把文件截成「只够 10 条 tick」，模拟 capacity 改小但未扩容的历史文件
	const uint64_t small = (uint64_t)sizeof(RTTickBlock) + 10 * sizeof(WTSTickStruct);
	std::error_code ec;
	std::filesystem::resize_file(file, small, ec);
	ASSERT_FALSE(ec) << ec.message();
	ASSERT_EQ((uint64_t)std::filesystem::file_size(file), small);

	WtDataWriter* w2 = new_writer();
	WtDataWriter::TickBlockPair* p2 = w2->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(p2 != NULL);
	ASSERT_TRUE(p2->_block != NULL);
	// 自愈分支：按真实文件大小重算 capacity，size 取 min(真实, 标记)
	EXPECT_EQ(p2->_block->_capacity, 10u);
	EXPECT_EQ(p2->_block->_size, 10u);
	EXPECT_EQ(p2->_block->_date, kTickDate);
}

TEST_F(P10RtBlockTest, KlineBlocksUseTradingMinutesAsCapacity)
{
	WtDataWriter* w = new_writer();
	const uint32_t totalMins = _ct->getCommInfo()->getSessionInfo()->getTradingMins();
	ASSERT_GT(totalMins, 0u);

	WtDataWriter::KBlockPair* m1 = w->getKlineBlock(_ct, KP_Minute1, true);
	ASSERT_TRUE(m1 != NULL);
	ASSERT_TRUE(m1->_block != NULL);
	RTKlineBlock* blk = m1->_block;
	EXPECT_EQ(std::memcmp(blk->_blk_flag, BLK_FLAG, sizeof(blk->_blk_flag)), 0);
	EXPECT_EQ(blk->_type, (uint16_t)BT_RT_Minute1);
	EXPECT_EQ(blk->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(blk->_capacity, totalMins);
	EXPECT_EQ(blk->_size, 0u);

	WtDataWriter::KBlockPair* m5 = w->getKlineBlock(_ct, KP_Minute5, true);
	ASSERT_TRUE(m5 != NULL);
	ASSERT_TRUE(m5->_block != NULL);
	EXPECT_EQ(m5->_block->_type, (uint16_t)BT_RT_Minute5);
	EXPECT_EQ(m5->_block->_capacity, totalMins / 5);

	// 同一合约同一周期命中缓存表
	WtDataWriter::KBlockPair* again = w->getKlineBlock(_ct, KP_Minute1, true);
	EXPECT_EQ(again, m1);

	// 文件形态与容量一致
	const std::string f1 = _base + "rt/min1/SHFE/rb2210.dmb";
	const std::string f5 = _base + "rt/min5/SHFE/rb2210.dmb";
	ASSERT_TRUE(std::filesystem::exists(f1));
	ASSERT_TRUE(std::filesystem::exists(f5));
	EXPECT_EQ(std::filesystem::file_size(f1), (uint64_t)(sizeof(RTKlineBlock) + totalMins * sizeof(WTSBarStruct)));
	EXPECT_EQ(std::filesystem::file_size(f5), (uint64_t)(sizeof(RTKlineBlock) + (totalMins / 5) * sizeof(WTSBarStruct)));

	const std::string content = read_file_bytes(f1);
	WTSBarStruct bar;
	std::memcpy(&bar, content.data() + sizeof(RTKlineBlock), sizeof(bar));
	EXPECT_EQ(bar.date, 0u);
	EXPECT_EQ(bar.time, 0u);
}

TEST_F(P10RtBlockTest, HftBlocksCreateAndReportTypes)
{
	WtDataWriter* w = new_writer();

	WtDataWriter::TransBlockPair* trans = w->getTransBlock(_ct, kTickDate, true);
	ASSERT_TRUE(trans != NULL);
	ASSERT_TRUE(trans->_block != NULL);
	EXPECT_EQ(std::memcmp(trans->_block->_blk_flag, BLK_FLAG, sizeof(trans->_block->_blk_flag)), 0);
	EXPECT_EQ(trans->_block->_type, (uint16_t)BT_RT_Trnsctn);
	EXPECT_EQ(trans->_block->_capacity, kHftStep);
	EXPECT_EQ(trans->_block->_size, 0u);
	EXPECT_EQ(trans->_block->_date, kTickDate);

	WtDataWriter::OrdQueBlockPair* que = w->getOrdQueBlock(_ct, kTickDate, true);
	ASSERT_TRUE(que != NULL);
	ASSERT_TRUE(que->_block != NULL);
	EXPECT_EQ(que->_block->_type, (uint16_t)BT_RT_OrdQueue);
	EXPECT_EQ(que->_block->_capacity, kHftStep);
	EXPECT_EQ(que->_block->_date, kTickDate);

	WtDataWriter::OrdDtlBlockPair* dtl = w->getOrdDtlBlock(_ct, kTickDate, true);
	ASSERT_TRUE(dtl != NULL);
	ASSERT_TRUE(dtl->_block != NULL);
	EXPECT_EQ(dtl->_block->_type, (uint16_t)BT_RT_OrdDetail);
	EXPECT_EQ(dtl->_block->_capacity, kHftStep);
	EXPECT_EQ(dtl->_block->_date, kTickDate);

	// 三个逐笔块都落在 rt/<subdir>/<exchg>/<code>.dmb
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/trans/SHFE/rb2210.dmb"));
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/queue/SHFE/rb2210.dmb"));
	EXPECT_TRUE(std::filesystem::exists(_base + "rt/orders/SHFE/rb2210.dmb"));
}

TEST_F(P10RtBlockTest, NullContractAndNoAutoCreateRejected)
{
	WtDataWriter* w = new_writer();

	// ct == NULL：五个入口都先返回 NULL
	EXPECT_TRUE(w->getTickBlock(NULL, kTickDate, true) == NULL);
	EXPECT_TRUE(w->getKlineBlock(NULL, KP_Minute1, true) == NULL);
	EXPECT_TRUE(w->getTransBlock(NULL, kTickDate, true) == NULL);
	EXPECT_TRUE(w->getOrdQueBlock(NULL, kTickDate, true) == NULL);
	EXPECT_TRUE(w->getOrdDtlBlock(NULL, kTickDate, true) == NULL);

	// 非法周期：switch 的 default 分支 -> cache_map 为空 -> NULL
	WtDataWriter::KBlockPair* bad = w->getKlineBlock(_ct, (WTSKlinePeriod)99, true);
	EXPECT_TRUE(bad == NULL);

	// 文件不存在且 bAutoCreate=false：返回 NULL，且不建目录、不落文件
	WtDataWriter* w2 = new WtDataWriter();
	w2->_base_dir = _dir.string() + "/no_auto/";
	EXPECT_TRUE(w2->getTickBlock(_ct, kTickDate, false) == NULL);
	EXPECT_TRUE(w2->getKlineBlock(_ct, KP_Minute1, false) == NULL);
	EXPECT_TRUE(w2->getTransBlock(_ct, kTickDate, false) == NULL);
	EXPECT_TRUE(w2->getOrdQueBlock(_ct, kTickDate, false) == NULL);
	EXPECT_TRUE(w2->getOrdDtlBlock(_ct, kTickDate, false) == NULL);
	EXPECT_FALSE(std::filesystem::exists(_dir.string() + "/no_auto/rt/ticks/SHFE/rb2210.dmb"));
	w2->release();
	delete w2;
}

TEST_F(P10RtBlockTest, ReleaseBlockCancelsCachedPair)
{
	WtDataWriter* w = new_writer();
	WtDataWriter::TickBlockPair* pair = w->getTickBlock(_ct, kTickDate, true);
	ASSERT_TRUE(pair != NULL);
	ASSERT_TRUE(pair->_block != NULL);

	// 释放单个块：映射置空、缓存表移除，下次取用会重建
	w->releaseBlock(pair);
	w->_rt_ticks_blocks.erase(std::string(_ct->getFullCode()));

	WtDataWriter::TickBlockPair* fresh = w->getTickBlock(_ct, kNextDate, true);
	ASSERT_TRUE(fresh != NULL);
	ASSERT_TRUE(fresh->_block != NULL);
	EXPECT_EQ(fresh->_block->_date, kNextDate);
	EXPECT_EQ(fresh->_block->_capacity, kHftStep);
	EXPECT_EQ(fresh->_block->_size, 0u);
}

}  // namespace
