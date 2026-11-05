#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include "WtBtCore/HisDataReplayer.h"
#include "WTSTools/WTSLogger.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

struct HisDataReplayerTestAccess {
    static WTSBaseDataMgr& manager(HisDataReplayer& r) { return r._bd_mgr; }
    static WTSHotMgr& hot(HisDataReplayer& r) { return r._hot_mgr; }
    static void setup(HisDataReplayer& r, IDataSink* sink, const std::string& code, const std::vector<WTSTickStruct>& items) {
        r._listener = sink; r._cur_tdate=20240223; r._tick_enabled=true; r._running.store(true); r._terminated.store(false);
        r._dispatched_tick_subscriptions.insert(code); auto& l=r._replay_ticks_cache[code][20240223];
        l._items=items; l._count=items.size(); l._cursor=UINT_MAX;
    }
    static bool dispatch(HisDataReplayer& r, const std::string& code) {
        HisDataReplayer::ReplaySubscriptionSnapshot s; s.ticks.insert(code); bool did=false;
        return r.replayHftInterval(20240223090000000ULL,20240223100000000ULL,did,s) && did;
    }
    static void stopped(HisDataReplayer& r) { r._running.store(false); }
    static bool terminated(const HisDataReplayer& r) { return r._terminated.load(); }
    static void reset(HisDataReplayer& r) { r.reset(); }
    static void ready(HisDataReplayer& r, IDataSink* sink) { r._time_range_valid=true; r._listener=sink;
        r._range_begin_date=20240223; r._range_begin_hhmm=900; r._range_end_date=20240223; r._range_end_hhmm=1000; }
};
struct Sink : IDataSink {
    HisDataReplayer* rp=nullptr; std::vector<std::string> callback, last; std::vector<double> fees, legacy;
    void handle_tick(const char* code,WTSTickData* tick,uint32_t) override {
        auto* c=tick->getContractInfo(); callback.emplace_back(c ? c->getFullCode() : "");
        wt_fee::Context ctx; std::string err; bool ok=rp->prepare_fee_context(code,c,ctx,err);
        fees.push_back(ok ? wt_fee::calculate(ctx,100,1,0).value : -1);
        legacy.push_back(rp->calc_fee(code,100,1,0));
        WTSTickData* t=rp->get_last_tick(code); auto* lc=t ? t->getContractInfo() : nullptr;
        last.emplace_back(lc ? lc->getFullCode() : ""); if(t) t->release();
    }
    void handle_bar_close(const char*,const char*,uint32_t,WTSBarStruct*) override {}
    void handle_schedule(uint32_t,uint32_t) override {}
    void handle_init() override {}
    void handle_session_begin(uint32_t) override {}
    void handle_session_end(uint32_t) override {}
};
class FeeIdentityTest : public ::testing::Test {
protected:
    HisDataReplayer r; Sink sink; std::filesystem::path root;
    std::string write(const char* name,const std::string& data) { auto p=root/name; std::ofstream(p)<<data; return p.string(); }
    void SetUp() override {
        static bool initialized=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)initialized;
        char name[]="/tmp/wt-replay-fee-XXXXXX"; char* dir=mkdtemp(name); ASSERT_NE(dir,nullptr); root=dir; sink.rp=&r;
        auto sessions=write("sessions.json",R"({"ALL":{"name":"all","sections":[{"from":0,"to":0}]},"TRADING":{"name":"trading","sections":[{"from":900,"to":1500}]}})");
        auto holidays=write("holidays.json",R"({"CHINA":[]})");
        auto comms=write("comms.json",R"({"SHFE":{"rb":{"name":"steel","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":10},"cu":{"name":"copper","session":"ALL","holiday":"CHINA","pricetick":1,"volscale":5}}})");
        auto contracts=write("contracts.json",R"({"SHFE":{"rb2405":{"name":"steel May","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101},"rb2410":{"name":"steel Oct","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101,"volscale":20},"rb2501":{"name":"zero scale","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101,"volscale":0},"rb1901":{"name":"expired historical","exchg":"SHFE","product":"rb","opendate":20180101,"expiredate":20191231},"HOT":{"name":"not physical","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20300101},"cu2405":{"name":"copper May","exchg":"SHFE","product":"cu","opendate":20200101,"expiredate":20300101}}})");
        auto& m=HisDataReplayerTestAccess::manager(r); ASSERT_TRUE(m.loadSessions(sessions.c_str())); ASSERT_TRUE(m.loadHolidays(holidays.c_str())); ASSERT_TRUE(m.loadCommodities(comms.c_str())); ASSERT_TRUE(m.loadContracts(contracts.c_str()));
    }
    void TearDown() override { HisDataReplayerTestAccess::stopped(r); std::error_code ec; std::filesystem::remove_all(root,ec); }
    std::string fees() { return write("fees.json",R"({"version":2,"contracts":{"SHFE.rb2405":{"open":0,"close":0,"closetoday":0,"byvolume":true}},"products":{"SHFE.rb":{"open":2,"close":2,"closetoday":2,"byvolume":true}}})"); }
    WTSTickStruct tick(const char* code,const char* exchg,uint32_t time,uint32_t tdate=20240223) {
        WTSTickStruct t{}; std::strncpy(t.code,code,sizeof(t.code)-1); std::strncpy(t.exchg,exchg,sizeof(t.exchg)-1);
        t.trading_date=tdate; t.action_date=20240223; t.action_time=time; t.price=100; return t;
    }
};
TEST_F(FeeIdentityTest, ContextUsesPhysicalOverrideAndProductFallbackWithZeroValid) {
    ASSERT_TRUE(r.loadFeesChecked(fees().c_str())); auto& mgr=HisDataReplayerTestAccess::manager(r); auto* may=mgr.getContract("rb2405","SHFE"); auto* oct=mgr.getContract("rb2410","SHFE"); ASSERT_NE(may,nullptr); ASSERT_NE(oct,nullptr);
    wt_fee::Context ctx; std::string err;
    ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.2405",nullptr,ctx,err)); EXPECT_EQ(ctx.physical_contract,"SHFE.rb2405"); EXPECT_EQ(ctx.product,"SHFE.rb"); EXPECT_EQ(ctx.rates.open,0); EXPECT_EQ(wt_fee::calculate(ctx,100,1,0).value,0);
    ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.HOT",oct,ctx,err)); EXPECT_EQ(ctx.physical_contract,"SHFE.rb2410"); EXPECT_EQ(ctx.rates.open,2); EXPECT_EQ(ctx.volume_scale,20); auto amount=ctx; amount.rates.by_volume=false; amount.rates.open=0.01; EXPECT_EQ(wt_fee::calculate(amount,100,1,0).value,20);
    EXPECT_TRUE(std::isnan(r.calc_fee("SHFE.rb.HOT",100,1,0)));
    wt_fee::Context saved=ctx;
    auto rules=write("rules.json", "{}"); ASSERT_TRUE(HisDataReplayerTestAccess::hot(r).loadCustomRules("ROLL",rules.c_str()));
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.ROLL",nullptr,ctx,err));
    ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.ROLL",oct,ctx,err)); EXPECT_EQ(ctx.physical_contract,"SHFE.rb2410");
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2ND",nullptr,ctx,err));
    ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.2ND",oct,ctx,err));
    ctx.physical_contract="sentinel"; EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.HOT",nullptr,ctx,err)); EXPECT_EQ(ctx.physical_contract,"sentinel");
    EXPECT_EQ(saved.physical_contract,"SHFE.rb2410"); EXPECT_EQ(saved.volume_scale,20);
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",oct,ctx,err)); EXPECT_EQ(ctx.physical_contract,"sentinel");
    EXPECT_FALSE(r.prepare_fee_context("SHFE.cu.2405",may,ctx,err)); EXPECT_EQ(ctx.physical_contract,"sentinel");
    EXPECT_FALSE(r.prepare_fee_context("SHFE.cu.2405",nullptr,ctx,err)); EXPECT_EQ(ctx.physical_contract,"sentinel");
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2501",nullptr,ctx,err)); EXPECT_EQ(ctx.physical_contract,"sentinel"); EXPECT_NE(err.find("volume scale"),std::string::npos);
}
TEST_F(FeeIdentityTest, RealDispatchAndGetterCarrySameEventIdentityAcrossRoll) {
    ASSERT_TRUE(r.loadFeesChecked(fees().c_str())); std::string code="SHFE.rb.HOT";
    std::vector<WTSTickStruct> ticks{tick("rb2405","SHFE",93000000),tick("rb2410","SHFE",93100000),tick("cu2405","SHFE",93200000),tick("","SHFE",93300000),tick("rb2405","SHFE",93400000,20240222)};
    HisDataReplayerTestAccess::setup(r,&sink,code,ticks); ASSERT_TRUE(HisDataReplayerTestAccess::dispatch(r,code));
    EXPECT_EQ(sink.callback,(std::vector<std::string>{"SHFE.rb2405","SHFE.rb2410","","",""})); EXPECT_EQ(sink.last,sink.callback);
    EXPECT_EQ(sink.fees,(std::vector<double>{0,2,-1,-1,-1}));
    ASSERT_EQ(sink.legacy.size(),5u); EXPECT_EQ(sink.legacy[0],0); EXPECT_EQ(sink.legacy[1],2);
    for (size_t i=2;i<5;i++) EXPECT_TRUE(std::isnan(sink.legacy[i]));
    WTSTickData* last=r.get_last_tick(code.c_str()); ASSERT_NE(last,nullptr); EXPECT_EQ(last->getContractInfo(),nullptr); last->release();
    HisDataReplayerTestAccess::reset(r); EXPECT_EQ(r.get_last_tick(code.c_str()),nullptr);
}
TEST_F(FeeIdentityTest, ReloadInvalidatesOldRatesAndSuccessfulYamlReloadClearsFirstError) {
    ASSERT_TRUE(r.loadFeesChecked(fees().c_str())); auto bad=write("bad.json",R"({"SHFE.rb":{"open":0,"close":0,"byvolume":true}})");
    EXPECT_FALSE(r.loadFeesChecked(bad.c_str())); wt_fee::Context c; c.product="sentinel"; std::string e;
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",nullptr,c,e)); EXPECT_EQ(c.product,"sentinel");
    r.report_fee_error("first"); r.report_fee_error("second"); EXPECT_EQ(r.fee_error(),"first"); EXPECT_TRUE(HisDataReplayerTestAccess::terminated(r));
    auto yaml=write("fees.yaml","SHFE.rb:\n  open: 1\n  close: 2\n  closetoday: 3\n  byvolume: true\n");
    ASSERT_TRUE(r.loadFeesChecked(yaml.c_str())); EXPECT_TRUE(r.fee_error().empty()); ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.2405",nullptr,c,e)); EXPECT_EQ(c.rates.open,1);
    auto badyaml=write("bad.yaml","SHFE.rb:\n  open: 0\n  close: 0\n  byvolume: true\n"); EXPECT_FALSE(r.loadFeesChecked(badyaml.c_str())); EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",nullptr,c,e));
}

TEST_F(FeeIdentityTest, InitAllowsNoFeesButRejectsExplicitBadFees) {
    auto config=[&](const char* fee) {
        auto* cfg=WTSVariant::createObject(); cfg->append("mode","csv"); cfg->append("path",root.c_str());
        cfg->append("stime",uint64_t(202402230900)); cfg->append("etime",uint64_t(202402231000));
        auto* base=WTSVariant::createObject(); cfg->append("basefiles",base); base->release();
        if (fee) cfg->append("fees",fee); return cfg;
    };
    auto* cfg=config(nullptr); EXPECT_TRUE(r.init(cfg,nullptr,nullptr)); cfg->release();
    wt_fee::Context c; std::string e; EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",nullptr,c,e));
    const auto bad=write("bad-init.json",R"({"SHFE.rb":{"open":0,"close":0,"byvolume":true}})");
    cfg=config(bad.c_str()); EXPECT_FALSE(r.init(cfg,nullptr,nullptr)); cfg->release();
    cfg=config(nullptr); auto* invalid=WTSVariant::createObject(); cfg->append("fees",invalid); invalid->release();
    EXPECT_FALSE(r.init(cfg,nullptr,nullptr)); cfg->release();
}
TEST_F(FeeIdentityTest, FirstFeeErrorStopsPrepareAndExplicitSuccessfulReloadEnablesRetry) {
    ASSERT_TRUE(r.loadFeesChecked(fees().c_str())); HisDataReplayerTestAccess::ready(r,&sink);
    r.report_fee_error("first"); r.report_fee_error("second"); EXPECT_EQ(r.fee_error(),"first"); EXPECT_FALSE(r.prepare());
    r.run(); EXPECT_TRUE(HisDataReplayerTestAccess::terminated(r)); EXPECT_EQ(r.fee_error(),"first");
    auto bad=write("invalid.json",R"({"SHFE.rb":{"open":0}})"); EXPECT_FALSE(r.loadFeesChecked(bad.c_str())); EXPECT_EQ(r.fee_error(),"first");
    ASSERT_TRUE(r.loadFeesChecked(fees().c_str())); EXPECT_TRUE(r.fee_error().empty()); EXPECT_TRUE(r.prepare());
    r.stop(); r.run();
}
TEST_F(FeeIdentityTest, InitAmountFeeRequiresLoadedProductAndAcceptsValidProduct) {
    auto init=[&](const std::string& contents) {
        auto path=write("product-init.json",contents); auto* cfg=WTSVariant::createObject();
        cfg->append("mode","csv"); cfg->append("path",root.c_str());
        cfg->append("stime",uint64_t(202402230900)); cfg->append("etime",uint64_t(202402231000)); cfg->append("fees",path.c_str());
        auto* base=WTSVariant::createObject(); cfg->append("basefiles",base); base->release();
        bool result=r.init(cfg,nullptr,nullptr); cfg->release(); return result;
    };
    EXPECT_TRUE(init(R"({"SHFE.rb":{"open":0.001,"close":0.002,"closetoday":0,"byvolume":false}})"));
    r.report_fee_error("earlier failure");
    EXPECT_FALSE(init(R"({"SHFE.rb":{"open":0.001,"close":0.002,"closetoday":0,"byvolume":false},"SHFE.unknown":{"open":1,"close":1,"closetoday":1,"byvolume":false}})"));
    EXPECT_EQ(r.fee_error(),"earlier failure");
    wt_fee::Context out; out.product="untouched"; std::string error;
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",nullptr,out,error)); EXPECT_EQ(out.product,"untouched");
    EXPECT_TRUE(init(R"({"SHFE.unknown":{"open":1,"close":1,"closetoday":1,"byvolume":true}})"));
    EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.2405",nullptr,out,error));
}
TEST_F(FeeIdentityTest, InitAmountFeeRequiresRealContractAndPositiveMultiplier) {
    auto init=[&](const std::string& contents) {
        auto path=write("contract-init.json",contents); auto* cfg=WTSVariant::createObject();
        cfg->append("mode","csv"); cfg->append("path",root.c_str());
        cfg->append("stime",uint64_t(202402230900)); cfg->append("etime",uint64_t(202402231000)); cfg->append("fees",path.c_str());
        auto* base=WTSVariant::createObject(); cfg->append("basefiles",base); base->release();
        bool result=r.init(cfg,nullptr,nullptr); cfg->release(); return result;
    };
    EXPECT_TRUE(init(R"({"version":2,"contracts":{"SHFE.rb2410":{"open":0.01,"close":0.01,"closetoday":0,"byvolume":false}},"products":{}})"));
    EXPECT_TRUE(init(R"({"version":2,"contracts":{"SHFE.rb1901":{"open":0.01,"close":0.01,"closetoday":0,"byvolume":false}},"products":{}})"));
    EXPECT_FALSE(init(R"({"version":2,"contracts":{"SHFE.rb9999":{"open":0.01,"close":0.01,"closetoday":0,"byvolume":false}},"products":{}})"));
    EXPECT_FALSE(init(R"({"version":2,"contracts":{"SHFE.rb2501":{"open":0.01,"close":0.01,"closetoday":0,"byvolume":false}},"products":{}})"));
    EXPECT_FALSE(init(R"({"version":2,"contracts":{"SHFE.HOT":{"open":0.01,"close":0.01,"closetoday":0,"byvolume":false}},"products":{}})"));
    EXPECT_TRUE(init(R"({"version":2,"contracts":{"SHFE.rb9999":{"open":1,"close":1,"closetoday":1,"byvolume":true}},"products":{}})"));
    wt_fee::Context out; std::string error; EXPECT_FALSE(r.prepare_fee_context("SHFE.rb.9999",nullptr,out,error));
}
TEST_F(FeeIdentityTest, ActiveReplayRejectsReloadWithoutDiscardingExistingSchedule) {
    auto good=fees(); ASSERT_TRUE(r.loadFeesChecked(good.c_str()));
    wt_fee::Context before; std::string error; ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.2405",nullptr,before,error));
    HisDataReplayerTestAccess::setup(r,&sink,"SHFE.rb.HOT",{});
    auto bad=write("bad-active.json",R"({"broken":null})"); EXPECT_FALSE(r.loadFeesChecked(bad.c_str()));
    wt_fee::Context after; ASSERT_TRUE(r.prepare_fee_context("SHFE.rb.2405",nullptr,after,error));
    EXPECT_EQ(after.physical_contract,before.physical_contract); EXPECT_EQ(after.rates.open,before.rates.open);
}
