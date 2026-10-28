// p10 / WtDataStorage：WtDataWriter 的日线 .dsb 文件形态写入路径（写入→读回→逐字段比对）
//
// 覆盖对象（全部为文件形态落盘，无命名共享内存、无后台线程）：
//  - WtDataWriter::dump_day_data（新建块头 / 追加 / 同日期替换 / 旧日期忽略 / >100 条压缩 / 压缩态保持 / altcode 重命名 / 打开失败拒绝）
//  - WtDataWriter::proc_block_data（作为读回解码器：RAW_V2 与 CMP_V2 两条分支）
//  - WtDataWriter::dump_bars_to_file（NULL 合约入参拒绝）
//
// 落盘：std::filesystem::temp_directory_path()/p10_day_<pid>_<seq>，TearDown 清理；不碰 dist/
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

// 覆盖率需要直接驱动私有成员方法/成员变量；本仓库已有同类先例（p03）
#include "p10_writer_prefix.h"   // 第三方头需先于访问宏 include，见该文件注释

#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "WTSUtils/WTSCmpHelper.hpp"

using namespace wtp;

namespace {

static std::string p10_read_file(const std::string& path)
{
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return std::string();
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

static void p10_set_flag(char* dst, size_t cap)
{
	std::memcpy(dst, BLK_FLAG, cap < sizeof(BLK_FLAG) - 1 ? cap : sizeof(BLK_FLAG) - 1);
}

static WTSBarStruct p10_make_bar(uint32_t date, uint32_t seq)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = 0;
	bar.open = 1000.0 + seq * 1.5;
	bar.high = 1010.0 + seq * 1.5;
	bar.low = 990.0 + seq * 1.5;
	bar.close = 1005.0 + seq * 1.5;
	bar.settle = 1004.0 + seq * 1.5;
	bar.money = 123456.75 + seq;
	bar.vol = 1000 + seq;
	bar.hold = 2000 + seq;
	bar.add = 7 + seq;
	return bar;
}

class P10DayDsbTest : public ::testing::Test
{
protected:
	std::filesystem::path _dir;
	std::string _base;
	WtDataWriter* _writer;
	WTSSessionInfo* _session;
	WTSCommodityInfo* _comm;
	WTSContractInfo* _ct;

	void SetUp() override
	{
		static std::atomic<uint32_t> seq(0);
		_dir = std::filesystem::temp_directory_path()
			/ ("p10_day_" + std::to_string(getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();
		_base = _dir.string() + "/";

		_writer = new WtDataWriter();
		_writer->_base_dir = _base;          // 不通过 init()，避免起后台线程
		_writer->_save_tick_log = false;

		_session = WTSSessionInfo::create("P10_DAY_SESS", "p10 日盘", 0);
		_session->addTradingSection(0, 2359);
		_comm = WTSCommodityInfo::create("rb", "p10 螺纹钢", "SHFE", "P10_DAY_SESS", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
		_ct = WTSContractInfo::create("rb2210", "p10 rb2210", "SHFE", "rb");
		_ct->setCommInfo(_comm);
	}

	void TearDown() override
	{
		if (_writer != NULL)
		{
			_writer->release();     // _proc_thrd 从未创建，此处只回收块指针
			delete _writer;
			_writer = NULL;
		}
		if (_ct != NULL) { _ct->release(); _ct = NULL; }
		if (_comm != NULL) { _comm->release(); _comm = NULL; }
		if (_session != NULL) { _session->release(); _session = NULL; }

		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::string day_file(const char* code)
	{
		return _base + "his/day/SHFE/" + code + ".dsb";
	}
};

TEST_F(P10DayDsbTest, NewDayFileWritesRawV2HeaderAndFirstBar)
{
	WTSBarStruct bar = p10_make_bar(20240101, 1);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &bar));

	const std::string file = day_file("rb2210");
	ASSERT_TRUE(std::filesystem::exists(file));

	const std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + sizeof(WTSBarStruct));

	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(std::memcmp(hdr._blk_flag, BLK_FLAG, sizeof(hdr._blk_flag)), 0);
	EXPECT_EQ(hdr._type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_FALSE(hdr.is_old_version());
	EXPECT_FALSE(hdr.is_compressed());

	WTSBarStruct stored;
	std::memcpy(&stored, content.data() + sizeof(BlockHeader), sizeof(stored));
	EXPECT_EQ(stored.date, bar.date);
	EXPECT_EQ(stored.time, bar.time);
	EXPECT_DOUBLE_EQ(stored.open, bar.open);
	EXPECT_DOUBLE_EQ(stored.high, bar.high);
	EXPECT_DOUBLE_EQ(stored.low, bar.low);
	EXPECT_DOUBLE_EQ(stored.close, bar.close);
	EXPECT_DOUBLE_EQ(stored.settle, bar.settle);
	EXPECT_DOUBLE_EQ(stored.money, bar.money);
	EXPECT_DOUBLE_EQ(stored.vol, bar.vol);
	EXPECT_DOUBLE_EQ(stored.hold, bar.hold);
	EXPECT_DOUBLE_EQ(stored.add, bar.add);
	// 头部之后的载荷应当与结构体逐字节一致
	EXPECT_EQ(std::memcmp(content.data() + sizeof(BlockHeader), &bar, sizeof(bar)), 0);
}

TEST_F(P10DayDsbTest, AppendNewerDateKeepsOrderAndCount)
{
	WTSBarStruct b1 = p10_make_bar(20240101, 1);
	WTSBarStruct b2 = p10_make_bar(20240102, 2);
	WTSBarStruct b3 = p10_make_bar(20240103, 3);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b1));
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b2));
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b3));

	const std::string file = day_file("rb2210");
	std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + 3 * sizeof(WTSBarStruct));

	// 用被测解码器去掉头部，再逐条比对
	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	ASSERT_EQ(content.size(), 3 * sizeof(WTSBarStruct));

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	EXPECT_EQ(bars[0].date, 20240101u);
	EXPECT_EQ(bars[1].date, 20240102u);
	EXPECT_EQ(bars[2].date, 20240103u);
	EXPECT_EQ(std::memcmp(&bars[0], &b1, sizeof(b1)), 0);
	EXPECT_EQ(std::memcmp(&bars[1], &b2, sizeof(b2)), 0);
	EXPECT_EQ(std::memcmp(&bars[2], &b3, sizeof(b3)), 0);
}

TEST_F(P10DayDsbTest, SameDateDifferentValuesReplacesLastBar)
{
	WTSBarStruct b1 = p10_make_bar(20240101, 1);
	WTSBarStruct b2 = p10_make_bar(20240102, 2);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b1));
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b2));

	WTSBarStruct fixed = b2;
	fixed.close = 9999.25;
	fixed.vol = 424242.0;
	ASSERT_TRUE(_writer->dump_day_data(_ct, &fixed));

	const std::string file = day_file("rb2210");
	std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + 2 * sizeof(WTSBarStruct));
	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));

	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	EXPECT_EQ(std::memcmp(&bars[0], &b1, sizeof(b1)), 0);
	EXPECT_EQ(bars[1].date, 20240102u);
	EXPECT_DOUBLE_EQ(bars[1].close, 9999.25);
	EXPECT_DOUBLE_EQ(bars[1].vol, 424242.0);
	// 未被覆盖的字段仍应等于原值
	EXPECT_DOUBLE_EQ(bars[1].open, b2.open);
	EXPECT_DOUBLE_EQ(bars[1].money, b2.money);
}

TEST_F(P10DayDsbTest, SameDateIdenticalBarIsNoOp)
{
	WTSBarStruct b1 = p10_make_bar(20240101, 1);
	WTSBarStruct b2 = p10_make_bar(20240102, 2);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b1));
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b2));

	const std::string before = p10_read_file(day_file("rb2210"));
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b2));
	const std::string after = p10_read_file(day_file("rb2210"));

	EXPECT_EQ(before.size(), after.size());
	EXPECT_EQ(before, after);
}

TEST_F(P10DayDsbTest, OlderThanLastDateIsIgnored)
{
	WTSBarStruct b2 = p10_make_bar(20240105, 2);
	WTSBarStruct b1 = p10_make_bar(20240108, 1);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b2));
	// 20240105 < 20240108，既不追加也不替换
	ASSERT_TRUE(_writer->dump_day_data(_ct, &b1));

	const std::string file = day_file("rb2210");
	std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + sizeof(WTSBarStruct));
	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	EXPECT_EQ(bars[0].date, 20240105u);
	EXPECT_EQ(std::memcmp(&bars[0], &b2, sizeof(b2)), 0);
}

TEST_F(P10DayDsbTest, OverOneHundredBarsSwitchesToCompressedV2)
{
	const uint32_t total = 101;
	WTSBarStruct bars[total];
	for (uint32_t i = 0; i < total; i++)
	{
		bars[i] = p10_make_bar(20240101 + i, i);
		ASSERT_TRUE(_writer->dump_day_data(_ct, &bars[i]));
	}

	const std::string file = day_file("rb2210");
	const std::string raw = p10_read_file(file);
	ASSERT_GT(raw.size(), sizeof(BlockHeaderV2));

	BlockHeaderV2 hdr;
	std::memcpy(&hdr, raw.data(), sizeof(hdr));
	EXPECT_EQ(std::memcmp(hdr._blk_flag, BLK_FLAG, sizeof(hdr._blk_flag)), 0);
	EXPECT_EQ(hdr._type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_CMP_V2);
	EXPECT_TRUE(hdr.is_compressed());
	EXPECT_FALSE(hdr.is_old_version());
	// 头部记录的压缩长度必须与文件剩余字节严格一致
	EXPECT_EQ(hdr._size, (uint64_t)(raw.size() - sizeof(BlockHeaderV2)));

	std::string content = raw;
	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	ASSERT_EQ(content.size(), total * sizeof(WTSBarStruct));
	EXPECT_EQ(std::memcmp(content.data(), bars, content.size()), 0);
}

TEST_F(P10DayDsbTest, CompressedFileStaysCompressedAfterAppend)
{
	const uint32_t total = 102;
	for (uint32_t i = 0; i < total; i++)
	{
		WTSBarStruct bar = p10_make_bar(20240101 + i, i);
		ASSERT_TRUE(_writer->dump_day_data(_ct, &bar));
	}

	const std::string file = day_file("rb2210");
	std::string content = p10_read_file(file);
	ASSERT_GT(content.size(), sizeof(BlockHeaderV2));
	BlockHeaderV2 hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(hdr._size, (uint64_t)(content.size() - sizeof(BlockHeaderV2)));

	// 已是压缩态时，追加一条后仍是压缩态（bCompressed 由旧头部继承）
	WTSBarStruct extra = p10_make_bar(20240101 + total, total);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &extra));

	content = p10_read_file(file);
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_CMP_V2);
	EXPECT_EQ(hdr._size, (uint64_t)(content.size() - sizeof(BlockHeaderV2)));

	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	ASSERT_EQ(content.size(), (total + 1) * sizeof(WTSBarStruct));
	const WTSBarStruct* bars = (const WTSBarStruct*)content.data();
	EXPECT_EQ(std::memcmp(&bars[total], &extra, sizeof(extra)), 0);
	EXPECT_EQ(bars[total].date, 20240101u + total);
	EXPECT_EQ(bars[0].date, 20240101u);
}

TEST_F(P10DayDsbTest, ExactlyOneHundredBarsStaysRaw)
{
	// 边界：barcnt > 100 才压缩，100 条仍应是 RAW_V2
	for (uint32_t i = 0; i < 100; i++)
	{
		WTSBarStruct bar = p10_make_bar(20240101 + i, i);
		ASSERT_TRUE(_writer->dump_day_data(_ct, &bar));
	}

	const std::string file = day_file("rb2210");
	std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + 100 * sizeof(WTSBarStruct));
	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_RAW_V2);
	EXPECT_FALSE(hdr.is_compressed());

	ASSERT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	EXPECT_EQ(content.size(), 100 * sizeof(WTSBarStruct));
}

TEST_F(P10DayDsbTest, AltCodeFileRenamedBeforeWrite)
{
	// 先用老代码 rb2001 落 3 条
	WTSContractInfo* oldCt = WTSContractInfo::create("rb2001", "p10 rb2001", "SHFE", "rb");
	oldCt->setCommInfo(_comm);
	for (uint32_t i = 0; i < 3; i++)
	{
		WTSBarStruct bar = p10_make_bar(20240101 + i, i);
		ASSERT_TRUE(_writer->dump_day_data(oldCt, &bar));
	}
	oldCt->release();
	const std::string oldFile = day_file("rb2001");
	const std::string newFile = day_file("rb2210");
	ASSERT_TRUE(std::filesystem::exists(oldFile));
	const std::string oldContent = p10_read_file(oldFile);
	ASSERT_EQ(oldContent.size(), sizeof(BlockHeader) + 3 * sizeof(WTSBarStruct));

	// 换绑 altcode：code 与 altcode 不同，且 altcode 文件存在 -> 重命名后按新文件写
	_ct->setAltCode("rb2001");
	WTSBarStruct bar = p10_make_bar(20240201, 9);
	ASSERT_TRUE(_writer->dump_day_data(_ct, &bar));

	EXPECT_FALSE(std::filesystem::exists(oldFile));
	ASSERT_TRUE(std::filesystem::exists(newFile));
	std::string content = p10_read_file(newFile);
	ASSERT_EQ(content.size(), sizeof(BlockHeader) + sizeof(WTSBarStruct));
	BlockHeader hdr;
	std::memcpy(&hdr, content.data(), sizeof(hdr));
	EXPECT_EQ(hdr._type, (uint16_t)BT_HIS_Day);
	EXPECT_EQ(hdr._version, (uint16_t)BLOCK_VERSION_RAW_V2);
	WTSBarStruct stored;
	std::memcpy(&stored, content.data() + sizeof(BlockHeader), sizeof(stored));
	EXPECT_EQ(std::memcmp(&stored, &bar, sizeof(bar)), 0);
}

TEST_F(P10DayDsbTest, UnwritableBaseDirRejected)
{
	// 把 _base_dir 指到一个普通文件下面：目录创建失败、文件打不开 -> 返回 false 且不产生文件
	std::string blocker = _dir.string() + "/blocker.txt";
	{
		std::ofstream ofs(blocker.c_str(), std::ios::binary);
		ofs << "x";
	}
	_writer->_base_dir = blocker + "/";

	WTSBarStruct bar = p10_make_bar(20240101, 1);
	EXPECT_FALSE(_writer->dump_day_data(_ct, &bar));
	EXPECT_FALSE(std::filesystem::exists(blocker + "/his/day/SHFE/rb2210.dsb"));
}

TEST_F(P10DayDsbTest, DumpBarsToFileRejectsNullContract)
{
	EXPECT_EQ(_writer->dump_bars_to_file(NULL), 0u);
}

TEST_F(P10DayDsbTest, EmptyHistoryFileWithHeaderOnlyDecodesToEmptyPayload)
{
	// 手写一个只有头部的 .dsb（模拟被截断的历史文件），解码应得到 0 长度载荷
	std::string file = day_file("rb2210");
	std::filesystem::create_directories(std::filesystem::path(file).parent_path());
	{
		std::ofstream ofs(file.c_str(), std::ios::binary | std::ios::trunc);
		BlockHeader hdr;
		std::memset(&hdr, 0, sizeof(hdr));
		p10_set_flag(hdr._blk_flag, sizeof(hdr._blk_flag));
		hdr._type = BT_HIS_Day;
		hdr._version = BLOCK_VERSION_RAW_V2;
		ofs.write((const char*)&hdr, sizeof(hdr));
	}

	std::string content = p10_read_file(file);
	ASSERT_EQ(content.size(), sizeof(BlockHeader));
	EXPECT_TRUE(_writer->proc_block_data(file.c_str(), content, true, false));
	EXPECT_EQ(content.size(), 0u);

	// 空载荷 + 新 bar：barcnt==0 时既不追加也不替换，且 bNeedWrite 从未参与写入判断
	// 实测（待审核 BUG-P10-004）：新 bar 被静默丢弃，函数仍返回 true，文件仍只有 12 字节头部。
	// 这里刻意不再从 content.data()+12 读取结构体，避免越界读。
	WTSBarStruct bar = p10_make_bar(20240301, 5);
	EXPECT_TRUE(_writer->dump_day_data(_ct, &bar));
	content = p10_read_file(file);
	EXPECT_EQ(content.size(), sizeof(BlockHeader));
}

}  // namespace
