/*
 * r214_porter_runner_api_test.cpp  (lane R214 · wave29)
 *
 * 靶子：WtRtRunner.h 的内联公开面
 *   - registerExtDataLoader（243-247）+ loadRawHisBars 周期映射
 *   - getEngine()（324）
 *   - on_initialize_event / on_schedule_event / on_session_event（420-446）
 *
 * 说明：R35 已证明 registerExtDataLoader 会在真实运行中被调用，但覆盖率报告
 * 仍把 WtRtRunner.h 的 18 行记为 0（见 manifests/wave29/R214.md §留白：
 * 头文件内联函数在本覆盖率构建里没有可归因的行计数）。本文件仍用公开 API
 * 把三条内联路径各走一遍，断言行为，避免只依赖报告数字。
 *
 * 边界：不配置引擎、不起线程、无网络；loader 只记录调用参数。
 */
/* 必须先以相对路径包含：否则后续经 -isystem <repo>/src 命中的同一头文件会被
 * clang 当作系统头，不生成覆盖映射（R213 已用最小实验确认）。 */
#include "../../../WtPorter/WtRtRunner.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

std::string g_code;
std::string g_period;
uint32_t g_loads = 0;

bool raw_loader(const char* stdCode, const char* period)
{
	g_loads++;
	g_code = stdCode ? stdCode : "";
	g_period = period ? period : "";
	return true;
}

bool fnl_loader(const char* stdCode, const char* period)
{
	g_loads++;
	g_code = stdCode ? stdCode : "";
	g_period = period ? period : "";
	return true;
}

bool fct_loader(const char* stdCode)
{
	g_loads++;
	g_code = stdCode ? stdCode : "";
	g_period = "fct";
	return true;
}

struct EvtRec
{
	uint32_t	count = 0;
	WtUInt32	id = 0;
	WtUInt32	date = 0;
	WtUInt32	time = 0;
};

EvtRec g_evt;

void evt_cb(WtUInt32 evtId, WtUInt32 curDate, WtUInt32 curTime)
{
	g_evt.count++;
	g_evt.id = evtId;
	g_evt.date = curDate;
	g_evt.time = curTime;
}

void reset_singleton()
{
	WtRtRunner& r = getRunner();
	r.WtRtRunner::~WtRtRunner();
	new (&r) WtRtRunner();
}

} // namespace

TEST(R214PorterRunnerApi, ExtLoaderPeriodsAndEngineGetter)
{
	reset_singleton();
	WtRtRunner& r = getRunner();
	ASSERT_EQ(nullptr, r.getEngine());

	g_loads = 0;
	r.registerExtDataLoader(fnl_loader, raw_loader, fct_loader, NULL);
	EXPECT_TRUE(r.loadFinalHisBars(NULL, "SHFE.rb.2510", KP_DAY, NULL));
	EXPECT_EQ("d1", g_period);
	EXPECT_EQ("SHFE.rb.2510", g_code);
	EXPECT_TRUE(r.loadFinalHisBars(NULL, "SHFE.rb.2510", KP_Minute1, NULL));
	EXPECT_EQ("m1", g_period);
	EXPECT_TRUE(r.loadFinalHisBars(NULL, "SHFE.rb.2510", KP_Minute5, NULL));
	EXPECT_EQ("m5", g_period);
	EXPECT_FALSE(r.loadFinalHisBars(NULL, "SHFE.rb.2510", (WTSKlinePeriod)99, NULL));
	EXPECT_EQ(3U, g_loads);

	r.registerExtDataLoader(NULL, NULL, NULL, NULL);
	EXPECT_FALSE(r.loadRawHisBars(NULL, "SHFE.rb.2510", KP_DAY, NULL));
}

TEST(R214PorterRunnerApi, EngineEventCallbacksUsePorterEventIds)
{
	reset_singleton();
	WtRtRunner& r = getRunner();
	g_evt = EvtRec();

	r.registerEvtCallback(evt_cb);
	r.on_initialize_event();
	EXPECT_EQ(1U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_INIT, g_evt.id);
	EXPECT_EQ(0U, g_evt.date);

	r.on_schedule_event(20240102, 93000);
	EXPECT_EQ(2U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_SCHDL, g_evt.id);
	EXPECT_EQ(20240102U, g_evt.date);
	EXPECT_EQ(93000U, g_evt.time);

	r.on_session_event(20240102, true);
	EXPECT_EQ(EVENT_SESSION_BEGIN, g_evt.id);
	EXPECT_EQ(20240102U, g_evt.date);

	r.on_session_event(20240102, false);
	EXPECT_EQ(EVENT_SESSION_END, g_evt.id);
	EXPECT_EQ(4U, g_evt.count);

	r.registerEvtCallback(NULL);
	r.on_initialize_event();
	EXPECT_EQ(4U, g_evt.count);
}
