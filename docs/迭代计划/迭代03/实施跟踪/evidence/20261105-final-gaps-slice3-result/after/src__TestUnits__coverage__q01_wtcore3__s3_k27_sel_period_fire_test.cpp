/**
 * BUG-R13-004 / decision K27=A (2026-11-04): positive firing controls of the
 * live WtSelEngine for the w/m/y/min schedule periods.
 *
 * Contract (src/Includes/SelScheduleRules.hpp, WtSelEngine::on_minute_end):
 *  - d/w/m/y fire at the HHMM time, i.e. when the next minute equals _time;
 *  - w: _day is the weekday 0..6; m: _day is the day of month 1..31 (a target
 *    day inside a holiday run is carried forward to the next trading day);
 *    y: _day is MMDD;
 *  - min: _time is the interval in minutes; the task fires when the session
 *    minute of curTime is a non-zero multiple of the interval; a task whose
 *    session is unknown never fires.
 * Every fired task runs exactly once per minute. Counts are read after
 * release() so that the assertions do not depend on the dispatch threading
 * model (K25 runs schedules synchronously; older code dispatched threads that
 * release() joined).
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>

#include "q01_sel_env.h"

#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>

using namespace wtp;
namespace stdfs = std::filesystem;

namespace
{
class S3K27SelEngine final : public WtSelEngine
{
public:
	void attach(IBaseDataMgr* bd, IHotMgr* hot, uint32_t curDate)
	{
		_base_data_mgr = bd;
		_hot_mgr = hot;
		set_date_time(curDate, 930, 0);
		set_trading_date(curDate);
	}
};

// Base data whose holiday calendar is an explicit date set.
class HolidayBaseDataMgr final : public q01sel::BaseDataMgr
{
public:
	std::set<uint32_t> holidays;
	bool isHoliday(const char* /*pid*/, uint32_t uDate, bool /*isTpl*/ = false) override
	{
		return holidays.count(uDate) != 0;
	}
};

// Base data without any session: get_session_info() returns NULL.
class NoSessionBaseDataMgr final : public q01sel::BaseDataMgr
{
public:
	WTSSessionInfo* getSession(const char* /*sid*/) override { return NULL; }
};

std::string tempDir(const char* tag)
{
	const stdfs::path dir = stdfs::temp_directory_path()
		/ ("s3_k27_sel_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(dir, ec);
	stdfs::create_directories(dir, ec);
	return dir.string() + "/";
}

using CtxPtr = std::shared_ptr<q01sel::SelCtx>;

CtxPtr makeCtx(WtSelEngine* engine, const char* name)
{
	return CtxPtr(new q01sel::SelCtx(engine, name));
}
}

// 2025-01-10 is a Friday (weekday 5).
TEST(S3K27SelPeriodFire, WeeklyTaskFiresOnTargetWeekdayOnceAtItsMinute)
{
	WtHelper::setGenerateDir(tempDir("weekly").c_str());
	q01sel::BaseDataMgr bd;
	bd.regisSession("TRADING");
	q01sel::HotMgr hot;
	std::unique_ptr<S3K27SelEngine> engine(new S3K27SelEngine());
	engine->attach(&bd, &hot, 20250110);

	CtxPtr hit = makeCtx(engine.get(), "s3_week_hit");
	CtxPtr miss = makeCtx(engine.get(), "s3_week_miss");
	engine->addContext(hit, 5, 931, TPT_Weekly);
	engine->addContext(miss, 3, 931, TPT_Weekly);

	engine->on_minute_end(20250110, 929);	// next minute 930: not the task time
	engine->on_minute_end(20250110, 930);	// next minute 931: fires
	engine->on_minute_end(20250110, 930);	// same minute: no repeat
	engine->release();

	EXPECT_EQ(1u, hit->schedule_fires);
	EXPECT_EQ(0u, miss->schedule_fires);
	EXPECT_EQ(20250110u, hit->_sched_date);
	EXPECT_EQ(931u, hit->_sched_time);
}

TEST(S3K27SelPeriodFire, MonthlyTaskUsesTheDayOfMonth)
{
	WtHelper::setGenerateDir(tempDir("monthly").c_str());
	q01sel::BaseDataMgr bd;
	bd.regisSession("TRADING");
	q01sel::HotMgr hot;
	std::unique_ptr<S3K27SelEngine> engine(new S3K27SelEngine());
	engine->attach(&bd, &hot, 20250110);

	CtxPtr hit = makeCtx(engine.get(), "s3_month_hit");
	CtxPtr miss = makeCtx(engine.get(), "s3_month_miss");
	engine->addContext(hit, 10, 931, TPT_Monthly);
	engine->addContext(miss, 11, 931, TPT_Monthly);

	engine->on_minute_end(20250110, 930);
	engine->on_minute_end(20250110, 930);
	engine->release();

	EXPECT_EQ(1u, hit->schedule_fires);
	EXPECT_EQ(0u, miss->schedule_fires);
}

// The 10th falls into a holiday run (Fri 10th .. Sun 12th): the monthly task is
// carried forward to Monday the 13th, the first trading day after it.
TEST(S3K27SelPeriodFire, MonthlyTargetDayInsideHolidaysIsCarriedForward)
{
	WtHelper::setGenerateDir(tempDir("monthly_holiday").c_str());
	HolidayBaseDataMgr bd;
	bd.regisSession("TRADING");
	bd.holidays = { 20250110, 20250111, 20250112 };
	q01sel::HotMgr hot;
	std::unique_ptr<S3K27SelEngine> engine(new S3K27SelEngine());
	engine->attach(&bd, &hot, 20250113);

	CtxPtr carried = makeCtx(engine.get(), "s3_month_carried");
	CtxPtr later = makeCtx(engine.get(), "s3_month_later");
	engine->addContext(carried, 10, 931, TPT_Monthly);
	engine->addContext(later, 20, 931, TPT_Monthly);

	engine->on_minute_end(20250113, 930);
	engine->release();

	EXPECT_EQ(1u, carried->schedule_fires);
	EXPECT_EQ(0u, later->schedule_fires);
}

// y: _day is MMDD; it fires on the first trading day reaching the target date.
TEST(S3K27SelPeriodFire, YearlyTaskUsesMonthAndDay)
{
	WtHelper::setGenerateDir(tempDir("yearly").c_str());
	q01sel::BaseDataMgr bd;
	bd.regisSession("TRADING");
	q01sel::HotMgr hot;
	std::unique_ptr<S3K27SelEngine> engine(new S3K27SelEngine());
	engine->attach(&bd, &hot, 20250110);

	CtxPtr hit = makeCtx(engine.get(), "s3_year_hit");
	CtxPtr miss = makeCtx(engine.get(), "s3_year_miss");
	engine->addContext(hit, 110, 931, TPT_Yearly);
	engine->addContext(miss, 111, 931, TPT_Yearly);

	engine->on_minute_end(20250110, 930);
	engine->on_minute_end(20250110, 930);
	engine->release();

	EXPECT_EQ(1u, hit->schedule_fires);
	EXPECT_EQ(0u, miss->schedule_fires);
}

// min: _time is the interval; session 09:00-15:00, interval 15 fires at the
// session minutes 15 and 30 (09:15, 09:30) and not at 09:14 or 09:20.
TEST(S3K27SelPeriodFire, MinuteTaskFiresOnSessionMinuteMultiplesOfItsInterval)
{
	WtHelper::setGenerateDir(tempDir("minute").c_str());
	q01sel::BaseDataMgr bd;
	bd.regisSessionWithTime("TRADING", 900, 1500);
	q01sel::HotMgr hot;
	std::unique_ptr<S3K27SelEngine> engine(new S3K27SelEngine());
	engine->attach(&bd, &hot, 20250110);

	CtxPtr every15 = makeCtx(engine.get(), "s3_min_15");
	CtxPtr every7 = makeCtx(engine.get(), "s3_min_7");
	engine->addContext(every15, 0, 15, TPT_Minute);
	engine->addContext(every7, 0, 7, TPT_Minute);

	const uint32_t times[] = { 914, 915, 915, 920, 930 };
	for (uint32_t t : times)
		engine->on_minute_end(20250110, t);
	engine->release();

	EXPECT_EQ(2u, every15->schedule_fires) << "09:15 and 09:30, the repeated 09:15 must not fire again";
	EXPECT_EQ(1u, every7->schedule_fires) << "only 09:14 (minute 14) is a multiple of 7";
}

namespace
{
// Exit code 0 when a minute task whose session cannot be resolved neither
// fires nor crashes.
int minuteTaskWithoutSession()
{
	WtHelper::setGenerateDir(tempDir("minute_nosession").c_str());
	NoSessionBaseDataMgr bd;
	q01sel::HotMgr hot;
	S3K27SelEngine* engine = new S3K27SelEngine();
	engine->attach(&bd, &hot, 20250110);
	CtxPtr ctx = makeCtx(engine, "s3_min_nosession");
	// The interval equals the next minute's HHMM value, so an implementation
	// that matched _time against the next minute would reach the session.
	engine->addContext(ctx, 0, 931, TPT_Minute);
	engine->on_minute_end(20250110, 930);
	engine->release();
	const int fires = (int)ctx->schedule_fires;
	delete engine;
	return fires == 0 ? 0 : 10 + fires;
}
}

TEST(S3K27SelPeriodFire, MinuteTaskWithoutSessionNeverFires)
{
	EXPECT_EXIT(std::exit(minuteTaskWithoutSession()), ::testing::ExitedWithCode(0), "");
}
