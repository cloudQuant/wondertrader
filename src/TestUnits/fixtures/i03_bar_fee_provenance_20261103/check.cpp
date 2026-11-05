#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"
#include "WtBtCore/WtHelper.h"
#include "WtDataStorage/DataDefine.h"
#include "WTSTools/WTSLogger.h"
#define private public
#define protected public
#include "WtBtCore/HisDataReplayer.h"
#include "WtBtCore/CtaMocker.h"
#undef protected
#undef private

static WTSBarStruct bar(uint32_t date,double price=100) {
    WTSBarStruct b{}; b.date=date; b.open=b.high=b.low=b.close=b.settle=price; b.vol=10; return b;
}
struct Loader : IBtDataLoader {
    std::map<std::string,std::vector<WTSBarStruct>> raw, final;
    HisDataReplayer* replayer=nullptr; bool reenter=false, reentered=false;
    bool loadFinalHisBars(void* o,const char* code,WTSKlinePeriod,FuncReadBars cb) override {
        auto i=final.find(code); if(i==final.end()) return false; cb(o,i->second.data(),static_cast<uint32_t>(i->second.size())); return true;
    }
    bool loadRawHisBars(void* o,const char* code,WTSKlinePeriod,FuncReadBars cb) override {
        if(reenter && !reentered && replayer) {
            reentered=true;
            WTSKlineSlice* s=replayer->get_kline_slice("SHFE.rb.2405","d",10,1,false); if(s) s->release();
        }
        auto i=raw.find(code); if(i==raw.end()) return false; cb(o,i->second.data(),static_cast<uint32_t>(i->second.size())); return true;
    }
    bool loadAllAdjFactors(void*,FuncReadFactors) override { return false; }
    bool loadAdjFactors(void*,const char*,FuncReadFactors) override { return false; }
    bool loadRawHisTicks(void*,const char*,uint32_t,FuncReadTicks) override { return false; }
    bool isAutoTrans() override { return false; }
};
struct ConsumerSink : IDataSink {
    HisDataReplayer* r; CtaMocker* mocker; std::string subscription, period="d"; uint32_t times; bool reenter=false, queried=false, trade=true;
    unsigned done=0; std::vector<std::string> sources; std::vector<uint32_t> dates; std::vector<double> rates, scales, prices;
    ConsumerSink(HisDataReplayer* rp,CtaMocker* m,std::string code,uint32_t n=1) : r(rp),mocker(m),subscription(std::move(code)),times(n) {}
    void handle_init() override { auto* s=r->get_kline_slice(subscription.c_str(),period.c_str(),10,times,true); if(s) s->release(); }
    void handle_tick(const char* code,WTSTickData* tick,uint32_t type) override {
        std::string expected=subscription; if (!expected.empty() && (expected.back()=='+' || expected.back()=='-')) expected.pop_back();
        if (code != expected) return;
        auto* physical=tick->getContractInfo(); sources.emplace_back(physical ? physical->getFullCode() : ""); dates.push_back(r->get_trading_date());
        wt_fee::Context context; std::string error; bool ok=r->prepare_fee_context(code,physical,context,error);
        rates.push_back(ok ? context.rates.open : -1); scales.push_back(ok ? context.volume_scale : -1); prices.push_back(tick->price());
        if (trade) mocker->handle_tick(code,tick,type);
        if(trade && type==0) mocker->stra_set_position(code,r->get_trading_date()==20240102 ? 1 : 2,"bar");
        if(reenter && !queried) { queried=true; auto* s=r->get_kline_slice("SHFE.rb.2405","d",10,1,false); if(s) s->release(); }
    }
    void handle_bar_close(const char*,const char*,uint32_t,WTSBarStruct*) override {}
    void handle_schedule(uint32_t,uint32_t) override {}
    void handle_session_begin(uint32_t) override {}
    void handle_session_end(uint32_t) override {}
    void handle_replay_done() override { ++done; }
};
class BarFeeTest : public ::testing::Test {
protected:
    HisDataReplayer r; Loader loader; std::filesystem::path root;
    std::string write(const char* name,const std::string& data) { auto p=root/name; std::ofstream(p)<<data; return p.string(); }
    void SetUp() override {
        static bool logger=[] { WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})",false,nullptr); return true; }(); (void)logger;
        char name[]="/tmp/wt-bar-fee-XXXXXX"; char* dir=mkdtemp(name); ASSERT_NE(dir,nullptr); root=dir;
        WtHelper::setInstDir("/tmp/ctpm-i03-build/build_x64/Debug/bin/"); WtHelper::setOutputDir(root.c_str()); loader.replayer=&r;
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root,ec); }
    void fileBars(const char* code,const std::vector<WTSBarStruct>& bars) {
        auto p=root/"his"/"day"/"SHFE"/(std::string(code)+".dsb"); std::filesystem::create_directories(p.parent_path());
        BlockHeader h{}; std::strcpy(h._blk_flag,BLK_FLAG); h._type=BT_HIS_Day; h._version=BLOCK_VERSION_RAW_V2;
        std::ofstream out(p,std::ios::binary); out.write(reinterpret_cast<const char*>(&h),sizeof(h)); out.write(reinterpret_cast<const char*>(bars.data()),bars.size()*sizeof(WTSBarStruct));
    }
    bool initialize(bool useLoader, const char* mode="bin", bool missingSecond=false) {
        auto sessions=write("sessions.json",R"({"TRADING":{"name":"trading","sections":[{"from":900,"to":1500}]}})");
        auto holidays=write("holidays.json",R"({"CHINA":[]})");
        auto comms=write("comms.json",R"({"SHFE":{"rb":{"name":"steel","session":"TRADING","holiday":"CHINA","pricetick":1,"volscale":10}}})");
        auto contracts=write("contracts.json",missingSecond ? R"({"SHFE":{"rb2405":{"name":"May","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20291231,"volscale":10}}})" : R"({"SHFE":{"rb2405":{"name":"May","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20291231,"volscale":10},"rb2409":{"name":"Sep","exchg":"SHFE","product":"rb","opendate":20200101,"expiredate":20291231,"volscale":20}}})");
        auto rules=write("rules.json",R"({"SHFE":{"rb":[{"date":20240102,"from":"","to":"rb2405","oldclose":0,"newclose":100},{"date":20240103,"from":"rb2405","to":"rb2409","oldclose":200,"newclose":100}]}})");
        auto fees=write("fees.json",missingSecond ? R"({"version":2,"contracts":{"SHFE.rb2405":{"open":2,"close":2,"closetoday":2,"byvolume":true},"SHFE.rb2409":{"open":7,"close":7,"closetoday":7,"byvolume":true}},"products":{}})" : R"({"version":2,"contracts":{"SHFE.rb2405":{"open":0.01,"close":0.01,"closetoday":0.01,"byvolume":false},"SHFE.rb2409":{"open":0.02,"close":0.02,"closetoday":0.02,"byvolume":false}},"products":{}})");
        auto* cfg=WTSVariant::createObject(); cfg->append("mode",mode); cfg->append("path",root.c_str()); cfg->append("fees",fees.c_str());
        cfg->append("stime",uint64_t(202401020900)); cfg->append("etime",uint64_t(202401031500)); cfg->append("tick",false);
        auto* base=WTSVariant::createObject(); base->append("session",sessions.c_str()); base->append("holiday",holidays.c_str());
        base->append("commodity",comms.c_str()); base->append("contract",contracts.c_str()); base->append("hot",rules.c_str()); cfg->append("basefiles",base); base->release();
        bool ok=r.init(cfg,nullptr,useLoader ? &loader : nullptr); cfg->release(); return ok;
    }
    void sources() {
        loader.raw["SHFE.rb.2405"]={bar(20240101,90),bar(20240102),bar(20240103,999)};
        loader.raw["SHFE.rb.2409"]={bar(20240102,999),bar(20240103),bar(20240104,90)};
        fileBars("rb2405",loader.raw["SHFE.rb.2405"]); fileBars("rb2409",loader.raw["SHFE.rb.2409"]);
    }
};
TEST_F(BarFeeTest, ExternalRawSegmentsFeedActualCtaFillsAcrossRoll) {
    sources(); loader.reenter=true; ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-raw",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT"); sink.reenter=true;
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run();
    EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED); EXPECT_TRUE(r.fee_error().empty()) << r.fee_error();
    ASSERT_EQ(sink.sources.size(),8u); for(size_t i=0;i<4;i++) { EXPECT_EQ(sink.sources[i],"SHFE.rb2405"); EXPECT_EQ(sink.rates[i],0.01); EXPECT_EQ(sink.scales[i],10); EXPECT_EQ(sink.dates[i],20240102u); }
    for(size_t i=4;i<8;i++) { EXPECT_EQ(sink.sources[i],"SHFE.rb2409"); EXPECT_EQ(sink.rates[i],0.02); EXPECT_EQ(sink.scales[i],20); EXPECT_EQ(sink.dates[i],20240103u); }
    EXPECT_TRUE(loader.reentered); EXPECT_TRUE(sink.queried); EXPECT_DOUBLE_EQ(m._fund_info._total_fees,50); EXPECT_DOUBLE_EQ(m._pos_map["SHFE.rb.HOT"]._volume,2); EXPECT_EQ(sink.done,1u);
}
TEST_F(BarFeeTest, RawFilesPreserveEachPhysicalContract) {
    sources(); ASSERT_TRUE(initialize(false)); CtaMocker m(&r,"bar-files",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED);
    ASSERT_EQ(sink.sources.size(),8u); EXPECT_EQ(sink.sources.front(),"SHFE.rb2405"); EXPECT_EQ(sink.sources.back(),"SHFE.rb2409"); EXPECT_DOUBLE_EQ(m._fund_info._total_fees,50);
}
TEST_F(BarFeeTest, AdjustedSegmentsRetainPhysicalSource) {
    sources(); ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-adjusted",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT+"); sink.trade=false;
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED);
    ASSERT_EQ(sink.sources.size(),16u);
    for(size_t i=0;i<sink.sources.size();++i) EXPECT_EQ(sink.sources[i],sink.dates[i]==20240102 ? "SHFE.rb2405" : "SHFE.rb2409");
    EXPECT_NE(std::find(sink.prices.begin(),sink.prices.end(),200),sink.prices.end());
    EXPECT_NE(std::find(sink.prices.begin(),sink.prices.end(),100),sink.prices.end());
}
TEST_F(BarFeeTest, ContinuousAggregateFileCannotSupplyPhysicalIdentity) {
    sources(); fileBars("SHFE.rb_HOT",{bar(20240102),bar(20240103)}); ASSERT_TRUE(initialize(false)); CtaMocker m(&r,"bar-aggregate",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED);
    ASSERT_FALSE(sink.sources.empty()); EXPECT_EQ(sink.sources.front(),""); EXPECT_FALSE(r.fee_error().empty()); EXPECT_EQ(m._pos_map.count("SHFE.rb.HOT"),0u);
}
TEST_F(BarFeeTest, ExternalFinalContinuousBarsRemainUnknown) {
    sources(); loader.final["SHFE.rb.HOT"]={bar(20240102),bar(20240103)}; ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-final",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED);
    ASSERT_FALSE(sink.sources.empty()); EXPECT_EQ(sink.sources.front(),""); EXPECT_FALSE(r.fee_error().empty()); EXPECT_EQ(m._pos_map.count("SHFE.rb.HOT"),0u);
}
TEST_F(BarFeeTest, MultiDayAggregateOfDifferentContractsIsRejected) {
    sources(); ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-multi",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT",2);
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare());
    auto raw=r._bars_cache.at("SHFE.rb.HOT#d#1"); auto aggregate=r._bars_cache.at("SHFE.rb.HOT#d#2");
    ASSERT_EQ(raw->_physical_sources.size(),3u); EXPECT_EQ(raw->_physical_sources[0],"rb2405"); EXPECT_EQ(raw->_physical_sources[1],"rb2409");
    ASSERT_EQ(aggregate->_bars.size(),2u); EXPECT_EQ(aggregate->_bars[0].vol,20); ASSERT_EQ(aggregate->_physical_sources.size(),2u); EXPECT_TRUE(aggregate->_physical_sources[0].empty()); EXPECT_EQ(aggregate->_physical_sources[1],"rb2409");
    r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED);
    ASSERT_FALSE(sink.sources.empty()); EXPECT_EQ(sink.sources.front(),""); EXPECT_FALSE(r.fee_error().empty()); EXPECT_EQ(m._pos_map.count("SHFE.rb.HOT"),0u);
}
TEST_F(BarFeeTest, ExplicitPhysicalSubscriptionRemainsSupported) {
    sources(); ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-physical",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.2405");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED);
    ASSERT_FALSE(sink.sources.empty()); for(auto& source:sink.sources) EXPECT_EQ(source,"SHFE.rb2405");
}

TEST_F(BarFeeTest, PartialAggregateStaysUnknownWhileLaterRawSegmentIsKnown) {
    sources(); fileBars("SHFE.rb_HOT",{bar(20240102)}); ASSERT_TRUE(initialize(false));
    CtaMocker m(&r,"bar-partial",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT"); sink.trade=false;
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED);
    ASSERT_EQ(sink.sources.size(),8u); for(size_t i=0;i<4;i++) EXPECT_TRUE(sink.sources[i].empty());
    for(size_t i=4;i<8;i++) EXPECT_EQ(sink.sources[i],"SHFE.rb2409");
}
TEST_F(BarFeeTest, DuplicateTimestampInvalidatesAllContinuousProvenance) {
    sources(); loader.raw["SHFE.rb.2405"]={bar(20240102),bar(20240102)}; ASSERT_TRUE(initialize(true));
    CtaMocker m(&r,"bar-duplicate",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); auto raw=r._bars_cache.at("SHFE.rb.HOT#d#1");
    ASSERT_EQ(raw->_bars.size(),4u); EXPECT_TRUE(raw->_physical_sources.empty()); r.run();
    EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED); EXPECT_FALSE(r.fee_error().empty());
    ASSERT_FALSE(sink.sources.empty()); EXPECT_TRUE(sink.sources.front().empty());
}
TEST_F(BarFeeTest, ContinuousCsvWithoutSourceIsRejected) {
    std::filesystem::create_directories(root/"csv"); std::ofstream out(root/"csv"/"SHFE.rb.HOT_d.csv");
    out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n"
        << "20240102,150000,100,100,100,100,10,1000,0,0,100\n20240103,150000,100,100,100,100,10,1000,0,0,100\n";
    out.close(); ASSERT_TRUE(out.good()); ASSERT_TRUE(initialize(false,"csv"));
    CtaMocker m(&r,"bar-csv",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT"); r.register_sink(&sink,"test");
    ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED); EXPECT_FALSE(r.fee_error().empty());
    ASSERT_FALSE(sink.sources.empty()); EXPECT_TRUE(sink.sources.front().empty()); EXPECT_EQ(m._pos_map.count("SHFE.rb.HOT"),0u);
}

TEST_F(BarFeeTest, MissingRegisteredMonthlyContractRejectsOnlyThatFill) {
    sources(); ASSERT_TRUE(initialize(true,"bin",true)); CtaMocker m(&r,"bar-missing",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED);
    ASSERT_GT(sink.sources.size(),4u); EXPECT_EQ(sink.sources[0],"SHFE.rb2405"); EXPECT_TRUE(sink.sources[4].empty());
    EXPECT_FALSE(r.fee_error().empty()); EXPECT_DOUBLE_EQ(m._pos_map["SHFE.rb.HOT"]._volume,1); EXPECT_DOUBLE_EQ(m._fund_info._total_fees,2);
}
TEST_F(BarFeeTest, FailedUnorderedLoadCanRetryWithoutStaleCache) {
    sources(); loader.raw["SHFE.rb.2409"]={bar(20240104),bar(20240103)};
    ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-retry",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT");
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); EXPECT_EQ(r._bars_cache.count("SHFE.rb.HOT#d#1"),0u);
    loader.raw["SHFE.rb.2409"]={bar(20240103),bar(20240104)};
    auto* slice=r.get_kline_slice("SHFE.rb.HOT","d",10,1,true); if(slice) slice->release();
    ASSERT_EQ(r._bars_cache.count("SHFE.rb.HOT#d#1"),1u);
    auto cached=r._bars_cache.at("SHFE.rb.HOT#d#1"); ASSERT_EQ(cached->_bars.size(),3u); ASSERT_EQ(cached->_physical_sources.size(),3u);
    r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED); EXPECT_DOUBLE_EQ(m._fund_info._total_fees,50);
}

TEST_F(BarFeeTest, MinuteRawSegmentsFeedActualCtaFillsAcrossRoll) {
    auto mbar=[](uint32_t date, uint32_t minute) { WTSBarStruct value=bar(date); value.time=static_cast<uint64_t>(date-19900000)*10000+minute; return value; };
    loader.raw["SHFE.rb.2405"]={mbar(20240101,930),mbar(20240102,930),mbar(20240103,930)};
    loader.raw["SHFE.rb.2409"]={mbar(20240102,930),mbar(20240103,930),mbar(20240104,930)};
    ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-minute",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT"); sink.period="m";
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare()); r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED);
    ASSERT_EQ(sink.sources.size(),8u); for(size_t i=0;i<4;i++) EXPECT_EQ(sink.sources[i],"SHFE.rb2405");
    for(size_t i=4;i<8;i++) EXPECT_EQ(sink.sources[i],"SHFE.rb2409");
    EXPECT_DOUBLE_EQ(m._fund_info._total_fees,50); EXPECT_DOUBLE_EQ(m._pos_map["SHFE.rb.HOT"]._volume,2);
}
