/*!
 * \file r164_state_monitor_holiday_test.cpp
 * \brief StateMonitor 剩余可达状态：竞价开始后的 RECEIVING、节假日品种的 Holiday 保持
 *        （迭代01 第二十四波 R164）
 *
 * R154 已把 ORIGINAL/INITIALIZED/PAUSED/CLOSED/PROCING/PROCED/Holiday 的主干打完；
 * 本文件只补两段当前 profdata 仍为 0、且在公开 API 下可达的分支：
 *   StateMonitor.cpp:261-270、333-353  有 auction 的会话首轮直接 RECEIVING，次轮维持
 *   StateMonitor.cpp:469-511           有品种且该品种今天在节假日模板里，Holiday 保持
 *
 * run() 内部按墙上时钟 1 秒节流，墙钟只作为等待上限，不断言“多久发生”；
 * 目标状态由会话/节假日模板保证与具体 HHMM 无关（仅跨 23:58 的初始化边界跳过）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <TimeUtils.hpp>

#include "r164_test_util.h"

namespace {

class R164StateMonitorHolidayTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r164::make_tmp_dir("state_holiday"); }

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	void load_sessions(const std::string& body)
	{
		r164::write_file(_dir / "sessions.json", body);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
	}

	void load_commodities(const std::string& body)
	{
		r164::write_file(_dir / "commodities.json", body);
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	}

	void load_holidays(const std::string& body)
	{
		r164::write_file(_dir / "holidays.json", body);
		ASSERT_TRUE(_bdMgr.loadHolidays((_dir / "holidays.json").string().c_str()));
	}

	std::filesystem::path state_cfg(const std::string& body)
	{
		std::filesystem::path p = _dir / "state.json";
		r164::write_file(p, body);
		return p;
	}

	std::filesystem::path _dir;
	WTSBaseDataMgr _bdMgr;
	DataManager _dtMgr;
};

/*
 * StateMonitor.cpp:261-270、333-353
 * AUCREC：auction 0..1（getAuctionStartTime(true)==0）、交易区间 1..2359 换算成
 * [0,2400)，任意 HHMM 都命中；close=2400 使 offTime 不可能越过配置收盘。
 * 首轮 ORIGINAL 走 auction 分支且 isInSections 为真 -> 直接 RECEIVING（L261-270）；
 * 第二轮 SS_RECEIVING：未到 offClose，offTime>=aucStart 为真，且 offTime 小于市场
 * 收盘、仍在区间内 -> 维持（L347-353，不再进入 PAUSED 的 L359）。
 */
TEST_F(R164StateMonitorHolidayTest, AuctionStartedSessionStaysReceiving)
{
	if (r164::is_weekend())
		GTEST_SKIP() << "周末 isTradingDate 恒为 false，非节假日序列不可达";

	load_sessions(R"JSON({
		"AUCREC": {
			"name": "竞价即接收", "offset": 0,
			"auction": {"from": 0, "to": 1},
			"sections": [{"from": 1, "to": 2359}]
		}
	})JSON");
	load_commodities(R"JSON({
		"SHFE": {"AR": {"name": "竞价品种", "session": "AUCREC", "holiday": "R164_NONE"}}
	})JSON");

	StateMonitor mon;
	r164::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"AUCREC": {"inittime": 0, "closetime": 2400, "proctime": 2400}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(r164::wait_state(mon, "AUCREC", SS_RECEIVING))
		<< "首轮未由 auction 分支进入 SS_RECEIVING";

	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	EXPECT_TRUE(mon.isInState("AUCREC", SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_PAUSED));
}

/*
 * StateMonitor.cpp:469-511
 * 品种 SHFE.HA 的 holiday 模板 R164_HOL 覆盖今天（昨天/今天/明天，避免跨日漂移），
 * 会话区间全覆盖、offset=0：
 *  - 首轮 ORIGINAL：唯一品种 isTradingDate=false -> isAllHoliday=true -> SS_Holiday；
 *  - 第二轮 SS_Holiday：offTime < inittime=2359，pCommSet 非空，逐品种重新判断
 *    节假日；今天仍是节假日 -> isAllHoliday 保持 true，不重置回 ORIGINAL。
 * 这条路径同时驱动 L489-503 的条件求值（既有 Holiday 用例没有关联品种）。
 * 23:58 之后初始化会跨过 inittime，跳过以免第二轮不再进入该条件。
 */
TEST_F(R164StateMonitorHolidayTest, HolidayInstrumentStaysHolidayAcrossSecondRound)
{
	const uint32_t curMin = r164::current_minute();
	if (curMin >= 2358)
		GTEST_SKIP() << "当前 HHMM=" << curMin << " 已逼近 inittime 边界";

	load_sessions(R"JSON({
		"HSESS": {"name": "节假日会话", "offset": 0, "sections": [{"from": 1, "to": 2359}]}
	})JSON");
	load_commodities(R"JSON({
		"SHFE": {"HA": {"name": "假日品种", "session": "HSESS", "holiday": "R164_HOL"}}
	})JSON");

	const uint32_t today = TimeUtils::getCurDate();
	const std::string window = std::string("[")
		+ std::to_string(TimeUtils::getNextDate(today, -1)) + ","
		+ std::to_string(today) + ","
		+ std::to_string(TimeUtils::getNextDate(today, 1)) + "]";
	load_holidays(std::string(R"JSON({"R164_HOL":)JSON") + window + "}");
	ASSERT_FALSE(_bdMgr.isTradingDate("SHFE.HA", today));

	StateMonitor mon;
	r164::MonitorGuard guard(mon);
	ASSERT_TRUE(mon.initialize(
		state_cfg(R"JSON({"HSESS": {"inittime": 2359, "closetime": 2400, "proctime": 2359}})JSON")
			.string().c_str(),
		&_bdMgr, &_dtMgr));
	mon.run();
	guard.arm();

	ASSERT_TRUE(r164::wait_state(mon, "HSESS", SS_Holiday))
		<< "首轮未进入 SS_Holiday";

	// 第二轮在首轮结束后 1 秒节流；等足两轮再查，避免只看首轮。
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	EXPECT_TRUE(mon.isInState("HSESS", SS_Holiday));
	EXPECT_TRUE(mon.isAnyInState(SS_Holiday));
	// isAllInState 会跳过 SS_Holiday 的会话（StateMonitor.h:189-203），当唯一会话
	// 保持 Holiday 时它对 SS_ORIGINAL 恒为 true；要证明“未被重置回 ORIGINAL”
	// 只能看任一会话是否真的处于 ORIGINAL。
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("HSESS", SS_ORIGINAL));
}

}	// namespace
