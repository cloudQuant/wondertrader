/*!
 * \file r154_state_monitor_states_test.cpp
 * \brief StateMonitor 非 ORIGINAL 状态机与收盘清理（迭代01 第二十三波 R154）
 *
 * R145 只让 run() 跑完首轮（SS_ORIGINAL，L196-296）。本文件覆盖：
 *   StateMonitor::advanceOnce  INITIALIZED / RECEIVING / PAUSED / CLOSED / PROCING / PROCED
 *   StateMonitor::advanceOnce  Holiday 的转换与保持
 *   StateMonitor::run          全表 PROCING 时的 CMD_CLEAR_CACHE 清理
 *
 * 两个 PAUSED 分支通过 advanceOnce 的固定日期/HHMM seam 逐轮推进，不使用墙钟等待；
 * 其余后台线程用例仍验证 run() 的生产节流与状态推进。
 */
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <WtDtCore/StateMonitor.h>

#include "r154_test_util.h"

struct R154StateMonitorTestAccess
{
	static void advanceOnce(StateMonitor& monitor, uint32_t date, uint32_t minute)
	{
		monitor.advanceOnce(date, minute);
	}
};

namespace {

using r154::wait_state;

constexpr uint32_t kR154OrdinaryWeekday = 20261026; // Monday; fixture has no holiday entries.
constexpr uint32_t kR154PauseTestTime = 900;

class R154StateMonitorStatesTest : public r154::StateFixture
{
};

/*
 * StateMonitor::advanceOnce（isInSections 命中 -> RECEIVING、随后维持）
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
 * StateMonitor::advanceOnce（isInSections 未命中 -> PAUSED、随后维持）
 * 固定普通日 2026-10-26、09:00 位于 INITEMP 的 09:30–15:00 交易段之前。
 * 每次显式推进一轮：ORIGINAL -> INITIALIZED -> PAUSED，第三轮仍维持 PAUSED。
 */
TEST_F(R154StateMonitorStatesTest, InitializedOutsideSectionsTransitionsToPaused)
{
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"INITEMP": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));

	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	ASSERT_TRUE(mon.isInState("INITEMP", SS_INITIALIZED)) << "ORIGINAL 未推进到 SS_INITIALIZED";
	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	ASSERT_TRUE(mon.isInState("INITEMP", SS_PAUSED));
	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	EXPECT_TRUE(mon.isInState("INITEMP", SS_PAUSED));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
}

/*
 * StateMonitor::advanceOnce 与 StateMonitor::run
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
 * StateMonitor::advanceOnce 的 CLOSED -> PAUSED 分支
 * 固定普通日 2026-10-26、09:00 时，close=1 使首轮 ORIGINAL -> CLOSED；auction
 * 0..1 提供开始阈值 0，09:00 小于 proctime=2359 且不在 09:30–15:00 交易段，
 * 第二轮 SS_CLOSED -> PAUSED，第三轮仍维持 PAUSED。
 */
TEST_F(R154StateMonitorStatesTest, ClosedBeforeProcTimeFallsBackToPaused)
{
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"CLOSEPAUSE": {"inittime": 2359, "closetime": 1, "proctime": 2359}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));

	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	ASSERT_TRUE(mon.isInState("CLOSEPAUSE", SS_CLOSED)) << "ORIGINAL 未推进到 SS_CLOSED";
	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	ASSERT_TRUE(mon.isInState("CLOSEPAUSE", SS_PAUSED));
	R154StateMonitorTestAccess::advanceOnce(mon, kR154OrdinaryWeekday, kR154PauseTestTime);
	EXPECT_TRUE(mon.isInState("CLOSEPAUSE", SS_PAUSED));
	EXPECT_FALSE(mon.isInState("CLOSEPAUSE", SS_CLOSED));
}

/*
 * StateMonitor::advanceOnce 的 SS_Holiday 保持分支（重置分支不可达，见清单）
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
 * StateMonitor::advanceOnce 的竞价已配置但尚未开始分支
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
