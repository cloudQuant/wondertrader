#include <gtest/gtest.h>
#include <Includes/WTSVariant.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSSessionInfo.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSTools/WTSLogger.h>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtDtCore/StateMonitor.h>
#include <filesystem>
#include <fstream>
#include <dlfcn.h>
#include <ctime>
#include <cstdlib>
#include <unistd.h>
#define private public
#include <WtDtPorter/WtDtRunner.h>
#undef private
using namespace wtp;
namespace fs = std::filesystem;
WtDtRunner& getRunner() { static WtDtRunner runner; return runner; }
struct Q05StateMonitorConfigTestAccess {
    static const StateInfo* state(const StateMonitor& m,const char* key) { auto it=m._map.find(key); return it==m._map.end()?nullptr:it->second.get(); }
    static WTSBaseDataMgr* manager(const StateMonitor& m) { return m._bd_mgr; }
};
static thread_local bool fakeClock=false, failClock=false;
static thread_local int clockCalls=0;
static thread_local time_t fakeTime=1708768800;
extern "C" int clock_gettime(clockid_t clock, struct timespec* out) {
    if (fakeClock && clock==CLOCK_REALTIME) { ++clockCalls; if(failClock) return -1; out->tv_sec=fakeTime; out->tv_nsec=0; return 0; }
    using Fn=int (*)(clockid_t,struct timespec*); static Fn real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"clock_gettime")); return real(clock,out);
}
static void save(const fs::path& p,const std::string& text) { std::ofstream f(p); f<<text; ASSERT_TRUE(f.good()); }
class MonitorDate : public ::testing::Test {
protected:
    fs::path root; WTSBaseDataMgr manager; StateMonitor monitor;
    void SetUp() override {
        setenv("TZ","UTC",1);tzset(); char p[]="/tmp/i03-monitor-date-XXXXXX"; char* d=mkdtemp(p); ASSERT_NE(nullptr,d); root=d;
        static bool log=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)log;
        save(root/"sessions.json",R"({"DAY":{"name":"day","offset":0,"sections":[{"from":930,"to":1500}]},"NIGHT":{"name":"night","offset":300,"sections":[{"from":2100,"to":2300}]},"ALL":{"name":"all","offset":0,"sections":[{"from":0,"to":0}]},"BAD":{"name":"bad","offset":0,"sections":[{"from":930,"to":1500}]}})");
        save(root/"holidays.json",R"({"CHINA":[20240226]})");
        save(root/"commodities.json",R"({"EX":{"P":{"name":"p","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1},"Q":{"name":"q","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":1},"N":{"name":"night","session":"NIGHT","holiday":"CHINA","pricetick":1,"volscale":1},"A":{"name":"all","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":1},"M":{"name":"missing","session":"BAD","holiday":"MISSING","pricetick":1,"volscale":1}}})");
        ASSERT_TRUE(manager.loadSessions((root/"sessions.json").c_str())); ASSERT_TRUE(manager.loadHolidays((root/"holidays.json").c_str())); ASSERT_TRUE(manager.loadCommodities((root/"commodities.json").c_str()));
        fakeTime=1708768800;clockCalls=0;failClock=false;
    }
    void TearDown() override { fakeClock=false;failClock=false; std::error_code ec; fs::remove_all(root,ec); }
    bool initialize(const std::string& text) { save(root/"states.json",text); fakeClock=true; bool ok=monitor.initialize((root/"states.json").c_str(),&manager,nullptr); fakeClock=false; return ok; }
    uint32_t cache(const char* commodity="EX.P") { auto a=manager.getDateMappingV1(); auto r=a.getTradingDate(&a,commodity,0,0,0); EXPECT_EQ(DateMappingStatus::Ok,r.status); return r.value; }
};
TEST_F(MonitorDate, WeekendHolidayAndSharedTemplateUseSamePureMapping) {
    auto api=manager.getDateMappingV1(); auto pure=api.nextTradingDate(&api,"EX.P",20240224,1,0);
    ASSERT_EQ(DateMappingStatus::Ok,pure.status); ASSERT_EQ(20240227u,pure.value);
    WTSBaseDataMgr oracle; ASSERT_TRUE(oracle.loadSessions((root/"sessions.json").c_str()));
    ASSERT_TRUE(oracle.loadHolidays((root/"holidays.json").c_str())); ASSERT_TRUE(oracle.loadCommodities((root/"commodities.json").c_str()));
    const auto legacySemantics=oracle.checkedGetTradingDate("EX.P",20240224,1000,false);
    ASSERT_EQ(DateMappingStatus::Ok,legacySemantics.status); EXPECT_EQ(legacySemantics.value,pure.value);
    EXPECT_TRUE(initialize(R"({"DAY":{},"ALL":{}})")); EXPECT_EQ(1,clockCalls);
    EXPECT_TRUE(monitor.isInState("DAY",SS_ORIGINAL)); EXPECT_TRUE(monitor.isInState("ALL",SS_ORIGINAL));
    EXPECT_EQ(20240227u,cache("EX.P")); EXPECT_EQ(20240227u,cache("EX.Q")); EXPECT_EQ(20240227u,cache("EX.A"));
}
TEST_F(MonitorDate, NightShiftAndSharedTemplatePreserveLastWriteOrder) {
    fakeTime=1708722000;
    const std::string text=R"({"DAY":{},"NIGHT":{}})"; WTSVariant* config=WTSCfgLoader::load_from_content(text.c_str()); ASSERT_NE(nullptr,config);
    auto keys=config->memberNames(); config->release(); ASSERT_EQ(2u,keys.size());
    EXPECT_TRUE(initialize(text)); EXPECT_EQ(1,clockCalls);
    const uint32_t expected=keys.back()=="NIGHT" ? 20240227 : 20240223;
    EXPECT_EQ(expected,cache("EX.P")); EXPECT_EQ(expected,cache("EX.N"));
}
TEST_F(MonitorDate, ProductFailureLeavesPriorStateAndAllSharedCacheUnchanged) {
    fakeTime=1709028000; ASSERT_TRUE(initialize(R"({"DAY":{}})"));
    const StateInfo* before=Q05StateMonitorConfigTestAccess::state(monitor,"DAY"); ASSERT_NE(nullptr,before);
    manager.setTradingDate("EX.P",20240115,false); ASSERT_EQ(20240115u,cache());
    fakeTime=1708768800;clockCalls=0;
    const std::string attempt=R"({"DAY":{},"BAD":{}})";
    WTSVariant* stagedConfig=WTSCfgLoader::load_from_content(attempt.c_str()); ASSERT_NE(nullptr,stagedConfig);
    const auto order=stagedConfig->memberNames(); stagedConfig->release(); ASSERT_EQ(2u,order.size()); ASSERT_EQ("DAY",order[0]);
    auto api=manager.getDateMappingV1(); ASSERT_EQ(DateMappingStatus::Ok,api.nextTradingDate(&api,"EX.P",20240224,1,0).status);
    ASSERT_EQ(DateMappingStatus::MissingCalendar,api.nextTradingDate(&api,"EX.M",20240224,1,0).status);
    EXPECT_FALSE(initialize(attempt)); EXPECT_EQ(1,clockCalls);
    EXPECT_EQ(before,Q05StateMonitorConfigTestAccess::state(monitor,"DAY")); EXPECT_EQ(nullptr,Q05StateMonitorConfigTestAccess::state(monitor,"BAD"));
    EXPECT_EQ(20240115u,cache("EX.P")); EXPECT_EQ(20240115u,cache("EX.Q")); EXPECT_EQ(&manager,Q05StateMonitorConfigTestAccess::manager(monitor));
}
TEST_F(MonitorDate, ClockFailureAndInvalidOffsetRetainStateAndCache) {
    fakeTime=1709028000; ASSERT_TRUE(initialize(R"({"DAY":{}})"));
    auto* before=Q05StateMonitorConfigTestAccess::state(monitor,"DAY"); manager.setTradingDate("EX.P",20240115,false);
    failClock=true;clockCalls=0; EXPECT_FALSE(initialize(R"({"ALL":{}})")); EXPECT_EQ(1,clockCalls);failClock=false;
    EXPECT_EQ(before,Q05StateMonitorConfigTestAccess::state(monitor,"DAY")); EXPECT_EQ(nullptr,Q05StateMonitorConfigTestAccess::state(monitor,"ALL")); EXPECT_EQ(20240115u,cache());
    fakeTime=253402300740; // Last minute of year 9999; positive night offset exceeds the date range.
    EXPECT_FALSE(initialize(R"({"NIGHT":{}})"));
    EXPECT_EQ(before,Q05StateMonitorConfigTestAccess::state(monitor,"DAY")); EXPECT_EQ(20240115u,cache());
}
TEST_F(MonitorDate, ActualRunnerBlocksBeforeParserAndCasterStartup) {
    const char* lib=getenv("I03_MONITOR_SPY"); ASSERT_NE(nullptr,lib); fs::create_directories(root/"parsers"); ASSERT_TRUE(fs::copy_file(lib,root/"parsers/libMonitorSpy.dylib"));
    void* handle=dlopen((root/"parsers/libMonitorSpy.dylib").c_str(),RTLD_NOW); ASSERT_NE(nullptr,handle);
    auto reset=reinterpret_cast<void(*)()>(dlsym(handle,"spy_reset")); auto created=reinterpret_cast<int(*)()>(dlsym(handle,"spy_created"));
    auto initialized=reinterpret_cast<int(*)()>(dlsym(handle,"spy_initialized")); auto connected=reinterpret_cast<int(*)()>(dlsym(handle,"spy_connected"));
    ASSERT_TRUE(reset && created && initialized && connected); reset();
    const std::string r=root.string(); const std::string cfg="{\"basefiles\":{\"session\":\""+r+"/sessions.json\",\"commodity\":\""+r+"/commodities.json\",\"holiday\":\""+r+"/holidays.json\"},\"writer\":{},\"statemonitor\":\""+r+"/bad-state.json\",\"shmcaster\":{\"active\":true,\"path\":\""+r+"/shm.bin\"},\"broadcaster\":{\"active\":true,\"sport\":0},\"parsers\":[{\"active\":true,\"id\":\"probe\",\"module\":\"MonitorSpy\"}]}";
    for (int mode=0; mode<3; ++mode) {
        save(root/"bad-state.json", mode==0 ? R"({"BAD":{}})" : mode==1 ? R"({"NIGHT":{}})" : R"({"DAY":{}})");
        fakeTime = mode==1 ? 253402300740 : 1708768800; failClock = mode==2; clockCalls=0;
        WtDtRunner runner; fakeClock=true; runner.initialize(cfg.c_str(),R"({"root":{"level":"debug","async":false,"sinks":[]}})",r.c_str(),false,false); fakeClock=false; failClock=false;
        EXPECT_TRUE(runner._startup_blocked) << mode; EXPECT_EQ(0,created()); EXPECT_EQ(0,initialized()); EXPECT_EQ(0,connected()); EXPECT_FALSE(fs::exists(root/"shm.bin"));
        EXPECT_FALSE(runner._udp_caster.m_bStarted); EXPECT_EQ(nullptr,runner._udp_caster.m_thrdIO.get());
        runner.start(true,false); EXPECT_FALSE(runner.initialParserCommandsSubmitted()); EXPECT_FALSE(runner.createExtParser("late"));
        EXPECT_EQ(0,created()); EXPECT_EQ(0,connected());
    }
    dlclose(handle);
}
TEST_F(MonitorDate, FailedReinitializeWithDifferentManagerKeepsOriginalBindings) {
    fakeTime=1709028000; ASSERT_TRUE(initialize(R"({"DAY":{}})"));
    auto* before=Q05StateMonitorConfigTestAccess::state(monitor,"DAY"); ASSERT_NE(nullptr,before);
    manager.setTradingDate("EX.P",20240115,false); WTSBaseDataMgr other;
    save(root/"other-state.json",R"({"DAY":{}})"); fakeClock=true; failClock=true; clockCalls=0;
    const bool ok=monitor.initialize((root/"other-state.json").c_str(),&other,nullptr);
    fakeClock=false; failClock=false; EXPECT_FALSE(ok); EXPECT_EQ(1,clockCalls);
    EXPECT_EQ(&manager,Q05StateMonitorConfigTestAccess::manager(monitor)); EXPECT_EQ(before,Q05StateMonitorConfigTestAccess::state(monitor,"DAY"));
    EXPECT_EQ(20240115u,cache());
}
TEST_F(MonitorDate, ValidStateAndAllDayBypassDoNotSetMonitorGate) {
    const std::string r=root.string(); save(root/"good-state.json",R"({"DAY":{}})");
    for (bool allDay : {false,true}) {
        const std::string statePath = allDay ? r+"/no-such-state.json" : r+"/good-state.json";
        const std::string cfg="{\"basefiles\":{\"session\":\""+r+"/sessions.json\",\"commodity\":\""+r+"/commodities.json\",\"holiday\":\""+r+"/holidays.json\"},\"writer\":{},\"allday\":"+(allDay?"true":"false")+",\"statemonitor\":\""+statePath+"\"}";
        WtDtRunner runner; fakeClock=true; failClock=false;
        runner.initialize(cfg.c_str(),R"({"root":{"level":"debug","async":false,"sinks":[]}})",(r+"/").c_str(),false,false); fakeClock=false;
        EXPECT_FALSE(runner._startup_blocked); EXPECT_EQ(allDay,runner.isAlldayMode());
        if (!allDay) EXPECT_TRUE(runner._state_mon.isInState("DAY",SS_ORIGINAL));
    }
}
TEST_F(MonitorDate, WeekdayHolidayRetainsExistingMappingSemantics) {
    fakeTime=1708941600; // Monday 2024-02-26 10:00 UTC is configured as a holiday.
    auto api=manager.getDateMappingV1(); auto expected=api.getTradingDate(&api,"EX.P",20240226,1000,0);
    ASSERT_EQ(DateMappingStatus::Ok,expected.status); ASSERT_EQ(20240226u,expected.value);
    ASSERT_TRUE(initialize(R"({"DAY":{}})")); EXPECT_EQ(20240226u,cache("EX.P")); EXPECT_EQ(20240226u,cache("EX.Q"));
}
