/*!
 * \file r174_state_monitor_queries_test.cpp
 * \brief StateMonitor.h 纯查询/区间边界的 out-of-line 覆盖（迭代01 第二十五波 R174）
 *
 * 背景：q05/r18 已经调用过 isAnyInState/isAllInState/isInState/isInSections，
 * 但它们在类内定义、调用点被前端内联，profdata 里独立的函数记录始终为 0
 * （最新报表 StateMonitor.h 49/57 未覆盖）。本文件通过**成员函数指针**发起
 * 间接调用，强制链接器保留的 out-of-line 副本真正执行，不依赖墙钟/线程/socket：
 *   StateMonitor.h:70-81   StateInfo::isInSections 半开区间与空区间
 *   StateMonitor.h:169-182 StateMonitor::isAnyInState 命中/全表未命中
 *   StateMonitor.h:189-203 StateMonitor::isAllInState 全会话命中/首个不命中
 *   StateMonitor.h:211-222 StateMonitor::isInState 已存在/不存在 sid
 *
 * initialize 只装配 StateInfo（_state 保持 SS_ORIGINAL），不启动 run()。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>

#include "r174_test_util.h"

namespace {

// 两个确实存在的会话模板；无关联品种，initialize 不会读取当前日期/时间。
const char* kSessions = R"JSON({
	"Q1": {
		"name": "日盘会话", "offset": 0,
		"auction": {"from": 915, "to": 925},
		"sections": [{"from": 930, "to": 1130}, {"from": 1300, "to": 1500}]
	},
	"Q2": {
		"name": "夜盘会话", "offset": -120,
		"auctions": [{"from": 2055, "to": 2100}, {"from": 855, "to": 900}],
		"sections": [{"from": 2100, "to": 2300}, {"from": 900, "to": 1015}]
	}
})JSON";

const char* kStateCfg = R"JSON({
	"Q1": {"inittime": 900, "closetime": 1510, "proctime": 1530},
	"Q2": {"inittime": 2000, "closetime": 2300, "proctime": 2310}
})JSON";

class R174StateMonitorQueryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r174::make_tmp_dir("sm_queries");
		const std::filesystem::path sessions = _dir / "sessions.json";
		r174::write_file(sessions, kSessions);
		ASSERT_TRUE(_bdMgr.loadSessions(sessions.string().c_str()));

		_stateFile = _dir / "state.json";
		r174::write_file(_stateFile, kStateCfg);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
	std::filesystem::path _stateFile;
	WTSBaseDataMgr _bdMgr;
};

/*
 * StateMonitor.h:70-81
 * 两段区间 [930,1130)、[1300,1500)：左闭右开；空 _sections 恒 false。
 * 通过成员函数指针调用，避免前端把整个循环内联进测试函数。
 */
TEST(R174StateInfoSectionsTest, HalfOpenBoundsAndEmptySections)
{
	StateInfo empty;
	bool (StateInfo::*inSections)(uint32_t) = &StateInfo::isInSections;
	EXPECT_TRUE(empty._sections.empty());
	EXPECT_FALSE((empty.*inSections)(930));
	EXPECT_FALSE((empty.*inSections)(0));

	StateInfo info;
	info._sections.push_back(StateInfo::Section{930, 1130});
	info._sections.push_back(StateInfo::Section{1300, 1500});
	EXPECT_FALSE((info.*inSections)(929));
	EXPECT_TRUE((info.*inSections)(930));
	EXPECT_TRUE((info.*inSections)(1129));
	EXPECT_FALSE((info.*inSections)(1130));
	EXPECT_TRUE((info.*inSections)(1300));
	EXPECT_FALSE((info.*inSections)(1500));
	EXPECT_FALSE((info.*inSections)(2359));
}

/*
 * StateMonitor.h:169-222
 * 两个会话都处于 initialize 后的默认 SS_ORIGINAL：
 *  - isInState：命中且状态相等、命中但状态不等、sid 不存在三条路径；
 *  - isAnyInState：首个条目即命中 -> true；遍历全表无命中 -> false；
 *  - isAllInState：所有非 Holiday 条目都等于目标 -> true；首个不匹配 -> false。
 * 这里直接覆盖三函数的全部可执行行，包括 return true/false 两侧。
 */
TEST_F(R174StateMonitorQueryTest, InitializedMapHitAndMissThroughMemberPointers)
{
	StateMonitor mon;
	ASSERT_TRUE(mon.initialize(_stateFile.string().c_str(), &_bdMgr, nullptr));

	using InStateFn = bool (StateMonitor::*)(const char*, SimpleState) const;
	using AnyStateFn = bool (StateMonitor::*)(SimpleState) const;
	InStateFn inState = &StateMonitor::isInState;
	AnyStateFn anyState = &StateMonitor::isAnyInState;
	AnyStateFn allState = &StateMonitor::isAllInState;

	EXPECT_TRUE((mon.*inState)("Q1", SS_ORIGINAL));
	EXPECT_FALSE((mon.*inState)("Q1", SS_RECEIVING));
	EXPECT_FALSE((mon.*inState)("GHOST", SS_ORIGINAL));

	EXPECT_TRUE((mon.*anyState)(SS_ORIGINAL));
	EXPECT_FALSE((mon.*anyState)(SS_RECEIVING));

	EXPECT_TRUE((mon.*allState)(SS_ORIGINAL));
	EXPECT_FALSE((mon.*allState)(SS_RECEIVING));
	EXPECT_FALSE((mon.*allState)(SS_Holiday));
}

/*
 * StateMonitor.h:169-182、189-203、211-222
 * 空 _map：isAnyInState 走完整循环返回 false；isAllInState 空表 vacuously true；
 * isInState 在 find 直接命中 end() 返回 false。用于锁死空表边界。
 */
TEST(R174StateMonitorEmptyQueryTest, EmptyMapQueriesAreVacuous)
{
	StateMonitor mon;

	using InStateFn = bool (StateMonitor::*)(const char*, SimpleState) const;
	using AnyStateFn = bool (StateMonitor::*)(SimpleState) const;
	InStateFn inState = &StateMonitor::isInState;
	AnyStateFn anyState = &StateMonitor::isAnyInState;
	AnyStateFn allState = &StateMonitor::isAllInState;

	EXPECT_FALSE((mon.*anyState)(SS_ORIGINAL));
	EXPECT_TRUE((mon.*allState)(SS_ORIGINAL));
	EXPECT_TRUE((mon.*allState)(SS_Holiday));
	EXPECT_FALSE((mon.*inState)("Q1", SS_ORIGINAL));
	EXPECT_FALSE((mon.*inState)("", SS_ORIGINAL));
}

}	// namespace
