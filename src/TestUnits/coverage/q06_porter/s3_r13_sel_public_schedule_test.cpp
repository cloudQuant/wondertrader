/**
 * BUG-R13-004 acceptance (with decision K27=A, 2026-11-04): a SEL context
 * created through the public C export create_sel_context with a documented
 * period is driven by the live scheduler (WtSelEngine::on_minute_end) and the
 * host observes the schedule through the registered SEL calc callback.
 *
 * Documented periods (WtPorter.h create_sel_context): d/w/m/y fire at the HHMM
 * time; w takes the weekday 0..6, m the day of month 1..31, y MMDD; min takes
 * the interval in minutes. Aliases such as d1/w1/m1 and unschedulable values
 * are rejected with a zero handle and register no task.
 *
 * 2024-06-10 is a Monday; the fixture runner (i03s5_fresh_configured_porter)
 * uses the all-day session ALLDAY (0-0) and no holiday file, so only weekends
 * are holidays.
 */
#include <gtest/gtest.h>

#include <map>
#include <mutex>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"
#include "WtCore/WtSelEngine.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();
bool i03s5_fresh_configured_porter(const char* engine);

namespace
{
std::mutex g_calc_mutex;
std::map<CtxHandler, uint32_t> g_calc_count;
std::map<CtxHandler, uint32_t> g_calc_time;

void s3_sel_calc(CtxHandler cHandle, WtUInt32 /*curDate*/, WtUInt32 curTime)
{
	std::lock_guard<std::mutex> lock(g_calc_mutex);
	++g_calc_count[cHandle];
	g_calc_time[cHandle] = curTime;
}

uint32_t calcCount(CtxHandler h)
{
	std::lock_guard<std::mutex> lock(g_calc_mutex);
	auto it = g_calc_count.find(h);
	return it == g_calc_count.end() ? 0u : it->second;
}

uint32_t calcTime(CtxHandler h)
{
	std::lock_guard<std::mutex> lock(g_calc_mutex);
	auto it = g_calc_time.find(h);
	return it == g_calc_time.end() ? 0u : it->second;
}

class S3R13SelPublicSchedule : public ::testing::Test
{
protected:
	void SetUp() override
	{
		{
			std::lock_guard<std::mutex> lock(g_calc_mutex);
			g_calc_count.clear();
			g_calc_time.clear();
		}
		ASSERT_TRUE(i03s5_fresh_configured_porter("sel"));
		register_sel_callbacks(NULL, NULL, s3_sel_calc, NULL, NULL);
		_engine = dynamic_cast<WtSelEngine*>(getRunner().getEngine());
		ASSERT_NE(_engine, nullptr);
	}

	void TearDown() override
	{
		register_sel_callbacks(NULL, NULL, NULL, NULL, NULL);
	}

	void setNow(uint32_t date, uint32_t time)
	{
		_engine->set_date_time(date, time, 0);
		_engine->set_trading_date(date);
	}

	WtSelEngine* _engine = NULL;
};
}

TEST_F(S3R13SelPublicSchedule, DocumentedPeriodsCreatedThroughTheCApiFireOnTheirSchedule)
{
	const CtxHandler daily = create_sel_context("s3_pub_d", 0, 930, "d", "CHINA", "ALLDAY", 0);
	const CtxHandler weekly = create_sel_context("s3_pub_w", 1, 930, "w", "CHINA", "ALLDAY", 0);
	const CtxHandler weeklyOther = create_sel_context("s3_pub_w_other", 3, 930, "w", "CHINA", "ALLDAY", 0);
	const CtxHandler monthly = create_sel_context("s3_pub_m", 10, 930, "m", "CHINA", "ALLDAY", 0);
	const CtxHandler monthlyOther = create_sel_context("s3_pub_m_other", 11, 930, "m", "CHINA", "ALLDAY", 0);
	const CtxHandler yearly = create_sel_context("s3_pub_y", 610, 930, "y", "CHINA", "ALLDAY", 0);
	const CtxHandler minute = create_sel_context("s3_pub_min", 0, 15, "min", "CHINA", "ALLDAY", 0);
	for (CtxHandler h : { daily, weekly, weeklyOther, monthly, monthlyOther, yearly, minute })
		ASSERT_NE(0U, h);

	setNow(20240610, 929);
	_engine->on_minute_end(20240610, 929);	// next minute 09:30: d/w/m/y fire
	_engine->on_minute_end(20240610, 929);	// same minute again: nothing repeats
	setNow(20240610, 930);
	_engine->on_minute_end(20240610, 930);	// session minute 570 = 38 * 15: min fires
	_engine->on_minute_end(20240610, 931);	// minute 571: nothing

	EXPECT_EQ(1u, calcCount(daily));
	EXPECT_EQ(1u, calcCount(weekly));
	EXPECT_EQ(0u, calcCount(weeklyOther));
	EXPECT_EQ(1u, calcCount(monthly));
	EXPECT_EQ(0u, calcCount(monthlyOther));
	EXPECT_EQ(1u, calcCount(yearly));
	EXPECT_EQ(1u, calcCount(minute));
	EXPECT_EQ(930u, calcTime(daily)) << "the strategy sees the scheduled HHMM time";
	EXPECT_EQ(931u, calcTime(minute));
}

TEST_F(S3R13SelPublicSchedule, AliasesAndUnschedulableInputsAreRejectedAndNeverFire)
{
	const CtxHandler valid = create_sel_context("s3_pub_valid", 0, 930, "d", "CHINA", "ALLDAY", 0);
	ASSERT_NE(0U, valid);
	EXPECT_EQ(0U, create_sel_context("s3_pub_d1", 0, 930, "d1", "CHINA", "ALLDAY", 0));
	EXPECT_EQ(0U, create_sel_context("s3_pub_w1", 1, 930, "w1", "CHINA", "ALLDAY", 0));
	EXPECT_EQ(0U, create_sel_context("s3_pub_m1", 10, 930, "m1", "CHINA", "ALLDAY", 0));
	EXPECT_EQ(0U, create_sel_context("s3_pub_hhmmss", 0, 93000, "d", "CHINA", "ALLDAY", 0));
	EXPECT_EQ(0U, create_sel_context("s3_pub_bogus", 0, 930, "not-a-period", "CHINA", "ALLDAY", 0));

	setNow(20240610, 929);
	_engine->on_minute_end(20240610, 929);

	// Only the valid context was registered and scheduled.
	EXPECT_EQ(1u, calcCount(valid));
	std::lock_guard<std::mutex> lock(g_calc_mutex);
	EXPECT_EQ(1u, g_calc_count.size());
}
