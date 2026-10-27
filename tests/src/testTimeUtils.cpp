#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "Share/TimeUtils.hpp"
#include <chrono>
#include <thread>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>

// Test fixture for TimeUtils
class TimeUtilsTest : public ::testing::Test {
protected:
    // You can set up any common resources here if needed
    TimeUtilsTest() {}

    // You can tear down common resources here if needed
    ~TimeUtilsTest() override {}
};

// Test case for getLocalTimeNowOld function
TEST_F(TimeUtilsTest, GetLocalTimeNowOldTest) {
    // Call the method under test
    int64_t result = TimeUtils::getLocalTimeNowOld();
    std::cout << "TimeUtils::getLocalTimeNowOld() = " << result << std::endl;
    EXPECT_GT(result, 0);
}

// Test case for getLocalTimeNow function (non-Windows platform using clock_gettime)
TEST_F(TimeUtilsTest, GetLocalTimeNowTest) {
    // Call the method under test
    int64_t result = TimeUtils::getLocalTimeNow();
    std::cout << "TimeUtils::getLocalTimeNow() = " << result << std::endl;
    EXPECT_GT(result, 0);
}

// 测试 getLocalTime 函数，包含毫秒
TEST_F(TimeUtilsTest, GetLocalTimeWithMilliseconds) {
    // 获取当前时间
    std::string result = TimeUtils::getLocalTime(true);
    std::cout << "TimeUtils::getLocalTime(true) = " << result << std::endl;
    // 检查格式是否符合 "hh:mm:ss,SSS" 格式
    ASSERT_EQ(result.size(), 12); // 格式应该是 8 (hh:mm:ss) + 1 (',') + 3 (SSS)

    // 检查小时、分钟、秒和毫秒部分是否合理
    int hour, min, sec, millisec;
    int items = sscanf(result.c_str(), "%02d:%02d:%02d,%03d", &hour, &min, &sec, &millisec);

    ASSERT_EQ(items, 4);  // 必须成功解析4个整数
    ASSERT_GE(hour, 0);
    ASSERT_LT(hour, 24);
    ASSERT_GE(min, 0);
    ASSERT_LT(min, 60);
    ASSERT_GE(sec, 0);
    ASSERT_LT(sec, 60);
    ASSERT_GE(millisec, 0);
    ASSERT_LT(millisec, 1000);
}

// 测试 getLocalTime 函数，不包含毫秒
TEST_F(TimeUtilsTest, GetLocalTimeWithoutMilliseconds) {
    // 获取当前时间
    std::string result = TimeUtils::getLocalTime(false);
    std::cout << "TimeUtils::getLocalTime(false) = " << result << std::endl;
    // 检查格式是否符合 "hh:mm:ss" 格式
    ASSERT_EQ(result.size(), 8);  // 格式应该是 8 (hh:mm:ss)

    // 检查小时、分钟、秒是否合理
    int hour, min, sec;
    int items = sscanf(result.c_str(), "%02d:%02d:%02d", &hour, &min, &sec);

    ASSERT_EQ(items, 3);  // 必须成功解析3个整数
    ASSERT_GE(hour, 0);
    ASSERT_LT(hour, 24);
    ASSERT_GE(min, 0);
    ASSERT_LT(min, 60);
    ASSERT_GE(sec, 0);
    ASSERT_LT(sec, 60);
}

// 测试 getYYYYMMDDhhmmss 函数
TEST_F(TimeUtilsTest, GetYYYYMMDDhhmmss) {
    // 获取当前日期时间（格式：YYYYMMDDhhmmss）
    uint64_t result = TimeUtils::getYYYYMMDDhhmmss();
    std::cout << "TimeUtils::getYYYYMMDDhhmmss() = " << result << std::endl;
    // 检查返回值的格式
    // 日期部分应该是 8 位数（YYYYMMDD）
    // 时间部分应该是 6 位数（hhmmss）
    ASSERT_GE(result, 10000000000000ULL);  // 应该至少为 14 位
    ASSERT_LT(result, 100000000000000ULL); // 应该小于 16 位

    // 提取日期部分（YYYYMMDD）
    uint64_t date = result / 1000000;
    // 提取时间部分（hhmmss）
    uint64_t time = result % 1000000;

    // 检查日期部分的范围（YYYYMMDD）
    uint64_t year = date / 10000;
    uint64_t month = (date / 100) % 100;
    uint64_t day = date % 100;

    ASSERT_GE(year, 1900);  // 年份应该大于等于 1900
    ASSERT_LE(month, 12);   // 月份应该小于等于 12
    ASSERT_GE(month, 1);    // 月份应该大于等于 1
    ASSERT_LE(day, 31);     // 日期应该小于等于 31
    ASSERT_GE(day, 1);      // 日期应该大于等于 1

    // 检查时间部分的范围（hhmmss）
    uint64_t hour = time / 10000;
    uint64_t minute = (time / 100) % 100;
    uint64_t second = time % 100;

    ASSERT_GE(hour, 0);     // 小时应该大于等于 0
    ASSERT_LT(hour, 24);    // 小时应该小于 24
    ASSERT_GE(minute, 0);   // 分钟应该大于等于 0
    ASSERT_LT(minute, 60);  // 分钟应该小于 60
    ASSERT_GE(second, 0);   // 秒数应该大于等于 0
    ASSERT_LT(second, 60);  // 秒数应该小于 60
}

// 测试 getDateTime 函数
TEST_F(TimeUtilsTest, GetDateTimeFormat) {
    uint32_t date = 0, time = 0;

    // 调用 getDateTime 函数
    TimeUtils::getDateTime(date, time);
    std::cout << "TimeUtils::getDateTimeFormat() = " << date << " " << time << std::endl;
    // 检查返回的日期格式（YYYYMMDD）
    uint32_t year = date / 10000;
    uint32_t month = (date / 100) % 100;
    uint32_t day = date % 100;

    ASSERT_GE(year, 1900);  // 年份应该大于等于 1900
    ASSERT_LE(month, 12);   // 月份应该小于等于 12
    ASSERT_GE(month, 1);    // 月份应该大于等于 1
    ASSERT_LE(day, 31);     // 日期应该小于等于 31
    ASSERT_GE(day, 1);      // 日期应该大于等于 1

    // 检查返回的时间格式（hhmmssSSS）
    uint32_t hour = time / 10000000;
    uint32_t minute = (time / 100000) % 100;
    uint32_t second = (time / 1000) % 100;
    uint32_t millisecond = time % 1000;

    ASSERT_GE(hour, 0);     // 小时应该大于等于 0
    ASSERT_LT(hour, 24);    // 小时应该小于 24
    ASSERT_GE(minute, 0);   // 分钟应该大于等于 0
    ASSERT_LT(minute, 60);  // 分钟应该小于 60
    ASSERT_GE(second, 0);   // 秒数应该大于等于 0
    ASSERT_LT(second, 60);  // 秒数应该小于 60
    ASSERT_GE(millisecond, 0);   // 毫秒数应该大于等于 0
    ASSERT_LT(millisecond, 1000);  // 毫秒数应该小于 1000
}


// 测试 getCurDate 函数
TEST_F(TimeUtilsTest, GetCurDateFormat) {
    uint32_t date = 0;

    // 调用 getCurDate 函数
    date = TimeUtils::getCurDate();
    std::cout << "TimeUtils::getCurDate() = " << date << std::endl;
    // 检查返回的日期格式（YYYYMMDD）
    uint32_t year = date / 10000;
    uint32_t month = (date / 100) % 100;
    uint32_t day = date % 100;

    ASSERT_GE(year, 1900);  // 年份应该大于等于 1900
    ASSERT_LE(month, 12);   // 月份应该小于等于 12
    ASSERT_GE(month, 1);    // 月份应该大于等于 1
    ASSERT_LE(day, 31);     // 日期应该小于等于 31
    ASSERT_GE(day, 1);      // 日期应该大于等于 1
}

// 测试 getCurDate 边界条件
TEST_F(TimeUtilsTest, GetCurDateBoundary) {
    uint32_t date = 0;
    // 调用 getCurDate 函数并验证返回结果
    date = TimeUtils::getCurDate();
    ASSERT_GE(date, 20241216); // 应该返回 2023-01-31
}

TEST_F(TimeUtilsTest, NowDateSpecificDate) {
    uint32_t weekday = TimeUtils::getWeekDay(); // 当前日期
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 将当前时间点转换为时间
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);

    // 转换为本地时间
    std::tm* local_time = std::localtime(&current_time);

    // 输出星期几
    std::cout << "Today is: " << local_time->tm_wday << std::endl;
    // 2023年12月16日是星期六
    EXPECT_EQ(weekday, local_time->tm_wday); // 6 代表星期六
}

// 测试用例2：测试特定日期的星期几
TEST_F(TimeUtilsTest, SpecificDate) {
    uint32_t weekday = TimeUtils::getWeekDay(20231216); // 传入 2023年12月16日
    // 2023年12月16日是星期六
    EXPECT_EQ(weekday, 6); // 6 代表星期六
}

// 测试用例3：测试不同的日期，验证不同星期几的返回值
TEST_F(TimeUtilsTest, AnotherSpecificDate) {
    uint32_t weekday = TimeUtils::getWeekDay(20231218); // 传入 2023年12月18日
    // 2023年12月18日是星期一
    EXPECT_EQ(weekday, 1); // 1 代表星期一
}

// 测试用例1：测试 getCurMin 返回当前时间（格式：HHMMSS）
TEST_F(TimeUtilsTest, GetCurMinTest_CurrentTime) {
    // 获取当前时间戳
    uint64_t current_time = TimeUtils::getLocalTimeNow();
    time_t now = current_time / 1000;
    tm * tNow = localtime(&now);
    uint32_t expected_time = tNow->tm_hour * 10000 + tNow->tm_min * 100 + tNow->tm_sec;

    // 使用 getCurMin 获取当前时间
    uint32_t curMin = TimeUtils::getCurMin();
    std::cout << "Current time is: " << curMin << std::endl;
    // 验证返回值是否与当前时间一致
    EXPECT_LE(curMin, expected_time);
}

// 测试用例1：测试 getTZOffset 在当前时区环境下返回正确的时区偏移量
TEST_F(TimeUtilsTest, GetTZOffsetTest) {
    // 获取当前时区偏移量
    int32_t offset = TimeUtils::getTZOffset();

    // 由于时区偏移量是系统相关的，这里我们只验证返回值合理
    // 偏移量通常在 -12 到 +14 之间，具体范围视所在地区和夏令时规则而定
    EXPECT_GE(offset, -12);  // 偏移量不应小于 -12 小时
    EXPECT_LE(offset, 14);   // 偏移量不应大于 +14 小时
}

// 测试用例2：验证缓存机制，第一次调用后再调用时，offset不改变
TEST_F(TimeUtilsTest, GetTZOffsetCacheTest) {
    // 第一次调用，计算并存储偏移量
    int32_t first_offset = TimeUtils::getTZOffset();

    // 第二次调用，应该返回相同的值，验证缓存
    int32_t second_offset = TimeUtils::getTZOffset();

    EXPECT_EQ(first_offset, second_offset);  // 偏移量应相等
}




