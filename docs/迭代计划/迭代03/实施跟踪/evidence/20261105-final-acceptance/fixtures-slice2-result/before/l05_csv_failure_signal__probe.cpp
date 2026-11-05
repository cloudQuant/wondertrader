#include <gtest/gtest.h>
#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

extern "C" void wt_dt_helper_fail_next_close_for_test();
namespace fs = std::filesystem;
static std::vector<std::string> captured_messages;
static void log_message(WtString message) { captured_messages.emplace_back(message ? message : ""); }
static bool has(const char* word) {
    for (const auto& message : captured_messages) if (message.find(word) != std::string::npos) return true;
    return false;
}
struct SizeLimit {
    struct rlimit original{};
    struct sigaction old_signal{};
    bool active = false;
    SizeLimit() {
        if (getrlimit(RLIMIT_FSIZE, &original)) return;
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        if (sigaction(SIGXFSZ, &ignore, &old_signal)) return;
        auto limit = original; limit.rlim_cur = 1;
        if (setrlimit(RLIMIT_FSIZE, &limit)) { sigaction(SIGXFSZ, &old_signal, nullptr); return; }
        active = true;
    }
    ~SizeLimit() { if (active) { setrlimit(RLIMIT_FSIZE, &original); sigaction(SIGXFSZ, &old_signal, nullptr); } }
};
class CsvSignal : public ::testing::TestWithParam<bool> {
protected:
    fs::path root, input, output;
    void SetUp() override {
        char name[] = "/tmp/wt-l05-csv-signal-XXXXXX";
        char* dir = mkdtemp(name); ASSERT_NE(dir, nullptr);
        root = dir; input = root / "input"; output = root / "output";
        ASSERT_TRUE(fs::create_directories(input)); ASSERT_TRUE(fs::create_directories(output));
        captured_messages.clear();
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
    void source(int count) {
        auto path = (input / "one.dsb").string();
        if (GetParam()) {
            WTSTickStruct tick{};
            std::strncpy(tick.exchg, "SHFE", sizeof(tick.exchg)-1);
            std::strncpy(tick.code, "rb2010", sizeof(tick.code)-1);
            tick.action_date = tick.trading_date = 20200116; tick.action_time = 93000; tick.price = 3000;
            std::vector<WTSTickStruct> ticks(count, tick);
            ASSERT_TRUE(store_ticks(path.c_str(), ticks.data(), count, nullptr));
        } else {
            WTSBarStruct bar{}; bar.date = 20200116; bar.open = 3000; bar.close = 3001;
            std::vector<WTSBarStruct> bars(count, bar);
            ASSERT_TRUE(store_bars(path.c_str(), bars.data(), count, "d", nullptr));
        }
    }
    void dump(FuncLogCallback logger) {
        auto in = input.string(), out = output.string();
        if (GetParam()) dump_ticks(in.c_str(), out.c_str(), "", logger);
        else dump_bars(in.c_str(), out.c_str(), "", logger);
    }
    std::string csv() {
        std::ifstream in(output / "one.csv", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
};
TEST_P(CsvSignal, NormalWithLoggerAndNullLogger) {
    source(1); dump(log_message);
    ASSERT_TRUE(has("写入完成")); ASSERT_TRUE(has("全部导出完成"));
    auto expected = csv(); ASSERT_FALSE(expected.empty());
    captured_messages.clear(); dump(nullptr); EXPECT_EQ(csv(), expected); EXPECT_TRUE(captured_messages.empty());
}
TEST_P(CsvSignal, CloseFailureWithLogger) {
    source(1); wt_dt_helper_fail_next_close_for_test(); dump(log_message);
    EXPECT_TRUE(has("关闭失败")); EXPECT_FALSE(has("写入完成")); EXPECT_FALSE(has("全部导出完成"));
}
TEST_P(CsvSignal, ShortWriteOrFlushFailureWithLogger) {
    source(1); SizeLimit limit; ASSERT_TRUE(limit.active); dump(log_message);
    EXPECT_TRUE(has("写入失败") || has("关闭失败"));
    EXPECT_FALSE(has("写入完成")); EXPECT_FALSE(has("全部导出完成"));
}
TEST_P(CsvSignal, LargeWriteFailureWithLogger) {
    source(1000); SizeLimit limit; ASSERT_TRUE(limit.active); dump(log_message);
    EXPECT_TRUE(has("写入失败")); EXPECT_FALSE(has("写入完成")); EXPECT_FALSE(has("全部导出完成"));
}
// Expected red: the original public API has no status channel. This test asks
// whether its currently observable output distinguishes a post-close failure.
// Capturing stderr is an observational probe, not an asserted API contract.
TEST_P(CsvSignal, KnownRedNullLoggerFailureNotObservable) {
    source(1);
    testing::internal::CaptureStderr(); dump(nullptr);
    auto normal_stderr = testing::internal::GetCapturedStderr(); auto normal_csv = csv();
    ASSERT_FALSE(normal_csv.empty());
    wt_dt_helper_fail_next_close_for_test();
    testing::internal::CaptureStderr(); dump(nullptr);
    auto failure_stderr = testing::internal::GetCapturedStderr(); auto failure_csv = csv();
    EXPECT_TRUE(normal_stderr != failure_stderr || normal_csv != failure_csv)
        << "void API returned normally and same file and stderr under injected close failure";
}
INSTANTIATE_TEST_SUITE_P(BarAndTick, CsvSignal, ::testing::Values(false, true),
    [](const ::testing::TestParamInfo<bool>& v) { return v.param ? "Tick" : "Bar"; });
