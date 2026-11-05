/*!
 * 迭代03 K59（BUG-I02-NEW-026，用户裁定 2026-11-06）：StateMonitor 的周末规则，用固定日期/时间驱动，
 * 不依赖运行当天是星期几。
 *
 * 合同：
 *  - 普通会话（offset ≤ 0）：周六、周日为假日；工作日（非节假日）不是假日。
 *  - 正偏移会话（夜盘归下一交易日）：周六凌晨、属于周五夜盘延续且在交易时段内 → 正常（不是假日）；
 *    周六收盘后、周六白天、周日 → 假日。
 *  - 周五是节假日时，周五夜盘不存在，周六凌晨也是假日。
 *
 * 驱动：每个时点新建一个监控器，从 SS_ORIGINAL 推进一次（advanceOnce 只读传入的日期/时间），
 * 断言是否进入 SS_Holiday。日期：2026-10-30 周五、10-31 周六、11-01 周日、11-02 周一。
 */
#include <gtest/gtest.h>

#include <string>

#include <WtDtCore/StateMonitor.h>

#include "r154_test_util.h"

// 与 r154_state_monitor_states_test.cpp 中的定义逐字相同（StateMonitor 的友元）。
struct R154StateMonitorTestAccess
{
	static void advanceOnce(StateMonitor& monitor, uint32_t date, uint32_t minute)
	{
		monitor.advanceOnce(date, minute);
	}
};

namespace {

constexpr uint32_t kFri = 20261030;
constexpr uint32_t kSat = 20261031;
constexpr uint32_t kSun = 20261101;
constexpr uint32_t kMon = 20261102;

const char* kSessions = R"JSON({
	"K59DAY": {
		"name": "日盘", "offset": 0,
		"auction": {"from": 859, "to": 900},
		"sections": [{"from": 900, "to": 1015}, {"from": 1030, "to": 1130}, {"from": 1330, "to": 1500}]
	},
	"K59NIGHT": {
		"name": "夜盘至0230", "offset": 300,
		"auction": {"from": 2059, "to": 2100},
		"sections": [{"from": 2100, "to": 230}, {"from": 900, "to": 1015}, {"from": 1030, "to": 1130}, {"from": 1330, "to": 1500}]
	},
	"K59NIGHTFH": {
		"name": "夜盘至0230（周五节假日）", "offset": 300,
		"auction": {"from": 2059, "to": 2100},
		"sections": [{"from": 2100, "to": 230}, {"from": 900, "to": 1015}, {"from": 1030, "to": 1130}, {"from": 1330, "to": 1500}]
	}
})JSON";

const char* kCommodities = R"JSON({
	"SHFE": {
		"KD": {"name": "日盘品种", "session": "K59DAY", "holiday": "K59_NONE", "pricetick": 1, "volscale": 1},
		"KN": {"name": "夜盘品种", "session": "K59NIGHT", "holiday": "K59_NONE", "pricetick": 1, "volscale": 1},
		"KF": {"name": "夜盘品种（周五节假日）", "session": "K59NIGHTFH", "holiday": "K59_FRIHOL", "pricetick": 1, "volscale": 1}
	}
})JSON";

const char* kStates = R"JSON({
	"K59DAY": {"inittime": 830, "closetime": 1515, "proctime": 1600},
	"K59NIGHT": {"inittime": 2030, "closetime": 1515, "proctime": 1600},
	"K59NIGHTFH": {"inittime": 2030, "closetime": 1515, "proctime": 1600}
})JSON";

class I03K59WeekendTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r154::make_tmp_dir("k59");
		r154::write_file(_dir / "sessions.json", kSessions);
		r154::write_file(_dir / "commodities.json", kCommodities);
		r154::write_file(_dir / "holidays.json", R"JSON({"K59_NONE": [], "K59_FRIHOL": [20261030]})JSON");
		r154::write_file(_dir / "state.json", kStates);
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadHolidays((_dir / "holidays.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		r154::fs::remove_all(_dir, ec);
	}

	// 新监控器从 SS_ORIGINAL 推进一次后，该会话是否为假日
	bool holidayAt(const char* sid, uint32_t date, uint32_t minute)
	{
		StateMonitor mon;
		EXPECT_TRUE(mon.initialize((_dir / "state.json").string().c_str(), &_bdMgr, &_dtMgr));
		R154StateMonitorTestAccess::advanceOnce(mon, date, minute);
		SimpleState state = SS_ORIGINAL;
		EXPECT_TRUE(mon.tryGetRecordedState(sid, state)) << sid;
		return state == SS_Holiday;
	}

	r154::fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	DataManager _dtMgr;
};

} // namespace

TEST_F(I03K59WeekendTest, DaySessionWeekendIsHoliday)
{
	EXPECT_FALSE(holidayAt("K59DAY", kFri, 1000));
	EXPECT_TRUE(holidayAt("K59DAY", kSat, 100));
	EXPECT_TRUE(holidayAt("K59DAY", kSat, 1000));
	EXPECT_TRUE(holidayAt("K59DAY", kSun, 1000));
	EXPECT_FALSE(holidayAt("K59DAY", kMon, 1000));
}

TEST_F(I03K59WeekendTest, FridayNightContinuationOnSaturdayIsTrading)
{
	EXPECT_FALSE(holidayAt("K59NIGHT", kFri, 2200));	// 周五夜盘
	EXPECT_FALSE(holidayAt("K59NIGHT", kSat, 100));		// 周六凌晨，周五夜盘延续
	EXPECT_FALSE(holidayAt("K59NIGHT", kSat, 220));		// 收盘前
}

TEST_F(I03K59WeekendTest, RestOfWeekendIsHolidayForNightSession)
{
	EXPECT_TRUE(holidayAt("K59NIGHT", kSat, 300));		// 周六夜盘收盘后
	EXPECT_TRUE(holidayAt("K59NIGHT", kSat, 1000));		// 周六白天
	EXPECT_TRUE(holidayAt("K59NIGHT", kSat, 2200));		// 周六晚上没有夜盘
	EXPECT_TRUE(holidayAt("K59NIGHT", kSun, 1000));		// 周日白天
	EXPECT_TRUE(holidayAt("K59NIGHT", kSun, 2200));		// 周日晚上
	EXPECT_FALSE(holidayAt("K59NIGHT", kMon, 1000));	// 周一日盘
}

TEST_F(I03K59WeekendTest, FridayHolidayMeansNoSaturdayContinuation)
{
	EXPECT_TRUE(holidayAt("K59NIGHTFH", kFri, 2200));
	EXPECT_TRUE(holidayAt("K59NIGHTFH", kSat, 100));
	EXPECT_TRUE(holidayAt("K59NIGHTFH", kSat, 220));
}
