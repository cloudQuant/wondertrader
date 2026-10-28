/*!
 * \file r154_state_monitor_states_test.cpp
 * \brief StateMonitor 非 ORIGINAL 状态机与收盘清理（迭代01 第二十三波 R154）
 *
 * R145 只让 run() 跑完首轮（SS_ORIGINAL，L196-296）。本文件用「无论当前 HHMM
 * 是多少都落到同一出口」的会话/状态模板，让产品线程继续推进 2~4 轮，覆盖：
 *   StateMonitor.cpp:298-331  SS_INITIALIZED（RECEIVING / PAUSED / 竞价未到）
 *   StateMonitor.cpp:333-371  SS_RECEIVING（维持接收）
 *   StateMonitor.cpp:373-425  SS_PAUSED（维持休盘）
 *   StateMonitor.cpp:427-463  SS_CLOSED（PROCING / PAUSED）
 *   StateMonitor.cpp:465-467  SS_PROCING -> SS_PROCED
 *   StateMonitor.cpp:469-513  SS_PROCED / SS_Holiday（重置与保持节假日）
 *   StateMonitor.cpp:521-526  全表 PROCING 时的 CMD_CLEAR_CACHE 清理
 *
 * run() 内部按墙上时钟做 1 秒节流，因此轮次间隔无法注入；墙钟在这里只作为
 * 「等下一轮」的上限超时，断言的目标状态由模板保证与具体钟点无关。周末
 * isTradingDate 硬编码 false，非节假日序列按环境 GTEST_SKIP。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <WtDtCore/StateMonitor.h>

#include "r154_test_util.h"

namespace {

using r154::wait_state;

class R154StateMonitorStatesTest : public r154::StateFixture
{
};

/*
 * StateMonitor.cpp:298-331（isInSections 命中 -> RECEIVING）、333-371（维持）
 * INITALL 的区间 1..2359 换算成 [0,2400)，任意 HHMM 都在交易区间内。
 * 首轮 ORIGINAL：close=2400 不收盘、无竞价 -> INITIALIZED；
 * 第二轮 SS_INITIALIZED：isInSections 为 true -> RECEIVING；
 * 第三轮 SS_RECEIVING：offTime 既未到 close=2400，也 < getCloseTime=2400，
 * 且仍在区间内 -> 维持 RECEIVING。
 */
TEST_F(R154StateMonitorStatesTest, InitializedThenReceivingStaysReceiving)
{
	if (r154::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"INITALL": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "INITALL", SS_INITIALIZED))
		<< "首轮未进入 SS_INITIALIZED";
	ASSERT_TRUE(wait_state(mon, "INITALL", SS_RECEIVING, 5000))
		<< "SS_INITIALIZED 未推进到 SS_RECEIVING";

	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	EXPECT_TRUE(mon.isInState("INITALL", SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_RECEIVING));
}

/*
 * StateMonitor.cpp:298-331（isInSections 未命中 -> PAUSED）、373-425（维持）
 * INITEMP 的区间 0..0 在 StateMonitor::initialize 里 stime-- 下溢，isInSections
 * 恒为 false；close=2400 使 offTime < getCloseTime 恒成立。
 * 首轮 INITIALIZED，第二轮 SS_INITIALIZED -> PAUSED，第三轮 SS_PAUSED：
 * 品种非节假日且 offTime 不在区间内 -> 维持 PAUSED。
 */
TEST_F(R154StateMonitorStatesTest, InitializedWithoutSectionsTransitionsToPaused)
{
	if (r154::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"INITEMP": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "INITEMP", SS_INITIALIZED))
		<< "首轮未进入 SS_INITIALIZED";
	ASSERT_TRUE(wait_state(mon, "INITEMP", SS_PAUSED, 5000))
		<< "SS_INITIALIZED 未推进到 SS_PAUSED";

	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	EXPECT_TRUE(mon.isInState("INITEMP", SS_PAUSED));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
}

/*
 * StateMonitor.cpp:427-467、469-513、521-526
 * CLOSEPROC：close=1、proc=0、init=2359（不触发 2400 的 offsetTime 回绕）。
 * 首轮 ORIGINAL -> CLOSED；第二轮 SS_CLOSED 的 offTime>=offProcTime -> PROCING，
 * 并在轮末 isAllInState(SS_PROCING) 命中时调用 CMD_CLEAR_CACHE；第三轮
 * SS_PROCING -> PROCED；第四轮 SS_PROCED 因 offTime<offInitTime 且品种非节假日
 * -> 重置 ORIGINAL。
 * 00:00 时 offTime=0<offCloseTime，23:57 之后可能跨日/撞上 init 边界，按环境跳过。
 */
TEST_F(R154StateMonitorStatesTest, ClosedProcessingProcedThenResetToOriginal)
{
	if (r154::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	const uint32_t curMin = r154::current_minute();
	if (curMin == 0 || curMin >= 2357)
		GTEST_SKIP() << "当前 HHMM=" << curMin << " 落在收盘/跨日边界";

	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"CLOSEPROC": {"inittime": 2359, "closetime": 1, "proctime": 0}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "CLOSEPROC", SS_CLOSED)) << "首轮未进入 SS_CLOSED";
	ASSERT_TRUE(wait_state(mon, "CLOSEPROC", SS_PROCING, 4000)) << "未进入 SS_PROCING";
	ASSERT_TRUE(wait_state(mon, "CLOSEPROC", SS_PROCED, 4000)) << "未进入 SS_PROCED";
	ASSERT_TRUE(wait_state(mon, "CLOSEPROC", SS_ORIGINAL, 4000)) << "未重置回 SS_ORIGINAL";
	EXPECT_TRUE(mon.isInState("CLOSEPROC", SS_ORIGINAL));
}

/*
 * StateMonitor.cpp:427-463 的 CLOSED -> PAUSED 分支
 * CLOSEPAUSE：close=1、proc=2359，auction 0..1 使 getAuctionStartTime(true)==0，
 * 且 01:00 之后竞价位不再命中；交易区间 0..0 下溢为无效区间，isInSections=false。
 * 首轮 ORIGINAL -> CLOSED；第二轮 SS_CLOSED：offTime 未到 offProcTime=2359，
 * 却落在 [aucStart=0, getCloseTime=2400] 内且不在区间 -> PAUSED。
 * 00:00 与 23:57 之后按环境跳过（offTime 会落到 init/收盘边界）。
 */
TEST_F(R154StateMonitorStatesTest, ClosedBeforeProcTimeFallsBackToPaused)
{
	if (r154::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	const uint32_t curMin = r154::current_minute();
	if (curMin == 0 || curMin >= 2357)
		GTEST_SKIP() << "当前 HHMM=" << curMin << " 落在收盘/跨日边界";

	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"CLOSEPAUSE": {"inittime": 2359, "closetime": 1, "proctime": 2359}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "CLOSEPAUSE", SS_CLOSED)) << "首轮未进入 SS_CLOSED";
	ASSERT_TRUE(wait_state(mon, "CLOSEPAUSE", SS_PAUSED, 5000))
		<< "SS_CLOSED 未推进到 SS_PAUSED";

	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	EXPECT_TRUE(mon.isInState("CLOSEPAUSE", SS_PAUSED));
	EXPECT_FALSE(mon.isInState("CLOSEPAUSE", SS_CLOSED));
}

/*
 * StateMonitor.cpp:469-513 的 SS_Holiday 保持分支（重置分支不可达，见清单）
 * HOL 会话不挂任何品种：getSessionComms 返回 NULL，首轮 ORIGINAL 直接 SS_Holiday；
 * 第二轮 SS_Holiday 在 offTime<offInitTime 内，pCommSet 仍为空 -> isAllHoliday
 * 保持 true，不会重置为 ORIGINAL。
 */
TEST_F(R154StateMonitorStatesTest, HolidaySessionStaysHolidayAcrossSecondRound)
{
	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"HOL": {"inittime": 2359, "closetime": 2400, "proctime": 2359}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "HOL", SS_Holiday)) << "首轮未进入 SS_Holiday";

	// 第二轮在首轮结束后 1 秒节流，等待其执行完 SS_Holiday 分支。
	std::this_thread::sleep_for(std::chrono::milliseconds(2000));
	EXPECT_TRUE(mon.isInState("HOL", SS_Holiday));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
}

/*
 * StateMonitor.cpp:298-331 的 offAucSTime==-1 反面（竞价已配置但尚未开始）
 * INITAUC 的 auction 2359..2359 使 getAuctionStartTime(true)==2359，区间覆盖全天
 * 但竞价分支要 offTime>=2359 才进入。非 23:59 时首轮 ORIGINAL 走 init 分支
 * -> INITIALIZED；第二轮 SS_INITIALIZED 因 offTime<aucStart 不改变状态。
 */
TEST_F(R154StateMonitorStatesTest, InitializedWaitsForAuctionStart)
{
	if (r154::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	const uint32_t curMin = r154::current_minute();
	if (curMin >= 2358)
		GTEST_SKIP() << "当前 HHMM=" << curMin << " 已逼近集合竞价时间";

	StateMonitor mon;
	r154::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"INITAUC": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(wait_state(mon, "INITAUC", SS_INITIALIZED)) << "首轮未进入 SS_INITIALIZED";
	std::this_thread::sleep_for(std::chrono::milliseconds(1300));
	EXPECT_TRUE(mon.isInState("INITAUC", SS_INITIALIZED));
}

}	// namespace
