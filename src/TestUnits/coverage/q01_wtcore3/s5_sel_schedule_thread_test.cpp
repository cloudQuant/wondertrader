/**
 * I03-NEW-066: WtSelEngine::on_minute_end task dispatch.
 *
 * Before the fix every fired task created a std::thread held only by a local
 * shared_ptr; destroying the still-joinable thread calls std::terminate. The
 * context was also looked up with the task sequence number instead of the
 * context id under which the task is registered.
 *
 * Contract checked (no choice of the R10-004 threading model is implied): a
 * fired task runs exactly once on its own context, the engine owns the
 * dispatch thread, and release() joins it. Cases run in child processes so a
 * terminate in the red control is reported as a failed assertion.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "q01_sel_env.h"

#include <WtCore/WtHelper.h>
#include <WtCore/WtSelEngine.h>

using namespace wtp;
namespace stdfs = std::filesystem;

namespace
{
class S5SelEngine final : public WtSelEngine
{
public:
	void attach(IBaseDataMgr* bd, IHotMgr* hot)
	{
		_base_data_mgr = bd;
		_hot_mgr = hot;
		set_date_time(20250110, 930, 0);
		set_trading_date(20250110);
	}
};

std::string tempDir(const char* tag)
{
	const stdfs::path dir = stdfs::temp_directory_path()
		/ ("s5_sel_sched_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(dir, ec);
	stdfs::create_directories(dir, ec);
	return dir.string() + "/";
}

// Exit code 0 only if the fired task ran exactly once on its own context and
// the other context was not scheduled.
int fireOneTaskAndRelease()
{
	WtHelper::setGenerateDir(tempDir("one").c_str());
	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	S5SelEngine* engine = new S5SelEngine();
	engine->attach(&bd, &hot);

	// A first context shifts context ids away from task sequence numbers.
	std::shared_ptr<q01sel::SelCtx> other(new q01sel::SelCtx(engine, "s5_other"));
	std::shared_ptr<q01sel::SelCtx> ctx(new q01sel::SelCtx(engine, "s5_task"));
	engine->addContext(other, 20250110, 1400, TPT_Daily);
	engine->addContext(ctx, 20250110, 931, TPT_Daily);

	engine->on_minute_end(20250110, 930);	// 9:31 task fires
	engine->release();						// joins the dispatch thread

	int failures = 0;
	if (ctx->schedule_fires != 1) failures |= 1;
	if (other->schedule_fires != 0) failures |= 2;
	delete engine;
	return failures;
}

// Several fired tasks over consecutive days are all joined by release().
int fireOnSeveralDays()
{
	WtHelper::setGenerateDir(tempDir("days").c_str());
	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	S5SelEngine* engine = new S5SelEngine();
	engine->attach(&bd, &hot);
	std::shared_ptr<q01sel::SelCtx> ctx(new q01sel::SelCtx(engine, "s5_days"));
	engine->addContext(ctx, 20250110, 931, TPT_Daily);

	const uint32_t days[] = { 20250110, 20250113, 20250114, 20250115 };
	for (uint32_t d : days)
		engine->on_minute_end(d, 930);
	engine->release();
	const int fires = (int)ctx->schedule_fires;
	delete engine;
	return fires == 4 ? 0 : 10 + fires;
}
}

TEST(S5SelScheduleThread, FiredTaskRunsOnceOnItsContextAndIsJoined)
{
	EXPECT_EXIT(std::exit(fireOneTaskAndRelease()), ::testing::ExitedWithCode(0), "");
}

TEST(S5SelScheduleThread, TasksOnSeveralDaysAreAllJoinedByRelease)
{
	EXPECT_EXIT(std::exit(fireOnSeveralDays()), ::testing::ExitedWithCode(0), "");
}
