/*!
 * \file q05_state_monitor_test.cpp
 * \brief WtDtCore/StateMonitor 覆盖用例（迭代01 第五波 q05_dtsrv）
 *
 * 覆盖目标：
 *  - StateMonitor::initialize 的文件缺失 / 配置解析失败 / 会话模板缺失 /
 *    正常装配（集合竞价与交易时段区间换算、_session_to_pids 为空的跳过分支）
 *  - isAnyInState / isAllInState / isInState 三个状态查询
 *  纯内存 + 临时目录 JSON 文件；不起 run()/stop() 的监控线程。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSUtils/WTSCfgLoader.h>
#include <Includes/WTSVariant.hpp>

namespace fs = std::filesystem;

namespace {

std::filesystem::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

// 两个会话模板：一个日盘（单段集合竞价）、一个夜盘（多段集合竞价 + 负偏移）
// sessions.json 缺 sections 的条目会被 loadSessions 静默丢弃（不加入 _session_map）
const char* kSessionCfg = R"JSON({
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
	"NODATA": {
		"name": "缺时段条目",
		"offset": 0
	}
})JSON";

class Q05StateMonitorTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("q05_sm");
		_sessionsFile = _dir / "sessions.json";
		write_file(_sessionsFile, kSessionCfg);
		ASSERT_TRUE(_bdMgr.loadSessions(_sessionsFile.string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path initStateCfg(const std::string& content)
	{
		fs::path p = _dir / "state.json";
		write_file(p, content);
		return p;
	}

	fs::path _dir;
	fs::path _sessionsFile;
	WTSBaseDataMgr _bdMgr;
};

TEST_F(Q05StateMonitorTest, InitializeMissingFileReturnsFalse)
{
	StateMonitor mon;
	const fs::path missing = _dir / "not_written.json";
	EXPECT_FALSE(mon.initialize(missing.string().c_str(), &_bdMgr, nullptr));
	// 空映射表上：isAnyInState 恒 false，isAllInState 恒真（vacuous）
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
}

TEST_F(Q05StateMonitorTest, InitializeMalformedJsonReturnsFalse)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg("{ this is ] not json [");
	EXPECT_FALSE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
}

TEST_F(Q05StateMonitorTest, InitializeEmptyObjectSucceedsWithNoSessions)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg("{}");
	EXPECT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
}

TEST_F(Q05StateMonitorTest, InitializeSkipsUnknownSessionTemplate)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"NOSUCH": {"inittime": 900, "closetime": 1500, "proctime": 1530}
	})JSON");
	// 会话模板不存在 -> 记 error 后 continue，整体仍返回 true
	EXPECT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon.isInState("NOSUCH", SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	// 空 map 上 isAllInState 对任意非 SS_Holiday 状态恒真
	EXPECT_TRUE(mon.isAllInState(SS_CLOSED));
}

TEST_F(Q05StateMonitorTest, InitializeAssemblesOriginalStateForKnownSessions)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE":  {"inittime": 900,  "closetime": 1510, "proctime": 1530},
		"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600},
		"NODATA": {"inittime": 0, "closetime": 0, "proctime": 0}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	// initialize 只装配 StateInfo，_state 保持构造默认值 SS_ORIGINAL
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	// "NODATA" 在 sessions.json 里没有 sections，被 loadSessions 丢弃（不入库）
	EXPECT_FALSE(mon.isInState("NODATA", SS_ORIGINAL));

	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
	EXPECT_FALSE(mon.isAnyInState(SS_CLOSED));
	EXPECT_FALSE(mon.isAllInState(SS_RECEIVING));

	// 查询不存在的 sid：命中 _map.end() 分支
	EXPECT_FALSE(mon.isInState("UNKNOWN_SID", SS_ORIGINAL));
	// SS_Holiday 分支：没有任何会话被置为节假日
	EXPECT_FALSE(mon.isAnyInState(SS_Holiday));
	EXPECT_FALSE(mon.isAllInState(SS_Holiday));
}

TEST_F(Q05StateMonitorTest, RepeatedInitializeReplacesNothingButStaysIdempotentTrue)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	// 第二次 initialize 对同一 sid 覆写 _map[key]，状态仍为 SS_ORIGINAL
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

}  // namespace
