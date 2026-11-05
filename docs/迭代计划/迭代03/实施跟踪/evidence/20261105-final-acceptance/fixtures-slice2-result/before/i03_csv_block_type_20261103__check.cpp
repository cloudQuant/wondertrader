#include <gtest/gtest.h>
#include <WtDtHelper/WtDtHelper.h>
#include <WtDataStorage/DataDefine.h>
#include <WTSUtils/WTSCmpHelper.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace wtp;

static std::string payload(bool tick, bool old) {
    if (tick && old) {
        WTSTickStructOld t; std::strcpy(t.exchg, "SHFE"); std::strcpy(t.code, "ag2512"); t.trading_date = 20251103; t.action_date = 20251103; t.price = 100;
        return std::string(reinterpret_cast<char*>(&t), sizeof(t));
    }
    if (tick) {
        WTSTickStruct t{}; std::strcpy(t.exchg, "SHFE"); std::strcpy(t.code, "ag2512"); t.trading_date = 20251103; t.action_date = 20251103; t.price = 100;
        return std::string(reinterpret_cast<char*>(&t), sizeof(t));
    }
    if (old) { WTSBarStructOld b; b.date = 20251103; b.open = 100; b.close = 101; return std::string(reinterpret_cast<char*>(&b), sizeof(b)); }
    WTSBarStruct b{}; b.date = 20251103; b.open = 100; b.close = 101; return std::string(reinterpret_cast<char*>(&b), sizeof(b));
}
static std::string block(bool tick, int version, uint16_t barType = BT_HIS_Day) {
    const bool old = version == BLOCK_VERSION_RAW || version == BLOCK_VERSION_CMP;
    const bool compressed = version == BLOCK_VERSION_CMP || version == BLOCK_VERSION_CMP_V2;
    const auto raw = payload(tick, old);
    const uint16_t type = tick ? BT_HIS_Ticks : barType;
    if (!compressed) {
        BlockHeader h{}; std::memcpy(h._blk_flag, BLK_FLAG, FLAG_SIZE); h._type=type; h._version=version;
        return std::string(reinterpret_cast<char*>(&h),sizeof(h)) + raw;
    }
    auto zipped = WTSCmpHelper::compress_data(raw.data(), raw.size());
    BlockHeaderV2 h{}; std::memcpy(h._blk_flag, BLK_FLAG, FLAG_SIZE); h._type=type; h._version=version; h._size=zipped.size();
    return std::string(reinterpret_cast<char*>(&h),sizeof(h)) + zipped;
}
static void write(const fs::path& path, const std::string& bytes) { std::ofstream f(path, std::ios::binary); f.write(bytes.data(),bytes.size()); ASSERT_TRUE(f.good()); }
static std::string read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f),{}); }
class CsvBlock : public ::testing::TestWithParam<bool> {
protected:
    fs::path root, in, out;
    void SetUp() override { char p[]="/tmp/i03-csv-types-XXXXXX"; auto* d=mkdtemp(p); ASSERT_NE(nullptr,d); root=d; in=root/"in"; out=root/"out"; ASSERT_TRUE(fs::create_directories(in)); ASSERT_TRUE(fs::create_directories(out)); }
    void TearDown() override { std::error_code ec; fs::remove_all(root,ec); }
    uint32_t run(const char* filter="") { return GetParam() ? dump_ticks_v2(in.c_str(),out.c_str(),filter,nullptr) : dump_bars_v2(in.c_str(),out.c_str(),filter,nullptr); }
};
TEST_P(CsvBlock, AllFourSupportedFormats) {
    for (int version : {BLOCK_VERSION_RAW, BLOCK_VERSION_CMP, BLOCK_VERSION_RAW_V2, BLOCK_VERSION_CMP_V2}) {
        const std::vector<uint16_t> types = GetParam() ? std::vector<uint16_t>{BT_HIS_Ticks} : std::vector<uint16_t>{BT_HIS_Minute1, BT_HIS_Minute5, BT_HIS_Day};
        for (uint16_t type : types) {
            write(in/"good.dsb",block(GetParam(),version,type));
            ASSERT_EQ(WT_CSV_DUMP_SUCCESS,run()) << version << ":" << type;
            auto csv=read(out/"good.csv"); ASSERT_FALSE(csv.empty());
            const char* expected = GetParam() ? "SHFE,ag2512,20251103" : (type == BT_HIS_Day ? "20251103,0,100.000000" : "19900000,0,100.000000");
            EXPECT_NE(std::string::npos,csv.find(expected)) << version << ":" << type;
            fs::remove(out/"good.csv");
        }
    }
}
TEST_P(CsvBlock, WrongTypeAndShortHeaderAreInputFailures) {
    for (int version : {BLOCK_VERSION_RAW_V2, BLOCK_VERSION_CMP_V2}) {
        write(in/"wrong.dsb",block(!GetParam(),version));
        EXPECT_EQ(WT_CSV_DUMP_INPUT_ERROR,run()); EXPECT_FALSE(fs::exists(out/"wrong.csv"));
    }
    auto valid=block(GetParam(),BLOCK_VERSION_RAW_V2); valid.resize(4); write(in/"wrong.dsb",valid);
    EXPECT_EQ(WT_CSV_DUMP_INPUT_ERROR,run()); EXPECT_FALSE(fs::exists(out/"wrong.csv"));
}
TEST_P(CsvBlock, MixedBatchIsPartialAndValidFileIsWritten) {
    write(in/"valid.dsb",block(GetParam(),BLOCK_VERSION_CMP_V2)); write(in/"wrong.dsb",block(!GetParam(),BLOCK_VERSION_RAW_V2));
    EXPECT_EQ(WT_CSV_DUMP_PARTIAL_FAILURE,run()); EXPECT_FALSE(read(out/"valid.csv").empty()); EXPECT_FALSE(fs::exists(out/"wrong.csv"));
}
TEST_P(CsvBlock, EmptyDirectoryAndIgnoredFilter) {
    EXPECT_EQ(WT_CSV_DUMP_SUCCESS,run("no_match*")); EXPECT_TRUE(fs::is_empty(out));
    write(in/"kept.dsb",block(GetParam(),BLOCK_VERSION_RAW_V2));
    EXPECT_EQ(WT_CSV_DUMP_SUCCESS,run("no_match*")); EXPECT_FALSE(read(out/"kept.csv").empty());
}
TEST_P(CsvBlock, EmptyValidBlockHasNoCsv) {
    BlockHeader h{}; std::memcpy(h._blk_flag, BLK_FLAG, FLAG_SIZE); h._type=GetParam()?BT_HIS_Ticks:BT_HIS_Day; h._version=BLOCK_VERSION_RAW_V2;
    write(in/"empty.dsb",std::string(reinterpret_cast<char*>(&h),sizeof(h)));
    EXPECT_EQ(WT_CSV_DUMP_SUCCESS,run()); EXPECT_FALSE(fs::exists(out/"empty.csv"));
}
INSTANTIATE_TEST_SUITE_P(BarAndTick,CsvBlock,::testing::Values(false,true),[](const auto& p){return p.param?"Tick":"Bar";});
