/*!
 * \file r44_state_monitor_lifecycle_test.cpp
 * \brief WtDtCore/StateMonitor 生命周期与重入边界补测（迭代01 第十波 R44）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05_state_monitor_test.cpp / r5_state_monitor_bdt_test.cpp 已把
 *    initialize 的文件缺失/坏配置/未知会话/区间换算/品种节假日循环打满；
 *  - r18_state_info_datamgr_test.cpp 已覆盖 StateInfo 与三个查询的空表语义。
 * 本文件只补三处此前没有用例走到的安全分支：
 *  - stop() 在从未 run() 时的 _thrd==NULL 守卫（StateMonitor.cpp:537-543）
 *  - initialize 的「文件缺失/空文件」在 bd_mgr 解引用之前返回（L43-54 的时序）
 *  - initialize 重入时不清空旧 _map 的现状（L107 只覆写/追加），登记为待审核缺陷
 *
 * 不调用 run()（会创建线程、sleep 2ms、依赖墙钟），不读真实时钟。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>

namespace fs = std::filesystem;

namespace {

fs::path r44_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r44_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

const char* kR44Sessions = R"JSON({
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
	}
})JSON";

class R44StateMonitorLifecycleTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r44_make_tmp_dir("r44_sm");
		r44_write_file(_dir / "sessions.json", kR44Sessions);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path initCfg(const std::string& body)
	{
		fs::path p = _dir / "state.json";
		r44_write_file(p, body);
		return p;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

// StateMonitor.cpp:537-543 —— 从未 run() 时 _thrd 为空，stop 只置位并跳过 join；空表查询语义不变
TEST_F(R44StateMonitorLifecycleTest, StopBeforeRunIsSafeAndIdempotent)
{
	StateMonitor mon;
	mon.stop();
	mon.stop();

	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	// 空表上 isAllInState 恒真（StateMonitor.h:196-210 的循环 0 次）
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
}

// StateMonitor.cpp:40-47 —— 文件不存在时在 _bd_mgr->getSession 之前就返回，bdMgr==NULL 也安全
TEST_F(R44StateMonitorLifecycleTest, MissingFileWithNullBdMgrReturnsBeforeDeref)
{
	StateMonitor mon;
	const fs::path missing = _dir / "not_written.json";
	EXPECT_FALSE(mon.initialize(missing.string().c_str(), nullptr, nullptr));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

// StateMonitor.cpp:49-54 —— 0 字节配置被 WTSCfgLoader 判为空内容并返回 NULL，走 Loading state config failed
TEST_F(R44StateMonitorLifecycleTest, EmptyConfigFileReturnsFalse)
{
	fs::path cfg = _dir / "empty.json";
	r44_write_file(cfg, "");
	StateMonitor mon;
	EXPECT_FALSE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
}

// StateMonitor.cpp:56-66 —— 空字符串 session key 查不到模板，走 continue；同文件里的合法 key 照常装配
TEST_F(R44StateMonitorLifecycleTest, EmptySessionKeyIsSkippedButValidSessionAssembles)
{
	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({
		"":    {"inittime": 900, "closetime": 1500, "proctime": 1530},
		"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
}

// StateMonitor.cpp:107 —— initialize 只做 _map[key]=stateInfo，不清空旧表；二次装配后已删除的 SSE 仍留在表中。
// 现状锁定：BUG-R44-001（配置重载后旧会话继续被状态机管理）。
TEST_F(R44StateMonitorLifecycleTest, ReinitializeKeepsRemovedSessionInStateTable)
{
	StateMonitor mon;
	fs::path cfg1 = initCfg(R"JSON({"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}})JSON");
	ASSERT_TRUE(mon.initialize(cfg1.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));

	fs::path cfg2 = initCfg(R"JSON({"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600}})JSON");
	ASSERT_TRUE(mon.initialize(cfg2.string().c_str(), &_bdMgr, nullptr));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	// 新配置里已经删掉的 SSE 仍然存在（initialize 没有 _map.clear()）
	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

/*
 * 触发条件（禁用原因）：StateMonitor::initialize（L61）直接 `_bd_mgr->getSession(...)`，
 * 没有对 bdMgr 判空。文件存在且 JSON 合法时，传 bdMgr==NULL 就是空指针解引用。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R44.md BUG-R44-002。
 */
TEST_F(R44StateMonitorLifecycleTest, DISABLED_InitializeCrashesWithNullBdMgr)
{
	StateMonitor mon;
	fs::path cfg = initCfg(R"JSON({"SSE": {"inittime": 900, "closetime": 1510, "proctime": 1530}})JSON");
	mon.initialize(cfg.string().c_str(), nullptr, nullptr);	// 崩在 L61
}

}  // namespace
