/*!
 * \file r5_state_monitor_bdt_test.cpp
 * \brief WtDtCore/StateMonitor::initialize 的「会话->品种集合」分支覆盖（迭代01 第七波 R5）
 *
 * 与既有 q05_state_monitor_test.cpp 的分工：那份只覆盖 _session_to_pids 为空
 * （getSessionComms 返回 NULL）的装配路径；本文件补 L109-134 的
 * 品种循环 + 节假日判定，以及 _session[16] 的 strcpy 边界与缺省时间字段。
 *
 * 手法：纯内存临时目录 JSON（sessions/commodities/holidays），用 setTradingDate
 * 预置哨兵交易日，检查 initialize 是否经 setTradingDate 覆写监控表里的交易日。
 * 不起线程，不调用 run()/stop()。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WTSTools/WTSBaseDataMgr.h>
#include <TimeUtils.hpp>
#include <Share/StdUtils.hpp>
#include <Includes/FasterDefs.h>
#include <Includes/WTSMarcos.h>
#include <Includes/WTSSessionInfo.hpp>

// Keep the inspection seam local to this test TU so integration assertions can
// observe the sections actually stored by StateMonitor::initialize.
#define private public
#include <WtDtCore/StateMonitor.h>
#undef private

namespace fs = std::filesystem;

namespace {

std::filesystem::path r5_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r5_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

// 4 个会话模板：SSE（日盘/偏移 0/单段集合竞价）、SHFE（夜盘/偏移 -120/两段集合竞价）、
// SESSTESTLONG15C（15 字符，正好塞满 StateInfo::_session[16]）、NODATA（无 sections，被 loadSessions 丢弃）
const char* kR5Sessions = R"JSON({
	"SSE": {
		"name": "上交所",
		"offset": 0,
		"auction": {"from": 915, "to": 925},
		"sections": [
			{"from": 930, "to": 1130},
			{"from": 1300, "to": 1500}
		]
	},
	"SHFE": {
		"name": "上期所",
		"offset": -120,
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
	"SESSTESTLONG15C": {
		"name": "15字符会话",
		"offset": 0,
		"sections": [
			{"from": 900, "to": 1530}
		]
	},
	"NODATA": {
		"name": "缺时段条目",
		"offset": 0
	},
	"NOAUC": {
		"name": "无集合竞价会话",
		"offset": 0,
		"sections": [
			{"from": 930, "to": 1130},
			{"from": 1300, "to": 1500}
		]
	},
	"ZEROST": {
		"name": "零起点会话",
		"offset": 0,
		"auction": {"from": 0, "to": 0},
		"sections": [
			{"from": 0, "to": 1500}
		]
	},
	"R5ALLDAY": {
		"name": "全天会话",
		"offset": 0,
		"sections": [
			{"from": 0, "to": 2400}
		]
	},
	"R5ORDINARY": {
		"name": "普通时段会话",
		"offset": 0,
		"sections": [
			{"from": 930, "to": 1130}
		]
	},
	"R5MIDBEFORE": {
		"name": "午夜前会话",
		"offset": 0,
		"sections": [
			{"from": 2330, "to": 2400}
		]
	},
	"R5MIDAFTER": {
		"name": "午夜后会话",
		"offset": 0,
		"sections": [
			{"from": 0, "to": 30}
		]
	},
	"R5OFFSETNIGHT": {
		"name": "偏移后单调夜盘",
		"offset": -120,
		"sections": [
			{"from": 2100, "to": 2300}
		]
	}
})JSON";

// stock/etf 挂在 SSE（模板 SSE_H）、longpid 挂在 15 字符会话（独立模板 LONG_H，
// 便于把「同模板互相覆写」的干扰排除掉）、rb 挂在 SHFE（偏移 -120 分支）
const char* kR5Commodities = R"JSON({
	"SSE": {
		"stock":   {"name": "股票", "session": "SSE", "holiday": "SSE_H"},
		"etf":     {"name": "基金", "session": "SSE", "holiday": "SSE_H"},
		"longpid": {"name": "长会话品种", "session": "SESSTESTLONG15C", "holiday": "LONG_H"}
	},
	"SHFE": {
		"rb": {"name": "螺纹钢", "session": "SHFE", "holiday": "SHFE_H"}
	}
})JSON";

class R5StateMonitorBdtTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r5_make_tmp_dir("r5_sm");
		r5_write_file(_dir / "sessions.json", kR5Sessions);
		r5_write_file(_dir / "commodities.json", kR5Commodities);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void loadHolidayFile(const std::string& content)
	{
		r5_write_file(_dir / "holidays.json", content);
		ASSERT_TRUE(_bdMgr.loadHolidays((_dir / "holidays.json").string().c_str()));
	}

	fs::path initStateCfg(const std::string& content)
	{
		fs::path p = _dir / "state.json";
		r5_write_file(p, content);
		return p;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

TEST_F(R5StateMonitorBdtTest, SessionCommsPresentOverwritesSeededTradingDate)
{
	// 空节假日集合 -> _trading_days 里先建出 SSE_H / SHFE_H 模板（loadHolidays 会插入 hid 键）
	loadHolidayFile(R"JSON({"SSE_H": [], "SHFE_H": []})JSON");

	// 预置哨兵交易日：initialize 内的品种循环会用真实计算值覆写它
	_bdMgr.setTradingDate("SSE.stock", 20200101, false);
	_bdMgr.setTradingDate("SHFE.rb", 20200102, false);
	ASSERT_EQ(_bdMgr.getTradingDate("SSE.stock"), 20200101u);

	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE":  {"inittime": 900,  "closetime": 1510, "proctime": 1530},
		"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	// 循环确实跑过：哨兵被覆写（getSessionComms 非空 -> L110 的 if 为真）
	EXPECT_NE(_bdMgr.getTradingDate("SSE.stock"), 20200101u);
	EXPECT_NE(_bdMgr.getTradingDate("SHFE.rb"), 20200102u);

	// 状态表仍然只装配到 SS_ORIGINAL
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

TEST_F(R5StateMonitorBdtTest, SessionWithoutCommsKeepsSeededTradingDate)
{
	// 对照组：三个模板都建出来，但只让 SESSTESTLONG15C 的品种循环真正跑到
	loadHolidayFile(R"JSON({"SSE_H": [], "SHFE_H": [], "LONG_H": []})JSON");
	_bdMgr.setTradingDate("SSE.stock", 20200101, false);
	_bdMgr.setTradingDate("SSE.longpid", 20200103, false);
	ASSERT_EQ(_bdMgr.getTradingDate("SSE.stock"), 20200101u);
	ASSERT_EQ(_bdMgr.getTradingDate("SSE.longpid"), 20200103u);

	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SESSTESTLONG15C": {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	// SESSTESTLONG15C 在 commodities 里有 longpid -> getSessionComms 非空，循环覆写 LONG_H
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_NE(_bdMgr.getTradingDate("SSE.longpid"), 20200103u);
	// 未参与本次配置的 SSE 模板不受影响
	EXPECT_EQ(_bdMgr.getTradingDate("SSE.stock"), 20200101u);

	// 会话模板不存在：命中 L62 的 NULL 分支并 continue，不碰任何交易日
	fs::path cfg2 = initStateCfg(R"JSON({"NOSUCH": {"inittime": 900}})JSON");
	ASSERT_TRUE(mon.initialize(cfg2.string().c_str(), &_bdMgr, nullptr));
	EXPECT_EQ(_bdMgr.getTradingDate("SSE.stock"), 20200101u);
	EXPECT_FALSE(mon.isInState("NOSUCH", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SESSTESTLONG15C", SS_ORIGINAL));
}

TEST_F(R5StateMonitorBdtTest, TodayAsHolidayDrivesHolidayLogBranchOfInitLoop)
{
	// 把「今天」写进节假日表，令 isTradingDate(pid, offDate) 为假 -> 走 L131 的 info 分支
	const uint32_t curDate = TimeUtils::getCurDate();
	loadHolidayFile(std::string("{\"SSE_H\": [") + std::to_string(curDate)
		+ ", " + std::to_string(TimeUtils::getNextDate(curDate, -1)) + "]}");
	_bdMgr.setTradingDate("SSE.stock", 20200101, false);

	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}
	})JSON");
	// 循环里两个品种（stock/etf）都命中节假日判定，但 initialize 仍返回 true
	EXPECT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_NE(_bdMgr.getTradingDate("SSE.stock"), 20200101u);
}

TEST_F(R5StateMonitorBdtTest, FifteenCharSessionIdFitsStateKeyBuffer)
{
	loadHolidayFile(R"JSON({"SSE_H": []})JSON");
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SESSTESTLONG15C": {"inittime": 900, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	// StateInfo::_session 是 char[16]，strcpy 恰好收下 15 字符
	EXPECT_TRUE(mon.isInState("SESSTESTLONG15C", SS_ORIGINAL));
	// 截断后的键不应存在（若 strcpy 被改成定长拷贝且丢字符，这里会误命中）
	EXPECT_FALSE(mon.isInState("SESSTESTLONG15", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	// 16 字符以上的会话名会溢出 char[16]，本轮不触发，见 bugs/R5_wtdtcore.md
	EXPECT_FALSE(mon.isInState("SESSTESTLONG15CX", SS_ORIGINAL));
}

TEST_F(R5StateMonitorBdtTest, MissingTimeFieldsDefaultToZeroAndStillAssembleSections)
{
	StateMonitor mon;
	// 三个时间字段全部缺失 -> getUInt32 走 defVal=0 路径；集合竞价与交易区间仍要换算入表
	fs::path cfg = initStateCfg(R"JSON({"SSE": {}, "SHFE": {"inittime": 2045}})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	// 时间字段全 0 也不该被推进到任何其它状态
	EXPECT_FALSE(mon.isAllInState(SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
}

TEST_F(R5StateMonitorBdtTest, ReinitializeWithExtraSessionGrowsStateTable)
{
	StateMonitor mon;
	fs::path cfg1 = initStateCfg(R"JSON({"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}})JSON");
	ASSERT_TRUE(mon.initialize(cfg1.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon.isInState("SHFE", SS_ORIGINAL));

	fs::path cfg2 = initStateCfg(R"JSON({
		"SSE":  {"inittime": 915, "closetime": 1510, "proctime": 1530},
		"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg2.string().c_str(), &_bdMgr, nullptr));
	// _map[key] 覆写同键、追加新键
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_CLOSED) == false);
}

TEST_F(R5StateMonitorBdtTest, SessionWithoutAuctionKeepsOnlyTradingSections)
{
	StateMonitor mon;
	// NOAUC 没有 auction/auctions -> getAuctionSections() 为空，L77-88 的换算循环 0 次
	fs::path cfg = initStateCfg(R"JSON({
		"NOAUC": {"inittime": 900, "closetime": 1510, "proctime": 1530}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("NOAUC", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("NOAUC", SS_RECEIVING));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	// 该会话没有登记品种 -> getSessionComms 为空，L110 的 if 走 false 分支
	EXPECT_EQ(_bdMgr.getSessionComms("NOAUC"), nullptr);
}

TEST_F(R5StateMonitorBdtTest, InitializeSectionsDriveMembershipForMidnightAndOrdinarySessions)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"R5ALLDAY":      {"inittime": 0, "closetime": 2400, "proctime": 2400},
		"R5ORDINARY":    {"inittime": 0, "closetime": 2400, "proctime": 2400},
		"R5MIDBEFORE":   {"inittime": 0, "closetime": 2400, "proctime": 2400},
		"R5MIDAFTER":    {"inittime": 0, "closetime": 2400, "proctime": 2400},
		"R5OFFSETNIGHT": {"inittime": 0, "closetime": 2400, "proctime": 2400}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	auto allDayIt = mon._map.find("R5ALLDAY");
	ASSERT_NE(allDayIt, mon._map.end());
	StateInfo& allDay = *allDayIt->second;
	ASSERT_EQ(allDay._sections.size(), 1u);
	EXPECT_EQ(allDay._sections[0]._from, 0u);
	EXPECT_EQ(allDay._sections[0]._end, 2400u);
	EXPECT_TRUE(allDay.isInSections(0));
	EXPECT_TRUE(allDay.isInSections(2359));
	EXPECT_FALSE(allDay.isInSections(2400)); // 左闭右开，24:00 是日内上界

	auto ordinaryIt = mon._map.find("R5ORDINARY");
	ASSERT_NE(ordinaryIt, mon._map.end());
	StateInfo& ordinary = *ordinaryIt->second;
	ASSERT_EQ(ordinary._sections.size(), 1u);
	EXPECT_EQ(ordinary._sections[0]._from, 929u);
	EXPECT_EQ(ordinary._sections[0]._end, 1131u);
	EXPECT_FALSE(ordinary.isInSections(928));
	EXPECT_TRUE(ordinary.isInSections(929));
	EXPECT_TRUE(ordinary.isInSections(1130));
	EXPECT_FALSE(ordinary.isInSections(1131));

	auto beforeMidnightIt = mon._map.find("R5MIDBEFORE");
	ASSERT_NE(beforeMidnightIt, mon._map.end());
	StateInfo& beforeMidnight = *beforeMidnightIt->second;
	ASSERT_EQ(beforeMidnight._sections.size(), 1u);
	EXPECT_EQ(beforeMidnight._sections[0]._from, 2329u);
	EXPECT_EQ(beforeMidnight._sections[0]._end, 2400u);
	EXPECT_TRUE(beforeMidnight.isInSections(2359));
	EXPECT_FALSE(beforeMidnight.isInSections(0));

	auto afterMidnightIt = mon._map.find("R5MIDAFTER");
	ASSERT_NE(afterMidnightIt, mon._map.end());
	StateInfo& afterMidnight = *afterMidnightIt->second;
	ASSERT_EQ(afterMidnight._sections.size(), 1u);
	EXPECT_EQ(afterMidnight._sections[0]._from, 0u);
	EXPECT_EQ(afterMidnight._sections[0]._end, 31u);
	EXPECT_TRUE(afterMidnight.isInSections(0));
	EXPECT_TRUE(afterMidnight.isInSections(30));
	EXPECT_FALSE(afterMidnight.isInSections(31));

	auto offsetNightIt = mon._map.find("R5OFFSETNIGHT");
	ASSERT_NE(offsetNightIt, mon._map.end());
	StateInfo& offsetNight = *offsetNightIt->second;
	ASSERT_EQ(offsetNight._sections.size(), 1u);
	// offset=-120 将 21:00-23:00 映射为 19:00-21:00，保持单调后再扩边界。
	EXPECT_EQ(offsetNight._sections[0]._from, 1859u);
	EXPECT_EQ(offsetNight._sections[0]._end, 2101u);
	EXPECT_FALSE(offsetNight.isInSections(1858));
	EXPECT_TRUE(offsetNight.isInSections(1859));
	EXPECT_TRUE(offsetNight.isInSections(2100));
	EXPECT_FALSE(offsetNight.isInSections(2101));
	// offset 后 from>to 的区间仍未定义：本测试不推断应拆分还是拒绝。
}

TEST_F(R5StateMonitorBdtTest, ZeroStartSectionUsesClampedMidnightPadding)
{
	StateMonitor mon;
	// ZEROST 的 from=00:00：前扩一分钟夹在日内下界，不应发生 uint32_t 下溢。
	fs::path cfg = initStateCfg(R"JSON({
		"ZEROST": {"inittime": 0, "closetime": 0, "proctime": 0}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	auto zeroStartIt = mon._map.find("ZEROST");
	ASSERT_NE(zeroStartIt, mon._map.end());
	const auto& auctionSections = zeroStartIt->second->_sInfo->getAuctionSections();
	ASSERT_EQ(auctionSections.size(), 1u);
	EXPECT_EQ(auctionSections[0].first_raw, 0u);
	EXPECT_EQ(auctionSections[0].second_raw, 0u);
	EXPECT_EQ(auctionSections[0].first, 0u);
	EXPECT_EQ(auctionSections[0].second, 0u);
	EXPECT_FALSE(zeroStartIt->second->_sInfo->isInAuctionTime(0));
	EXPECT_FALSE(zeroStartIt->second->_sInfo->isInAuctionTime(1200));

	const auto tradingSection = std::find_if(
		zeroStartIt->second->_sections.begin(), zeroStartIt->second->_sections.end(),
		[](const StateInfo::Section& section) {
			return section._from == 0 && section._end == 1501;
		});
	ASSERT_NE(tradingSection, zeroStartIt->second->_sections.end());
	EXPECT_EQ(tradingSection->_from, 0u);
	EXPECT_EQ(tradingSection->_end, 1501u);

	// The fixture also has auction 0..0, stored separately before the trading section.
	// Isolate each section created by initialize so its membership is tested independently.
	auto emptyAuctionSection = std::find_if(
		zeroStartIt->second->_sections.begin(), zeroStartIt->second->_sections.end(),
		[](const StateInfo::Section& section) {
			return section._from == 0 && section._end == 0;
		});
	ASSERT_NE(emptyAuctionSection, zeroStartIt->second->_sections.end());
	StateInfo auctionOnly;
	auctionOnly._sections.push_back(*emptyAuctionSection);
	EXPECT_FALSE(auctionOnly.isInSections(0));
	EXPECT_FALSE(auctionOnly.isInSections(1200));

	StateInfo tradingOnly;
	tradingOnly._sections.push_back(*tradingSection);
	EXPECT_TRUE(tradingOnly.isInSections(0));
	EXPECT_TRUE(tradingOnly.isInSections(1500));
	EXPECT_FALSE(tradingOnly.isInSections(1501));

	// initialize 将会话登记到状态表，且配置的零时刻不会把状态推进。
	EXPECT_TRUE(mon.isInState("ZEROST", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
}

TEST_F(R5StateMonitorBdtTest, NonObjectEntriesDegradeToDefaultTimes)
{
	StateMonitor mon;
	// 会话项不是对象（这里是数组）：getUInt32 取不到键，退回 defVal=0，不抛不崩
	fs::path cfg = initStateCfg(R"JSON({"SSE": [1, 2, 3], "SHFE": "not-an-object"})JSON");
	EXPECT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));

	// Config root must be an object; a root-level array is rejected by WTSCfgLoader.
	StateMonitor mon2;
	fs::path cfg2 = initStateCfg(R"JSON([{"SSE": 1}])JSON");
	EXPECT_FALSE(mon2.initialize(cfg2.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon2.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon2.isInState("SSE", SS_ORIGINAL));
}

TEST_F(R5StateMonitorBdtTest, MixedKnownAndUnknownSessionsKeepTableConsistent)
{
	loadHolidayFile(R"JSON({"SSE_H": [], "SHFE_H": [], "LONG_H": []})JSON");
	_bdMgr.setTradingDate("SSE.stock", 20200101, false);

	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"ZZZ1": {"inittime": 900},
		"SSE":  {"inittime": 900,  "closetime": 1510, "proctime": 1530},
		"ZZZ2": {"inittime": 900},
		"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600},
		"ZZZ3": {"inittime": 900}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	// 未知模板逐个 continue，不影响已知会话入表
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("ZZZ1", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("ZZZ2", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("ZZZ3", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	// 已知会话的品种循环跑到了，哨兵被覆写
	EXPECT_NE(_bdMgr.getTradingDate("SSE.stock"), 20200101u);
}

// StateMonitor::run() 的整个状态机（StateMonitor.cpp L145-530）在 StdThread 的 lambda 内，
// 且时钟只能取 TimeUtils::getCurDate()/getCurMin() 的本地真实时间，没有任何可注入接缝；
// 触发条件：调用 run() 即创建线程并 sleep_for(2ms) 轮询，再 stop() join —— 同时违反本轮
// 「不起线程 / 不 sleep」约束，且无法把 curMin 控制在目标 HHMM 上。故整条留白，见 bugs 报告。
TEST_F(R5StateMonitorBdtTest, DISABLED_StateMachineAdvanceNeedsInjectableClockSeam)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	mon.run();
	mon.stop();
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
}

}  // namespace
