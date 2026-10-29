/*
 * L05 - WtDtHelper：K线（bar）相关的二进制/CSV 转换入口
 *
 * 覆盖 src/WtDtHelper/WtDtHelper.cpp 中 store_bars / read_dsb_bars / read_dmb_bars /
 * dump_bars / trans_csv_bars，以及内部 proc_block_data、strToDate、strToTime（经由导出
 * 函数间接覆盖，避免依赖非导出符号的链接）。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 * 全部落盘在 std::filesystem::temp_directory_path() 下的 l05_ 前缀唯一目录，TearDown 清理。
 */
#include <gtest/gtest.h>

#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include "WtDataStorage/DataDefine.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

USING_NS_WTP;

namespace {

	std::vector<std::string>& log_sink()
	{
		static std::vector<std::string> s_logs;
		return s_logs;
	}

	std::vector<WTSBarStruct>& bar_sink()
	{
		static std::vector<WTSBarStruct> s_bars;
		return s_bars;
	}

	uint32_t& count_sink()
	{
		static uint32_t s_cnt = 0;
		return s_cnt;
	}

	void log_cb(WtString message)
	{
		log_sink().assign(1, message == NULL ? std::string("<null>") : std::string(message));
	}

	void bars_cb(WTSBarStruct* bar, WtUInt32 count, bool isLast)
	{
		if (bar != NULL && count > 0)
			bar_sink().assign(bar, bar + count);
		(void)isLast;
	}

	void count_cb(WtUInt32 dataCnt)
	{
		count_sink() = dataCnt;
	}

	bool any_log_has(const char* key)
	{
		for (const auto& s : log_sink())
		{
			if (s.find(key) != std::string::npos)
				return true;
		}
		return false;
	}

	WTSBarStruct make_day_bar(uint32_t date, double price)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(WTSBarStruct));
		bar.date = date;
		bar.time = 0;
		bar.open = price;
		bar.high = price + 1;
		bar.low = price - 1;
		bar.close = price + 0.5;
		bar.settle = price;
		bar.vol = price * 10;
		bar.money = price * 100;
		bar.hold = price * 5;
		bar.add = 1;
		return bar;
	}

} // namespace

class L05HelperBarTest : public ::testing::Test
{
protected:
	std::filesystem::path _root;

	void SetUp() override
	{
		static int s_seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("l05_helper_bar_" + std::to_string(::getpid()) + "_" + std::to_string(++s_seq));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
		log_sink().clear();
		bar_sink().clear();
		count_sink() = 0;
		ASSERT_FALSE(ec);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::string file(const std::string& name) const
	{
		return (_root / name).string();
	}
};

TEST_F(L05HelperBarTest, StoreBarsRejectsZeroCount)
{
	EXPECT_FALSE(store_bars(file("none.dsb").c_str(), NULL, 0, "d", log_cb));
	EXPECT_TRUE(any_log_has("0"));
	// 未注册日志回调时同样只返回 false，不崩溃
	EXPECT_FALSE(store_bars(file("none.dsb").c_str(), NULL, 0, "d", NULL));
	EXPECT_FALSE(std::filesystem::exists(_root / "none.dsb"));
}

TEST_F(L05HelperBarTest, StoreBarsRejectsUnknownPeriod)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	EXPECT_FALSE(store_bars(file("bad.dsb").c_str(), bars.data(), (int)bars.size(), "w1", log_cb));
	EXPECT_TRUE(any_log_has("m1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "bad.dsb"));
}

TEST_F(L05HelperBarTest, StoreAndReadBarsRoundtrip)
{
	const char* periods[] = { "d", "m1", "m5" };
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
		bars.push_back(make_day_bar(20200101 + i, 100 + i));

	for (const char* period : periods)
	{
		const std::string name = std::string("rb2001_") + period + ".dsb";
		ASSERT_TRUE(store_bars(file(name).c_str(), bars.data(), (int)bars.size(), period, NULL));
		ASSERT_TRUE(std::filesystem::exists(_root / name));

		bar_sink().clear();
		count_sink() = 0;
		WtUInt32 reaCnt = read_dsb_bars(file(name).c_str(), bars_cb, count_cb, log_cb);
		EXPECT_EQ(reaCnt, (WtUInt32)bars.size());
		EXPECT_EQ(count_sink(), (uint32_t)bars.size());
		ASSERT_EQ(bar_sink().size(), bars.size());
		for (size_t i = 0; i < bars.size(); i++)
		{
			EXPECT_DOUBLE_EQ(bar_sink()[i].open, bars[i].open);
			EXPECT_DOUBLE_EQ(bar_sink()[i].close, bars[i].close);
			EXPECT_DOUBLE_EQ(bar_sink()[i].vol, bars[i].vol);
			EXPECT_EQ(bar_sink()[i].date, bars[i].date);
		}
		EXPECT_TRUE(any_log_has("read") || !log_sink().empty());
	}
}

TEST_F(L05HelperBarTest, StoreBarsReportsMissingParentDirectoryFailure)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	const std::filesystem::path missing_dir = _root / "no_such_dir";
	ASSERT_FALSE(std::filesystem::exists(missing_dir));
	const std::filesystem::path output = missing_dir / "x.dsb";
	EXPECT_FALSE(store_bars(output.string().c_str(), bars.data(), (int)bars.size(), "d", NULL));
	EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(L05HelperBarTest, ReadBarsHeaderValidation)
{
	// 文件过小：直接返回 0，不调用计数回调
	{
		std::ofstream ofs(file("tiny.dsb"), std::ios::binary | std::ios::trunc);
		ofs << "abc";
	}
	count_sink() = 12345;
	bar_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("tiny.dsb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("头部校验失败"));

	// 文件不存在：同样走头部校验失败分支
	log_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("not_exists.dsb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_TRUE(any_log_has("头部校验失败"));

	// 不传日志回调也不能崩
	log_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("not_exists.dsb").c_str(), bars_cb, count_cb, NULL), (WtUInt32)0);
}

TEST_F(L05HelperBarTest, DumpBarsMissingSourceFolder)
{
	dump_bars((_root / "nothing").string().c_str(), file("csv").c_str(), "", log_cb);
	EXPECT_TRUE(any_log_has("不存在"));
}

TEST_F(L05HelperBarTest, DumpBarsWritesCsvAndSkipsOthers)
{
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 3; i++)
		bars.push_back(make_day_bar(20200101 + i, 3000 + i));
	ASSERT_TRUE(store_bars(file("SHFE.rb2010.dsb").c_str(), bars.data(), (int)bars.size(), "d", NULL));

	// tick 文件（块类型不属于 K 线）应被跳过
	std::string tick_blob(sizeof(HisTickBlock) + sizeof(WTSTickStruct), '\0');
	{
		HisTickBlock* header = (HisTickBlock*)&tick_blob[0];
		memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
		header->_type = BT_HIS_Ticks;
		header->_version = BLOCK_VERSION_RAW_V2;
		std::ofstream ofs(file("tick_like.dsb"), std::ios::binary | std::ios::trunc);
		ofs.write(tick_blob.data(), (std::streamsize)tick_blob.size());
	}
	// 非法小文件 + 非 .dsb 文件 + 子目录
	{
		std::ofstream ofs(file("broken.dsb"), std::ios::binary | std::ios::trunc);
		ofs << "xy";
		ofs.close();
		std::ofstream ignore(file("ignore.txt"), std::ios::trunc);
		ignore << "whatever";
	}
	std::filesystem::create_directories(_root / "sub_dir");

	const std::string csvDir = file("csv_out");
	dump_bars(file("").c_str(), csvDir.c_str(), "", log_cb);
	// srcFolder 用 standardisePath 归一化，直接传根目录
	dump_bars(_root.string().c_str(), csvDir.c_str(), "", log_cb);

	const std::string csv = (std::filesystem::path(csvDir) / "SHFE.rb2010.csv").string();
	ASSERT_TRUE(std::filesystem::exists(csv));
	std::ifstream ifs(csv);
	std::string line;
	ASSERT_TRUE(std::getline(ifs, line));
	EXPECT_EQ(line, "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest");
	int dataLines = 0;
	while (std::getline(ifs, line))
	{
		if (!line.empty())
			dataLines++;
	}
	EXPECT_EQ(dataLines, 3);
	EXPECT_EQ(line.find("3002"), std::string::npos);
	EXPECT_TRUE(any_log_has("导出完成"));
}

TEST_F(L05HelperBarTest, TransCsvBarsMissingSourceFolder)
{
	trans_csv_bars((_root / "no_csv").string().c_str(), file("bin").c_str(), "d", log_cb);
	EXPECT_FALSE(std::filesystem::exists(_root / "bin"));
	EXPECT_TRUE(log_sink().empty());
}

TEST_F(L05HelperBarTest, TransCsvBarsDailyRoundtrip)
{
	const std::string csvDir = file("csv_in");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "DCE.i2001.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ofs << "2020/01/16,0,1745,1752,1740,1748.5,1746,12000,21000000,35000,120" << std::endl;
		ofs << "20200117,0,1748,1760,1745,1755,1753,15000,26325000,36000,1000" << std::endl;
		std::ofstream ignore((std::filesystem::path(csvDir) / "notes.md").string());
		ignore << "ignored";
		std::filesystem::create_directories(std::filesystem::path(csvDir) / "nested");
	}

	const std::string binDir = file("bin_out");
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);

	const std::string dsb = (std::filesystem::path(binDir) / "DCE.i2001.dsb").string();
	ASSERT_TRUE(std::filesystem::exists(dsb));

	bar_sink().clear();
	count_sink() = 0;
	WtUInt32 reaCnt = read_dsb_bars(dsb.c_str(), bars_cb, count_cb, log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)2);
	ASSERT_EQ(bar_sink().size(), (size_t)2);
	EXPECT_EQ(bar_sink()[0].date, 20200116u);
	EXPECT_DOUBLE_EQ(bar_sink()[0].open, 1745);
	EXPECT_DOUBLE_EQ(bar_sink()[1].close, 1755);
	EXPECT_TRUE(any_log_has("转储至"));
}

TEST_F(L05HelperBarTest, TransCsvBarsMinuteAndUnknownPeriod)
{
	const std::string csvDir = file("csv_min");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "CFFEX.if2003.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ofs << "2020-03-02 00:00:00,09:30:00,3785,3786,3780,3782,0,120,453840,0,0" << std::endl;
		ofs << "2020-03-02,09:31:00,3782,3790,3782,3788,0,200,757800,0,0" << std::endl;
	}

	for (const char* period : { "m1", "m5", "week" })
	{
		const std::string binDir = file(std::string("bin_") + period);
		trans_csv_bars(csvDir.c_str(), binDir.c_str(), period, log_cb);
		const std::string dsb = (std::filesystem::path(binDir) / "CFFEX.if2003.dsb").string();
		ASSERT_TRUE(std::filesystem::exists(dsb)) << period;

		bar_sink().clear();
		count_sink() = 0;
		EXPECT_EQ(read_dsb_bars(dsb.c_str(), bars_cb, count_cb, NULL), (WtUInt32)2) << period;
		ASSERT_EQ(bar_sink().size(), (size_t)2) << period;
		EXPECT_EQ(bar_sink()[0].date, 20200302u) << period;
		if (strcmp(period, "week") != 0)
			EXPECT_NE(bar_sink()[0].time, 0u) << period;
		else
			EXPECT_EQ(bar_sink()[0].time, 0u) << period;
	}
}

TEST_F(L05HelperBarTest, TransCsvBarsUnreadableFileIsSkipped)
{
	const std::string csvDir = file("csv_bad");
	std::filesystem::create_directories(csvDir);
	std::filesystem::create_directories(std::filesystem::path(csvDir) / "empty.csv");

	const std::string binDir = file("bin_bad");
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);
	EXPECT_TRUE(log_sink().empty() || any_log_has("失败"));
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(binDir) / "empty.dsb"));
}

TEST_F(L05HelperBarTest, ReadDmbBarsValidationPaths)
{
	{
		std::ofstream ofs(file("tiny.dmb"), std::ios::binary | std::ios::trunc);
		ofs << "123";
	}
	log_sink().clear();
	count_sink() = 7;
	EXPECT_EQ(read_dmb_bars(file("tiny.dmb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_TRUE(any_log_has("头部校验失败"));
	EXPECT_EQ(count_sink(), 7u);

	// 合法头部但 _size 为 0
	std::string blob(sizeof(RTKlineBlock), '\0');
	((RTKlineBlock*)&blob[0])->_size = 0;
	{
		std::ofstream ofs(file("zero.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	log_sink().clear();
	count_sink() = 0;
	EXPECT_EQ(read_dmb_bars(file("zero.dmb").c_str(), bars_cb, count_cb, NULL), (WtUInt32)0);
	EXPECT_EQ(count_sink(), 0u);

	// 头部 + 2 条 bar
	blob.resize(sizeof(RTKlineBlock) + 2 * sizeof(WTSBarStruct));
	RTKlineBlock* block = (RTKlineBlock*)&blob[0];
	block->_size = 2;
	WTSBarStruct* outBars = block->_bars;
	outBars[0] = make_day_bar(20200102, 100);
	outBars[1] = make_day_bar(20200103, 101);
	{
		std::ofstream ofs(file("two.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	bar_sink().clear();
	count_sink() = 0;
	EXPECT_EQ(read_dmb_bars(file("two.dmb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)2);
	EXPECT_EQ(count_sink(), 2u);
	ASSERT_EQ(bar_sink().size(), (size_t)2);
	EXPECT_EQ(bar_sink()[1].date, 20200103u);
	EXPECT_TRUE(any_log_has("读取完成"));
}
