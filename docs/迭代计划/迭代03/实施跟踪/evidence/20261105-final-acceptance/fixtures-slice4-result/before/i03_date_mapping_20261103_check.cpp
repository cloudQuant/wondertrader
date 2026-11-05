#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSSessionInfo.hpp"
#include "Share/TimeUtils.hpp"
using namespace wtp;

static std::string config(const std::string& content)
{
    char name[] = "/tmp/wt-date-v1-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return {};
    close(fd);
    std::string path = std::string(name) + ".json";
    if (rename(name, path.c_str()) != 0) return {};
    std::ofstream out(path); out << content;
    return path;
}
class DateMappingTest : public ::testing::Test {
protected:
    WTSBaseDataMgr mgr;
    void SetUp() override {
        static bool logger = [] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr); return true; }();
        (void)logger;
        auto sessions = config(R"({"POS":{"name":"pos","offset":300,"sections":[{"from":2100,"to":2300}]},"NEG":{"name":"neg","offset":-300,"sections":[{"from":2100,"to":2300}]},"ALLP":{"name":"allp","offset":300,"sections":[{"from":0,"to":0}]},"ALLN":{"name":"alln","offset":-300,"sections":[{"from":0,"to":0}]},"EMPTY":{"name":"empty","sections":[]}})");
        auto holidays = config(R"({"CHINA":[20240226,99991227,99991228,99991229,99991230,99991231]})");
        auto commodities = config(R"({"EX":{"P":{"name":"pos","session":"POS","holiday":"CHINA","pricetick":1,"volscale":1},"N":{"name":"neg","session":"NEG","holiday":"CHINA","pricetick":1,"volscale":1},"M":{"name":"missing","session":"POS","holiday":"MISSING","pricetick":1,"volscale":1}}})");
        ASSERT_TRUE(mgr.loadSessions(sessions.c_str()));
        ASSERT_TRUE(mgr.loadHolidays(holidays.c_str()));
        ASSERT_TRUE(mgr.loadCommodities(commodities.c_str()));
        unlink(sessions.c_str()); unlink(holidays.c_str()); unlink(commodities.c_str());
    }
};
TEST_F(DateMappingTest, VersionedTableAndInvalidInputs)
{
    auto api = mgr.getDateMappingV1();
    ASSERT_EQ(sizeof(DateMappingV1), api.size);
    ASSERT_EQ(1u, api.version);
    ASSERT_EQ(&mgr, api.context);
    auto wrong = api;
    wrong.context = nullptr;
    wrong.size--;
    EXPECT_EQ(DateMappingStatus::Unsupported, api.calcTradingDate(&wrong, "POS", 20240229, 900, 1).status);
    EXPECT_EQ(DateMappingStatus::Unsupported, api.getBoundaryTime(&wrong, "POS", 20240229, 1, 1).status);
    wrong.size = sizeof(DateMappingV1);
    wrong.version++;
    EXPECT_EQ(DateMappingStatus::Unsupported, api.getTradingDate(&wrong, "CHINA", 20240229, 0, 1).status);
    EXPECT_EQ(DateMappingStatus::Unsupported, api.nextTradingDate(&wrong, "CHINA", 20240229, 1, 1).status);
    EXPECT_EQ(DateMappingStatus::Unsupported, api.prevTradingDate(&wrong, "CHINA", 20240229, 1, 1).status);
    for (auto r : {api.calcTradingDate(&api, "POS", 20240230, 900, 1),
                   api.calcTradingDate(&api, "POS", 20241301, 900, 1)}) {
        EXPECT_EQ(DateMappingStatus::InvalidDate, r.status); EXPECT_EQ(0u, r.value);
    }
    auto badTime = api.calcTradingDate(&api, "ALLP", 20240229, 1260, 1);
    EXPECT_EQ(DateMappingStatus::InvalidTime, badTime.status); EXPECT_EQ(0u, badTime.value);
    EXPECT_EQ(DateMappingStatus::InvalidTime, api.calcTradingDate(&api, "NO_SESSION", 20240229, 2401, 1).status);
    EXPECT_EQ(DateMappingStatus::InvalidArgument, api.calcTradingDate(nullptr, "POS", 20240229, 900, 1).status);
    EXPECT_EQ(DateMappingStatus::InvalidArgument, api.calcTradingDate(&api, nullptr, 20240229, 900, 1).status);
    EXPECT_EQ(DateMappingStatus::InvalidArgument, api.calcTradingDate(&api, "POS", 20240229, 900, 2).status);
    EXPECT_EQ(DateMappingStatus::OutOfRange, api.calcTradingDate(&api, "POS", 9991231, 900, 1).status);
    EXPECT_EQ(DateMappingStatus::Ok, api.calcTradingDate(&api, "POS", 20240229, 2400, 1).status);
    EXPECT_EQ(DateMappingStatus::InvalidArgument, api.nextTradingDate(&api, "CHINA", 20240229, 0, 1).status);
    EXPECT_EQ(20240227u, api.nextTradingDate(&api, "CHINA", 20240223, 1, 1).value);
    EXPECT_EQ(20240223u, api.prevTradingDate(&api, "CHINA", 20240227, 1, 1).value);
    EXPECT_EQ(202402232100ULL, api.getBoundaryTime(&api, "POS", 20240227, 1, 1).value);
    EXPECT_EQ(20240229u, api.getTradingDate(&api, "CHINA", 20240229, 0, 1).value);
    EXPECT_EQ(0u, mgr.calcTradingDate(nullptr, 20240229, 900, true));
    EXPECT_EQ(0u, mgr.getBoundaryTime("POS", 20240230, true, true));
}
TEST_F(DateMappingTest, MissingMetadataAndCalendar)
{
    EXPECT_EQ(DateMappingStatus::MissingCommodity, mgr.checkedCalcTradingDate("EX.UNKNOWN", 20240229, 900).status);
    EXPECT_EQ(DateMappingStatus::MissingSession, mgr.checkedCalcTradingDate("UNKNOWN", 20240229, 900, true).status);
    EXPECT_EQ(DateMappingStatus::MissingSession, mgr.checkedCalcTradingDate("EMPTY", 20240229, 900, true).status);
    EXPECT_EQ(DateMappingStatus::MissingCalendar, mgr.checkedCalcTradingDate("EX.M", 20240229, 900).status);
    EXPECT_EQ(DateMappingStatus::MissingCalendar, mgr.checkedGetTradingDate("MISSING", 20240229, 0, true).status);
    WTSBaseDataMgr empty;
    auto missing = empty.checkedGetTradingDate("CHINA", 0, 0, true);
    EXPECT_EQ(DateMappingStatus::MissingCalendar, missing.status); EXPECT_EQ(0u, missing.value);
}
TEST_F(DateMappingTest, TradingDaysAndBounds)
{
    auto next = mgr.checkedNextTradingDate("CHINA", 20240223, 1, true);
    ASSERT_EQ(DateMappingStatus::Ok, next.status); EXPECT_EQ(20240227u, next.value);
    EXPECT_EQ(20240223u, mgr.checkedPrevTradingDate("CHINA", 20240227, 1, true).value);
    EXPECT_EQ(20240301u, mgr.checkedNextTradingDate("CHINA", 20240229, 1, true).value);
    EXPECT_EQ(20240311u, mgr.checkedNextTradingDate("CHINA", 20240308, 1, true).value);
    EXPECT_EQ(DateMappingStatus::OutOfRange, mgr.checkedNextTradingDate("CHINA", 99991231, 1, true).status);
    EXPECT_EQ(DateMappingStatus::OutOfRange, mgr.checkedPrevTradingDate("CHINA", 10000101, 1, true).status);
    EXPECT_EQ(DateMappingStatus::OutOfRange, mgr.checkedCalcTradingDate("ALLP", 99991231, 2100, true).status);
    EXPECT_EQ(DateMappingStatus::OutOfRange, mgr.checkedGetBoundaryTime("NEG", 99991231, true, false).status);
}
TEST_F(DateMappingTest, OffsetSessionAndFullDayCompatibility)
{
    EXPECT_EQ(20240227u, mgr.checkedCalcTradingDate("EX.P", 20240223, 2100).value);
    EXPECT_EQ(20240226u, mgr.checkedCalcTradingDate("EX.P", 20240226, 900).value);
    EXPECT_EQ(20240226u, mgr.checkedGetTradingDate("CHINA", 20240226, 0, true).value);
    EXPECT_EQ(20240223u, mgr.checkedCalcTradingDate("EX.N", 20240224, 100).value);
    EXPECT_EQ(20240227u, mgr.checkedCalcTradingDate("EX.N", 20240224, 900).value);
    EXPECT_EQ(20240224u, mgr.checkedCalcTradingDate("ALLP", 20240223, 2100, true).value);
    EXPECT_EQ(20240223u, mgr.checkedCalcTradingDate("ALLN", 20240224, 100, true).value);
    EXPECT_EQ(202402232100ULL, mgr.checkedGetBoundaryTime("POS", 20240227, true, true).value);
    EXPECT_EQ(202402242300ULL, mgr.checkedGetBoundaryTime("NEG", 20240223, true, false).value);
    for (auto d : {20240222u, 20240223u, 20240227u}) {
        for (auto t : {900u, 2100u, 2300u}) {
            auto actual = mgr.checkedCalcTradingDate("EX.P", d, t);
            ASSERT_EQ(DateMappingStatus::Ok, actual.status);
            EXPECT_EQ(mgr.calcTradingDate("EX.P", d, t), actual.value);
        }
        EXPECT_EQ(mgr.getBoundaryTime("EX.P", d, false, false), mgr.checkedGetBoundaryTime("EX.P", d, false, false).value);
    }
}
TEST_F(DateMappingTest, CacheCurrentAndFailurePublication)
{
    mgr.setTradingDate("CHINA", 20240220, true);
    EXPECT_EQ(20240220u, mgr.checkedGetTradingDate("CHINA", 0, 0, true).value);
    EXPECT_EQ(DateMappingStatus::InvalidDate, mgr.checkedGetTradingDate("CHINA", 20240230, 0, true).status);
    EXPECT_EQ(DateMappingStatus::InvalidTime, mgr.checkedGetTradingDate("CHINA", 0, 1260, true).status);
    EXPECT_EQ(DateMappingStatus::OutOfRange, mgr.checkedGetTradingDate("CHINA", 99991226, 0, true).status);
    EXPECT_EQ(20240220u, mgr.checkedGetTradingDate("CHINA", 0, 0, true).value);
    EXPECT_EQ(20240227u, mgr.checkedGetTradingDate("CHINA", 20240224, 0, true).value);
    EXPECT_EQ(20240227u, mgr.checkedGetTradingDate("CHINA", 0, 0, true).value);
    mgr.setTradingDate("CHINA", 0, true);
    uint32_t beforeDate, beforeTime, afterDate, afterTime;
    ASSERT_TRUE(TimeUtils::tryGetDateTime(beforeDate, beforeTime));
    auto current = mgr.checkedGetTradingDate("CHINA", 0, 0, true);
    ASSERT_TRUE(TimeUtils::tryGetDateTime(afterDate, afterTime));
    ASSERT_EQ(DateMappingStatus::Ok, current.status);
    const auto expected = [&](uint32_t date) {
        uint32_t weekday = TimeUtils::getWeekDay(date);
        return weekday == 0 || weekday == 6 ? mgr.checkedNextTradingDate("CHINA", date, 1, true).value : date;
    };
    EXPECT_TRUE(current.value == expected(beforeDate) || current.value == expected(afterDate));
    EXPECT_EQ(DateMappingStatus::Ok, mgr.checkedCalcTradingDate("POS", 0, 0, true).status);
    EXPECT_EQ(DateMappingStatus::Ok, mgr.checkedGetBoundaryTime("POS", 0, true, false).status);
}
