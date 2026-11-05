#include <gtest/gtest.h>
#include <Includes/WTSSessionInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSTools/WTSHotMgr.h>
#include <WtDtServo/ParserAdapter.h>
#include <filesystem>
#include <fstream>
#include <dlfcn.h>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#define private public
#include <WtDtServo/WtDtRunner.h>
#undef private

using namespace wtp;
namespace fs = std::filesystem;
static thread_local bool useFakeClock = false;
static thread_local int clockCalls = 0;
static thread_local bool failFakeClock = false;
extern "C" int clock_gettime(clockid_t clock, struct timespec* result) {
    if (useFakeClock && clock == CLOCK_REALTIME) {
        if (failFakeClock) { ++clockCalls; return -1; }
        result->tv_sec = clockCalls++ == 0 ? 1709078399 : 1709078401;
        result->tv_nsec = 0; return 0;
    }
    using Fn = int (*)(clockid_t, struct timespec*);
    static Fn real = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "clock_gettime"));
    return real(clock, result);
}
static void save(const fs::path& p, const std::string& s) { std::ofstream f(p); f << s; ASSERT_TRUE(f.good()); }
struct Spy {
    void* handle=nullptr;
    void (*reset)()=nullptr;
    unsigned (*ranges)()=nullptr, (*counts)()=nullptr;
    uint64_t (*start)()=nullptr, (*end)()=nullptr;
    bool open(const std::string& path) {
        handle=dlopen(path.c_str(),RTLD_NOW); if(!handle) return false;
        reset=reinterpret_cast<void(*)()>(dlsym(handle,"spy_reset"));
        ranges=reinterpret_cast<unsigned(*)()>(dlsym(handle,"spy_range_calls")); counts=reinterpret_cast<unsigned(*)()>(dlsym(handle,"spy_count_calls"));
        start=reinterpret_cast<uint64_t(*)()>(dlsym(handle,"spy_start")); end=reinterpret_cast<uint64_t(*)()>(dlsym(handle,"spy_end"));
        return reset && ranges && counts && start && end;
    }
    ~Spy() { if(handle) dlclose(handle); }
};
class ServoDate : public ::testing::Test {
protected:
    fs::path root; Spy spy; WtDtRunner runner;
    void SetUp() override {
        setenv("TZ","UTC",1); tzset(); char name[]="/tmp/i03-servo-date-XXXXXX"; char* dir=mkdtemp(name); ASSERT_NE(nullptr,dir); root=dir;
        const char* module=std::getenv("I03_DATE_SPY"); ASSERT_NE(nullptr,module);
        ASSERT_TRUE(fs::copy_file(module,root/"libDateSpy.dylib")); ASSERT_TRUE(spy.open((root/"libDateSpy.dylib").string()));
        save(root/"sessions.json",R"({"DAY":{"name":"day","offset":0,"sections":[{"from":930,"to":1500}]},"POS":{"name":"night","offset":300,"sections":[{"from":2100,"to":2300}]},"NEG":{"name":"next","offset":-300,"sections":[{"from":2100,"to":2300}]},"ALL":{"name":"all","offset":0,"sections":[{"from":0,"to":0}]}})");
        save(root/"holidays.json",R"({"CHINA":[]})");
        save(root/"commodities.json",R"({"EX":{"D":{"name":"day","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1},"P":{"name":"night","session":"POS","holiday":"CHINA","pricetick":1,"volscale":1},"N":{"name":"next","session":"NEG","holiday":"CHINA","pricetick":1,"volscale":1},"A":{"name":"all","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":1},"M":{"name":"missing","session":"DAY","holiday":"MISSING","pricetick":1,"volscale":1}}})");
        const std::string r=root.string();
        save(root/"runner.json","{\"disable_dump\":true,\"basefiles\":{\"session\":\""+r+"/sessions.json\",\"commodity\":\""+r+"/commodities.json\",\"holiday\":\""+r+"/holidays.json\"},\"data\":{\"store\":{\"module\":\"DateSpy\"}}}");
        runner.initialize((root/"runner.json").c_str(),true,r.c_str(),(root/"missing-log.yaml").c_str(),nullptr,nullptr);
        ASSERT_TRUE(runner.isInitialized()); ASSERT_EQ(&runner.getBaseDataMgr(), runner._data_mgr.get_basedata_mgr());
        ASSERT_EQ(&runner.getHotMgr(),runner._data_mgr.get_hot_mgr()); spy.reset();
    }
    void TearDown() override { useFakeClock=false; failFakeClock=false; std::error_code ec; fs::remove_all(root,ec); }
    void expectRejected(const char* code,uint32_t date) {
        const auto before = runner._data_mgr._bars_cache.size();
        for(const char* period : {"m1","m2"}) {
            EXPECT_EQ(nullptr,runner.get_bars_by_date(code,period,date));
            EXPECT_EQ(0u,spy.ranges()); EXPECT_EQ(0u,spy.counts()); EXPECT_EQ(before,runner._data_mgr._bars_cache.size());
        }
    }
};
TEST_F(ServoDate, ValidDayRangeUsesCheckedBoundaries) {
    auto* slice=runner.get_bars_by_date("EX.D.2406","m1",20240227); ASSERT_NE(nullptr,slice); slice->release();
    EXPECT_EQ(1u,spy.ranges()); EXPECT_EQ(202402270930ULL,spy.start()); EXPECT_EQ(202402271500ULL,spy.end());
}
TEST_F(ServoDate, ZeroDateUsesOneSnapshotAcrossMidnight) {
    clockCalls=0; useFakeClock=true; auto* slice=runner.get_bars_by_date("EX.D.2406","m1",0); useFakeClock=false;
    ASSERT_NE(nullptr,slice); slice->release(); EXPECT_EQ(1,clockCalls);
    EXPECT_EQ(202402270930ULL,spy.start()); EXPECT_EQ(202402271500ULL,spy.end());
}
TEST_F(ServoDate, ClockFailureNeverReachesReaderOrCache) {
    const auto before=runner._data_mgr._bars_cache.size(); clockCalls=0; failFakeClock=true; useFakeClock=true;
    auto* slice=runner.get_bars_by_date("EX.D.2406","m1",0); useFakeClock=false; failFakeClock=false;
    EXPECT_EQ(nullptr,slice); EXPECT_EQ(1,clockCalls); EXPECT_EQ(0u,spy.ranges()); EXPECT_EQ(0u,spy.counts());
    EXPECT_EQ(before,runner._data_mgr._bars_cache.size());
}
TEST_F(ServoDate, InvalidMetadataAndDateNeverReachReaderOrCache) {
    runner._data_mgr._bars_cache["sentinel"]._times=7; const auto before=runner._data_mgr._bars_cache.size();
    expectRejected("EX.D.2406",20240230); expectRejected("EX.D.2406",20240224); expectRejected("EX.UNKNOWN.2406",20240227); expectRejected("EX.M.2406",20240227);
    expectRejected(nullptr,20240227); expectRejected("",20240227);
    auto* commodity=runner.getBaseDataMgr().getCommodity("EX","D"); ASSERT_NE(nullptr,commodity);
    auto* session=commodity->getSessionInfo(); commodity->setSessionInfo(nullptr); expectRejected("EX.D.2406",20240227); commodity->setSessionInfo(session);
    EXPECT_EQ(before,runner._data_mgr._bars_cache.size()); EXPECT_EQ(7u,runner._data_mgr._bars_cache["sentinel"]._times);
}
TEST_F(ServoDate, SingleSidedFailuresNeverReachReaderOrCache) {
    auto api=runner.getBaseDataMgr().getDateMappingV1();
    auto a=api.getBoundaryTime(&api,"EX.P",10000101,0,1), b=api.getBoundaryTime(&api,"EX.P",10000101,0,0);
    ASSERT_NE(DateMappingStatus::Ok,a.status); ASSERT_EQ(DateMappingStatus::Ok,b.status); expectRejected("EX.P.2406",10000101);
    a=api.getBoundaryTime(&api,"EX.N",99991231,0,1); b=api.getBoundaryTime(&api,"EX.N",99991231,0,0);
    ASSERT_EQ(DateMappingStatus::Ok,a.status); ASSERT_NE(DateMappingStatus::Ok,b.status); expectRejected("EX.N.2406",99991231);
}
TEST_F(ServoDate, NightAndAllDayControls) {
    auto* night=runner.get_bars_by_date("EX.P.2406","m1",20240227); ASSERT_NE(nullptr,night); night->release();
    EXPECT_EQ(202402262100ULL,spy.start()); EXPECT_EQ(202402272300ULL,spy.end());
    auto* full=runner.get_bars_by_date("EX.A.2406","m1",20240227); ASSERT_NE(nullptr,full); full->release();
    EXPECT_EQ(202402270000ULL,spy.start()); EXPECT_EQ(202402270000ULL,spy.end());
}
TEST_F(ServoDate, MissingDependenciesAndForeignManagerAreRejected) {
    WtDataManager fresh; EXPECT_EQ(nullptr,fresh.get_kline_slice_by_date("EX.D.2406",KP_Minute1,1,20240227));
    WTSBaseDataMgr unrelated; auto* saved=runner._data_mgr._bd_mgr; runner._data_mgr._bd_mgr=&unrelated;
    expectRejected("EX.D.2406",20240227); runner._data_mgr._bd_mgr=saved;
    EXPECT_EQ(nullptr,runner.get_bars_by_date("EX.D.2406",nullptr,20240227));
}
TEST_F(ServoDate, DirectRangeStillPassesZeroEndToReader) {
    auto* slice=runner._data_mgr.get_kline_slice_by_range("EX.D.2406",KP_Minute1,1,202402270930ULL,0);
    ASSERT_NE(nullptr,slice); slice->release(); EXPECT_EQ(1u,spy.ranges()); EXPECT_EQ(202402270930ULL,spy.start()); EXPECT_EQ(0u,spy.end());
}
