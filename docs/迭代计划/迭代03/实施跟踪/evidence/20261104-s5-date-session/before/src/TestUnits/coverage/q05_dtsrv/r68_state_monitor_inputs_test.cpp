/*!
 * \file r68_state_monitor_inputs_test.cpp
 * \brief WtDtCore/StateMonitor::initialize 的输入形态补测（迭代01 第十三波 R68）
 *
 * 与既有用例的分工（均只读，不修改）：
 *  - q05_state_monitor_test.cpp / r5_state_monitor_bdt_test.cpp / r44_state_monitor_lifecycle_test.cpp
 *    已覆盖文件缺失、坏 JSON、空对象、未知会话、区间换算、品种节假日循环、重入保留旧表；
 *  - 本文件只补此前没有喂过的顶层会话值形态：JSON null（rapidjson 转换时直接丢弃该 key）、
 *    数字、布尔、以及空串文件名。所有输入都是纯内存/临时目录 JSON。
 *
 * run() 的状态机（StateMonitor.cpp:145-530）依赖墙钟与监控线程，按任务令不硬闯，
 * 见 bugs/R5_wtdtcore.md BUG-R5-002；本文件不调用 run()。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r68_test_util.h"

namespace fs = std::filesystem;

namespace {

const char* kR68Sessions = R"JSON({
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

class R68StateMonitorInputsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r68::make_tmp_dir("sm");
		r68::write_file(_dir / "sessions.json", kR68Sessions);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path initStateCfg(const std::string& content)
	{
		fs::path p = _dir / "state.json";
		r68::write_file(p, content);
		return p;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

// StateMonitor.cpp:56-59 —— WTSCfgLoader 的 json_to_variant 对 kNullType 直接跳过，
// memberNames() 里不会出现该 key；同一份配置里的合法兄弟节点照常装配。
TEST_F(R68StateMonitorInputsTest, JsonNullMemberIsTreatedAsAbsent)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({
		"SSE": null,
		"SHFE": {"inittime": 2045, "closetime": 1530, "proctime": 1600}
	})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	// null 的 SSE 没有被装配（不是「装配成 0 点时间」）
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

// StateMonitor.cpp:68-74 —— 会话值不是对象（数字）时 getUInt32 取不到键，
// 三个时间字段退回 0；会话模板存在，所以仍会构造 StateInfo 并保持 SS_ORIGINAL。
TEST_F(R68StateMonitorInputsTest, NumericSessionEntryDefaultsTimesToZero)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({"SSE": 12345})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("SSE", SS_ORIGINAL));
	// 大小写/尾随空格不同即为另一个 key（_map 用 std::string 精确匹配）
	EXPECT_FALSE(mon.isInState("sse", SS_ORIGINAL));
	EXPECT_FALSE(mon.isInState("SSE ", SS_ORIGINAL));
	EXPECT_FALSE(mon.isAnyInState(SS_RECEIVING));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));
}

// StateMonitor.cpp:68-74 —— 布尔会话值同样落到默认时间（getUInt32 → defVal=0），
// 且夜盘模板的集合竞价/交易区间换算循环仍会执行。
TEST_F(R68StateMonitorInputsTest, BooleanSessionEntryDefaultsTimesToZero)
{
	StateMonitor mon;
	fs::path cfg = initStateCfg(R"JSON({"SHFE": true})JSON");
	ASSERT_TRUE(mon.initialize(cfg.string().c_str(), &_bdMgr, nullptr));

	EXPECT_TRUE(mon.isInState("SHFE", SS_ORIGINAL));
	EXPECT_TRUE(mon.isAnyInState(SS_ORIGINAL));
	// 时间字段全 0 也不会在 initialize 阶段推进任何状态
	EXPECT_FALSE(mon.isAnyInState(SS_INITIALIZED));
	EXPECT_FALSE(mon.isAnyInState(SS_PAUSED));
}

// StateMonitor.cpp:38-47 —— 空串文件名走 StdFile::exists("")==false 的 error 分支，
// 返回 false 且状态表保持为空（与「文件不存在」同一守卫）。
TEST_F(R68StateMonitorInputsTest, EmptyFilenameReturnsFalseBeforeConfigLoad)
{
	StateMonitor mon;
	EXPECT_FALSE(mon.initialize("", &_bdMgr, nullptr));

	EXPECT_FALSE(mon.isAnyInState(SS_ORIGINAL));
	EXPECT_TRUE(mon.isAllInState(SS_ORIGINAL));	// 空表上 isAllInState 恒真
	EXPECT_FALSE(mon.isInState("SSE", SS_ORIGINAL));
}

}	// namespace
