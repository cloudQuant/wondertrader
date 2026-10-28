/**
 * r80_replayer_task_calendar_test.cpp  (lane R80)
 *
 * 靶子：src/WtBtCore/HisDataReplayer.cpp 的 run_by_tasks 日频及以上分支
 *   - L954-L975   endtime 对齐、bIsPreDay、isHoliday/preTDate 回溯与 days 累计
 *   - L978-L1033  TPT_Daily / TPT_Monthly / TPT_Weekly 的节假日顺延判定
 *   - L1042-L1058 未触发日的时间补齐与 newTDate 换日（含节假日自然日）
 *   - L1060-L1101 触发日的 session begin/end、simTicks/onMinuteEnd 与收尾
 *
 * 数据只用 dist/common/{sessions,stk_comms,holidays}.json（只读）；
 * 自定义节假日文件写在系统临时目录。断言均为固定日历下的确定序列。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "r80_common.h"

namespace
{
	const uint32_t kEndMin = TimeUtils::getNextMinute(1500, -1);

	void check_task_tail(const r80::Sink& sink, const std::vector<uint32_t>& dates)
	{
		EXPECT_EQ(dates, sink.scheduleDates);
		ASSERT_EQ(dates.size(), sink.scheduleTimes.size());
		for (uint32_t t : sink.scheduleTimes)
			EXPECT_EQ(kEndMin, t);
		EXPECT_EQ(1u, sink.doneCalls);
		// 每次 session_begin 之后都会配一次 session_end，收尾再补一次 end
		EXPECT_EQ(sink.beginCalls + 1u, sink.endCalls);
	}
}

class R80TaskCalendar : public r80::Fixture
{
protected:
	bool run_task(uint32_t day, uint32_t time, const char* period,
		uint64_t stime, uint64_t etime, r80::Sink& sink,
		const r80::CfgOpts& opts, const char* session = "SD0930")
	{
		HisDataReplayer rp;
		WTSVariant* cfg = r80::make_cfg(_root, _base, stime, etime, opts);
		EXPECT_TRUE(rp.init(cfg));
		cfg->release();
		rp.register_sink(&sink, "r80_task");
		rp.register_task(1, day, time, period, "CHINA", session);
		if (!rp.prepare())
			return false;
		rp.run(false);
		return true;
	}
};

/*
 * 日频任务跨越 2020 春节（1/24、1/27-1/31 休市）：
 * 触发日只有 1/20-1/23 与 2/3-2/4；节后 2/3 的 preTDate 回溯会连续命中
 * isHoliday（L970-L975），days 累计 >1。
 */
TEST_F(R80TaskCalendar, DailyTaskCrossesSpringFestivalHoliday)
{
	r80::CfgOpts opts;
	r80::Sink sink;
	ASSERT_TRUE(run_task(0, 1500, "d", 202001200930ull, 202002041500ull, sink, opts));

	const std::vector<uint32_t> want = {
		20200120u, 20200121u, 20200122u, 20200123u, 20200203u, 20200204u
	};
	check_task_tail(sink, want);
	EXPECT_GE(sink.beginCalls, 6u);
	EXPECT_GE(sink.endCalls, 7u);
}

/*
 * 月频任务 _day=200124（即 1/24）恰好落在春节假期：
 * 2/3 复盘时 preTDate=1/23、跨月且 bHasHoliday=true，
 * 命中 L996-L999 的「跨月顺延」分支。
 */
TEST_F(R80TaskCalendar, MonthlyTaskHolidayShiftFiresOnResume)
{
	r80::CfgOpts opts;
	r80::Sink sink;
	ASSERT_TRUE(run_task(200124, 1500, "m", 202001200930ull, 202002051500ull, sink, opts));

	check_task_tail(sink, std::vector<uint32_t>({ 20200203u }));
}

/*
 * 周频任务目标周一，跨 2020 国庆（10/1-10/8 休市）：
 * 9/28 正常触发；10/9 复盘时 days>=7 且 weekDay(周五) > _day(周一)，
 * 命中 L1014-L1016 的长假顺延分支；10/12 恢复常规触发。
 */
TEST_F(R80TaskCalendar, WeeklyTaskCatchUpAfterLongHoliday)
{
	r80::CfgOpts opts;
	r80::Sink sink;
	ASSERT_TRUE(run_task(1, 1500, "w", 202009280930ull, 202010121500ull, sink, opts));

	check_task_tail(sink, std::vector<uint32_t>({ 20200928u, 20201009u, 20201012u }));
}

/*
 * 周频任务目标周二，跨 1/27-1/28 两天休市：
 * 1/21 正常触发；1/29(周三) 复盘时 preWD(周四) > weekDay(周三) 且 weekDay > _day(周二)，
 * 命中 L1018-L1022 的「上一交易日在周内更靠后」分支。
 */
TEST_F(R80TaskCalendar, WeeklyTaskCatchUpAfterHolidayGap)
{
	r80::CfgOpts opts;
	r80::Sink sink;
	ASSERT_TRUE(run_task(2, 1500, "w", 202001200930ull, 202001311500ull, sink, opts));

	check_task_tail(sink, std::vector<uint32_t>({ 20200121u, 20200129u }));
}

/*
 * 周频任务目标周四，节假日文件只把 1/8(周三)、1/9(周四) 标为休市：
 * 1/10(周五) 复盘时 preWD(周二) < _day(周四) 且 weekDay(周五) > _day，
 * 命中 L1023-L1026 的「假期夹在周中」分支。
 */
TEST_F(R80TaskCalendar, WeeklyTaskHolidayInsideWeek)
{
	const std::filesystem::path custom = _base / "holidays_custom.json";
	ASSERT_TRUE(r80::write_text(custom, "{\n  \"CHINA\": [20200108, 20200109]\n}\n"));

	r80::CfgOpts opts;
	opts.holiday = custom.string();
	r80::Sink sink;
	ASSERT_TRUE(run_task(4, 1500, "w", 202001060930ull, 202001101500ull, sink, opts));

	check_task_tail(sink, std::vector<uint32_t>({ 20200110u }));
}
