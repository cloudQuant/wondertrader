// p10 / WtDataStorage：WtDataWriter::proc_block_data 的块解码装配（版本标志 / 压缩开关 / 老结构体升版）
//
// 覆盖对象（纯内存数据装配，无文件、无共享内存、无线程）：
//  - 新版未压缩（RAW_V2）：保留头部 / 去头部 两条分支
//  - 新版压缩（CMP_V2）：长度校验通过 -> 解压；长度不符 -> 返回 false 且内容不被改写
//  - 老版本未压缩 bar（BLOCK_VERSION_RAW）：WTSBarStructOld -> WTSBarStruct 升版
//  - 老版本压缩 bar（BlockHeaderV2 + BLOCK_VERSION_CMP）：解压后升级旧结构体
//  - 老版本未压缩 tick：WTSTickStructOld -> WTSTickStruct 升版
//  - 未知版本号：既非压缩也非老版本 -> 直接透传
//  - 块标志非法值候选输入（BUG-P10-003：输入契约待定，用例暂禁用）
//  - WtDataWriter::isSessionProceeded（纯查表分支，_proc_date 空/命中/未命中）
//
// 崩溃类输入（空 content / 头部不足一个 BlockHeader）整类收进一个 DISABLED_ 用例，启用用例零残留。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "p10_writer_prefix.h"   // 第三方头需先于访问宏 include，见该文件注释

#include "Includes/WTSStruct.h"
#include "WTSUtils/WTSCmpHelper.hpp"

using namespace wtp;

namespace {

static void set_flag(char* dst, size_t cap)
{
	std::memset(dst, 0, cap);
	std::memcpy(dst, BLK_FLAG, cap < sizeof(BLK_FLAG) - 1 ? cap : sizeof(BLK_FLAG) - 1);
}

// 组装一个「新版未压缩」条块：12 字节 BlockHeader + N 个 88 字节 WTSBarStruct
static std::string make_raw_bar_block(uint16_t version, uint32_t count)
{
	std::string content;
	content.resize(sizeof(BlockHeader) + count * sizeof(WTSBarStruct));
	BlockHeader* hdr = (BlockHeader*)&content[0];
	std::memset(hdr, 0, sizeof(*hdr));
	set_flag(hdr->_blk_flag, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Day;
	hdr->_version = version;

	WTSBarStruct* bars = (WTSBarStruct*)(content.data() + sizeof(BlockHeader));
	for (uint32_t i = 0; i < count; i++)
	{
		bars[i].date = 20240101 + i;
		bars[i].time = 93000 + i;
		bars[i].open = 100.0 + i;
		bars[i].high = 110.0 + i;
		bars[i].low = 90.0 + i;
		bars[i].close = 105.0 + i;
		bars[i].settle = 104.0 + i;
		bars[i].money = 1000.0 + i;
		bars[i].vol = 10 + i;
		bars[i].hold = 20 + i;
		bars[i].add = 30 + i;
	}
	return content;
}

static std::string make_cmp_bar_block(uint32_t count, uint64_t declaredSizeOffset)
{
	std::string payload;
	payload.resize(count * sizeof(WTSBarStruct));
	WTSBarStruct* bars = (WTSBarStruct*)&payload[0];
	for (uint32_t i = 0; i < count; i++)
	{
		bars[i].date = 20250101 + i;
		bars[i].time = 140000 + i;
		bars[i].open = 3.5 * i;
		bars[i].high = 4.5 * i;
		bars[i].low = 2.5 * i;
		bars[i].close = 3.75 * i;
		bars[i].settle = 3.7 * i;
		bars[i].money = 88.0 + i;
		bars[i].vol = 7 + i;
		bars[i].hold = 9 + i;
		bars[i].add = 11 + i;
	}

	std::string cmp = WTSCmpHelper::compress_data(payload.data(), payload.size());
	std::string content;
	content.resize(sizeof(BlockHeaderV2));
	BlockHeaderV2* hdr = (BlockHeaderV2*)&content[0];
	std::memset(hdr, 0, sizeof(*hdr));
	set_flag(hdr->_blk_flag, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Minute1;
	hdr->_version = BLOCK_VERSION_CMP_V2;
	hdr->_size = (uint64_t)cmp.size() + declaredSizeOffset;
	content.append(cmp);
	return content;
}

static std::string make_short_cmp_v2_header()
{
	// Only construct the 16 bytes that exist: BlockHeader plus four bytes of
	// the BlockHeaderV2 size field. Do not write a full BlockHeaderV2 here.
	std::string content(16, '\0');
	BlockHeader prefix;
	std::memset(&prefix, 0, sizeof(prefix));
	set_flag(prefix._blk_flag, sizeof(prefix._blk_flag));
	prefix._type = BT_HIS_Day;
	prefix._version = BLOCK_VERSION_CMP_V2;
	std::memcpy(content.data(), &prefix, sizeof(prefix));
	return content;
}

static void expect_proc_block_data_rejects_without_mutation(WtDataWriter& writer, std::string content)
{
#if defined(__unix__) || defined(__APPLE__)
	struct rlimit core_limit;
	core_limit.rlim_cur = 0;
	core_limit.rlim_max = 0;
	if (setrlimit(RLIMIT_CORE, &core_limit) != 0)
		std::_Exit(4);
#endif

	const std::string before = content;
	try
	{
		if (writer.proc_block_data("unit", content, true, false) || content != before)
			std::_Exit(2);
		std::_Exit(0);
	}
	catch (...)
	{
		std::_Exit(3);
	}
}

// 老结构体条块：BlockHeader(RAW) + N 个 WTSBarStructOld（44 字节）
static std::string make_old_bar_block(uint32_t count)
{
	std::string content;
	content.resize(sizeof(BlockHeader) + count * sizeof(WTSBarStructOld));
	BlockHeader* hdr = (BlockHeader*)&content[0];
	std::memset(hdr, 0, sizeof(*hdr));
	set_flag(hdr->_blk_flag, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Day;
	hdr->_version = BLOCK_VERSION_RAW;

	WTSBarStructOld* bars = (WTSBarStructOld*)(content.data() + sizeof(BlockHeader));
	for (uint32_t i = 0; i < count; i++)
	{
		bars[i].date = 20190101 + i;
		bars[i].time = 80000 + i;
		bars[i].open = 11.0 + i;
		bars[i].high = 12.0 + i;
		bars[i].low = 10.0 + i;
		bars[i].close = 11.5 + i;
		bars[i].settle = 11.25 + i;
		bars[i].money = 2000.0 + i;
		bars[i].vol = 100 + i;
		bars[i].hold = 200 + i;
		bars[i].add = -5 + (int32_t)i;
	}
	return content;
}

static std::string make_old_cmp_bar_block(uint32_t count)
{
	const std::string oldBlock = make_old_bar_block(count);
	const char* oldPayload = oldBlock.data() + sizeof(BlockHeader);
	const size_t oldPayloadSize = oldBlock.size() - sizeof(BlockHeader);
	const std::string cmp = WTSCmpHelper::compress_data(oldPayload, oldPayloadSize);

	BlockHeaderV2 hdr;
	std::memset(&hdr, 0, sizeof(hdr));
	set_flag(hdr._blk_flag, sizeof(hdr._blk_flag));
	hdr._type = BT_HIS_Day;
	hdr._version = BLOCK_VERSION_CMP;
	hdr._size = (uint64_t)cmp.size();

	std::string content(sizeof(hdr), '\0');
	std::memcpy(content.data(), &hdr, sizeof(hdr));
	content.append(cmp);
	return content;
}

static std::string make_old_tick_block(uint32_t count)
{
	std::string content;
	content.resize(sizeof(BlockHeader) + count * sizeof(WTSTickStructOld));
	BlockHeader* hdr = (BlockHeader*)&content[0];
	std::memset(hdr, 0, sizeof(*hdr));
	set_flag(hdr->_blk_flag, sizeof(hdr->_blk_flag));
	hdr->_type = BT_HIS_Ticks;
	hdr->_version = BLOCK_VERSION_RAW;

	WTSTickStructOld* ticks = (WTSTickStructOld*)(content.data() + sizeof(BlockHeader));
	for (uint32_t i = 0; i < count; i++)
	{
		snprintf(ticks[i].exchg, sizeof(ticks[i].exchg), "%s", "SHFE");
		snprintf(ticks[i].code, sizeof(ticks[i].code), "%s", "rb2210");
		ticks[i].price = 5000.0 + i;
		ticks[i].open = 4990.0 + i;
		ticks[i].high = 5010.0 + i;
		ticks[i].low = 4980.0 + i;
		ticks[i].settle_price = 5005.0 + i;
		ticks[i].total_volume = 1000 + i;
		ticks[i].volume = 10 + i;
		ticks[i].open_interest = 20000 + i;
		ticks[i].trading_date = 20190101 + i;
		ticks[i].action_date = 20190101 + i;
		ticks[i].action_time = 93000000 + i;
		ticks[i].pre_interest = 19000 + i;
	}
	return content;
}

class P10ProcBlockTest : public ::testing::Test
{
protected:
	WtDataWriter _writer;

	void SetUp() override {}
	void TearDown() override {}
};

TEST_F(P10ProcBlockTest, RawV2BarBlockKeepsHeadUntouched)
{
	std::string content = make_raw_bar_block(BLOCK_VERSION_RAW_V2, 4);
	const std::string before = content;
	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, true));
	EXPECT_EQ(content, before);
	EXPECT_EQ(content.size(), sizeof(BlockHeader) + 4 * sizeof(WTSBarStruct));
}

TEST_F(P10ProcBlockTest, RawV2BarBlockStripsHead)
{
	std::string content = make_raw_bar_block(BLOCK_VERSION_RAW_V2, 4);
	const std::string payload(content.data() + sizeof(BlockHeader), content.size() - sizeof(BlockHeader));
	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, false));
	ASSERT_EQ(content.size(), 4 * sizeof(WTSBarStruct));
	EXPECT_EQ(content, payload);

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	for (uint32_t i = 0; i < 4; i++)
	{
		EXPECT_EQ(bars[i].date, 20240101u + i);
		EXPECT_EQ(bars[i].time, (uint64_t)(93000 + i));
		EXPECT_DOUBLE_EQ(bars[i].open, 100.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].add, 30.0 + i);
	}
}

TEST_F(P10ProcBlockTest, CmpV2BarBlockDecompresses)
{
	const uint32_t count = 37;
	std::string content = make_cmp_bar_block(count, 0);
	BlockHeaderV2 src;
	std::memcpy(&src, content.data(), sizeof(src));
	ASSERT_EQ(src._version, (uint16_t)BLOCK_VERSION_CMP_V2);
	ASSERT_EQ(content.size(), sizeof(BlockHeaderV2) + (size_t)src._size);

	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, false));
	ASSERT_EQ(content.size(), count * sizeof(WTSBarStruct));

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	EXPECT_EQ(bars[0].date, 20250101u);
	EXPECT_EQ(bars[count - 1].date, 20250101u + count - 1);
	EXPECT_DOUBLE_EQ(bars[count - 1].close, 3.75 * (count - 1));
	EXPECT_DOUBLE_EQ(bars[5].high, 4.5 * 5);
	EXPECT_EQ(bars[10].vol, 17.0);
}

TEST_F(P10ProcBlockTest, CmpV2DecompressKeepHeadRewritesRawV2Header)
{
	const uint32_t count = 12;
	std::string content = make_cmp_bar_block(count, 0);
	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, true));
	// 保留头部时头部被降级成 12 字节的未压缩头 + 原始载荷
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + count * sizeof(WTSBarStruct));
	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_FALSE(hdr.is_compressed());
	EXPECT_FALSE(hdr.is_old_version());
	EXPECT_EQ(std::memcmp(hdr._blk_flag, BLK_FLAG, sizeof(hdr._blk_flag)), 0);

	const WTSBarStruct* bars = (const WTSBarStruct*)(content.data() + sizeof(BlockHeader));
	for (uint32_t i = 0; i < count; i++)
		EXPECT_EQ(bars[i].date, 20250101u + i);
}

TEST_F(P10ProcBlockTest, CmpV2LengthMismatchRejectedAndContentPreserved)
{
	// 头部声明的长度与文件实际长度不符（+8）：应直接返回 false，且 content 未被改写
	std::string content = make_cmp_bar_block(9, 8);
	const std::string before = content;
	EXPECT_FALSE(_writer.proc_block_data("unit", content, true, false));
	EXPECT_EQ(content, before);

	std::string content2 = make_cmp_bar_block(9, (uint64_t)0 - 8);
	const std::string before2 = content2;
	EXPECT_FALSE(_writer.proc_block_data("unit", content2, true, false));
	EXPECT_EQ(content2, before2);
}

TEST_F(P10ProcBlockTest, OldBarBlockTransferredToNewStruct)
{
	const uint32_t count = 6;
	std::string content = make_old_bar_block(count);
	const std::string before = content;
	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, false));
	// 去头部模式下 content 被换成升版后的新结构体数组
	ASSERT_EQ(content.size(), count * sizeof(WTSBarStruct));

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	for (uint32_t i = 0; i < count; i++)
	{
		EXPECT_EQ(bars[i].date, 20190101u + i);
		EXPECT_EQ(bars[i].time, (uint64_t)(80000 + i));
		EXPECT_DOUBLE_EQ(bars[i].open, 11.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].high, 12.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].low, 10.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].close, 11.5 + i);
		EXPECT_DOUBLE_EQ(bars[i].settle, 11.25 + i);
		EXPECT_DOUBLE_EQ(bars[i].money, 2000.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].vol, 100.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].hold, 200.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].add, -5.0 + i);
	}

	// 保留头部模式下头部被就地改成 RAW_V2，老 content 的原始字节只在前 12 字节仍可辨
	std::string keepHead = before;
	ASSERT_TRUE(_writer.proc_block_data("unit", keepHead, true, true));
	BlockHeader hdr;
	std::memcpy(&hdr, keepHead.data(), sizeof(hdr));
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_EQ(hdr._type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(keepHead.size(), sizeof(BlockHeader) + count * sizeof(WTSBarStruct));
}

TEST_F(P10ProcBlockTest, OldTickBlockTransferredToNewStruct)
{
	const uint32_t count = 5;
	std::string content = make_old_tick_block(count);
	ASSERT_TRUE(_writer.proc_block_data("unit", content, false, false));
	ASSERT_EQ(content.size(), count * sizeof(WTSTickStruct));

	const WTSTickStruct* ticks = (const WTSTickStruct*)content.data();
	for (uint32_t i = 0; i < count; i++)
	{
		EXPECT_STREQ(ticks[i].exchg, "SHFE");
		EXPECT_STREQ(ticks[i].code, "rb2210");
		EXPECT_DOUBLE_EQ(ticks[i].price, 5000.0 + i);
		EXPECT_DOUBLE_EQ(ticks[i].open, 4990.0 + i);
		EXPECT_DOUBLE_EQ(ticks[i].high, 5010.0 + i);
		EXPECT_DOUBLE_EQ(ticks[i].low, 4980.0 + i);
		EXPECT_DOUBLE_EQ(ticks[i].settle_price, 5005.0 + i);
		EXPECT_EQ(ticks[i].total_volume, 1000u + i);
		EXPECT_EQ(ticks[i].open_interest, 20000u + i);
		EXPECT_EQ(ticks[i].pre_interest, 19000 + (int)i);
		EXPECT_EQ(ticks[i].trading_date, 20190101u + i);
	}
}

TEST_F(P10ProcBlockTest, UnknownVersionPassesThrough)
{
	// 0x09 既不是压缩版也不是老版本 -> 走「直接返回」分支；去头部时只削掉 12 字节
	std::string content = make_raw_bar_block(0x09, 3);
	const std::string payload(content.data() + sizeof(BlockHeader), content.size() - sizeof(BlockHeader));
	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, false));
	EXPECT_EQ(content, payload);

	std::string keep = make_raw_bar_block(0x0A, 3);
	const std::string keepBefore = keep;
	ASSERT_TRUE(_writer.proc_block_data("unit", keep, true, true));
	EXPECT_EQ(keep, keepBefore);
}

TEST_F(P10ProcBlockTest, BlockFlagMismatchRejectsWithoutMutatingContent)
{
	std::string content = make_raw_bar_block(BLOCK_VERSION_RAW_V2, 2);
	BlockHeader* hdr = (BlockHeader*)&content[0];
	std::memcpy(hdr->_blk_flag, "WRONGFLG", 8);
	const std::string before = content;
	EXPECT_FALSE(_writer.proc_block_data("unit", content, true, false));
	EXPECT_EQ(before, content);
}

TEST_F(P10ProcBlockTest, OldCompressedBarBlockDecompressesAndUpgradesStruct)
{
	const uint32_t count = 3;
	std::string content = make_old_cmp_bar_block(count);
	BlockHeaderV2 hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	ASSERT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_CMP);
	ASSERT_EQ(content.size(), sizeof(BlockHeaderV2) + (size_t)hdr._size);

	ASSERT_TRUE(_writer.proc_block_data("unit", content, true, false));
	ASSERT_EQ(content.size(), count * sizeof(WTSBarStruct));

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	for (uint32_t i = 0; i < count; ++i)
	{
		EXPECT_EQ(bars[i].date, 20190101u + i);
		EXPECT_EQ(bars[i].time, (uint64_t)(80000 + i));
		EXPECT_DOUBLE_EQ(bars[i].open, 11.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].high, 12.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].low, 10.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].close, 11.5 + i);
		EXPECT_DOUBLE_EQ(bars[i].settle, 11.25 + i);
		EXPECT_DOUBLE_EQ(bars[i].money, 2000.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].vol, 100.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].hold, 200.0 + i);
		EXPECT_DOUBLE_EQ(bars[i].add, -5.0 + i);
	}
}

TEST_F(P10ProcBlockTest, IsSessionProceededOnEmptyTable)
{
	EXPECT_FALSE(_writer.isSessionProceeded("P10_SESS"));
	_writer._proc_date["P10_SESS"] = 20240101;
	EXPECT_FALSE(_writer.isSessionProceeded("P10_UNKNOWN"));
	// 已记录日期 >= 今天 才算处理过；用确定的过去/未来日期两侧断言
	_writer._proc_date["P10_OLD"] = 19700101;
	EXPECT_FALSE(_writer.isSessionProceeded("P10_OLD"));
	_writer._proc_date["P10_FUTURE"] = 29991231;
	EXPECT_TRUE(_writer.isSessionProceeded("P10_FUTURE"));
}

// 崩溃类输入整类收口（缺陷记录见 bugs/p10_storage_writer.md BUG-P10-001/002）。
// 每个输入单独在子进程里执行，避免越界访问或崩溃终止测试宿主；即使不启用
// ASan 时通过，也不能据此证明所有未定义越界读取都已暴露或已消除。
TEST_F(P10ProcBlockTest, EmptyInputRejectedInChild)
{
	EXPECT_EXIT(expect_proc_block_data_rejects_without_mutation(_writer, std::string()),
		::testing::ExitedWithCode(0), "");
}

TEST_F(P10ProcBlockTest, FourByteInputRejectedInChild)
{
	EXPECT_EXIT(expect_proc_block_data_rejects_without_mutation(_writer, std::string("1234", 4)),
		::testing::ExitedWithCode(0), "");
}

TEST_F(P10ProcBlockTest, ShortCompressedHeaderRejectedInChild)
{
	EXPECT_EXIT(expect_proc_block_data_rejects_without_mutation(_writer, make_short_cmp_v2_header()),
		::testing::ExitedWithCode(0), "");
}

}  // namespace
