#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include "WtBtCore/HisDataReplayer.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSDataDef.hpp"
#include "Share/StdUtils.hpp"
#include "Includes/WTSObject.hpp"
#define private public
#include "WtBtCore/EventNotifier.h"
#undef private

struct HisDataReplayerTestAccess {
    static WTSBaseDataMgr& manager(HisDataReplayer& r) { return r._bd_mgr; }
    static void notifier(HisDataReplayer& r, wtp::EventNotifier* n) { r._notifier=n; }
    static void enableTicks(HisDataReplayer& r) { r._tick_enabled=true; }
    static bool running(const HisDataReplayer& r) { return r._running.load(); }
    static bool terminated(const HisDataReplayer& r) { return r._terminated.load(); }
    static void missingSubscription(HisDataReplayer& r) { r._tick_sub_map["EX.MISSING.2401"]; }
    static void outsideBar(HisDataReplayer& r) {
        r._tick_enabled=false; const std::string code="EX.D.2401";
        auto bars=std::make_shared<HisDataReplayer::BarsList>(); bars->_code=code; bars->_period=KP_DAY; bars->_times=1;
        bars->_notify_close=true; WTSBarStruct bar{}; bar.date=20240220; bars->_bars.push_back(bar); bars->_count=1;
        r._bars_cache[code]=bars; r._main_key=code;
    }
    static void filteredTick(HisDataReplayer& r) {
        const std::string code="EX.D.2401"; r._tick_sub_map[code];
        auto& list=r._replay_ticks_cache[code][20240223]; WTSTickStruct tick{};
        std::strcpy(tick.exchg,"EX"); std::strcpy(tick.code,"D2401"); tick.trading_date=tick.action_date=20240223;
        tick.action_time=93060000; list._items.push_back(tick); list._count=1; list._cursor=UINT_MAX;
    }
};
static std::vector<std::string> events, progress;
static std::vector<WtReplayStatusV1> eventStatuses;
static HisDataReplayer* eventOwner=nullptr;
static bool throwOnEnd=false;
static void publish(WtUInt32, const char* topic, const char* data, WtUInt32 size) {
    if (std::strcmp(topic,"BT_EVENT")==0) {
        std::string event(data,size);
        if (throwOnEnd && event=="BT_END") throw std::runtime_error("notification exception");
        events.push_back(event); if (eventOwner) eventStatuses.push_back(eventOwner->replay_status());
    }
    if (std::strcmp(topic,"BT_STATE")==0) progress.emplace_back(data,size);
}
struct Sink : IDataSink {
    HisDataReplayer* rp=nullptr; bool task=true, feeInit=false, throwInit=false, throwSchedule=false, throwDone=false;
    bool reenterDone=false, reenterInit=false, reportDuringDone=false; bool acceptedDuringDone=true, acceptedDuringInit=true;
    bool blockInit=false, blockSchedule=false, entered=false, release=false, stopSchedule=false;
    std::mutex barrierMutex; std::condition_variable barrier;
    unsigned doneCount=0, scheduleCount=0; WtReplayStatusV1 initStatus{}, doneStatus{};
    void handle_init() override {
        initStatus=rp->replay_status();
        if (blockInit) { std::unique_lock<std::mutex> lock(barrierMutex); entered=true; barrier.notify_all(); barrier.wait(lock,[&]{return release;}); }
        if (reenterInit) acceptedDuringInit=rp->prepare();
        if (throwInit) throw std::runtime_error("init exception");
        if (feeInit) rp->report_fee_error("test fee failure");
        if (task) rp->register_task(1,0,1,"min","CHINA","TRADING");
    }
    void handle_replay_done() override {
        ++doneCount; doneStatus=rp->replay_status();
        if (reenterDone) { acceptedDuringDone=rp->prepare(); rp->run(); }
        if (reportDuringDone) rp->report_replay_error(WT_REPLAY_ERROR_INTERNAL,"too late");
        if (throwDone) throw std::runtime_error("done exception");
    }
    void handle_tick(const char*, WTSTickData*, uint32_t) override {}
    void handle_bar_close(const char*,const char*,uint32_t,WTSBarStruct*) override {}
    void handle_schedule(uint32_t,uint32_t) override {
        ++scheduleCount;
        if (blockSchedule) { std::unique_lock<std::mutex> lock(barrierMutex); entered=true; barrier.notify_all(); barrier.wait(lock,[&]{return release;}); }
        if (stopSchedule) rp->stop();
        if (throwSchedule) throw std::runtime_error("schedule exception");
    }
    void handle_session_begin(uint32_t) override {}
    void handle_session_end(uint32_t) override {}
};
class ReplayStatusTest : public ::testing::Test {
protected:
    HisDataReplayer r; Sink sink; wtp::EventNotifier notifier; std::filesystem::path root;
    void SetUp() override {
        static bool logger=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)logger;
        char name[]="/tmp/wt-replay-status-XXXXXX"; char* dir=mkdtemp(name); ASSERT_NE(dir,nullptr); root=dir;
        auto sessions=root/"sessions.json", holidays=root/"holidays.json", comms=root/"comms.json";
        std::ofstream(sessions) << R"({"TRADING":{"name":"trading","offset":0,"sections":[{"from":900,"to":1500}]}})";
        std::ofstream(holidays) << R"({"CHINA":[]})";
        std::ofstream(comms) << R"({"EX":{"D":{"name":"day","session":"TRADING","holiday":"CHINA","pricetick":1,"volscale":1}}})";
        auto& manager=HisDataReplayerTestAccess::manager(r); ASSERT_TRUE(manager.loadSessions(sessions.c_str())); ASSERT_TRUE(manager.loadHolidays(holidays.c_str())); ASSERT_TRUE(manager.loadCommodities(comms.c_str()));
        r.set_time_range(202402230930ULL,202402230932ULL); HisDataReplayerTestAccess::enableTicks(r);
        sink.rp=&r; r.register_sink(&sink,"test"); events.clear(); progress.clear(); eventStatuses.clear(); throwOnEnd=false; eventOwner=&r; notifier._publisher=publish; notifier._mq_sid=1; HisDataReplayerTestAccess::notifier(r,&notifier);
    }
    void TearDown() override { eventOwner=nullptr; std::error_code ec; std::filesystem::remove_all(root,ec); }
    void state(uint32_t value,uint32_t error=WT_REPLAY_ERROR_NONE) {
        auto s=r.replay_status(); EXPECT_EQ(sizeof(WtReplayStatusV1),s.struct_size); EXPECT_EQ(1u,s.abi_version);
        EXPECT_EQ(value,s.state); EXPECT_EQ(error,s.error_code); EXPECT_EQ('\0',s.message[sizeof(s.message)-1]);
        const bool terminal=value==WT_REPLAY_SUCCEEDED || value==WT_REPLAY_STOPPED || value==WT_REPLAY_FAILED;
        EXPECT_EQ(terminal ? WT_REPLAY_STATUS_FINAL : 0u,s.flags);
    }
};
TEST_F(ReplayStatusTest, SuccessfulTaskPublishesTerminalBeforeDoneAndRejectsReentry) {
    state(WT_REPLAY_IDLE); sink.reenterInit=true; sink.reenterDone=true;
    ASSERT_TRUE(r.prepare()); state(WT_REPLAY_PREPARED); EXPECT_EQ(sink.initStatus.state,WT_REPLAY_PREPARING); EXPECT_FALSE(sink.acceptedDuringInit);
    auto gen=r.replay_status().generation; EXPECT_EQ(gen,1u); EXPECT_FALSE(r.prepare()); EXPECT_EQ(r.replay_status().generation,gen);
    r.run(); state(WT_REPLAY_SUCCEEDED); EXPECT_GT(sink.scheduleCount,0u); EXPECT_EQ(sink.doneCount,1u);
    EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_SUCCEEDED); EXPECT_EQ(sink.doneStatus.flags,0u); EXPECT_FALSE(sink.acceptedDuringDone);
    EXPECT_EQ(events,(std::vector<std::string>{"BT_START","BT_END"}));
    ASSERT_EQ(eventStatuses.size(),2u); EXPECT_EQ(eventStatuses.back().state,WT_REPLAY_SUCCEEDED); EXPECT_EQ(eventStatuses.back().flags,0u);
    EXPECT_FALSE(HisDataReplayerTestAccess::running(r));
    r.run(); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(r.replay_status().generation,gen);
    ASSERT_TRUE(r.prepare()); EXPECT_EQ(r.replay_status().generation,gen+1); r.stop(); r.run(); state(WT_REPLAY_STOPPED);
}
TEST_F(ReplayStatusTest, FeeFailureDuringInitStopsPrepareAndNotifiesOnce) {
    sink.feeInit=true; EXPECT_FALSE(r.prepare()); state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_FEE);
    EXPECT_STREQ(r.replay_status().message,"test fee failure"); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_FAILED); EXPECT_EQ(sink.doneStatus.flags,0u);
    EXPECT_EQ(events,(std::vector<std::string>{"BT_START","BT_FAILED"})); EXPECT_FALSE(HisDataReplayerTestAccess::running(r)); r.run(); EXPECT_EQ(sink.doneCount,1u);
}
TEST_F(ReplayStatusTest, StopAndPreparedInternalErrorHaveDistinctTerminalStates) {
    ASSERT_TRUE(r.prepare()); r.stop(); r.run(); state(WT_REPLAY_STOPPED); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_STOPPED);
    EXPECT_EQ(events,(std::vector<std::string>{"BT_START","BT_STOPPED"}));
    ASSERT_TRUE(r.prepare()); auto count=sink.scheduleCount; r.report_replay_error(WT_REPLAY_ERROR_INTERNAL,"thread creation failed"); r.run();
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_INTERNAL); EXPECT_EQ(sink.scheduleCount,count); EXPECT_EQ(sink.doneCount,2u); EXPECT_EQ(events.back(),"BT_FAILED");
}
TEST_F(ReplayStatusTest, InitExceptionRethrowsAndFinalizesExactlyOnce) {
    sink.throwInit=true; EXPECT_THROW(r.prepare(),std::runtime_error); state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_CALLBACK_EXCEPTION);
    EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_FAILED); EXPECT_EQ(events.back(),"BT_FAILED");
    EXPECT_FALSE(HisDataReplayerTestAccess::running(r)); sink.throwInit=false; ASSERT_TRUE(r.prepare()); r.stop(); r.run();
}
TEST_F(ReplayStatusTest, ScheduleExceptionRethrowsAndFinalizesExactlyOnce) {
    sink.throwSchedule=true; ASSERT_TRUE(r.prepare()); EXPECT_THROW(r.run(),std::runtime_error);
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_CALLBACK_EXCEPTION); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_FAILED);
    EXPECT_EQ(events.back(),"BT_FAILED"); EXPECT_FALSE(HisDataReplayerTestAccess::running(r)); r.run(); EXPECT_EQ(sink.doneCount,1u);
}
TEST_F(ReplayStatusTest, CompletionExceptionMarksFailedWithoutRepeatingHook) {
    sink.throwDone=true; ASSERT_TRUE(r.prepare()); EXPECT_THROW(r.run(),std::runtime_error);
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_CALLBACK_EXCEPTION); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_SUCCEEDED);
    EXPECT_EQ(events.back(),"BT_FAILED"); EXPECT_FALSE(HisDataReplayerTestAccess::running(r)); r.run(); EXPECT_EQ(sink.doneCount,1u);
}
TEST_F(ReplayStatusTest, NoSourceAndMissingMappingAreFailures) {
    sink.task=false; ASSERT_TRUE(r.prepare()); r.run(); state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_NO_SOURCE); EXPECT_EQ(sink.doneCount,1u);
    ASSERT_TRUE(r.prepare()); HisDataReplayerTestAccess::missingSubscription(r); r.run(); state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_DATE_MAPPING);
    EXPECT_EQ(sink.doneCount,2u); EXPECT_EQ(events.back(),"BT_FAILED");
}
TEST_F(ReplayStatusTest, StatusCopiesAndConcurrentReadsAreSafe) {
    ASSERT_TRUE(r.prepare()); auto saved=r.replay_status(); std::atomic<bool> readDone{false};
    std::thread reader([&] { for(int i=0;i<10000;++i) { auto v=r.replay_status(); EXPECT_EQ(v.abi_version,1u); } readDone=true; });
    r.stop(); r.run(); reader.join(); EXPECT_TRUE(readDone); EXPECT_EQ(saved.state,WT_REPLAY_PREPARED); state(WT_REPLAY_STOPPED);
}

TEST_F(ReplayStatusTest, ConcurrentPrepareAndRunDoNotOverwriteActiveGeneration) {
    sink.blockInit=true; bool firstResult=false;
    std::thread first([&]{ firstResult=r.prepare(); });
    { std::unique_lock<std::mutex> lock(sink.barrierMutex);
      EXPECT_TRUE(sink.barrier.wait_for(lock,std::chrono::seconds(5),[&]{return sink.entered;})); }
    auto active=r.replay_status(); EXPECT_EQ(active.state,WT_REPLAY_PREPARING); EXPECT_FALSE(r.prepare()); r.run();
    EXPECT_EQ(r.replay_status().generation,active.generation);
    { std::lock_guard<std::mutex> lock(sink.barrierMutex); sink.release=true; } sink.barrier.notify_all(); first.join();
    ASSERT_TRUE(firstResult); sink.blockSchedule=true;
    { std::lock_guard<std::mutex> lock(sink.barrierMutex); sink.entered=false; sink.release=false; }
    std::thread runner([&]{r.run();});
    { std::unique_lock<std::mutex> lock(sink.barrierMutex);
      EXPECT_TRUE(sink.barrier.wait_for(lock,std::chrono::seconds(5),[&]{return sink.entered;})); }
    active=r.replay_status(); EXPECT_EQ(active.state,WT_REPLAY_RUNNING); r.run(); EXPECT_FALSE(r.prepare());
    EXPECT_EQ(r.replay_status().generation,active.generation);
    { std::lock_guard<std::mutex> lock(sink.barrierMutex); sink.release=true; } sink.barrier.notify_all(); runner.join();
    state(WT_REPLAY_SUCCEEDED); EXPECT_EQ(sink.doneCount,1u);
}
TEST_F(ReplayStatusTest, InternalFailureAfterStopTakesPrecedence) {
    ASSERT_TRUE(r.prepare()); r.stop(); r.report_replay_error(WT_REPLAY_ERROR_INTERNAL,"internal failure"); r.run();
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_INTERNAL); EXPECT_EQ(events.back(),"BT_FAILED");
}
TEST_F(ReplayStatusTest, StopDuringScheduleFinishesAsStopped) {
    sink.stopSchedule=true; ASSERT_TRUE(r.prepare()); r.run(); state(WT_REPLAY_STOPPED);
    EXPECT_GE(sink.scheduleCount,1u); EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_STOPPED); EXPECT_EQ(sink.doneStatus.flags,0u);
    EXPECT_EQ(events.back(),"BT_STOPPED");
}
TEST_F(ReplayStatusTest, MissingInitialDateMappingFailsBeforeInit) {
    HisDataReplayer missing; Sink s; s.rp=&missing; missing.set_time_range(202402230930ULL,202402230932ULL);
    missing.register_sink(&s,"test"); EXPECT_FALSE(missing.prepare()); auto status=missing.replay_status();
    EXPECT_EQ(status.state,WT_REPLAY_FAILED); EXPECT_EQ(status.error_code,WT_REPLAY_ERROR_DATE_MAPPING);
    EXPECT_EQ(status.flags,WT_REPLAY_STATUS_FINAL); EXPECT_EQ(s.doneCount,1u); EXPECT_EQ(s.initStatus.state,WT_REPLAY_IDLE);
}

TEST_F(ReplayStatusTest, OutsideMainBarIsNoSourceAndDoesNotPublishCompletionProgress) {
    sink.task=false; ASSERT_TRUE(r.prepare()); HisDataReplayerTestAccess::outsideBar(r); r.run();
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_NO_SOURCE); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(events.back(),"BT_FAILED"); EXPECT_TRUE(progress.empty());
}
TEST_F(ReplayStatusTest, TickFileWithoutDispatchableEventIsNoSource) {
    sink.task=false; ASSERT_TRUE(r.prepare()); HisDataReplayerTestAccess::filteredTick(r); r.run();
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_NO_SOURCE); EXPECT_EQ(sink.doneCount,1u); EXPECT_EQ(events.back(),"BT_FAILED");
}

TEST_F(ReplayStatusTest, TerminalNotifierExceptionFinalizesFailedWithoutRepeatingDone) {
    throwOnEnd=true; ASSERT_TRUE(r.prepare()); EXPECT_THROW(r.run(),std::runtime_error);
    state(WT_REPLAY_FAILED,WT_REPLAY_ERROR_CALLBACK_EXCEPTION); EXPECT_EQ(sink.doneCount,1u);
    EXPECT_EQ(sink.doneStatus.state,WT_REPLAY_SUCCEEDED); EXPECT_EQ(sink.doneStatus.flags,0u);
    EXPECT_FALSE(HisDataReplayerTestAccess::running(r)); r.run(); EXPECT_EQ(sink.doneCount,1u);
}
TEST_F(ReplayStatusTest, InactiveInternalAbortPreservesPublishedStatus) {
    r.report_replay_error(WT_REPLAY_ERROR_INTERNAL,"idle error"); state(WT_REPLAY_IDLE);
    EXPECT_TRUE(HisDataReplayerTestAccess::terminated(r)); ASSERT_TRUE(r.prepare());
    EXPECT_FALSE(HisDataReplayerTestAccess::terminated(r)); sink.reportDuringDone=true; r.run();
    state(WT_REPLAY_SUCCEEDED); EXPECT_FALSE(HisDataReplayerTestAccess::terminated(r)); auto terminal=r.replay_status();
    r.report_replay_error(WT_REPLAY_ERROR_INTERNAL,"late error"); EXPECT_TRUE(HisDataReplayerTestAccess::terminated(r));
    auto after=r.replay_status(); EXPECT_EQ(after.state,terminal.state); EXPECT_EQ(after.generation,terminal.generation); EXPECT_EQ(after.flags,terminal.flags);
}
