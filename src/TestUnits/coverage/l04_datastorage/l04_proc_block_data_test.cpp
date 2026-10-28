// L04 / WtDataStorage：proc_block_data 与 WtBtDtReader 落盘读取链路测试
// 直接驱动 libWtDataStorage 中的 proc_block_data（WtDataReader.cpp:50），
// 并用临时目录里的 .dsb 文件驱动 WtBtDtReader 的各 read_raw_* 分支。
// 全部落盘到 std::filesystem::temp_directory_path()/l04_... 下，TearDown 清理。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSTypes.h"
#include "Share/StrUtil.hpp"
#include "WtDataStorage/DataDefine.h"
#include "WtDataStorage/WtBtDtReader.h"
#include "WTSUtils/WTSCmpHelper.hpp"

// WtDataReader.cpp 中的自由函数（外部链接，由 libWtDataStorage 导出）
extern bool proc_block_data(std::string& content, bool isBar, bool bKeepHead);

using namespace wtp;

namespace {

std::string fill_flag(BlockHeader* header, uint16_t type, uint16_t version)
{
	memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
	header->_type = type;
	header->_version = version;
	return std::string();
}

WTSBarStructOld make_old_bar(uint32_t date, uint32_t time, double base)
{
	WTSBarStructOld bar;
	bar.date = date;
	bar.time = time;
	bar.open = base;
	bar.high = base + 1.0;
	bar.low = base - 1.0;
	bar.close = base + 0.5;
	bar.settle = base + 0.25;
	bar.money = base * 100.0;
	bar.vol = (uint32_t)(base * 3.0);
	bar.hold = (uint32_t)(base * 4.0);
	bar.add = (int32_t)(base - 100.0);
	return bar;
}

class L04BtReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		std::random_device rd;
		_root = std::filesystem::temp_directory_path()
			/ ("l04_btdt_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	void write_file(const std::filesystem::path& path, const std::string& content)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(ofs.is_open());
		ofs.write(content.data(), (std::streamsize)content.size());
		ofs.flush();
	}

	std::filesystem::path _root;
};

}

//////////////////////////////////////////////////////////////////////////
// proc_block_data：未压缩 / 新版本
//////////////////////////////////////////////////////////////////////////

TEST(L04ProcBlockData, RawV2DropHead)
{
	std::vector<WTSBarStruct> bars(4);
	for (std::size_t i = 0; i < bars.size(); i++)
		bars[i] = WTSBarStruct(), bars[i].date = 20240102, bars[i].time = 931 + i;

	std::string content(BLOCK_HEADER_SIZE + bars.size() * sizeof(WTSBarStruct), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Minute1, BLOCK_VERSION_RAW_V2);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, bars.data(), bars.size() * sizeof(WTSBarStruct));

	std::string src = content;
	ASSERT_TRUE(proc_block_data(content, true, false));
	EXPECT_EQ(content.size(), src.size() - BLOCK_HEADER_SIZE);
	const WTSBarStruct* out = (const WTSBarStruct*)content.data();
	for (std::size_t i = 0; i < bars.size(); i++)
	{
		EXPECT_EQ(out[i].date, 20240102u);
		EXPECT_EQ(out[i].time, 931ull + i);
	}
}

TEST(L04ProcBlockData, RawV2KeepHead)
{
	std::string content(BLOCK_HEADER_SIZE + sizeof(WTSBarStruct), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Day, BLOCK_VERSION_RAW_V2);

	std::string src = content;
	ASSERT_TRUE(proc_block_data(content, true, true));
	EXPECT_EQ(content, src);	// 未压缩且非老版本，保留头部时原样返回
}

//////////////////////////////////////////////////////////////////////////
// proc_block_data：老版本（未压缩）结构体转换
//////////////////////////////////////////////////////////////////////////

TEST(L04ProcBlockData, OldBarsUpgraded)
{
	const uint32_t cnt = 6;
	std::vector<WTSBarStructOld> oldBars(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		oldBars[i] = make_old_bar(20240102, 931 + i, 100.0 + i);

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSBarStructOld), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Minute1, BLOCK_VERSION_RAW);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, oldBars.data(), cnt * sizeof(WTSBarStructOld));

	ASSERT_TRUE(proc_block_data(content, true, false));
	EXPECT_EQ(content.size(), cnt * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)content.data();
	for (uint32_t i = 0; i < cnt; i++)
	{
		EXPECT_EQ(out[i].date, 20240102u);
		EXPECT_EQ(out[i].time, (uint64_t)(931u + i));
		EXPECT_DOUBLE_EQ(out[i].open, 100.0 + i);
		EXPECT_DOUBLE_EQ(out[i].high, 101.0 + i);
		EXPECT_DOUBLE_EQ(out[i].vol, (100.0 + i) * 3.0);
	}
}

TEST(L04ProcBlockData, OldBarsUpgradedKeepHead)
{
	const uint32_t cnt = 3;
	std::vector<WTSBarStructOld> oldBars(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		oldBars[i] = make_old_bar(20240103, 932, 50.0 + i);

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSBarStructOld), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Day, BLOCK_VERSION_RAW);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, oldBars.data(), cnt * sizeof(WTSBarStructOld));

	ASSERT_TRUE(proc_block_data(content, true, true));
	// 头部保留但版本被改写为 RAW_V2，负载升级为新结构体
	EXPECT_EQ(content.size(), BLOCK_HEADER_SIZE + cnt * sizeof(WTSBarStruct));
	const BlockHeader* nh = (const BlockHeader*)content.data();
	EXPECT_EQ(memcmp(nh->_blk_flag, BLK_FLAG, FLAG_SIZE), 0);
	EXPECT_EQ(nh->_type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(nh->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_FALSE(nh->is_old_version());
	const WTSBarStruct* out = (const WTSBarStruct*)((const char*)content.data() + BLOCK_HEADER_SIZE);
	EXPECT_DOUBLE_EQ(out[2].close, 52.5);
}

TEST(L04ProcBlockData, OldTicksUpgraded)
{
	const uint32_t cnt = 5;
	std::vector<WTSTickStructOld> oldTicks(cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		oldTicks[i] = WTSTickStructOld();
		wt_strcpy(oldTicks[i].exchg, "SHFE");
		wt_strcpy(oldTicks[i].code, "rb2401");
		oldTicks[i].price = 3000.0 + i;
		oldTicks[i].volume = 5.0 + i;
	}

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSTickStructOld), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Ticks, BLOCK_VERSION_RAW);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, oldTicks.data(), cnt * sizeof(WTSTickStructOld));

	ASSERT_TRUE(proc_block_data(content, false, false));
	EXPECT_EQ(content.size(), cnt * sizeof(WTSTickStruct));
	const WTSTickStruct* out = (const WTSTickStruct*)content.data();
	for (uint32_t i = 0; i < cnt; i++)
	{
		EXPECT_STREQ(out[i].exchg, "SHFE");
		EXPECT_STREQ(out[i].code, "rb2401");
		EXPECT_DOUBLE_EQ(out[i].price, 3000.0 + i);
	}
}

//////////////////////////////////////////////////////////////////////////
// proc_block_data：压缩块
//////////////////////////////////////////////////////////////////////////

TEST(L04ProcBlockData, CompressedNewBarsUncompressed)
{
	std::vector<WTSBarStruct> bars(20);
	for (std::size_t i = 0; i < bars.size(); i++)
		bars[i] = WTSBarStruct(), bars[i].date = 20240105, bars[i].close = 10.0 + i;

	const std::string raw((const char*)bars.data(), bars.size() * sizeof(WTSBarStruct));
	const std::string cmped = WTSCmpHelper::compress_data(raw.data(), raw.size());

	std::string content(BLOCK_HEADERV2_SIZE + cmped.size(), 0);
	BlockHeaderV2* header = (BlockHeaderV2*)content.data();
	memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
	header->_type = BT_HIS_Minute5;
	header->_version = BLOCK_VERSION_CMP_V2;
	header->_size = cmped.size();
	memcpy((char*)content.data() + BLOCK_HEADERV2_SIZE, cmped.data(), cmped.size());

	ASSERT_TRUE(proc_block_data(content, true, false));
	ASSERT_EQ(content.size(), raw.size());
	const WTSBarStruct* out = (const WTSBarStruct*)content.data();
	for (std::size_t i = 0; i < bars.size(); i++)
	{
		EXPECT_EQ(out[i].date, 20240105u);
		EXPECT_DOUBLE_EQ(out[i].close, 10.0 + (double)i);
	}
}

TEST(L04ProcBlockData, CompressedOldBarsUncompressedAndUpgraded)
{
	const uint32_t cnt = 8;
	std::vector<WTSBarStructOld> oldBars(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		oldBars[i] = make_old_bar(20240108, 940 + i, 200.0 + i);

	const std::string raw((const char*)oldBars.data(), cnt * sizeof(WTSBarStructOld));
	const std::string cmped = WTSCmpHelper::compress_data(raw.data(), raw.size());

	std::string content(BLOCK_HEADERV2_SIZE + cmped.size(), 0);
	BlockHeaderV2* header = (BlockHeaderV2*)content.data();
	memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
	header->_type = BT_HIS_Minute1;
	header->_version = BLOCK_VERSION_CMP;	// 老结构体 + 压缩
	header->_size = cmped.size();
	memcpy((char*)content.data() + BLOCK_HEADERV2_SIZE, cmped.data(), cmped.size());

	ASSERT_TRUE(proc_block_data(content, true, false));
	ASSERT_EQ(content.size(), cnt * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)content.data();
	EXPECT_EQ(out[7].date, 20240108u);
	EXPECT_DOUBLE_EQ(out[7].open, 207.0);
	EXPECT_DOUBLE_EQ(out[0].vol, 600.0);
}

TEST(L04ProcBlockData, CompressedSizeMismatchRejected)
{
	const std::string raw(32, 'y');
	const std::string cmped = WTSCmpHelper::compress_data(raw.data(), raw.size());

	std::string content(BLOCK_HEADERV2_SIZE + cmped.size() + 7, 0);
	BlockHeaderV2* header = (BlockHeaderV2*)content.data();
	memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
	header->_type = BT_HIS_Ticks;
	header->_version = BLOCK_VERSION_CMP_V2;
	header->_size = cmped.size();	// 实际长度故意多 7 字节
	memcpy((char*)content.data() + BLOCK_HEADERV2_SIZE, cmped.data(), cmped.size());

	const std::string src = content;
	EXPECT_FALSE(proc_block_data(content, false, false));
	EXPECT_EQ(content, src);	// 失败路径不得改写内容
}

//////////////////////////////////////////////////////////////////////////
// WtBtDtReader：文件驱动
//////////////////////////////////////////////////////////////////////////

TEST_F(L04BtReaderTest, InitWithNullCfgKeepsEmptyBaseDir)
{
	WtBtDtReader reader;
	reader.init(NULL, NULL);
	std::string buffer;
	// 空 base_dir 下路径不存在，read_raw_* 直接返回 false
	EXPECT_FALSE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer));
	EXPECT_FALSE(reader.read_raw_ticks("SHFE", "rb2401", 20240102, buffer));
	EXPECT_FALSE(reader.read_raw_order_details("SZSE", "000001", 20240102, buffer));
	EXPECT_FALSE(reader.read_raw_order_queues("SZSE", "000001", 20240102, buffer));
	EXPECT_FALSE(reader.read_raw_transactions("SZSE", "000001", 20240102, buffer));
}

TEST_F(L04BtReaderTest, ReadsBarsFromTempStore)
{
	const uint32_t cnt = 7;
	std::vector<WTSBarStructOld> oldBars(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		oldBars[i] = make_old_bar(20240102, 931 + i, 300.0 + i);

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSBarStructOld), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Minute1, BLOCK_VERSION_RAW);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, oldBars.data(), cnt * sizeof(WTSBarStructOld));

	const std::filesystem::path file
		= _root / "his" / PERIOD_NAME[KP_Minute1] / "SHFE" / "rb2401.dsb";
	write_file(file, content);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	WtBtDtReader reader;
	reader.init(cfg, NULL);

	std::string buffer;
	ASSERT_TRUE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer));
	ASSERT_EQ(buffer.size(), cnt * sizeof(WTSBarStruct));
	const WTSBarStruct* out = (const WTSBarStruct*)buffer.data();
	EXPECT_EQ(out[0].date, 20240102u);
	EXPECT_EQ(out[0].time, 931ull);
	EXPECT_DOUBLE_EQ(out[6].close, 306.5);
	EXPECT_DOUBLE_EQ(out[6].money, 306.0 * 100.0);

	buffer.clear();
	EXPECT_FALSE(reader.read_raw_bars("SHFE", "nonexists", KP_Minute1, buffer));
	EXPECT_TRUE(buffer.empty());
}

TEST_F(L04BtReaderTest, ReadsTicksFromTempStore)
{
	const uint32_t cnt = 4;
	std::vector<WTSTickStructOld> oldTicks(cnt);
	for (uint32_t i = 0; i < cnt; i++)
	{
		oldTicks[i] = WTSTickStructOld();
		wt_strcpy(oldTicks[i].exchg, "DCE");
		wt_strcpy(oldTicks[i].code, "i2401");
		oldTicks[i].price = 1000.0 + i;
	}

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSTickStructOld), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Ticks, BLOCK_VERSION_RAW);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, oldTicks.data(), cnt * sizeof(WTSTickStructOld));

	const std::filesystem::path file
		= _root / "his" / "ticks" / "DCE" / "20240102" / "i2401.dsb";
	write_file(file, content);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	WtBtDtReader reader;
	reader.init(cfg, NULL);

	std::string buffer;
	ASSERT_TRUE(reader.read_raw_ticks("DCE", "i2401", 20240102, buffer));
	ASSERT_EQ(buffer.size(), cnt * sizeof(WTSTickStruct));
	const WTSTickStruct* out = (const WTSTickStruct*)buffer.data();
	EXPECT_STREQ(out[3].exchg, "DCE");
	EXPECT_DOUBLE_EQ(out[3].price, 1003.0);
}

TEST_F(L04BtReaderTest, ReadsHftSlicesFromTempStore)
{
	// 逐笔委托/委托队列/逐笔成交按原始 V2 负载写入，读取侧仅剥头
	const uint32_t cnt = 3;
	std::vector<WTSTransStruct> items(cnt);
	for (uint32_t i = 0; i < cnt; i++)
		items[i] = WTSTransStruct(), items[i].price = 12.5 + i;

	std::string content(BLOCK_HEADER_SIZE + cnt * sizeof(WTSTransStruct), 0);
	BlockHeader* header = (BlockHeader*)content.data();
	fill_flag(header, BT_HIS_Trnsctn, BLOCK_VERSION_RAW_V2);
	memcpy((char*)content.data() + BLOCK_HEADER_SIZE, items.data(), cnt * sizeof(WTSTransStruct));

	const std::filesystem::path orders = _root / "his" / "orders" / "SZSE" / "20240102" / "000001.dsb";
	const std::filesystem::path queues = _root / "his" / "queue" / "SZSE" / "20240102" / "000001.dsb";
	const std::filesystem::path trans = _root / "his" / "trans" / "SZSE" / "20240102" / "000001.dsb";
	write_file(orders, content);
	write_file(queues, content);
	write_file(trans, content);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	WtBtDtReader reader;
	reader.init(cfg, NULL);

	std::string buffer;
	ASSERT_TRUE(reader.read_raw_order_details("SZSE", "000001", 20240102, buffer));
	EXPECT_EQ(buffer.size(), cnt * sizeof(WTSTransStruct));
	buffer.clear();
	ASSERT_TRUE(reader.read_raw_order_queues("SZSE", "000001", 20240102, buffer));
	EXPECT_EQ(buffer.size(), cnt * sizeof(WTSTransStruct));
	buffer.clear();
	ASSERT_TRUE(reader.read_raw_transactions("SZSE", "000001", 20240102, buffer));
	const WTSTransStruct* out = (const WTSTransStruct*)buffer.data();
	EXPECT_DOUBLE_EQ(out[2].price, 14.5);
}

TEST_F(L04BtReaderTest, CorruptedBlockReturnsFalse)
{
	const std::string raw(48, 'z');
	const std::string cmped = WTSCmpHelper::compress_data(raw.data(), raw.size());

	std::string content(BLOCK_HEADERV2_SIZE + cmped.size(), 0);
	BlockHeaderV2* header = (BlockHeaderV2*)content.data();
	memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
	header->_type = BT_HIS_Minute1;
	header->_version = BLOCK_VERSION_CMP_V2;
	header->_size = cmped.size() + 128;	// 故意与文件长度不一致
	memcpy((char*)content.data() + BLOCK_HEADERV2_SIZE, cmped.data(), cmped.size());

	const std::filesystem::path file
		= _root / "his" / PERIOD_NAME[KP_DAY] / "CFFEX" / "IF2401.dsb";
	write_file(file, content);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	WtBtDtReader reader;
	reader.init(cfg, NULL);

	std::string buffer;
	EXPECT_FALSE(reader.read_raw_bars("CFFEX", "IF2401", KP_DAY, buffer));
}
