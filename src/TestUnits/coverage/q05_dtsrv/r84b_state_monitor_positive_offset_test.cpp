/*!
 * \file r84b_state_monitor_positive_offset_test.cpp
 * \brief WtDtCore/StateMonitor 正向偏移（夜盘）会话的节假日/交易日补测（迭代01 第十六波 R84b）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05_state_monitor_test.cpp / r5_state_monitor_bdt_test.cpp / r44_state_monitor_lifecycle_test.cpp /
 *    r68_state_monitor_inputs_test.cpp 已把 initialize 的文件缺失、坏 JSON、空对象、未知会话、
 *    区间换算、零/负偏移会话的品种节假日循环、重入保留旧表等分支打满；
 *  - r18_state_info_datamgr_test.cpp 已独立覆盖 StateInfo 构造、isInSections 边界和空表查询。
 *
 * 本文件只补一个此前零命中的行：StateMonitor.cpp:127。它是 L125-129 复合条件的第二个子条件
 * `!(isInTradingTime(curMin) && isTradingDate(pid, prevDate))`。只有「会话 offset > 0」且
 * 「当前自然日不是交易日」时才会走到 L127；既有用例的 SSE/NOAUC/ZEROST 都是 offset <= 0，
 * SHFE 是 offset -120，所以 L127 在最新全量 profdata 里仍是 0。
 *
 * 手法：临时目录 JSON（sessions/commodities/holidays），把「今天 ±1 天」写进品种的节假日模板，
 * 保证无论测试在星期几/是否跨零点运行，`!isTradingDate(pid, curDate)` 都为真，L126 命中后
 * 必然进入 L127。用 setTradingDate 预置哨兵交易日，检查 initialize 的品种循环确实执行。
 *
 * 约束：不调用 run()/stop()（状态机在 StdThread + 墙钟里，任务令禁止起线程）；
 * 不读挂钟做断言（仅用 getCurDate/getNextDate 构造节假日窗口）；不 sleep、不联网、不落业务文件。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSSessionInfo.hpp>
#include <TimeUtils.hpp>

namespace fs = std::filesystem;

namespace {

fs::path r84b_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r84b_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

/*
 * 三个会话模板：
 *  - POS60：offset +60，单段集合竞价 + 两段日盘，用于正向偏移的最小路径；
 *  - POS120：offset +120，两段集合竞价 + 夜盘/日盘四段，用于跨零点的时间换算；
 *  - POSMID：offset +30，无关联品种，用于「正偏移但 getSessionComms 为空」的控制组。
 */
const char* kR84bSessions = R"JSON({
	"POS60": {
		"name": "正向偏移60",
		"offset": 60,
		"auction": {"from": 855, "to": 900},
		"sections": [
			{"from": 900, "to": 1130},
			{"from": 1300, "to": 1500}
		]
	},
	"POS120": {
		"name": "正向偏移120",
		"offset": 120,
		"auctions": [
			{"from": 2055, "to": 2100},
			{"from": 855, "to": 900}
		],
		"sections": [
			{"from": 2100, "to": 2300},
			{"from": 900, "to": 1015},
			{"from": 1030, "to": 1130},
			{"from": 1330, "to": 1500}
		]
	},
	"POSMID": {
		"name": "正向偏移30无品种",
		"offset": 30,
		"sections": [
			{"from": 930, "to": 1130},
			{"from": 1300, "to": 1500}
		]
	}
})JSON";

/*
 * 品种 -> 会话/节假日模板的映射：
 *  - POS.aa、POS.bb 都挂在 POS60 上，共用 POS60_H；
 *  - POS.cc 挂在 POS120 上，用 POS120_H。
 * 每个品种都必须带点号，WTSBaseDataMgr::getTplIDByPID 会按 "." 切分。
 */
const char* kR84bCommodities = R"JSON({
	"POS": {
		"aa": {"name": "正向甲", "session": "POS60", "holiday": "POS60_H"},
		"bb": {"name": "正向乙", "session": "POS60", "holiday": "POS60_H"},
		"cc": {"name": "正向丙", "session": "POS120", "holiday": "POS120_H"}
	}
})JSON";

class R84bStateMonitorPositiveOffsetTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r84b_make_tmp_dir("r84b_sm");
		r84b_write_file(_dir / "sessions.json", kR84bSessions);
		r84b_write_file(_dir / "commodities.json", kR84bCommodities);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void loadHolidays(const std::string& body)
	{
		fs::path p = _dir / "holidays.json";
		r84b_write_file(p, body);
		ASSERT_TRUE(_bdMgr.loadHolidays(p.string().c_str()));
	}

	fs::path initCfg(const std::string& body)
	{
		fs::path p = _dir / "state.json";
		r84b_write_file(p, body);
		return p;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

/* 今天 ±1 天的节假日数组，避免 00:00 前后跨日导致「今天」漂移。 */
std::string r84b_today_window()
{
	const uint32_t today = TimeUtils::getCurDate();
	return std::string("[") + std::to_string(TimeUtils::getNextDate(today, -1))
		+ "," + std::to_string(today)
		+ "," + std::to_string(TimeUtils::getNextDate(today, 1)) + "]";
}

std::string r84b_holiday_cfg(const std::string& pos60, const std::string& pos120)
{
	return std::string("{\"POS60_H\":") + pos60 + ",\"POS120_H\":" + pos120 + "}";
}

// StateMonitor.cpp:112-134、L125-132（尤其 L127）—— 正向偏移 + 当天节假日，单品种。
// 断言链：offset > 0、today 不是交易日 -> L126 左值为真 -> 必然求值 L127。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetSingleCommodityTodayHoliday)
{
	loadHolidays(r84b_holiday_cfg(r84b_today_window(), "[]"));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.aa", today));
	WTSSessionInfo* ssInfo = _bdMgr.getSession("POS60");
	ASSERT_NE(ssInfo, nullptr);
	ASSERT_GT(ssInfo->getOffsetMins(), 0);

	// 哨兵交易日：initialize 的 L119-123 会用真实计算值覆写它
	_bdMgr.setTradingDate("POS.aa", 20200101, false);
	ASSERT_EQ(_bdMgr.getTradingDate("POS.aa"), 20200101u);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"POS60": {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POS60", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
	EXPECT_NE(_bdMgr.getTradingDate("POS.aa"), 20200101u);
}

// StateMonitor.cpp:119-133 —— 同一正向偏移会话挂两个品种，L127 在每个品种各求值一次。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetMultipleCommoditiesShareHoliday)
{
	loadHolidays(r84b_holiday_cfg(r84b_today_window(), "[]"));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.aa", today));
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.bb", today));

	_bdMgr.setTradingDate("POS.aa", 20200101, false);
	_bdMgr.setTradingDate("POS.bb", 20200102, false);
	ASSERT_EQ(_bdMgr.getTradingDate("POS.aa"), 20200101u);
	ASSERT_EQ(_bdMgr.getTradingDate("POS.bb"), 20200102u);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"POS60": {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POS60", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("POS120", SS_ORIGINAL));
	EXPECT_NE(_bdMgr.getTradingDate("POS.aa"), 20200101u);
	EXPECT_NE(_bdMgr.getTradingDate("POS.bb"), 20200102u);
}

// StateMonitor.cpp:56-135 —— 两个正向偏移会话各自独立的节假日模板：
// POS60 的今天在节假日表里（L127 必达），POS120 不在（L126 在周末才为真）。
// 两个会话的品种循环都应覆写哨兵交易日，状态表只装配本次配置的两个键。
TEST_F(R84bStateMonitorPositiveOffsetTest, TwoPositiveSessionsUseIndependentHolidayTemplates)
{
	loadHolidays(r84b_holiday_cfg(r84b_today_window(), "[]"));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.aa", today));

	_bdMgr.setTradingDate("POS.aa", 20200101, false);
	_bdMgr.setTradingDate("POS.cc", 20200102, false);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"POS60":  {"inittime": 900,  "closetime": 1530, "proctime": 1600},
		"POS120": {"inittime": 2045, "closetime": 1530, "proctime": 1700}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POS60", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("POS120", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("POSMID", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_NE(_bdMgr.getTradingDate("POS.aa"), 20200101u);
	EXPECT_NE(_bdMgr.getTradingDate("POS.cc"), 20200102u);
}

// StateMonitor.cpp:76-105、L112-134 —— offset +120 + 夜盘/日盘四段，集合竞价与交易区间
// 都经过 HHMM->分钟->HHMM 换算；今日节假日把 L127 打出来，跨零点的 offsetTime 不应影响入表。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetAcrossMidnightWithHoliday)
{
	loadHolidays(r84b_holiday_cfg("[]", r84b_today_window()));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.cc", today));
	WTSSessionInfo* ssInfo = _bdMgr.getSession("POS120");
	ASSERT_NE(ssInfo, nullptr);
	ASSERT_GT(ssInfo->getOffsetMins(), 0);

	_bdMgr.setTradingDate("POS.cc", 20200102, false);
	ASSERT_EQ(_bdMgr.getTradingDate("POS.cc"), 20200102u);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"POS120": {"inittime": 2045, "closetime": 1530, "proctime": 1700}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POS120", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
	EXPECT_NE(_bdMgr.getTradingDate("POS.cc"), 20200102u);
}

// StateMonitor.cpp:68-74、L125-132 —— 会话值不是对象（数字）时 getUInt32 全部回落 0，
// 但会话模板存在、品种循环照常执行，L127 仍然命中；状态不会被 initialize 推进。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetNonObjectEntryDefaultsTimes)
{
	loadHolidays(r84b_holiday_cfg(r84b_today_window(), "[]"));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.aa", today));

	_bdMgr.setTradingDate("POS.aa", 20200101, false);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({"POS60": 12345})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POS60", SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_INITIALIZED));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_PAUSED));
	EXPECT_NE(_bdMgr.getTradingDate("POS.aa"), 20200101u);
}

// StateMonitor.cpp:56-66、L107-134 —— 未知会话 continue 不影响已知正向偏移会话入表；
// 已知会话的品种循环仍会覆写交易日，L127 仍会求值。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetMixedWithUnknownSession)
{
	loadHolidays(r84b_holiday_cfg(r84b_today_window(), "[]"));

	const uint32_t today = TimeUtils::getCurDate();
	ASSERT_FALSE(_bdMgr.isTradingDate("POS.aa", today));

	_bdMgr.setTradingDate("POS.aa", 20200101, false);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"NOSUCH": {"inittime": 900},
		"POS60":  {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_FALSE(mon.isInState("NOSUCH", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("POS60", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_NE(_bdMgr.getTradingDate("POS.aa"), 20200101u);
}

// StateMonitor.cpp:107-110、L134 —— 正向偏移会话没有关联品种：getSessionComms 返回 NULL，
// L110 的 if 走 false，整段品种/交易日循环跳过；状态仍装配为 SS_ORIGINAL。
TEST_F(R84bStateMonitorPositiveOffsetTest, PositiveOffsetSessionWithoutCommoditiesSkipsDateLoop)
{
	loadHolidays(r84b_holiday_cfg("[]", "[]"));

	ASSERT_EQ(_bdMgr.getSessionComms("POSMID"), nullptr);

	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"POSMID": {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("POSMID", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
}

/*
 * 触发条件（禁用原因）：StateMonitor.h:211-222 的 isInState 直接用 `_map.find(sid)`，
 * sid==nullptr 时 StateMap（std::string 键）会用 nullptr 构造 std::string，属于未定义行为
 * （libc++ 通常触发断言/异常，部分实现直接 SIGSEGV）。空串 sid 已能安全返回 false，
 * NULL 却没有同等守卫。缺陷登记：docs/迭代计划/迭代01/bugs/R84b.md BUG-R84b-001。
 */
TEST_F(R84bStateMonitorPositiveOffsetTest, DISABLED_NullSessionIdCrashesIsInState)
{
	StateMonitor mon;
	EXPECT_FALSE(mon.isInState(nullptr, SS_ORIGINAL));
}

}	// namespace
