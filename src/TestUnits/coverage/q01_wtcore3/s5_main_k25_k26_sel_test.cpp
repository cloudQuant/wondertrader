/**
 * 迭代03 第五会话主线程：决策 K25=A（SEL 定时任务在 ticker strand 内同步执行，BUG-R10-004）
 * 与 K26=B1（组合增量以挂起目标为基准累加，BUG-I02-NEW-155）。
 */
#include <gtest/gtest.h>

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
class K25SelEngine final : public WtSelEngine
{
public:
	void attach(IBaseDataMgr* bd, IHotMgr* hot)
	{
		_base_data_mgr = bd;
		_hot_mgr = hot;
		set_date_time(20250110, 930, 0);
		set_trading_date(20250110);
	}
	double pendingTarget(const std::string& code) const { return pending_target_or(code, -999.0); }
};

std::string tempDir(const char* tag)
{
	const stdfs::path dir = stdfs::temp_directory_path()
		/ ("s5_k25_k26_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	stdfs::remove_all(dir, ec);
	stdfs::create_directories(dir, ec);
	return dir.string() + "/";
}
}

// K25=A：触发的任务在 on_minute_end 返回前已在调用线程上同步执行（不依赖 release 的 join）。
TEST(S5MainK25Sel, FiredScheduleRunsSynchronouslyOnTheCallingStrand)
{
	WtHelper::setGenerateDir(tempDir("sync").c_str());
	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	std::unique_ptr<K25SelEngine> engine(new K25SelEngine());
	engine->attach(&bd, &hot);
	std::shared_ptr<q01sel::SelCtx> ctx(new q01sel::SelCtx(engine.get(), "k25_task"));
	engine->addContext(ctx, 20250110, 931, TPT_Daily);

	engine->on_minute_end(20250110, 930);
	EXPECT_EQ(1u, ctx->schedule_fires);	// 同步：返回时已执行完毕
	engine->on_minute_end(20250110, 930);
	EXPECT_EQ(1u, ctx->schedule_fires);	// 同一时刻不重复触发
	engine->release();
}

// K26=B1：无有效价格时组合增量只写挂起目标；第二次增量以挂起目标为基准累加（+2 再 +3 得 5）。
TEST(S5MainK26Sel, SecondIncrementAccumulatesOntoPendingTarget)
{
	WtHelper::setGenerateDir(tempDir("pending").c_str());
	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	std::unique_ptr<K25SelEngine> engine(new K25SelEngine());
	engine->attach(&bd, &hot);

	const char* code = "SHFE.ag.2512";
	engine->handle_pos_change("k26_a", code, 2.0);
	EXPECT_DOUBLE_EQ(2.0, engine->pendingTarget(code));
	engine->handle_pos_change("k26_b", code, 3.0);
	EXPECT_DOUBLE_EQ(5.0, engine->pendingTarget(code));
	engine->release();
}
