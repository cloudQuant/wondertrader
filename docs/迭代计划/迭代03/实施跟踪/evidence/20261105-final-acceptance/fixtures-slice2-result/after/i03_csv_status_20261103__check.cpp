#include <gtest/gtest.h>
#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>

extern "C" void wt_dt_helper_fail_next_close_for_test();
extern "C" void wt_dt_helper_fail_next_csv_read_close_for_test();
namespace fs = std::filesystem;
static std::vector<std::string> captured_messages;
static void logger(WtString s) { captured_messages.emplace_back(s ? s : ""); }
static bool contains(const char* text) { for (auto& s : captured_messages) if (s.find(text) != std::string::npos) return true; return false; }
static std::string bytes(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
// Records differ per index (count == 1 is unchanged): identical records compress
// beyond the 1000:1 ratio WTSCmpHelper::uncompress_data accepts since decision K37=B
// (2026-11-04), and dump_* would reject the block as input before writing.
static bool source(const fs::path& p, bool tick, size_t count = 1) {
    if (tick) {
        WTSTickStruct v{}; std::strncpy(v.exchg,"SHFE",sizeof(v.exchg)-1); std::strncpy(v.code,"rb2010",sizeof(v.code)-1);
        v.action_date = v.trading_date = 20200116; v.action_time = 93000; v.price = 3000;
        std::vector<WTSTickStruct> data(count,v);
        for (size_t i = 0; i < count; i++) { data[i].action_time = 93000 + i; data[i].price = 3000 + i; data[i].volume = i; data[i].total_volume = i; }
        return store_ticks(p.string().c_str(),data.data(),count,nullptr);
    }
    WTSBarStruct v{}; v.date = 20200116; v.open = 3000; v.close = 3001;
    std::vector<WTSBarStruct> data(count,v);
    for (size_t i = 0; i < count; i++) { data[i].open = 3000 + i; data[i].close = 3001 + i; data[i].vol = i; }
    return store_bars(p.string().c_str(),data.data(),count,"d",nullptr);
}
static uint32_t dump(bool tick, const char* in, const char* out, FuncLogCallback cb) {
    return tick ? dump_ticks_v2(in,out,"",cb) : dump_bars_v2(in,out,"",cb);
}
static void legacy(bool tick, const char* in, const char* out) {
    if (tick) dump_ticks(in,out,"",nullptr); else dump_bars(in,out,"",nullptr);
}
class CsvStatus : public ::testing::TestWithParam<bool> {
protected:
    fs::path root, input, output;
    void SetUp() override {
        char name[] = "/tmp/wt-csv-status-XXXXXX"; char* dir = mkdtemp(name); ASSERT_NE(dir,nullptr);
        root = dir; input = root / "in"; output = root / "out";
        ASSERT_TRUE(fs::create_directories(input)); ASSERT_TRUE(fs::create_directories(output)); captured_messages.clear();
    }
    void TearDown() override { std::error_code ec; fs::remove_all(root,ec); }
    uint32_t run(FuncLogCallback cb) { return dump(GetParam(),input.c_str(),output.c_str(),cb); }
};
// The versioned API with and without a logger and the legacy void wrapper write the
// same bytes, and the CSV row carries the stored record.
TEST_P(CsvStatus, NormalAndLegacyWriteIdenticalBytes) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam()));
    ASSERT_EQ(run(logger),WT_CSV_DUMP_SUCCESS); EXPECT_TRUE(contains("全部导出完成"));
    auto content = bytes(output / "one.csv"); ASSERT_FALSE(content.empty());
    if (GetParam()) { EXPECT_NE(content.find("SHFE,rb2010,20200116"),std::string::npos) << content; EXPECT_NE(content.find("3000.000000"),std::string::npos) << content; }
    else { EXPECT_NE(content.find("20200116,0,3000.000000"),std::string::npos) << content; EXPECT_NE(content.find("3001.000000"),std::string::npos) << content; }
    captured_messages.clear(); ASSERT_EQ(run(nullptr),WT_CSV_DUMP_SUCCESS); EXPECT_EQ(content,bytes(output / "one.csv"));
    legacy(GetParam(),input.c_str(),output.c_str()); EXPECT_EQ(content,bytes(output / "one.csv"));
}
#ifdef CSV_STATUS_PRIOR_BYTES
// Only built with BASELINE_SOURCE: the bytes equal those of the saved pre-change helper.
TEST_P(CsvStatus, NormalMatchesPriorBytes) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam()));
    ASSERT_EQ(run(nullptr),WT_CSV_DUMP_SUCCESS);
    const char* baseline = getenv("CSV_BASELINE_DIR"); ASSERT_NE(baseline,nullptr);
    EXPECT_EQ(bytes(output / "one.csv"),bytes(fs::path(baseline) / (GetParam() ? "tick.csv" : "bar.csv")));
}
#endif
TEST_P(CsvStatus, InvalidAndDirectoryFailures) {
    EXPECT_EQ(dump(GetParam(),nullptr,output.c_str(),nullptr),WT_CSV_DUMP_INVALID_ARGUMENT);
    EXPECT_EQ(dump(GetParam(),"",output.c_str(),logger),WT_CSV_DUMP_INVALID_ARGUMENT);
    EXPECT_EQ(dump(GetParam(),input.c_str(),nullptr,nullptr),WT_CSV_DUMP_INVALID_ARGUMENT);
    EXPECT_EQ(dump(GetParam(),input.c_str(),"",logger),WT_CSV_DUMP_INVALID_ARGUMENT);
    EXPECT_EQ(dump(GetParam(),(root / "missing").c_str(),output.c_str(),nullptr),WT_CSV_DUMP_INPUT_ERROR);
    std::ofstream(root / "file") << "x";
    EXPECT_EQ(dump(GetParam(),input.c_str(),(root / "file").c_str(),nullptr),WT_CSV_DUMP_OUTPUT_ERROR);
    EXPECT_EQ(dump(GetParam(),(root / "file").c_str(),output.c_str(),logger),WT_CSV_DUMP_INPUT_ERROR);
}
TEST_P(CsvStatus, ReadAndCloseFailuresAreObservableWithoutLogger) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam()));
    wt_dt_helper_fail_next_csv_read_close_for_test(); EXPECT_EQ(run(nullptr),WT_CSV_DUMP_INPUT_ERROR);
    EXPECT_FALSE(fs::exists(output / "one.csv"));
    wt_dt_helper_fail_next_csv_read_close_for_test(); EXPECT_EQ(run(logger),WT_CSV_DUMP_INPUT_ERROR);
    EXPECT_TRUE(contains("失败")); EXPECT_FALSE(contains("全部导出完成"));
}
TEST_P(CsvStatus, OutputCloseFailureIsObservableWithoutLogger) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam()));
    wt_dt_helper_fail_next_close_for_test(); EXPECT_EQ(run(nullptr),WT_CSV_DUMP_OUTPUT_ERROR);
    wt_dt_helper_fail_next_close_for_test(); EXPECT_EQ(run(logger),WT_CSV_DUMP_OUTPUT_ERROR);
    EXPECT_TRUE(contains("关闭失败")); EXPECT_FALSE(contains("全部导出完成"));
}
TEST_P(CsvStatus, OutputOpenAndMalformedInputFailures) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam())); ASSERT_TRUE(fs::create_directory(output / "one.csv"));
    EXPECT_EQ(run(nullptr),WT_CSV_DUMP_OUTPUT_ERROR); EXPECT_EQ(run(logger),WT_CSV_DUMP_OUTPUT_ERROR);
    fs::remove(output / "one.csv"); std::ofstream(input / "one.dsb",std::ios::binary|std::ios::trunc) << "bad";
    EXPECT_EQ(run(nullptr),WT_CSV_DUMP_INPUT_ERROR); EXPECT_EQ(run(logger),WT_CSV_DUMP_INPUT_ERROR);
}
TEST_P(CsvStatus, MixedFileBatchIsPartialWithOrWithoutLogger) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam())); std::ofstream(input / "bad.dsb",std::ios::binary) << "bad";
    EXPECT_EQ(run(nullptr),WT_CSV_DUMP_PARTIAL_FAILURE); ASSERT_FALSE(bytes(output / "one.csv").empty());
    EXPECT_EQ(run(logger),WT_CSV_DUMP_PARTIAL_FAILURE); EXPECT_FALSE(contains("全部导出完成"));
}
TEST_P(CsvStatus, FlushOrWriteFailure) {
    ASSERT_TRUE(source(input / "one.dsb",GetParam(),1000));
    struct rlimit old{}, lim{}; ASSERT_EQ(getrlimit(RLIMIT_FSIZE,&old),0); lim=old; lim.rlim_cur=1;
    struct sigaction ignore{}, prev{}; ignore.sa_handler=SIG_IGN; sigemptyset(&ignore.sa_mask);
    ASSERT_EQ(sigaction(SIGXFSZ,&ignore,&prev),0); ASSERT_EQ(setrlimit(RLIMIT_FSIZE,&lim),0);
    auto result = run(nullptr);
    EXPECT_EQ(setrlimit(RLIMIT_FSIZE,&old),0); EXPECT_EQ(sigaction(SIGXFSZ,&prev,nullptr),0);
    EXPECT_EQ(result,WT_CSV_DUMP_OUTPUT_ERROR);
}
INSTANTIATE_TEST_SUITE_P(BarAndTick,CsvStatus,::testing::Values(false,true),
    [](const ::testing::TestParamInfo<bool>& p) { return p.param ? "Tick" : "Bar"; });
