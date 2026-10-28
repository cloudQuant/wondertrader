/*
 * r214_btporter_runner_inline_test.cpp  (lane R214 · wave29)
 *
 * 靶子：WtBtRunner.h 全部 36 行未覆盖内联面
 *   - isAutoTrans（123-125）
 *   - registerEvtCallback（203-205）
 *   - registerExtDataLoader（218-224）
 *   - cta_mocker / sel_mocker / hft_mocker / replayer / isAsync（510-539）
 *   - on_initialize_event / on_schedule_event / on_session_event /
 *     on_backtest_end（549-596）
 *
 * 为什么需要新文件：R143/R173 的同名用例都 PASS，但报告仍把这 36 行记为 0。
 * 原因是测试目标的 `-isystem <repo>/src` 让 clang 把经该路径命中的头文件当作
 * 系统头、不生成覆盖映射（R213 已用最小实验确认）。本文件把
 * `#include "../../../WtBtPorter/WtBtRunner.h"` 放在所有 include 之前，
 * 使该头按用户头编译；后续 `WtBtPorter/WtBtRunner.h` 被 `#pragma once` 挡掉。
 *
 * 边界：只在栈上构造未初始化实例，不配置、不跑回放、不起线程；
 * hft_mocker 未进构造函数初始化列表（BUG-Q06-003），只执行 getter 不断言其值。
 */
#include "../../../WtBtPorter/WtBtRunner.h"

#include <gtest/gtest.h>

#include <string>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"

USING_NS_WTP;

namespace {

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

bool fnl_loader(const char*, const char*) { return true; }
bool raw_loader(const char*, const char*) { return true; }
bool fct_loader(const char*) { return true; }
bool tick_loader(const char*, uint32_t) { return true; }

} // namespace

TEST(R214BtRunnerInline, AccessorsAndExtLoaderFlags)
{
	WtBtRunner runner;

	EXPECT_EQ(nullptr, runner.cta_mocker());
	EXPECT_EQ(nullptr, runner.sel_mocker());
	/* BUG-Q06-003：_hft_mocker 未初始化，只驱动 getter，不对返回值做断言。 */
	(void)runner.hft_mocker();
	EXPECT_NE(nullptr, &runner.replayer());
	EXPECT_FALSE(runner.isAsync());

	runner.registerExtDataLoader(fnl_loader, raw_loader, fct_loader, tick_loader, false);
	EXPECT_FALSE(runner.isAutoTrans());
	runner.registerExtDataLoader(NULL, NULL, NULL, NULL, true);
	EXPECT_TRUE(runner.isAutoTrans());
}

TEST(R214BtRunnerInline, EventCallbacksUseBacktestEventIds)
{
	WtBtRunner runner;
	g_evt = EvtRec();

	runner.registerEvtCallback(evt_cb);
	runner.on_initialize_event();
	EXPECT_EQ(1U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_INIT, g_evt.id);

	runner.on_schedule_event(20240102, 93000);
	EXPECT_EQ(2U, g_evt.count);
	EXPECT_EQ(EVENT_ENGINE_SCHDL, g_evt.id);
	EXPECT_EQ(20240102U, g_evt.date);
	EXPECT_EQ(93000U, g_evt.time);

	runner.on_session_event(20240102, true);
	EXPECT_EQ(EVENT_SESSION_BEGIN, g_evt.id);
	runner.on_session_event(20240102, false);
	EXPECT_EQ(EVENT_SESSION_END, g_evt.id);
	EXPECT_EQ(4U, g_evt.count);

	runner.on_backtest_end();
	EXPECT_EQ(EVENT_BACKTEST_END, g_evt.id);
	EXPECT_EQ(5U, g_evt.count);

	runner.registerEvtCallback(NULL);
	runner.on_backtest_end();
	EXPECT_EQ(5U, g_evt.count);
}
