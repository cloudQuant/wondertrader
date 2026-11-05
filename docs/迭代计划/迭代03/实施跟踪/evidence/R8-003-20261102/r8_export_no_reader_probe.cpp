#include <gtest/gtest.h>
#include <Includes/WTSStruct.h>
#include <WtDtServo/WtDtServo.h>

namespace {
int bars = 0, ticks = 0, counts = 0;
void onBars(WTSBarStruct*, WtUInt32, bool) { ++bars; }
void onTicks(WTSTickStruct*, WtUInt32, bool) { ++ticks; }
void onCount(WtUInt32) { ++counts; }
void assertEmpty() {
    EXPECT_EQ(0u, get_bars_by_range("CFFEX.IC.2204", "m1", 202401020930, 202401021500, onBars, onCount));
    EXPECT_EQ(0u, get_bars_by_date("CFFEX.IC.2204", "m1", 20240102, onBars, onCount));
    EXPECT_EQ(0u, get_bars_by_count("CFFEX.IC.2204", "m1", 10, 202401021500, onBars, onCount));
    EXPECT_EQ(0u, get_ticks_by_range("CFFEX.IC.2204", 202401020930, 202401021500, onTicks, onCount));
    EXPECT_EQ(0u, get_ticks_by_date("CFFEX.IC.2204", 20240102, onTicks, onCount));
    EXPECT_EQ(0u, get_ticks_by_count("CFFEX.IC.2204", 10, 202401021500, onTicks, onCount));
    EXPECT_EQ(0u, get_sbars_by_date("CFFEX.IC.2204", 30, 20240102, onBars, onCount));
    EXPECT_EQ(0, bars); EXPECT_EQ(0, ticks); EXPECT_EQ(0, counts);
}
TEST(R8ExportNoReaderProbe, NoStoreInitializedQueriesReturnZeroNoCallbacks) {
    ASSERT_FALSE(is_initialized());
    initialize(R"({"disable_dump":true,"basefiles":{},"data":{}})", false, "/no/such/wtdt-log.yaml", nullptr, nullptr);
    ASSERT_TRUE(is_initialized());
    assertEmpty();
}
TEST(R8ExportNoReaderProbe, UnavailableStoreFailsInitializationAndQueriesReturnZeroNoCallbacks) {
    ASSERT_FALSE(is_initialized());
    initialize(R"({"disable_dump":true,"basefiles":{},"data":{"store":{"module":"r8_never_exists"}}})", false, "/no/such/wtdt-log.yaml", nullptr, nullptr);
    ASSERT_FALSE(is_initialized());
    assertEmpty();
}
}
