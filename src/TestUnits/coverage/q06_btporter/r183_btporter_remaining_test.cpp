/*
 * \file r183_btporter_remaining_test.cpp
 * \brief R183 · WtBtPorter/WtBtRunner 剩余公开面（ext loader 未知周期、未运行 stop、门面默认态）
 *
 * 覆盖的生产行段：
 *  - WtBtPorter/WtBtRunner.cpp:113-117  loadRawHisBars 未知周期 -> false + 错误日志
 *  - WtBtPorter/WtBtRunner.cpp:138-142  loadFinalHisBars 未知周期 -> false + 错误日志
 *  - WtBtPorter/WtBtRunner.cpp:550-560  stop() 在 _running=false 且无 worker 时的早退
 *  - WtBtPorter/WtBtPorter.cpp:431-435/469-473/1157-1161/1213-1217/893-897
 *    门面在无 mocker / 非异步态的空返回
 *
 * 边界：本地 WtBtRunner 实例 + 全局 getRunner() 的默认态；不配置回放、不启动
 *      worker、不装载策略插件、不接柜台、不写共享内存。没有临时文件。
 */
#include <gtest/gtest.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

#include "Includes/WTSMarcos.h"

#include <new>
#include <string>

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace {

void reset_singleton()
{
	WtBtRunner& runner = getRunner();
	runner.~WtBtRunner();
	new (&runner) WtBtRunner();
}

struct LoaderRec
{
	std::string finalPeriod;
	std::string rawPeriod;
	bool finalCalled = false;
	bool rawCalled = false;
};

LoaderRec g_loader;

bool on_final_bars(const char* /*stdCode*/, const char* period)
{
	g_loader.finalCalled = true;
	g_loader.finalPeriod = period != nullptr ? period : "";
	return true;
}

bool on_raw_bars(const char* /*stdCode*/, const char* period)
{
	g_loader.rawCalled = true;
	g_loader.rawPeriod = period != nullptr ? period : "";
	return true;
}

bool on_adj_factors(const char* /*stdCode*/)
{
	return true;
}

bool on_raw_ticks(const char* /*stdCode*/, uint32_t /*uDate*/)
{
	return true;
}

void read_bars(void* /*obj*/, WTSBarStruct* /*bars*/, uint32_t /*count*/)
{
}

void get_bars_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/, const char* /*period*/,
	WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
{
}

void get_ticks_cb(CtxHandler /*cHandle*/, const char* /*stdCode*/,
	WTSTickStruct* /*tick*/, WtUInt32 /*count*/, bool /*isLast*/)
{
}

}	// namespace

/*
 * 生产: WtBtRunner.cpp:108-119/133-144
 * 先注册四个 ext loader（非 NULL），再请求 KP_Tick：两个函数都应进入 switch 的
 * default 分支返回 false；随后请求 KP_DAY 证明 loader 确实生效且周期映射为 d1。
 */
TEST(R183BtRunner, UnsupportedExtPeriodRejectedAfterLoaderInstall)
{
	WtBtRunner runner;
	g_loader = LoaderRec();

	int dummy = 0;
	runner.registerExtDataLoader(on_final_bars, on_raw_bars, on_adj_factors, on_raw_ticks, false);
	EXPECT_FALSE(runner.isAutoTrans());

	EXPECT_FALSE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_Tick, &read_bars));
	EXPECT_FALSE(g_loader.finalCalled);

	EXPECT_FALSE(runner.loadRawHisBars(&dummy, "SHFE.rb2510", KP_Tick, &read_bars));
	EXPECT_FALSE(g_loader.rawCalled);

	EXPECT_TRUE(runner.loadFinalHisBars(&dummy, "SHFE.rb2510", KP_DAY, &read_bars));
	EXPECT_TRUE(g_loader.finalCalled);
	EXPECT_EQ(std::string("d1"), g_loader.finalPeriod);

	EXPECT_TRUE(runner.loadRawHisBars(&dummy, "SHFE.rb2510", KP_Minute5, &read_bars));
	EXPECT_TRUE(g_loader.rawCalled);
	EXPECT_EQ(std::string("m5"), g_loader.rawPeriod);
}

/*
 * 生产: WtBtRunner.cpp:550-560
 * 从未 run() 的实例上 stop() 必须走 !_running 早退（_worker 为空），重复调用幂等。
 */
TEST(R183BtRunner, StopBeforeRunIsIdempotent)
{
	WtBtRunner runner;
	EXPECT_NO_THROW(runner.stop());
	EXPECT_NO_THROW(runner.stop());
	EXPECT_FALSE(runner.isAsync());
}

/*
 * 生产: WtBtPorter.cpp:431-435/469-473/1157-1161/1213-1217（无 mocker 的空返回）；
 *       WtBtPorter.cpp:893-897（非异步 cta_step 返回 false）
 * 先重置全局 runner 到默认态，保证 cta/sel 槽为 NULL：两族 bars/ticks 门面都返回 0，
 * cta_step 为 false；stop_backtest 在未运行时安全早退（WtBtRunner.cpp:550-560）。
 * 不读取 hft 槽：WtBtRunner 构造函数漏初始化 _hft_mocker（BUG-R173-005）。
 */
TEST(R183BtPorterFacade, DefaultStateQueriesAreSafe)
{
	reset_singleton();

	EXPECT_EQ(0U, cta_get_bars(1, "SHFE.rb2510", "d1", 10, true, &get_bars_cb));
	EXPECT_EQ(0U, cta_get_ticks(1, "SHFE.rb2510", 10, &get_ticks_cb));
	EXPECT_EQ(0U, sel_get_bars(1, "SHFE.rb2510", "d1", 10, &get_bars_cb));
	EXPECT_EQ(0U, sel_get_ticks(1, "SHFE.rb2510", 10, &get_ticks_cb));

	EXPECT_FALSE(cta_step(1));
	EXPECT_NO_THROW(stop_backtest());

	reset_singleton();
}
