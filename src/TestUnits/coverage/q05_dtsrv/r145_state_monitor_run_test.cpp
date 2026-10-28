/*!
 * \file r145_state_monitor_run_test.cpp
 * \brief WtDtCore/StateMonitor::run() 首轮状态机的短线程驱动（迭代01 第二十二波 R145）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05/r5/r18/r44/r68/r84b 已把 initialize() 的配置解析、区间换算、品种节假日循环、
 *    查询函数与 stop()-before-run 打满；run() 的 switch 因「线程 + 墙钟」一直全 0；
 *  - 本文件不再重复 DISABLED 的 “不可达” 说明，而是让 run() 真正跑**第一轮**：
 *      StateMonitor.cpp:145-193（线程 lambda/秒级节流外壳）
 *      StateMonitor.cpp:196-296（SS_ORIGINAL 的全部出口）
 *    第一轮的时间输入仍来自产品时钟，但每个会话模板都构造成「无论当前 HHMM 是多少，
 *    都必然落到同一出口」，断言不依赖具体钟点；主线程用 yield 自旋观察状态，
 *    观察到后立即 stop()/join，线程不会进入第二轮（1 秒节流）。
 *
 * 约束：不建 socket/shm，不 sleep，不读写业务文件；临时目录仅系统临时目录。
 *       周末 isTradingDate 硬编码为 false，非节假日分支按环境 GTEST_SKIP。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <thread>

#include <WtDtCore/StateMonitor.h>
#include <TimeUtils.hpp>

#include "r145_test_util.h"

namespace fs = std::filesystem;

namespace {

class R145StateMonitorRunTest : public r145::DtcoreFixture
{
};

/*
 * StateMonitor.cpp:145-150、153-193、243-248、251-255、516-530
 * 无关联品种的会话：getSessionComms 返回 NULL，ORIGINAL 直接走 else 置 SS_Holiday，
 * 随后 isAllHoliday 保持 true 再置一次 SS_Holiday。该路径与当前钟点无关，工作日/周末都成立。
 */
TEST_F(R145StateMonitorRunTest, HolidaySessionWithoutCommoditiesTransitionsToHoliday)
{
	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"HOLIDAY": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	const bool saw = r145::wait_state(mon, "HOLIDAY", SS_Holiday);
	mon.stop();

	ASSERT_TRUE(saw) << "run() 首轮未被调度到，未观察到 SS_Holiday";
	EXPECT_TRUE(mon.isInState("HOLIDAY", SS_Holiday));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
}

/*
 * StateMonitor.cpp:205-240、251-255
 * 会话挂了品种，但品种的节假日模板包含今天 ±1 天：品种循环走 L224-233 的 ss_a/日志分支，
 * isAllHoliday 保持 true -> L251-255。与星期几无关，节假日表由用例自己构造。
 */
TEST_F(R145StateMonitorRunTest, CommodityMarkedHolidayDrivesHolidayBranchOfOriginal)
{
	load_holidays(std::string("{\"R145_HOL\":") + r145::today_window() + "}");

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"HLM": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	const bool saw = r145::wait_state(mon, "HLM", SS_Holiday);
	mon.stop();

	ASSERT_TRUE(saw) << "run() 首轮未被调度到，未观察到 SS_Holiday";
	EXPECT_TRUE(mon.isInState("HLM", SS_Holiday));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
}

/*
 * StateMonitor.cpp:199-202、224-240、256-260
 * 工作日 + 非节假日品种 + offset=1、close_time=0、auction 0..0：
 * offCloseTime = offsetTime(0,false) = 1，offTime = curMin+1；绝大多数分钟 offTime>=1，
 * 走 L256-260 -> SS_CLOSED。23:59 这一分钟 offTime 回绕为 0，close 分支不成立，
 * 但 auction [0,2400) 命中 -> L263-270 SS_RECEIVING；两者都不是 SS_ORIGINAL。
 * 周末 isTradingDate 恒为 false，会话按产品语义进入 SS_Holiday，用例断言该分支。
 */
TEST_F(R145StateMonitorRunTest, ClosedSessionAfterConfiguredCloseTime)
{
	const bool weekend = r145::is_weekend();

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"CLOSED": {"inittime": 0, "closetime": 0, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	bool saw = r145::wait_state(mon, "CLOSED", weekend ? SS_Holiday : SS_CLOSED);
	if (!saw && !weekend)
		saw = r145::wait_state(mon, "CLOSED", SS_RECEIVING);	// 23:59 的 offTime 回绕
	mon.stop();

	EXPECT_TRUE(saw);
	if (weekend)
	{
		EXPECT_TRUE(mon.isInState("CLOSED", SS_Holiday));
	}
	else
	{
		EXPECT_TRUE(mon.isInState("CLOSED", SS_CLOSED)
			|| mon.isInState("CLOSED", SS_RECEIVING));
		EXPECT_FALSE(mon.isInState("CLOSED", SS_ORIGINAL));
	}
}

/*
 * StateMonitor.cpp:199-202、288-292
 * 无集合竞价（aucStartTime==-1）、close_time=2400（offCloseTime=2400，全天不收盘）：
 * offTime 必然 >= offInitTime(0)，ORIGINAL 最后一支 -> SS_INITIALIZED。
 * 周末品种被判为节假日 -> SS_Holiday；用例按环境断言对应分支。
 */
TEST_F(R145StateMonitorRunTest, SessionWithoutAuctionReachesInitialized)
{
	const bool weekend = r145::is_weekend();

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"INIT": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	const bool saw = r145::wait_state(mon, "INIT", weekend ? SS_Holiday : SS_INITIALIZED);
	mon.stop();

	EXPECT_TRUE(saw);
	if (weekend)
	{
		EXPECT_TRUE(mon.isInState("INIT", SS_Holiday));
	}
	else
	{
		EXPECT_TRUE(mon.isInState("INIT", SS_INITIALIZED));
		EXPECT_FALSE(mon.isInState("INIT", SS_RECEIVING));
	}
}

/*
 * StateMonitor.cpp:261-271
 * auction 0..0 换算后为 [0,2400)，交易区间 [1,2359] 换算后为 [0,2360)：
 * 集合竞价已开始且 offTime 落在 _sections 内 -> L263-270 SS_RECEIVING。
 * 周末品种被判为节假日 -> SS_Holiday；用例按环境断言对应分支。
 */
TEST_F(R145StateMonitorRunTest, AuctionStartedInsideSectionsReachesReceiving)
{
	const bool weekend = r145::is_weekend();

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"RECV": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	const bool saw = r145::wait_state(mon, "RECV", weekend ? SS_Holiday : SS_RECEIVING);
	mon.stop();

	EXPECT_TRUE(saw);
	if (weekend)
	{
		EXPECT_TRUE(mon.isInState("RECV", SS_Holiday));
	}
	else
	{
		EXPECT_TRUE(mon.isInState("RECV", SS_RECEIVING));
		EXPECT_FALSE(mon.isInState("RECV", SS_CLOSED));
	}
}

/*
 * StateMonitor.cpp:261-279
 * auction 0..1（[0,1)）、交易区间 0..0（stime-- 下溢成空区间）、getCloseTime(true)=2400：
 * 集合竞价已开始、offTime 不在 _sections 内、且 offTime < 市场收盘时间 -> L277-278 SS_PAUSED。
 * 仅 00:00 这一分钟 auction [0,1) 会命中，此时产品语义是 SS_RECEIVING（L263-270）；
 * 两种都不是 SS_ORIGINAL。周末品种被判为节假日 -> SS_Holiday。
 */
TEST_F(R145StateMonitorRunTest, AuctionStartedOutsideSectionsReachesPaused)
{
	const bool weekend = r145::is_weekend();

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"PAUSE": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	bool saw = r145::wait_state(mon, "PAUSE", weekend ? SS_Holiday : SS_PAUSED);
	if (!saw && !weekend)
		saw = r145::wait_state(mon, "PAUSE", SS_RECEIVING);	// 00:00 auction [0,1) 命中
	mon.stop();

	EXPECT_TRUE(saw);
	if (weekend)
	{
		EXPECT_TRUE(mon.isInState("PAUSE", SS_Holiday));
	}
	else
	{
		EXPECT_TRUE(mon.isInState("PAUSE", SS_PAUSED)
			|| mon.isInState("PAUSE", SS_RECEIVING));
		EXPECT_FALSE(mon.isInState("PAUSE", SS_ORIGINAL));
	}
}

/*
 * StateMonitor.cpp:272-284
 * auction 0..1、交易区间 0..1（getCloseTime(true)=1）：
 * offTime>=1 时既不在 _sections 内，又已经到达市场收盘时间 -> L280-284 仍转 SS_RECEIVING
 * （接收结算价窗口）。00:00 这一分钟走 L263-270，但两种路径最终都是 SS_RECEIVING。
 * 周末品种被判为节假日 -> SS_Holiday。
 */
TEST_F(R145StateMonitorRunTest, OutsideSectionsPastMarketCloseStillReceivesSettlement)
{
	const bool weekend = r145::is_weekend();

	StateMonitor mon;
	fs::path cfg = state_cfg(
		R"JSON({"SETTLE": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	const bool saw = r145::wait_state(mon, "SETTLE", weekend ? SS_Holiday : SS_RECEIVING);
	mon.stop();

	EXPECT_TRUE(saw);
	if (weekend)
	{
		EXPECT_TRUE(mon.isInState("SETTLE", SS_Holiday));
	}
	else
	{
		EXPECT_TRUE(mon.isInState("SETTLE", SS_RECEIVING));
		EXPECT_FALSE(mon.isInState("SETTLE", SS_ORIGINAL));
	}
}

/*
 * StateMonitor.cpp:146-150、153-193、519-526、537-543
 * 空状态表也要能起停：run() 首轮 _map 0 次循环；L522 的 isAllInState(SS_PROCING)
 * 与 isAllInState(SS_Holiday) 在空表上都返回 true，取反后第二个为 false，
 * 因此 L525 transHisData 不会执行（不需要 writer）。本用例只验证线程外壳与 stop() join
 * 在“没有任何会话”时也不挂、不崩。
 */
TEST_F(R145StateMonitorRunTest, EmptyMapRunThenStopJoinsWithoutWriter)
{
	StateMonitor mon;
	fs::path cfg = state_cfg(R"JSON({})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	// 给首轮一次调度机会；不断言线程一定跑过（空表无可观测状态），只保证 join 不挂。
	for (int i = 0; i < 200000; i++)
		std::this_thread::yield();
	mon.stop();

	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

/*
 * 触发条件（禁用原因）：StateMonitor::stop()（StateMonitor.cpp:537-543）在 _thrd 非空时
 * **无条件** join；std::thread::join 对已经 join 过的线程会抛 std::system_error。
 * run() 之后连续调用两次 stop()（重试/多路径关停的常见写法）第二次即抛异常。
 * R44 只验证了 “从未 run() 时 stop 两次”，没有覆盖 run 之后的非幂等。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R145.md BUG-R145-001。
 */
TEST_F(R145StateMonitorRunTest, DISABLED_SecondStopAfterRunThrows)
{
	StateMonitor mon;
	fs::path cfg = state_cfg(R"JSON({})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, &_dm));

	mon.run();
	mon.stop();
	mon.stop();	// std::system_error: Invalid argument
}

}	// namespace
