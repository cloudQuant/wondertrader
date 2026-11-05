#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include "WtBtCore/HisDataReplayer.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/ILogHandler.h"

struct HisDataReplayerTestAccess {
    static WTSBaseDataMgr& manager(HisDataReplayer& r) { return r._bd_mgr; }
    static void range(HisDataReplayer& r, uint32_t bd, uint32_t bt, uint32_t ed, uint32_t et) {
        r._range_begin_date=bd; r._range_begin_hhmm=bt; r._range_end_date=ed; r._range_end_hhmm=et;
    }
    static bool get(HisDataReplayer& r, uint32_t& b, uint32_t& e) { return r.get_hft_trading_date_range(b,e); }
    static void sub(HisDataReplayer& r, int kind, const char* code) {
        switch(kind) {
        case 0: r._tick_sub_map[code]; break;
        case 1: r._orddtl_sub_map[code]; break;
        case 2: r._ordque_sub_map[code]; break;
        case 3: r._trans_sub_map[code]; break;
        }
    }
    static void clear(HisDataReplayer& r) {
        r._tick_sub_map.clear(); r._orddtl_sub_map.clear(); r._ordque_sub_map.clear(); r._trans_sub_map.clear();
    }
};
struct Logs : ILogHandler {
    std::vector<std::string> lines;
    void handleLogAppend(WTSLogLevel, const char* msg) override { lines.emplace_back(msg ? msg : ""); }
    bool has(const std::string& a, const std::string& b) const {
        for (const auto& s: lines) if (s.find(a)!=s.npos && s.find(b)!=s.npos) return true;
        return false;
    }
};
static Logs logs;
class RangeTest : public ::testing::TestWithParam<int> {
protected:
    HisDataReplayer r;
    std::filesystem::path root;
    void SetUp() override {
        static bool initialized=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,&logs); return true; }();
        (void)initialized; logs.lines.clear();
        char name[]="/tmp/wt-replayer-range-XXXXXX"; char* dir=mkdtemp(name); ASSERT_NE(dir,nullptr); root=dir;
        auto write=[&](const char* name,const char* data) { auto p=root/name; std::ofstream(p)<<data; return p.string(); };
        auto sessions=write("sessions.json",R"({"DAY":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]},"NIGHT":{"name":"night","offset":300,"sections":[{"from":2100,"to":2300}]},"ALL":{"name":"all","offset":300,"sections":[{"from":0,"to":0}]},"EMPTY":{"name":"empty","sections":[]}})");
        auto holidays=write("holidays.json",R"({"CHINA":[20240226]})");
        auto comms=write("comms.json",R"({"EX":{"D":{"name":"day","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1},"N":{"name":"night","session":"NIGHT","holiday":"CHINA","pricetick":1,"volscale":1},"A":{"name":"all","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":1},"M":{"name":"missing","session":"DAY","holiday":"ABSENT","pricetick":1,"volscale":1},"E":{"name":"empty","session":"EMPTY","holiday":"CHINA","pricetick":1,"volscale":1}}})");
        auto& mgr=HisDataReplayerTestAccess::manager(r);
        ASSERT_TRUE(mgr.loadSessions(sessions.c_str())); ASSERT_TRUE(mgr.loadHolidays(holidays.c_str())); ASSERT_TRUE(mgr.loadCommodities(comms.c_str()));
        HisDataReplayerTestAccess::range(r,20240223,2100,20240227,900); logs.lines.clear();
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root,ec); }
    void sub(int kind,const char* code) { HisDataReplayerTestAccess::sub(r,kind,code); }
    void failure(const char* code,const char* status) {
        uint32_t b=12345678,e=87654321; logs.lines.clear();
        EXPECT_FALSE(HisDataReplayerTestAccess::get(r,b,e)); EXPECT_EQ(12345678u,b); EXPECT_EQ(87654321u,e);
        EXPECT_TRUE(logs.has(code,status));
    }
};
TEST_P(RangeTest, EachSubscriptionMapsOrdinaryNightAndAllDay) {
    for (const auto& entry : std::vector<std::pair<const char*,uint32_t>>{{"EX.D.2401",20240223},{"EX.N.2401",20240227},{"EX.A.2401",20240224}}) {
        HisDataReplayerTestAccess::clear(r); sub(GetParam(),entry.first);
        uint32_t b=1,e=2; ASSERT_TRUE(HisDataReplayerTestAccess::get(r,b,e)); EXPECT_EQ(entry.second,b); EXPECT_EQ(20240227u,e);
    }
}
TEST_P(RangeTest, EachSubscriptionRejectsMissingMetadataAndCalendar) {
    for (const auto& entry : std::vector<std::pair<const char*,const char*>>{{"EX.Z.2401","status 4"},{"EX.E.2401","status 5"},{"EX.M.2401","status 6"}}) {
        HisDataReplayerTestAccess::clear(r); sub(0,"EX.D.2401"); sub(GetParam(),entry.first); failure(entry.first,entry.second);
    }
}
TEST_P(RangeTest, EachSubscriptionRejectsInvalidBoundariesWithoutPublishing) {
    sub(GetParam(),"EX.D.2401");
    struct Row { uint32_t bd,bt,ed,et; const char* status; };
    for (auto x : std::vector<Row>{{0,900,20240227,900,"status 2"},{20240230,900,20240227,900,"status 2"},
        {20240223,1260,20240227,900,"status 3"},{20240223,900,0,900,"status 2"},
        {20240223,900,20240230,900,"status 2"},{20240223,900,20240227,2460,"status 3"},
        {20240223,1000,20240223,900,"status 8"}}) {
        HisDataReplayerTestAccess::range(r,x.bd,x.bt,x.ed,x.et); failure("EX.D.2401",x.status);
    }
    HisDataReplayerTestAccess::clear(r); sub(GetParam(),"EX.N.2401");
    HisDataReplayerTestAccess::range(r,99991231,2100,99991231,2300); failure("EX.N.2401","status 8");
}
TEST_P(RangeTest, NoSubscriptionsLeavesOutputsUntouched) {
    uint32_t b=12345678,e=87654321; EXPECT_FALSE(HisDataReplayerTestAccess::get(r,b,e)); EXPECT_EQ(12345678u,b); EXPECT_EQ(87654321u,e);
}
TEST_P(RangeTest, CrossKindMixedValidSubscriptionsProduceUnion) {
    sub(0,"EX.D.2401"); sub(1,"EX.N.2401"); sub(2,"EX.A.2401"); sub(3,"EX.N.2401");
    uint32_t b=1,e=2; ASSERT_TRUE(HisDataReplayerTestAccess::get(r,b,e)); EXPECT_EQ(20240223u,b); EXPECT_EQ(20240227u,e);
}
static std::string param_name(const ::testing::TestParamInfo<int>& p) {
    static const char* names[] = {"Tick","OrderDetail","OrderQueue","Transaction"};
    return names[p.param];
}
INSTANTIATE_TEST_SUITE_P(Subscriptions,RangeTest,::testing::Values(0,1,2,3),param_name);
