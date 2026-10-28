/*!
 * \file r11_ticker_stramgr_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - WtUftRtTicker 未起线程时的转发面 + UftStrategyMgr 注册/查找
 *
 * 覆盖:
 * - WtUftTicker.cpp: 构造/析构、stop() 的无线程分支、on_tick() 的 `_thrd==NULL` 提前转发分支
 *   (该分支只把 tick 交给引擎, 不推进平台时间, 因此"时间不变"是本用例自己喂出来的可证伪期望)。
 * - UftStrategyMgr.cpp: loadFactories() 的目录不存在 / 目录存在但无有效模块(子目录、非 .dylib、
 *   伪 .dylib 装载失败) / createStrategy 两种重载的工厂缺失回退 / getStrategy 未命中回退。
 * - WtUftEngine.cpp: set_date_time / set_trading_date 的显式时间入参(不读挂钟)。
 *
 * 不起线程: WtUftRtTicker::run() 会 new StdThread 并在循环里 sleep, 本文件不触碰它。
 */
#include <boost/noncopyable.hpp>

#include "WtUftCore/WtUftTicker.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "q04_trader_fixture.h"

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////////////
// WtUftRtTicker
//没有引擎时 on_tick 的 `_thrd==NULL` 分支只能落到 `if(_engine)` 的假分支
TEST(R11UftTicker, tickWithoutEngineAndThreadIsSilent)
{
	WtUftRtTicker ticker(nullptr);

	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, "SHFE");
	wt_strcpy(st.code, "ag2406");
	st.action_date = 20240315;
	st.action_time = 93000500;
	st.price = 7000.0;
	WTSTickData* tick = WTSTickData::create(st);
	ASSERT_NE(nullptr, tick);

	//_thrd 从未被 run() 赋值 -> 走早退分支, 不 deref 空引擎
	ticker.on_tick(tick);
	//stop() 在无线程时只置位 _stopped, 不会 join
	ticker.stop();
	//重复 stop 仍然安全(第二次只是再置一次位)
	ticker.stop();
	EXPECT_EQ((uint32_t)93000500, tick->actiontime());
	tick->release();
}

//有引擎时: 早退分支把 tick 交给引擎, 但绝不推进平台时间
TEST(R11UftTicker, tickBeforeThreadForwardsButKeepsEngineTime)
{
	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	WtUftEngine engine;
	engine.init(nullptr, &bd, nullptr, nullptr);

	//显式写入一个平台时间, 之后不许被 tick 改变
	engine.set_date_time(20240315, 93000, 21, 93000);
	engine.set_trading_date(20240315);
	ASSERT_EQ((uint32_t)20240315, engine.get_date());
	ASSERT_EQ((uint32_t)93000, engine.get_raw_time());

	WtUftRtTicker ticker(&engine);
	//引擎替身的 getSession 返回 nullptr, init 只是把 _s_info 置空并取挂钟日期
	ticker.init("NONE");

	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, "SHFE");
	wt_strcpy(st.code, "ag2406");
	st.action_date = 20240318;
	st.action_time = 145959500;
	st.price = 7111.0;
	WTSTickData* tick = WTSTickData::create(st);
	ASSERT_NE(nullptr, tick);

	ticker.on_tick(tick);

	//早退分支不会调用 set_date_time / set_trading_date
	EXPECT_EQ((uint32_t)20240315, engine.get_date());
	EXPECT_EQ((uint32_t)93000, engine.get_raw_time());
	EXPECT_EQ((uint32_t)20240315, engine.get_trading_date());
	EXPECT_EQ((uint32_t)93000, engine.get_min_time());
	EXPECT_EQ((uint32_t)21, engine.get_secs());
	tick->release();
	ticker.stop();
}

//引擎时间槽本身的语义: 传入 rawTime=0 时 _cur_raw_time 取 curTime
TEST(R11UftTicker, engineTimeSlotsFollowExplicitArguments)
{
	q04::FakeBaseDataMgr bd;
	WtUftEngine engine;
	engine.init(nullptr, &bd, nullptr, nullptr);

	engine.set_date_time(20240315, 91500, 5, 0);
	EXPECT_EQ((uint32_t)20240315, engine.get_date());
	EXPECT_EQ((uint32_t)91500, engine.get_min_time());
	EXPECT_EQ((uint32_t)5, engine.get_secs());
	//rawTime 传 0 -> 回落到 curTime
	EXPECT_EQ((uint32_t)91500, engine.get_raw_time());

	engine.set_date_time(20240315, 91500, 6, 211500);
	EXPECT_EQ((uint32_t)211500, engine.get_raw_time());
	EXPECT_EQ((uint32_t)91500, engine.get_min_time());

	engine.set_trading_date(20240318);
	EXPECT_EQ((uint32_t)20240318, engine.get_trading_date());
	//引擎没被 ticker 驱动, 时间槽不受交易日切换影响
	EXPECT_EQ((uint32_t)91500, engine.get_min_time());
}

//////////////////////////////////////////////////////////////////////////
// UftStrategyMgr
class StraFactEnv
{
public:
	explicit StraFactEnv(const char* tag) : _tmp(tag) {}

	q04::TempRoot	_tmp;
	std::string		path() const { return _tmp.dir().string(); }

	void putFile(const char* name, const std::string& content) const
	{
		std::ofstream ofs((_tmp.dir() / name).string().c_str(),
			std::ios::out | std::ios::trunc);
		ofs << content;
		ofs.close();
	}
};

TEST(R11UftStrategyMgr, loadFactoriesMissingDirectory)
{
	UftStrategyMgr mgr;
	std::filesystem::path nope = std::filesystem::temp_directory_path()
		/ ("r11_no_such_stra_dir_" + std::to_string((long)::getpid()));
	std::error_code ec;
	std::filesystem::remove_all(nope, ec);
	EXPECT_FALSE(mgr.loadFactories(nope.string().c_str()));

	//空目录: 循环不执行, 仍然返回 true
	StraFactEnv env("fact_empty");
	EXPECT_TRUE(mgr.loadFactories(env.path().c_str()));
}

//目录里有子目录 / 非 .dylib 文件 / 装载失败的伪 .dylib, 一个工厂都不会注册
TEST(R11UftStrategyMgr, loadFactoriesSkipsNonModules)
{
	StraFactEnv env("fact_skip");
	std::error_code ec;
	std::filesystem::create_directory(env._tmp.dir() / "subdir", ec);
	env.putFile("notes.txt", "not a module");
	env.putFile("libfake.dylib", "this is plain text, not a mach-o image");

	UftStrategyMgr mgr;
	EXPECT_TRUE(mgr.loadFactories(env.path().c_str()));

	//一个都没注册 -> 三种查找路径全部回退
	EXPECT_EQ(nullptr, mgr.getStrategy("stra-1").get());

	UftStrategyPtr byFact = mgr.createStrategy("FACT", "UNIT", "stra-2");
	EXPECT_EQ(nullptr, byFact.get());
	EXPECT_EQ(nullptr, mgr.getStrategy("stra-2").get());

	//name 里没有 "." -> ay.size()<2 回退
	UftStrategyPtr badName = mgr.createStrategy("NO_DOT_NAME", "stra-3");
	EXPECT_EQ(nullptr, badName.get());
	EXPECT_EQ(nullptr, mgr.getStrategy("stra-3").get());

	//合法 "fact.unit" 但工厂不存在
	UftStrategyPtr noFact = mgr.createStrategy("NOFACT.UNIT", "stra-4");
	EXPECT_EQ(nullptr, noFact.get());
}

TEST(R11UftStrategyMgr, getStrategyMissReturnsEmptyPointer)
{
	UftStrategyMgr mgr;
	EXPECT_EQ(nullptr, mgr.getStrategy("never-registered").get());
	//注册表为空时, 任何名字组合都查不到
	EXPECT_EQ(nullptr, mgr.createStrategy("a", "b", "c").get());
	EXPECT_EQ(nullptr, mgr.createStrategy("onlyone", "d").get());
	//工厂装载失败过一次后, 再次装载空目录依然得到"零工厂"的结果
	StraFactEnv env("fact_empty2");
	EXPECT_TRUE(mgr.loadFactories(env.path().c_str()));
	EXPECT_EQ(nullptr, mgr.getStrategy("e").get());
}

} // namespace
