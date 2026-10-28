/*!
 * \file q05_resample_bars_test.cpp
 * \brief WtDtHelper/resample_bars 覆盖用例（迭代01 第五波 q05_dtsrv）
 *
 * 覆盖目标（WtDtHelper.cpp:833-1034，l05 点名跳过、剩余 240 行的主体）：
 *  - 周期选择器 m1/m5/d/h/f 与非法周期
 *  - 日线与分钟线的时间格式校验、fromTime>endTime 交换、sessInfo JSON 解析、
 *    sections 缺失、文件头校验、空数据、区间越界（lower_bound 未命中分支）
 *  - 正常日线重采样：store_bars 落盘 -> resample_bars 读回（纯本地临时目录）
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSStruct.h>
#include <WtDtHelper/WtDtHelper.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

// resample_bars 以裸 C 函数指针回调，这里用文件级静态变量做捕获（单进程单线程用例）
std::string g_lastLog;
uint32_t g_cntArg { 0 };
uint32_t g_cbArg { 0 };
bool g_cbIsLast { false };
WTSBarStruct g_firstBar;
WTSBarStruct g_lastBar;

void resetCapture()
{
	g_lastLog.clear();
	g_cntArg = g_cbArg = 0;
	g_cbIsLast = false;
	g_firstBar = WTSBarStruct();
	g_lastBar = WTSBarStruct();
}

void q05Logger(WtString message)
{
	g_lastLog = message == nullptr ? "<null>" : message;
}

void q05Cnt(WtUInt32 dataCnt)
{
	g_cntArg = dataCnt;
}

void q05Bars(WTSBarStruct* bar, WtUInt32 count, bool isLast)
{
	g_cbArg = count;
	g_cbIsLast = isLast;
	if (bar != nullptr && count > 0)
	{
		g_firstBar = bar[0];
		g_lastBar = bar[count - 1];
	}
}

const char* kDaySess = R"JSON({"offset": 0, "auction": {"from": 0, "to": 0},
	"sections": [{"from": 0, "to": 2359}]})JSON";

const char* kSseSess = R"JSON({"offset": 0, "auction": {"from": 915, "to": 925},
	"sections": [{"from": 930, "to": 1130}, {"from": 1300, "to": 1500}]})JSON";

// (year-1990)*1e8 + mmddHHMM，即 resample_bars L958 的编码约定
uint64_t encMinTime(uint32_t year, uint32_t mon, uint32_t day, uint32_t hhmm)
{
	return (uint64_t)(year - 1990) * 100000000ull + mon * 1000000ull + day * 10000ull + hhmm;
}

class Q05ResampleTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("q05_rs");
		resetCapture();
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path _dir;
};

TEST_F(Q05ResampleTest, UnknownPeriodRejected)
{
	resetCapture();
	// 非 m1/m5/d/h/f 开头 -> "不是基础周期" 并返回 0
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 20200117, "w1", 1,
		kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("不是基础周期"), std::string::npos);
}

TEST_F(Q05ResampleTest, DayPeriodRejectsMinuteTimestamps)
{
	resetCapture();
	// isDay 分支：fromTime>=1e8 视为时间而非日期
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 202001060931ull, 202001171500ull,
		"d", 1, kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为日期"), std::string::npos);

	resetCapture();
	// endTime 超过 1e8 同样拒绝
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 100000001,
		"d", 1, kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为日期"), std::string::npos);
}

TEST_F(Q05ResampleTest, MinutePeriodRejectsDateTimestamps)
{
	resetCapture();
	// 分钟分支：fromTime<1e8 视为纯日期，非法
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 202001170931ull,
		"m1", 1, kSseSess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为时间"), std::string::npos);

	resetCapture();
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 202001060931ull, 20200117,
		"m5", 1, kSseSess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为时间"), std::string::npos);
}

TEST_F(Q05ResampleTest, HourAndHalfSelectorsFallIntoMinuteValidation)
{
	resetCapture();
	// 'h'/'f' 前缀被识别为分钟基准周期，随后因分钟校验失败退出（覆盖周期分派分支）
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 202001170931ull,
		"h", 1, kSseSess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为时间"), std::string::npos);

	resetCapture();
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 202001170931ull,
		"f", 1, kSseSess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("应为时间"), std::string::npos);
}

TEST_F(Q05ResampleTest, BadSessionTemplateRejected)
{
	resetCapture();
	// 非法 JSON -> "交易时间模板解析失败"
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 20200117,
		"d", 1, "{not json", q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("解析失败"), std::string::npos);

	resetCapture();
	// 缺 sections（auction 键必须存在，L906 直接 root["auction"] 取值）
	EXPECT_EQ(resample_bars("whatever", q05Bars, q05Cnt, 20200106, 20200117,
		"d", 1, R"JSON({"offset": 0, "auction": null})JSON", q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("格式错误"), std::string::npos);
}

TEST_F(Q05ResampleTest, MissingOrShortFileFailsHeadCheck)
{
	resetCapture();
	const fs::path missing = _dir / "no_such.dsb";
	// 文件缺失：buffer 为空 -> 小于 HisKlineBlock 头
	EXPECT_EQ(resample_bars(missing.string().c_str(), q05Bars, q05Cnt, 20200106, 20200117,
		"d", 1, kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("头部校验失败"), std::string::npos);

	resetCapture();
	fs::path shortFile = _dir / "short.dsb";
	{
		std::ofstream ofs(shortFile.string(), std::ios::binary | std::ios::trunc);
		ofs << "tiny";
	}
	EXPECT_EQ(resample_bars(shortFile.string().c_str(), q05Bars, q05Cnt, 20200106, 20200117,
		"d", 1, kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("头部校验失败"), std::string::npos);
}

TEST_F(Q05ResampleTest, DayResampleOfTenDailyBarsPassesThrough)
{
	// 10 个交易日（20200106 起隔日排布），store_bars("d") 落盘后日线重采样
	WTSBarStruct bars[10];
	uint32_t dates[10] = {20200106, 20200107, 20200108, 20200109, 20200110,
		20200113, 20200114, 20200115, 20200116, 20200117};
	for (int i = 0; i < 10; i++)
	{
		bars[i].date = dates[i];
		bars[i].open = 10.0 + i;
		bars[i].high = 11.0 + i;
		bars[i].low = 9.0 + i;
		bars[i].close = 10.5 + i;
		bars[i].vol = 1000 + i;
		bars[i].money = (1000 + i) * 10;
	}

	fs::path file = _dir / "day.dsb";
	resetCapture();
	ASSERT_TRUE(store_bars(file.string().c_str(), bars, 10, "d", q05Logger));

	resetCapture();
	// 全区间 d->d：times=1 与基础周期一致，按实现约定应为原样透传（10 条）
	WtUInt32 ret = resample_bars(file.string().c_str(), q05Bars, q05Cnt,
		20200106, 20200117, "d", 1, kDaySess, q05Logger);
	EXPECT_EQ(ret, 10u);
	EXPECT_EQ(g_cntArg, ret);
	EXPECT_EQ(g_cbArg, ret);
	EXPECT_TRUE(g_cbIsLast);
	if (ret == 10)
	{
		EXPECT_EQ(g_firstBar.date, 20200106u);
		EXPECT_EQ(g_lastBar.date, 20200117u);
	}

	// fromTime>endTime：L887-890 交换后应与正序结果一致
	resetCapture();
	WtUInt32 retSwap = resample_bars(file.string().c_str(), q05Bars, q05Cnt,
		20200117, 20200106, "d", 1, kDaySess, q05Logger);
	EXPECT_EQ(retSwap, ret);
}

TEST_F(Q05ResampleTest, DayRangeAfterLastBarFindsNothing)
{
	WTSBarStruct bars[3];
	uint32_t dates[3] = {20200106, 20200107, 20200108};
	for (int i = 0; i < 3; i++)
	{
		bars[i].date = dates[i];
		bars[i].open = bars[i].high = bars[i].low = bars[i].close = 10;
	}

	fs::path file = _dir / "tiny3.dsb";
	ASSERT_TRUE(store_bars(file.string().c_str(), bars, 3, "d", q05Logger));

	resetCapture();
	// 起点晚于全部数据 -> lower_bound 未命中 -> "没有找到指定时间范围的K线"
	EXPECT_EQ(resample_bars(file.string().c_str(), q05Bars, q05Cnt,
		20200301, 20200401, "d", 1, kDaySess, q05Logger), 0u);
	EXPECT_NE(g_lastLog.find("没有找到"), std::string::npos);
}

TEST_F(Q05ResampleTest, MinuteResampleReportsConsistentCounts)
{
	// 两个交易日各 10 根 m1（09:31 起），time 采用 (y-1990)*1e8+mmddHHMM 编码
	WTSBarStruct bars[20];
	for (int d = 0; d < 2; d++)
	{
		uint32_t date = d == 0 ? 20200116u : 20200117u;
		for (int i = 0; i < 10; i++)
		{
			WTSBarStruct& b = bars[d * 10 + i];
			b.date = date;
			uint32_t hhmm = 931 + i;
			b.time = encMinTime(2020, 1, (date % 100), hhmm);
			b.open = b.high = b.low = b.close = 10 + i;
			b.vol = 100;
			b.money = 1000;
		}
	}

	fs::path file = _dir / "min.dsb";
	resetCapture();
	ASSERT_TRUE(store_bars(file.string().c_str(), bars, 20, "m1", q05Logger));

	resetCapture();
	// 全区间 m1 数据按 m5(times=5) 重采样：合并条数由会话对齐决定，
	// 此处只断言自一致性（返回值 == cb 收到的条数 == cbCnt 参数）
	WtUInt32 ret = resample_bars(file.string().c_str(), q05Bars, q05Cnt,
		202001160931ull, 202001171000ull, "m5", 5, kSseSess, q05Logger);
	EXPECT_GT(ret, 0u) << g_lastLog;  // 若为 0 即“K线重采样失败”分支，标待裁定
	if (ret > 0)
	{
		EXPECT_EQ(g_cntArg, ret);
		EXPECT_EQ(g_cbArg, ret);
		EXPECT_TRUE(g_cbIsLast);
	}
}

}  // namespace
