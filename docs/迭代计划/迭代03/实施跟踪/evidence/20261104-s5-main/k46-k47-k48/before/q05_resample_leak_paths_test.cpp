/*!
 * \file q05_resample_leak_paths_test.cpp
 * \brief BUG-I02-NEW-104 (z15 finding B): resample_bars failure returns must release
 *        the WTSSessionInfo/WTSKlineSlice they created.
 *
 * The three failure paths are driven with a valid session template so the session
 * object is always created before the failure:
 *  - header-only block (zero records)          -> "数据为空"
 *  - range after the last bar                   -> "没有找到指定时间范围的K线"
 *  - same-period times=1 (factory returns NULL) -> "K线重采样失败"
 * Leak checking itself is external (macOS `leaks --atExit` or LSan); these cases only
 * fix the observable return value and diagnostic so the leak runs are attributable.
 * The times=1 return value (0) is the current behaviour, not a decided contract
 * (NEW-104 remains a card-15 Z item).
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <Includes/WTSStruct.h>
#include <WtDataStorage/DataDefine.h>
#include <WtDtHelper/WtDtHelper.h>

namespace fs = std::filesystem;

namespace {

std::string g_leakLastLog;
uint32_t g_leakBarsCalls { 0 };

void leakLogger(WtString message)
{
	g_leakLastLog = message == nullptr ? "<null>" : message;
}

void leakCnt(WtUInt32) {}

void leakBars(WTSBarStruct*, WtUInt32, bool)
{
	++g_leakBarsCalls;
}

const char* kLeakDaySess = R"JSON({"offset": 0, "auction": {"from": 0, "to": 0},
	"sections": [{"from": 0, "to": 2359}]})JSON";

class Q05ResampleLeakPathsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static std::atomic<int> seq { 0 };
		_dir = fs::temp_directory_path()
			/ ("q05_resample_leak_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		fs::create_directories(_dir);
		g_leakLastLog.clear();
		g_leakBarsCalls = 0;
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	std::string storeDays(const char* name, uint32_t first, int count)
	{
		std::string path = (_dir / name).string();
		std::vector<WTSBarStruct> bars(count);
		for (int i = 0; i < count; i++)
		{
			bars[i].date = first + (uint32_t)i;
			bars[i].open = bars[i].high = bars[i].low = bars[i].close = 10 + i;
		}
		EXPECT_TRUE(store_bars(path.c_str(), bars.data(), count, "d", nullptr));
		return path;
	}

	fs::path _dir;
};

TEST_F(Q05ResampleLeakPathsTest, HeaderOnlyBlockIsEmptyAndReturnsZero)
{
	const std::string path = (_dir / "empty_day.dsb").string();
	BlockHeader header;
	std::memset(&header, 0, sizeof(header));
	std::memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
	header._type = BT_HIS_Day;
	header._version = BLOCK_VERSION_RAW_V2;
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
	}

	EXPECT_EQ(resample_bars(path.c_str(), leakBars, leakCnt, 20200101, 20200131, "d", 2,
		kLeakDaySess, leakLogger), 0u);
	EXPECT_NE(g_leakLastLog.find("数据为空"), std::string::npos) << g_leakLastLog;
	EXPECT_EQ(g_leakBarsCalls, 0u);
}

TEST_F(Q05ResampleLeakPathsTest, RangeAfterLastBarReturnsZero)
{
	const std::string path = storeDays("tail.dsb", 20200106, 3);
	EXPECT_EQ(resample_bars(path.c_str(), leakBars, leakCnt, 20200301, 20200401, "d", 2,
		kLeakDaySess, leakLogger), 0u);
	EXPECT_NE(g_leakLastLog.find("没有找到"), std::string::npos) << g_leakLastLog;
	EXPECT_EQ(g_leakBarsCalls, 0u);
}

TEST_F(Q05ResampleLeakPathsTest, SamePeriodTimesOneFailsWithoutCallback)
{
	const std::string path = storeDays("same.dsb", 20200106, 4);
	EXPECT_EQ(resample_bars(path.c_str(), leakBars, leakCnt, 20200106, 20200109, "d", 1,
		kLeakDaySess, leakLogger), 0u);
	EXPECT_NE(g_leakLastLog.find("重采样失败"), std::string::npos) << g_leakLastLog;
	EXPECT_EQ(g_leakBarsCalls, 0u);
}

TEST_F(Q05ResampleLeakPathsTest, SuccessfulDayResampleControl)
{
	const std::string path = storeDays("ok.dsb", 20200106, 4);
	EXPECT_GT(resample_bars(path.c_str(), leakBars, leakCnt, 20200106, 20200109, "d", 2,
		kLeakDaySess, leakLogger), 0u);
	EXPECT_EQ(g_leakBarsCalls, 1u);
}

} // namespace
