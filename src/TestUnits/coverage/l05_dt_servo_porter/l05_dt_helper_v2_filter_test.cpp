/*
 * BUG-L05-002 (iteration 03, 2026-11-04, package dt-writer)
 *
 * Versioned CSV export (dump_bars_v2 / dump_ticks_v2):
 *  - strFilter is the contract filter documented by the original public header
 *    ("合约过滤器，支持通配符，默认为空（处理所有合约）"): one wildcard pattern matched
 *    against the whole .dsb file stem; '*' any sequence, '?' one character.
 *  - A readable folder in which no .dsb file matches returns WT_CSV_DUMP_NO_INPUT,
 *    never SUCCESS, and never logs "全部导出完成".
 *  - Unknown block version and unknown file identifier are rejected independently,
 *    each in a folder that contains only that one defect.
 *
 * Every case writes under a unique temp directory and removes it in TearDown.
 */
#include <gtest/gtest.h>

#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include "WtDataStorage/DataDefine.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

USING_NS_WTP;

namespace {

namespace fs = std::filesystem;

std::vector<std::string>& v2_logs()
{
	static std::vector<std::string> logs;
	return logs;
}

void v2_log_cb(WtString message)
{
	v2_logs().emplace_back(message == nullptr ? std::string("<null>") : std::string(message));
}

bool v2_log_has(const std::string& key)
{
	for (const std::string& line : v2_logs())
	{
		if (line.find(key) != std::string::npos)
			return true;
	}
	return false;
}

WTSBarStruct v2_day_bar(uint32_t date, double close)
{
	WTSBarStruct bar;
	std::memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.open = bar.high = bar.low = bar.close = close;
	bar.vol = 10;
	return bar;
}

WTSTickStruct v2_tick(const char* code, double price)
{
	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strncpy(tick.exchg, "SHFE", sizeof(tick.exchg) - 1);
	std::strncpy(tick.code, code, sizeof(tick.code) - 1);
	tick.price = price;
	tick.trading_date = 20200102;
	tick.action_date = 20200102;
	tick.action_time = 93000000;
	return tick;
}

class L05HelperV2FilterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static std::atomic<unsigned> sequence{ 0 };
		_root = fs::temp_directory_path() / ("l05_v2_filter_" + std::to_string(::getpid()) + "_" + std::to_string(sequence++));
		fs::create_directories(_root / "bin");
		v2_logs().clear();
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_root, ec);
	}

	std::string bin(const std::string& name = "") const { return (_root / "bin" / name).string(); }
	std::string csv(const std::string& name = "") const { return (_root / "csv" / name).string(); }

	void storeDay(const std::string& code, double close)
	{
		std::vector<WTSBarStruct> bars{ v2_day_bar(20200102, close), v2_day_bar(20200103, close + 1) };
		ASSERT_TRUE(store_bars(bin(code + ".dsb").c_str(), bars.data(), (int)bars.size(), "d", nullptr));
	}

	void storeTick(const std::string& code, double price)
	{
		std::vector<WTSTickStruct> ticks{ v2_tick(code.c_str(), price) };
		ASSERT_TRUE(store_ticks(bin(code + ".dsb").c_str(), ticks.data(), (int)ticks.size(), nullptr));
	}

	// Raw (uncompressed V2 layout) block with a caller-chosen identifier and version.
	void writeRawBlock(const std::string& name, const char* flag, uint16_t type, uint16_t version, size_t itemSize)
	{
		std::string blob(sizeof(BlockHeader) + itemSize, '\0');
		BlockHeader header;
		std::memset(&header, 0, sizeof(header));
		std::memcpy(header._blk_flag, flag, FLAG_SIZE);
		header._type = type;
		header._version = version;
		std::memcpy(&blob[0], &header, sizeof(header));
		std::ofstream out(bin(name), std::ios::binary | std::ios::trunc);
		out.write(blob.data(), (std::streamsize)blob.size());
	}

	std::vector<std::string> csvFiles() const
	{
		std::vector<std::string> names;
		std::error_code ec;
		if (!fs::exists(_root / "csv", ec))
			return names;
		for (const auto& entry : fs::directory_iterator(_root / "csv"))
			names.push_back(entry.path().filename().string());
		std::sort(names.begin(), names.end());
		return names;
	}

	fs::path _root;
};

TEST_F(L05HelperV2FilterTest, BarFilterWildcardSelectsMatchingStemsOnly)
{
	storeDay("rb2210", 100);
	storeDay("rb2301", 200);
	storeDay("ag2212", 300);

	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "rb*", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "rb2210.csv", "rb2301.csv" }));
	EXPECT_TRUE(v2_log_has("全部导出完成"));

	fs::remove_all(_root / "csv");
	v2_logs().clear();
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "rb22?0", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "rb2210.csv" }));

	fs::remove_all(_root / "csv");
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "ag2212", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "ag2212.csv" }));

	// Whole-stem match, case-sensitive: neither a prefix nor a different case matches.
	fs::remove_all(_root / "csv");
	v2_logs().clear();
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "rb22", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "RB*", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_TRUE(csvFiles().empty());
	EXPECT_FALSE(v2_log_has("全部导出完成"));
	EXPECT_TRUE(v2_log_has("nothing exported"));

	// Empty filter keeps the legacy "all contracts" behaviour.
	fs::remove_all(_root / "csv");
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "ag2212.csv", "rb2210.csv", "rb2301.csv" }));
}

TEST_F(L05HelperV2FilterTest, TickFilterWildcardSelectsMatchingStemsOnly)
{
	storeTick("rb2210", 100);
	storeTick("ag2212", 300);

	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "*2212", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "ag2212.csv" }));

	fs::remove_all(_root / "csv");
	v2_logs().clear();
	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "cu*", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_TRUE(csvFiles().empty());
	EXPECT_FALSE(v2_log_has("全部导出完成"));
}

TEST_F(L05HelperV2FilterTest, EmptyOrNonDsbFolderIsNoInputNotSuccess)
{
	v2_logs().clear();
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_FALSE(v2_log_has("全部导出完成"));

	std::ofstream(bin("notes.txt")) << "not a data block";
	fs::create_directories(bin("sub.dsb"));	// a directory named like a block is skipped
	v2_logs().clear();
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "", nullptr), (uint32_t)WT_CSV_DUMP_NO_INPUT);
	EXPECT_FALSE(v2_log_has("全部导出完成"));
	EXPECT_TRUE(csvFiles().empty());
}

TEST_F(L05HelperV2FilterTest, UnknownBlockVersionIsRejectedOnItsOwn)
{
	writeRawBlock("unknown_version.dsb", BLK_FLAG, BT_HIS_Day, 0x09, sizeof(WTSBarStruct));
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_INPUT_ERROR);
	EXPECT_TRUE(v2_log_has("Invalid data block header: " + bin("unknown_version.dsb")));
	EXPECT_FALSE(v2_log_has("全部导出完成"));
	EXPECT_TRUE(csvFiles().empty());

	// Positive control: the same bytes with a supported version export one bar.
	fs::remove(bin("unknown_version.dsb"));
	writeRawBlock("known_version.dsb", BLK_FLAG, BT_HIS_Day, BLOCK_VERSION_RAW_V2, sizeof(WTSBarStruct));
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_SUCCESS);
	EXPECT_EQ(csvFiles(), (std::vector<std::string>{ "known_version.csv" }));
}

TEST_F(L05HelperV2FilterTest, UnknownFileIdentifierIsRejectedOnItsOwn)
{
	const char badFlag[FLAG_SIZE] = { 'N', 'O', 'T', 'A', 'B', 'L', 'K', '\0' };
	writeRawBlock("bad_flag.dsb", badFlag, BT_HIS_Day, BLOCK_VERSION_RAW_V2, sizeof(WTSBarStruct));
	EXPECT_EQ(dump_bars_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_INPUT_ERROR);
	EXPECT_TRUE(v2_log_has("Invalid data block header: " + bin("bad_flag.dsb")));
	EXPECT_FALSE(v2_log_has("全部导出完成"));
	EXPECT_TRUE(csvFiles().empty());
}

TEST_F(L05HelperV2FilterTest, TickUnknownVersionAndIdentifierAreRejectedIndependently)
{
	writeRawBlock("tick_version.dsb", BLK_FLAG, BT_HIS_Ticks, 0x07, sizeof(WTSTickStruct));
	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_INPUT_ERROR);
	EXPECT_TRUE(v2_log_has("Invalid data block header: " + bin("tick_version.dsb")));
	fs::remove(bin("tick_version.dsb"));

	v2_logs().clear();
	const char badFlag[FLAG_SIZE] = { 'X', 'X', 'X', 'X', 'X', 'X', 'X', '\0' };
	writeRawBlock("tick_flag.dsb", badFlag, BT_HIS_Ticks, BLOCK_VERSION_RAW_V2, sizeof(WTSTickStruct));
	EXPECT_EQ(dump_ticks_v2(bin().c_str(), csv().c_str(), "", v2_log_cb), (uint32_t)WT_CSV_DUMP_INPUT_ERROR);
	EXPECT_TRUE(v2_log_has("Invalid data block header: " + bin("tick_flag.dsb")));
	EXPECT_FALSE(v2_log_has("全部导出完成"));
	EXPECT_TRUE(csvFiles().empty());
}

} // namespace
