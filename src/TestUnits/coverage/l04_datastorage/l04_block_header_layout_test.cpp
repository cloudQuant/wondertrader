// L04 / WtDataStorage：数据块结构（DataDefine.h）二进制布局与落盘 round-trip
// 覆盖 BlockHeader / BlockHeaderV2 版本标志位、各类 RT/HIS 数据块头部尺寸与
// 柔性数组偏移，并把数据块写入临时目录再读回校验（不依赖 dist/ 真实数据）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Includes/WTSStruct.h"
#include "WtDataStorage/DataDefine.h"

namespace {

class L04BlockFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = std::filesystem::temp_directory_path()
			/ ("l04_blk_" + std::to_string(::getpid()) + "_"
				+ std::to_string(std::hash<std::string>{}(std::string(__func__))));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::filesystem::path make_path(const char* name) const { return _root / name; }

	static void write_blob(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ASSERT_NO_THROW(ofs << content);
		ofs.flush();
	}

	static std::string read_blob(const std::filesystem::path& path, bool& ok)
	{
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs.is_open())
		{
			ok = false;
			return std::string();
		}
		std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		ok = true;
		return content;
	}

	WTSBarStruct make_bar(uint32_t date, uint64_t time, double base)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = time;
		bar.open = base;
		bar.high = base + 1.0;
		bar.low = base - 1.0;
		bar.close = base + 0.5;
		bar.settle = base + 0.25;
		bar.money = base * 1000.0;
		bar.vol = base * 10.0;
		bar.hold = base * 5.0;
		bar.add = base * 2.0;
		return bar;
	}

	std::filesystem::path _root;
};

}

//////////////////////////////////////////////////////////////////////////
// 头部尺寸与柔性数组偏移
//////////////////////////////////////////////////////////////////////////

TEST(L04BlockHeader, PackedSizes)
{
	EXPECT_EQ(sizeof(BlockHeader), (std::size_t)(FLAG_SIZE + 2 + 2));
	EXPECT_EQ(sizeof(BlockHeaderV2), (std::size_t)(FLAG_SIZE + 2 + 2 + 8));
	EXPECT_EQ(BLOCK_HEADER_SIZE, sizeof(BlockHeader));
	EXPECT_EQ(BLOCK_HEADERV2_SIZE, sizeof(BlockHeaderV2));

	EXPECT_EQ(sizeof(RTBlockHeader), sizeof(BlockHeader) + 2 * sizeof(uint32_t));
	EXPECT_EQ(sizeof(RTDayBlockHeader), sizeof(RTBlockHeader) + sizeof(uint32_t));
	EXPECT_EQ(sizeof(TickCacheItem), sizeof(uint32_t) + sizeof(WTSTickStruct));
}

TEST(L04BlockHeader, FlexibleArrayOffsets)
{
	std::vector<char> buf(sizeof(RTKlineBlock) + 4 * sizeof(WTSBarStruct), 0);
	RTKlineBlock* kblk = (RTKlineBlock*)buf.data();
	EXPECT_EQ((const char*)&kblk->_bars[0] - buf.data(), (std::ptrdiff_t)sizeof(RTDayBlockHeader));

	std::vector<char> tbuf(sizeof(RTTickBlock) + 3 * sizeof(WTSTickStruct), 0);
	RTTickBlock* tblk = (RTTickBlock*)tbuf.data();
	EXPECT_EQ((const char*)&tblk->_ticks[0] - tbuf.data(), (std::ptrdiff_t)sizeof(RTDayBlockHeader));

	std::vector<char> cbuf(sizeof(RTTickCache) + 2 * sizeof(TickCacheItem), 0);
	RTTickCache* cache = (RTTickCache*)cbuf.data();
	EXPECT_EQ((const char*)&cache->_ticks[0] - cbuf.data(), (std::ptrdiff_t)sizeof(RTBlockHeader));

	std::vector<char> hv2(sizeof(HisKlineBlockV2) + 2 * sizeof(WTSBarStruct), 0);
	HisKlineBlockV2* v2 = (HisKlineBlockV2*)hv2.data();
	EXPECT_EQ((const char*)&v2->_data[0] - hv2.data(), (std::ptrdiff_t)sizeof(BlockHeaderV2));

	std::vector<char> ov(sizeof(HisKlineBlockOld) + 2 * sizeof(WTSBarStructOld), 0);
	HisKlineBlockOld* old = (HisKlineBlockOld*)ov.data();
	EXPECT_EQ((const char*)&old->_bars[0] - ov.data(), (std::ptrdiff_t)sizeof(BlockHeader));
}

TEST(L04BlockHeader, BlkFlagContent)
{
	EXPECT_EQ(FLAG_SIZE, 8);
	EXPECT_EQ(strlen(BLK_FLAG), (std::size_t)(FLAG_SIZE - 1));
	EXPECT_STREQ(BLK_FLAG, "&^%$#@!");
	EXPECT_EQ(BLK_FLAG[7], '\0');
}

//////////////////////////////////////////////////////////////////////////
// 版本标志位
//////////////////////////////////////////////////////////////////////////

TEST(L04BlockHeader, VersionFlagsMatrix)
{
	const struct { uint16_t ver; bool old_; bool cmp; } cases[] = {
		{BLOCK_VERSION_RAW,		true,	false},
		{BLOCK_VERSION_CMP,		true,	true},
		{BLOCK_VERSION_RAW_V2,	false,	false},
		{BLOCK_VERSION_CMP_V2,	false,	true},
		{0x00,					false,	false},
		{0x05,					false,	false},
		{0xFFFF,				false,	false},
	};

	for (auto& c : cases)
	{
		BlockHeader h;
		memcpy(h._blk_flag, BLK_FLAG, FLAG_SIZE);
		h._type = BT_HIS_Day;
		h._version = c.ver;
		EXPECT_EQ(h.is_old_version(), c.old_) << "version " << c.ver;
		EXPECT_EQ(h.is_compressed(), c.cmp) << "version " << c.ver;

		BlockHeaderV2 h2;
		memcpy(h2._blk_flag, BLK_FLAG, FLAG_SIZE);
		h2._type = BT_HIS_Ticks;
		h2._version = c.ver;
		h2._size = 1024;
		EXPECT_EQ(h2.is_old_version(), c.old_) << "v2 version " << c.ver;
		EXPECT_EQ(h2.is_compressed(), c.cmp) << "v2 version " << c.ver;
	}
}

TEST(L04BlockHeader, BlockTypeEnumValues)
{
	EXPECT_EQ((int)BT_RT_Minute1, 1);
	EXPECT_EQ((int)BT_RT_Minute5, 2);
	EXPECT_EQ((int)BT_RT_Ticks, 3);
	EXPECT_EQ((int)BT_RT_Cache, 4);
	EXPECT_EQ((int)BT_RT_Trnsctn, 5);
	EXPECT_EQ((int)BT_RT_OrdDetail, 6);
	EXPECT_EQ((int)BT_RT_OrdQueue, 7);
	EXPECT_EQ((int)BT_HIS_Minute1, 21);
	EXPECT_EQ((int)BT_HIS_Minute5, 22);
	EXPECT_EQ((int)BT_HIS_Day, 23);
	EXPECT_EQ((int)BT_HIS_Ticks, 24);
	EXPECT_EQ((int)BT_HIS_Trnsctn, 25);
	EXPECT_EQ((int)BT_HIS_OrdDetail, 26);
	EXPECT_EQ((int)BT_HIS_OrdQueue, 27);
}

//////////////////////////////////////////////////////////////////////////
// 落盘 round-trip
//////////////////////////////////////////////////////////////////////////

TEST_F(L04BlockFixture, KlineBlockFileRoundTrip)
{
	const uint32_t bars_cnt = 5;
	std::string content(sizeof(RTDayBlockHeader) + bars_cnt * sizeof(WTSBarStruct), 0);
	RTKlineBlock* blk = (RTKlineBlock*)content.data();
	memcpy(blk->_blk_flag, BLK_FLAG, FLAG_SIZE);
	blk->_type = BT_HIS_Day;
	blk->_version = BLOCK_VERSION_RAW_V2;
	blk->_size = bars_cnt;
	blk->_capacity = 1000;
	blk->_date = 20240102;
	for (uint32_t i = 0; i < bars_cnt; i++)
		blk->_bars[i] = make_bar(20240102, 199001020931ull + i, 100.0 + i);

	auto path = make_path("kline.dsb");
	write_blob(path, content);

	bool ok = false;
	std::string back = read_blob(path, ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(back.size(), content.size());

	const RTKlineBlock* rb = (const RTKlineBlock*)back.data();
	EXPECT_EQ(memcmp(rb->_blk_flag, BLK_FLAG, FLAG_SIZE), 0);
	EXPECT_EQ(rb->_type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(rb->_version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(rb->_size, bars_cnt);
	EXPECT_EQ(rb->_capacity, 1000u);
	EXPECT_EQ(rb->_date, 20240102u);
	for (uint32_t i = 0; i < bars_cnt; i++)
	{
		EXPECT_EQ(rb->_bars[i].date, 20240102u);
		EXPECT_EQ(rb->_bars[i].time, 199001020931ull + i);
		EXPECT_DOUBLE_EQ(rb->_bars[i].open, 100.0 + i);
		EXPECT_DOUBLE_EQ(rb->_bars[i].close, 100.5 + i);
	}
}

TEST_F(L04BlockFixture, TickBlockFileRoundTrip)
{
	const uint32_t tick_cnt = 3;
	std::string content(sizeof(RTDayBlockHeader) + tick_cnt * sizeof(WTSTickStruct), 0);
	RTTickBlock* blk = (RTTickBlock*)content.data();
	memcpy(blk->_blk_flag, BLK_FLAG, FLAG_SIZE);
	blk->_type = BT_RT_Ticks;
	blk->_version = BLOCK_VERSION_RAW_V2;
	blk->_size = tick_cnt;
	blk->_capacity = 4096;
	blk->_date = 20240102;
	for (uint32_t i = 0; i < tick_cnt; i++)
	{
		WTSTickStruct& t = blk->_ticks[i];
		t = WTSTickStruct();
		wt_strcpy(t.exchg, "SHFE");
		wt_strcpy(t.code, "rb2401");
		t.price = 3500.0 + i;
		t.volume = 10.0 + i;
		t.trading_date = 20240102;
		t.action_date = 20240102;
		t.action_time = 93000000 + i;
	}

	auto path = make_path("ticks.dsb");
	write_blob(path, content);

	bool ok = false;
	std::string back = read_blob(path, ok);
	ASSERT_TRUE(ok);
	const RTTickBlock* rb = (const RTTickBlock*)back.data();
	EXPECT_EQ(rb->_size, tick_cnt);
	EXPECT_EQ(rb->_date, 20240102u);
	for (uint32_t i = 0; i < tick_cnt; i++)
	{
		EXPECT_STREQ(rb->_ticks[i].exchg, "SHFE");
		EXPECT_STREQ(rb->_ticks[i].code, "rb2401");
		EXPECT_DOUBLE_EQ(rb->_ticks[i].price, 3500.0 + i);
		EXPECT_EQ(rb->_ticks[i].action_time, 93000000u + i);
	}
}

TEST_F(L04BlockFixture, TickCacheBlockFileRoundTrip)
{
	const uint32_t cache_cnt = 4;
	std::string content(sizeof(RTBlockHeader) + cache_cnt * sizeof(TickCacheItem), 0);
	RTTickCache* cache = (RTTickCache*)content.data();
	memcpy(cache->_blk_flag, BLK_FLAG, FLAG_SIZE);
	cache->_type = BT_RT_Cache;
	cache->_version = BLOCK_VERSION_RAW_V2;
	cache->_size = cache_cnt;
	cache->_capacity = 50000;
	for (uint32_t i = 0; i < cache_cnt; i++)
	{
		cache->_ticks[i]._date = 20240102 + i;
		WTSTickStruct& t = cache->_ticks[i]._tick;
		t = WTSTickStruct();
		wt_strcpy(t.code, "000001");
		t.price = 10.0 + i;
		t.action_time = 143000000u;
	}

	auto path = make_path("tickcache.dsb");
	write_blob(path, content);

	bool ok = false;
	std::string back = read_blob(path, ok);
	ASSERT_TRUE(ok);
	const RTTickCache* rc = (const RTTickCache*)back.data();
	EXPECT_EQ(rc->_type, (uint16_t)BT_RT_Cache);
	EXPECT_EQ(rc->_capacity, 50000u);
	for (uint32_t i = 0; i < cache_cnt; i++)
	{
		EXPECT_EQ(rc->_ticks[i]._date, 20240102u + i);
		EXPECT_DOUBLE_EQ(rc->_ticks[i]._tick.price, 10.0 + i);
	}
}

TEST_F(L04BlockFixture, CompressedV2BlockHeaderSizeField)
{
	// V2 头用 _size 记录压缩后长度，读取侧据此校验整块完整性
	const std::string payload(64, 'x');
	std::string content(sizeof(BlockHeaderV2) + payload.size(), 0);
	BlockHeaderV2* h = (BlockHeaderV2*)content.data();
	memcpy(h->_blk_flag, BLK_FLAG, FLAG_SIZE);
	h->_type = BT_HIS_Minute1;
	h->_version = BLOCK_VERSION_CMP_V2;
	h->_size = payload.size();
	memcpy((char*)h + sizeof(BlockHeaderV2), payload.data(), payload.size());

	auto path = make_path("cmp.dsb");
	write_blob(path, content);

	bool ok = false;
	std::string back = read_blob(path, ok);
	ASSERT_TRUE(ok);
	const BlockHeaderV2* rh = (const BlockHeaderV2*)back.data();
	EXPECT_TRUE(rh->is_compressed());
	EXPECT_FALSE(rh->is_old_version());
	EXPECT_EQ(back.size(), sizeof(BlockHeaderV2) + (std::size_t)rh->_size);
}

TEST_F(L04BlockFixture, OldBarToNewBarFieldMapping)
{
	WTSBarStructOld oldBar;
	oldBar.date = 20240102;
	oldBar.time = 931;
	oldBar.open = 10;
	oldBar.high = 12;
	oldBar.low = 9;
	oldBar.close = 11;
	oldBar.settle = 11.5;
	oldBar.money = 2000;
	oldBar.vol = 200;
	oldBar.hold = 300;
	oldBar.add = -5;

	WTSBarStruct newBar;
	newBar = oldBar;
	EXPECT_EQ(newBar.date, oldBar.date);
	EXPECT_EQ(newBar.time, (uint64_t)oldBar.time);
	EXPECT_DOUBLE_EQ(newBar.open, oldBar.open);
	EXPECT_DOUBLE_EQ(newBar.high, oldBar.high);
	EXPECT_DOUBLE_EQ(newBar.low, oldBar.low);
	EXPECT_DOUBLE_EQ(newBar.close, oldBar.close);
	EXPECT_DOUBLE_EQ(newBar.settle, oldBar.settle);
	EXPECT_DOUBLE_EQ(newBar.money, oldBar.money);
	EXPECT_DOUBLE_EQ(newBar.vol, (double)oldBar.vol);
	EXPECT_DOUBLE_EQ(newBar.hold, (double)oldBar.hold);
	EXPECT_DOUBLE_EQ(newBar.add, (double)oldBar.add);
	// 新结构体多出的占位字段必须为 0，便于 memcmp 比较
	EXPECT_EQ(newBar.reserve_, 0u);
}
