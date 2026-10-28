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

#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <TimeUtils.hpp>

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

TEST_F(R5StateMonitorBdtTest, ZeroStartSectionStillAssemblesWithoutValidation)
{
	StateMonitor mon;
	// ZEROST 的 sections from=0：L99 的 stime-- 在 uint32_t 上会下溢，实现里没有任何校验
	fs::path cfg = initStateCfg(R"JSON({
		"ZEROST": {"inittime": 0, "closetime": 0, "proctime": 0}
	})JSON");
	EXPECT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	// 越界的时间没有被拒绝，条目照常入表（缺陷见 bugs/R5_wtdtcore.md）
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

	// 顶层不是对象：memberNames() 为空，循环 0 次，仍然返回 true
	StateMonitor mon2;
	fs::path cfg2 = initStateCfg(R"JSON([{"SSE": 1}])JSON");
	EXPECT_TRUE(mon2.initialize(cfg2.string().c_str(), &_bdMgr, nullptr));
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
