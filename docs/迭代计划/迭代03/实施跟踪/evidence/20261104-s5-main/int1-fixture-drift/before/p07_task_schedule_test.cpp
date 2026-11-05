/**
 * p07_task_schedule_test.cpp
 * 覆盖 HisDataReplayer::run_by_tasks(L940-L1241) —— p04 只做了 register_task 的解析分支，
 * 调度回放主体全部未覆盖（他们把整个函数放进 DISABLED_，理由是默认会话模板缺失；
 * 这里显式传 session="SD0930"（dist/common/sessions.json 存在）绕开 BUG-P04-007）。
 *   日频及以上分支 L949-L1103：TPT_Daily / TPT_Weekly / TPT_Monthly / TPT_Yearly / TPT_None，
 *     endtime = getNextMinute(task_time, -1) 的时刻对齐、isHoliday/preTDate 回溯、
 *     fired 分支里的 simTicks(4x)+onMinuteEnd+session begin/end、
 *     未 fired 时的时间补齐与 newTDate != _cur_tdate 换日分支、nextTime > _end_time 收尾
 *   分钟分支 L1104-L1241：timeToMinutes/minuteToTime 步进、小节跳跃（1130 -> 1305）、
 *     mins >= getTradingMins() 的闭盘与 getOffsetMins()==0 的整体换日
 * 断言全部来自固定的 stime/etime 与固定调度参数，不使用随机数、不断言墙钟。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/TimeUtils.hpp>

namespace
{
	std::filesystem::path lane_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("p07_task_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	WTSVariant* task_cfg_text(const std::filesystem::path& root, const std::filesystem::path& base,
		const char* stime, const char* etime)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	WTSVariant* task_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime)
	{
		const std::string beginText = std::to_string(stime);
		const std::string endText = std::to_string(etime);
		return task_cfg_text(root, base, beginText.c_str(), endText.c_str());
	}

	/*
	 * 任务调度回放里没有订阅K线，_bars_cache 为空：
	 * simTicks/onMinuteEnd 只会走到 handle_schedule，正好把调度时刻序列单独暴露出来。
	 */
	class TaskSink : public IDataSink
	{
	public:
		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		uint32_t sectionCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t replayDoneCalls {0};
		uint32_t tickCalls {0};
		uint32_t barCloseCalls {0};
		std::vector<uint32_t> scheduleDates;
		std::vector<uint32_t> scheduleTimes;
		std::vector<uint32_t> beginTDates;
		std::vector<uint32_t> endTDates;

		void handle_init() override { initCalls++; }
		void handle_tick(const char*, WTSTickData*, uint32_t) override { tickCalls++; }
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override { barCloseCalls++; }
		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			scheduleCalls++;
			scheduleDates.push_back(uDate);
			scheduleTimes.push_back(uTime);
		}
		void handle_session_begin(uint32_t curTDate) override
		{
			beginCalls++;
			beginTDates.push_back(curTDate);
		}
		void handle_session_end(uint32_t curTDate) override
		{
			endCalls++;
			endTDates.push_back(curTDate);
		}
		void handle_section_end(uint32_t, uint32_t) override { sectionCalls++; }
		void handle_replay_done() override { replayDoneCalls++; }
	};
}

class P07TaskScheduleTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/sessions.json"));
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/stk_comms.json"));
		_base = make_temp_dir("run");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;

	bool run_once(uint32_t day, uint32_t time, const char* period,
		uint64_t stime, uint64_t etime, TaskSink& sink, const char* session = "SD0930")
	{
		HisDataReplayer replayer;
		WTSVariant* cfg = task_cfg(_root, _base, stime, etime);
		if (!replayer.init(cfg))
		{
			cfg->release();
			return false;
		}
		cfg->release();
		replayer.register_sink(&sink, "p07_task");
		replayer.register_task(1, day, time, period, "CHINA", session);
		if (!replayer.prepare())
			return false;
		replayer.run(false);
		return true;
	}

	bool run_once_text(uint32_t time, const char* stime, const char* etime, TaskSink& sink)
	{
		HisDataReplayer replayer;
		WTSVariant* cfg = task_cfg_text(_root, _base, stime, etime);
		if (!replayer.init(cfg))
		{
			cfg->release();
			return false;
		}
		cfg->release();
		replayer.register_sink(&sink, "p07_task_range");
		replayer.register_task(1, 0, time, "min", "CHINA", "SD0930");
		if (!replayer.prepare())
			return false;
		replayer.run(false);
		return true;
	}
};

TEST_F(P07TaskScheduleTest, taskParametersAreBoundedAndInvalidReregistrationPreservesTask)
{
	const std::string maxField(15, 'x');
	const std::string oversizedField(16, 'x');
	EXPECT_TRUE(HisDataReplayer::validate_task_parameters("legacy-period", maxField.c_str(), maxField.c_str()));
	EXPECT_FALSE(HisDataReplayer::validate_task_parameters(nullptr, "CHINA", "SD0930"));
	EXPECT_FALSE(HisDataReplayer::validate_task_parameters("d", nullptr, "SD0930"));
	EXPECT_FALSE(HisDataReplayer::validate_task_parameters("d", "CHINA", nullptr));
	EXPECT_FALSE(HisDataReplayer::validate_task_parameters("d", oversizedField.c_str(), "SD0930"));
	EXPECT_FALSE(HisDataReplayer::validate_task_parameters("d", "CHINA", oversizedField.c_str()));
	EXPECT_TRUE(HisDataReplayer::validate_task_parameters("", "CHINA", "SD0930"));

	HisDataReplayer replayer;
	WTSVariant* cfg = task_cfg(_root, _base, 202001020930ull, 202001021500ull);
	ASSERT_NE(cfg, nullptr);
	if (!replayer.init(cfg))
	{
		cfg->release();
		FAIL() << "replayer initialization failed";
		return;
	}
	cfg->release();

	TaskSink sink;
	replayer.register_sink(&sink, "p07_task_invalid_registration");
	replayer.register_task(1, 0, 1500, "d", "CHINA", "SD0930");
	replayer.register_task(2, 0, 900, nullptr, "CHINA", "SD0930");
	replayer.register_task(2, 0, 900, "d", nullptr, "SD0930");
	replayer.register_task(2, 0, 900, "d", "CHINA", nullptr);
	replayer.register_task(2, 0, 900, "d", oversizedField.c_str(), "SD0930");
	replayer.register_task(2, 0, 900, "d", "CHINA", oversizedField.c_str());

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({TimeUtils::getNextMinute(1500, -1)}), sink.scheduleTimes);
}

/*
 * TPT_Daily：每个非节假日的交易日在 endtime = getNextMinute(1500, -1) 触发一次。
 * 20200102/20200103 两个交易日都在区间内 -> 2 次调度；
 * 收尾（nextTime > _end_time）额外补发 1 次 handle_session_end + 1 次 handle_replay_done。
 */
TEST_F(P07TaskScheduleTest, dailyTaskFiresOncePerTradingDay)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 1500, "d", 202001020930ull, 202001031500ull, sink));

	EXPECT_EQ(1u, sink.initCalls);
	ASSERT_EQ(2u, sink.scheduleCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.scheduleDates);
	// 触发时刻是任务时间的前一分钟
	const uint32_t endMinute = TimeUtils::getNextMinute(1500, -1);
	EXPECT_EQ(std::vector<uint32_t>({endMinute, endMinute}), sink.scheduleTimes);
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(3u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 没有订阅K线 -> 不应有闭盘/模拟tick
	EXPECT_EQ(0u, sink.barCloseCalls);
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(0u, sink.sectionCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
}

/*
 * TPT_Weekly：只在 weekDay == _task->_day 的那天触发。
 * 星期约定采用周日=0、周六=6；20200102（周四）任务应只触发一次。
 * 区间取到 20200106（含周末），只应触发 1 次，其余日期走「未触发 -> 换交易日」分支。
 */
TEST_F(P07TaskScheduleTest, weeklyTaskFiresOnlyOnTargetWeekday)
{
	const uint32_t wday = 4;
	ASSERT_LT(wday, 7u);
	EXPECT_EQ(4u, TimeUtils::getWeekDay(20200102));
	EXPECT_EQ(0u, TimeUtils::getWeekDay(20200105));
	EXPECT_EQ(6u, TimeUtils::getWeekDay(20200104));

	TaskSink sink;
	ASSERT_TRUE(run_once(wday, 1500, "w", 202001020930ull, 202001061500ull, sink));

	ASSERT_EQ(1u, sink.scheduleCalls);
	EXPECT_EQ(20200102u, sink.scheduleDates[0]);
	EXPECT_EQ(1u, sink.beginCalls);
	// 触发的 1 次闭盘 + 收尾 1 次
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

/*
 * TPT_Monthly 的公开 register_task 契约规定 day 为 1..31。
 * 在 20200102-20200303 的 SD0930 回放窗口中，每月 2 日任务应触发
 * 20200102、20200203（2 日休市后顺延）和 20200302。中间交易日不触发，
 * 并且每个目标月份仅触发一次。
 */
TEST_F(P07TaskScheduleTest, monthlyTaskUsesDayOfMonthContract)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(2, 1500, "m", 202001020930ull, 202003031500ull, sink));

	ASSERT_EQ(3u, sink.scheduleCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200203u, 20200302u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({TimeUtils::getNextMinute(1500, -1),
		TimeUtils::getNextMinute(1500, -1), TimeUtils::getNextMinute(1500, -1)}), sink.scheduleTimes);
	EXPECT_EQ(3u, sink.beginCalls);
	EXPECT_EQ(4u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, dailyTaskDoesNotDispatchAfterRangeEnd)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 1500, "d", 202001020930ull, 202001021200ull, sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, dailyTaskSkipsCloseBeforeRangeStartAndFiresNextDate)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 1500, "d", 20200102150030ull, 202001031500ull, sink));

	EXPECT_EQ(std::vector<uint32_t>({20200103u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({TimeUtils::getNextMinute(1500, -1)}), sink.scheduleTimes);
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, nightSessionDailyTaskDoesNotDispatchPastNaturalDateEnd)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 230, "d", 202001022100ull, 202001030228ull, sink, "FN0230"));

	// The 02:30 daily task fires at 02:29 on the natural date after the night
	// session starts. This range ends one minute before that event.
	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteStepAlignedBeyondRangeEndDoesNotDispatch)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001020932ull, 202001020932ull, sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskCeilsOffGridStartAndAdvancesByStep)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001020937ull, 202001020952ull, sink));

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({940u, 945u, 950u}), sink.scheduleTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskStartingOnBoundaryDispatchesThatBoundary)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001020935ull, 202001020945ull, sink));

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({935u, 940u, 945u}), sink.scheduleTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskSkipsLunchGapAndResumesOnSessionLinearGrid)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001021127ull, 202001021311ull, sink));

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({1130u, 1305u, 1310u}), sink.scheduleTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskStartingInsideLunchMovesToNextSection)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001021200ull, 202001021310ull, sink));

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({1300u, 1305u, 1310u}), sink.scheduleTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskStepGridContinuesAcrossOffsetMidnight)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001022200ull, 202001030010ull, sink, "FN0230"));

	ASSERT_EQ(27u, sink.scheduleDates.size());
	ASSERT_EQ(27u, sink.scheduleTimes.size());
	for (uint32_t i = 0; i < 27u; ++i)
	{
		const uint32_t elapsed = i * 5u;
		const uint32_t totalMinutes = 22u * 60u + elapsed;
		const uint32_t expectedDate = totalMinutes >= 24u * 60u ? 20200103u : 20200102u;
		const uint32_t minuteOfDay = totalMinutes % (24u * 60u);
		const uint32_t expectedTime = minuteOfDay / 60u * 100u + minuteOfDay % 60u;
		EXPECT_EQ(expectedDate, sink.scheduleDates[i]) << "index " << i;
		EXPECT_EQ(expectedTime, sink.scheduleTimes[i]) << "index " << i;
	}
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskOffGridStartBeforeEndDoesNotRepeatEarlierTime)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001020937ull, 202001020939ull, sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskNearCloseDoesNotDispatchAfterRangeEnd)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 5, "min", 202001021458ull, 202001021459ull, sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, minuteTaskWithZeroStepFailsClosed)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 0, "min", 202001020930ull, 202001020932ull, sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(0u, sink.tickCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

/*
 * TPT_Yearly 的公开 TaskInfo 契约要求 day 为 MMDD（0101..1231）。
 * 连续三个年份的 0101 均为节假日/周末，任务应各自顺延到首个交易日，且每年仅触发一次。
 * 2022-01-03 也列在 CHINA 假日表中，因此该年的首个开市日为 2022-01-04。
 */
TEST_F(P07TaskScheduleTest, yearlyJanFirstTaskFiresOncePerYearAfterHoliday)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(101, 1500, "y", 201912300930ull, 202201051500ull, sink));

	ASSERT_EQ(3u, sink.scheduleCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20210104u, 20220104u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({TimeUtils::getNextMinute(1500, -1),
		TimeUtils::getNextMinute(1500, -1), TimeUtils::getNextMinute(1500, -1)}), sink.scheduleTimes);
	EXPECT_EQ(3u, sink.beginCalls);
	EXPECT_EQ(4u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, yearlyTaskDoesNotFireBeforeNewYearTarget)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(101, 1500, "y", 202012290930ull, 202012311500ull, sink));

	EXPECT_EQ(0u, sink.scheduleCalls);
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, yearlyTaskFiresAtLastCalendarDay)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(1231, 1500, "y", 202012300930ull, 202101041500ull, sink));

	ASSERT_EQ(1u, sink.scheduleCalls);
	EXPECT_EQ(std::vector<uint32_t>({20201231u}), sink.scheduleDates);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, yearlyLeapDayFiresWhenTheCalendarDateExists)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(229, 1500, "y", 202002280930ull, 202003031500ull, sink));

	ASSERT_EQ(1u, sink.scheduleCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200302u}), sink.scheduleDates);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

/*
 * 未知周期 -> TPT_None：switch 里不置 fired，整个区间只推进日期，
 * 覆盖「时间补齐 + 换交易日判定 + 收尾」而不触发任何调度。
 */
TEST_F(P07TaskScheduleTest, unknownPeriodNeverFiresButTerminates)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(0, 1500, "unknown", 202001020930ull, 202001031500ull, sink));

	EXPECT_EQ(0u, sink.scheduleCalls);
	EXPECT_EQ(0u, sink.beginCalls);
	// 只有收尾那一次
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 日期按自然日推进到区间外
	EXPECT_EQ(20200104u, TimeUtils::getNextDate(20200103));
}

/*
 * TPT_Minute：SD0930 会话 240 分钟，任务步长 5 分钟。
 * 0930(mins=0) 起步步进到 1455(mins=235) 共 48 次，
 * 随后 mins=240 落入闭盘分支再调度 1 次（1500）-> 合计 49 次。
 * 小节边界上 1130 -> 1305 的跳跃由 minuteToTime 完成。
 */
TEST_F(P07TaskScheduleTest, minuteTaskStepsThroughBothSections)
{
	TaskSink sink;
	ASSERT_TRUE(run_once(5, 5, "min", 202001020930ull, 202001030930ull, sink));

	ASSERT_EQ(49u, sink.scheduleCalls);
	for (uint32_t i = 0; i < sink.scheduleCalls; i++)
		EXPECT_EQ(20200102u, sink.scheduleDates[i]) << "index " << i;

	EXPECT_EQ(930u, sink.scheduleTimes[0]);
	EXPECT_EQ(1130u, sink.scheduleTimes[24]);
	EXPECT_EQ(1305u, sink.scheduleTimes[25]);
	EXPECT_EQ(1455u, sink.scheduleTimes[47]);
	EXPECT_EQ(1500u, sink.scheduleTimes[48]);
	// 中间步长必须是 5 分钟（同一小节内）
	for (uint32_t i = 1; i <= 24; i++)
	{
		const uint32_t prevH = sink.scheduleTimes[i - 1] / 100, prevM = sink.scheduleTimes[i - 1] % 100;
		const uint32_t curH = sink.scheduleTimes[i] / 100, curM = sink.scheduleTimes[i] % 100;
		EXPECT_EQ(prevH * 60 + prevM + 5, curH * 60 + curM) << "index " << i;
	}

	// 入口 1 次 session_begin + 换日 1 次；闭盘 1 次 session_end + 收尾 1 次
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.beginTDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), sink.endTDates);
}

TEST_F(P07TaskScheduleTest, secondPrecisionRangeRoundsTaskStartAndIncludesEndMinute)
{
	TaskSink sink;
	ASSERT_TRUE(run_once_text(1, "20200102093030", "202001020932", sink));

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u}), sink.scheduleDates);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u}), sink.scheduleTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TaskScheduleTest, secondOnlyRangeDoesNotDispatchMinuteTasks)
{
	TaskSink sink;
	ASSERT_TRUE(run_once_text(1, "20200102093201", "20200102093259", sink));

	EXPECT_TRUE(sink.scheduleDates.empty());
	EXPECT_TRUE(sink.scheduleTimes.empty());
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(0u, sink.beginCalls);
}

TEST_F(P07TaskScheduleTest, invalidConfigAndExplicitRangesFailClosed)
{
	WTSVariant* invalidConfig = task_cfg_text(_root, _base, "202002300930", "202002301000");
	HisDataReplayer fromConfig;
	EXPECT_FALSE(fromConfig.init(invalidConfig));
	EXPECT_FALSE(fromConfig.prepare());
	invalidConfig->release();

	WTSVariant* validConfig = task_cfg(_root, _base, 202001020930ull, 202001021000ull);
	HisDataReplayer fromSetter;
	fromSetter.set_time_range(202001020932ull, 202001020931ull);
	EXPECT_FALSE(fromSetter.init(validConfig));
	EXPECT_FALSE(fromSetter.prepare());
	validConfig->release();
}

/*
 * 崩溃/挂死一类输入统一收进这一个禁用用例，启用用例里零残留：
 * 1) session 传不存在的模板（或依赖默认 "TRADING" 时底座里没有该模板）：
 *    run_by_tasks 里 sInfo = _bd_mgr.getSession(DEF_SESS) 返回 NULL 后，
 *    fired 分支直接 sInfo->offsetTime(...) 解引用（BUG-P04-007）。
 * 2) TPT_Minute 配 offset!=0 的会话（"TRADING" offset=300）时，
 *    换日逻辑 L1173-L1194 不会推进 _cur_date，nextTime 永不大于 _end_time -> 死循环，
 *    这正是 p04 把整个 run_by_tasks 放进 DISABLED_ 的原因，这里不重复开启。
 * 只保留第 1 条（确定性崩溃），第 2 条以注释形式归档，避免启用后整轮空转。
 */
TEST_F(P07TaskScheduleTest, DISABLED_unknownSessionTemplateCrashes)
{
	TaskSink sink;
	// sInfo == NULL -> fired 分支解引用崩溃
	run_once(0, 1500, "d", 202001020930ull, 202001031500ull, sink, "NO_SUCH_SESSION");
}
