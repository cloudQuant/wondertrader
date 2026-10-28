/*
 * L05 - WtDtServo 导出的 C API：未初始化状态下的参数校验/错误返回路径
 *
 * 覆盖 src/WtDtServo/WtDtServo.cpp 的 7 个查询入口（未初始化 → 切片为 NULL → 返回 0）
 * 与 src/WtDtServo/WtDtRunner.cpp 中对应的 _is_inited 守卫分支，
 * 以及 src/WtDtServo/WtDataManager.cpp::clear_cache 的“DataReader 未初始化”分支。
 *
 * 需要真实行情存储/解析器（wtdt storage、parser 插件、共享内存）的用例见
 * docs/迭代计划/迭代01/manifests/l05_dt_servo_porter.md 的跳过清单。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 */
#include <gtest/gtest.h>

#include "WtDtServo/WtDtServo.h"
#include "Includes/WTSDataDef.hpp"

#include <string>

USING_NS_WTP;

namespace {

	uint32_t& s_bar_calls()
	{
		static uint32_t s_calls = 0;
		return s_calls;
	}

	uint32_t& s_tick_calls()
	{
		static uint32_t s_calls = 0;
		return s_calls;
	}

	uint32_t& s_count_calls()
	{
		static uint32_t s_calls = 0;
		return s_calls;
	}

	void bars_cb(WTSBarStruct* bar, WtUInt32 count, bool isLast)
	{
		if (bar != NULL && count > 0 && !isLast)
			s_bar_calls() += 1;
		else
			s_bar_calls() += 1;
	}

	void ticks_cb(WTSTickStruct* tick, WtUInt32 count, bool isLast)
	{
		if (tick != NULL && count > 0 && !isLast)
			s_tick_calls() += 1;
		else
			s_tick_calls() += 1;
	}

	void count_cb(WtUInt32 dataCnt)
	{
		s_count_calls() += 1;
		(void)dataCnt;
	}

	void reset_sinks()
	{
		s_bar_calls() = 0;
		s_tick_calls() = 0;
		s_count_calls() = 0;
	}

} // namespace

class L05ServoCapiTest : public ::testing::Test
{
protected:
	void SetUp() override { reset_sinks(); }
};

TEST_F(L05ServoCapiTest, VersionStringIsBuilt)
{
	const char* ver = get_version();
	ASSERT_TRUE(ver != NULL);
	const std::string strVer(ver);
	EXPECT_NE(strVer.find("Build@"), std::string::npos);
	// 第二次调用命中缓存分支
	EXPECT_STREQ(get_version(), ver);
}

TEST_F(L05ServoCapiTest, BarsByRangeNotInitializedReturnsZero)
{
	// 未初始化：runner 直接返回 NULL 切片，C 入口返回 0 且不触发任何回调
	EXPECT_EQ(get_bars_by_range("SHFE.rb2010", "m1", 202001160930, 202001161500, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(s_bar_calls(), 0u);
	EXPECT_EQ(s_count_calls(), 0u);

	reset_sinks();
	EXPECT_EQ(get_bars_by_range("SHFE.rb2010", "d", 20200101, 0, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(s_bar_calls(), 0u);
}

TEST_F(L05ServoCapiTest, BarsByDateRejectsNonMinutePeriod)
{
	reset_sinks();
	EXPECT_EQ(get_bars_by_date("SHFE.rb2010", "m1", 20200116, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(get_bars_by_date("SHFE.rb2010", "d", 0, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(s_bar_calls(), 0u);
	EXPECT_EQ(s_count_calls(), 0u);
}

TEST_F(L05ServoCapiTest, BarsByCountNotInitialized)
{
	reset_sinks();
	EXPECT_EQ(get_bars_by_count("SHFE.rb2010", "m5", 10, 202001161500, bars_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_bars_by_count("SHFE.rb2010", "d", 10, 0, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(s_bar_calls(), 0u);
	EXPECT_EQ(s_count_calls(), 0u);
}

TEST_F(L05ServoCapiTest, SbarsByDateNotInitialized)
{
	reset_sinks();
	EXPECT_EQ(get_sbars_by_date("XHSE.10002270", 60, 20200116, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(get_sbars_by_date("XHSE.10002270", 300, 0, bars_cb, count_cb), (WtUInt32)0);
	EXPECT_EQ(s_bar_calls(), 0u);
	EXPECT_EQ(s_count_calls(), 0u);
}

TEST_F(L05ServoCapiTest, TicksQueriesNotInitialized)
{
	reset_sinks();
	EXPECT_EQ(get_ticks_by_range("SHFE.rb2010", 20200116093000, 20200116150000, ticks_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_ticks_by_range("SHFE.rb2010", 20200116093000, 0, ticks_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_ticks_by_count("SHFE.rb2010", 100, 20200116150000, ticks_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_ticks_by_count("SHFE.rb2010", 100, 0, ticks_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_ticks_by_date("SHFE.rb2010", 20200116, ticks_cb, count_cb), (WtUInt32)0);
	reset_sinks();
	EXPECT_EQ(get_ticks_by_date("SHFE.rb2010", 0, ticks_cb, count_cb), (WtUInt32)0);

	EXPECT_EQ(s_tick_calls(), 0u);
	EXPECT_EQ(s_count_calls(), 0u);
}

TEST_F(L05ServoCapiTest, ClearCacheWithoutStorageReader)
{
	// WtDataManager::clear_cache 在未初始化时只打日志并返回，不应崩溃
	clear_cache();
	EXPECT_EQ(s_count_calls(), 0u);
}
