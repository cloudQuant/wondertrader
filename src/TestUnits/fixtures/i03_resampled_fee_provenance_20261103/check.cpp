#include "../i03_bar_fee_provenance_20261103/check.cpp"
#include "WTSTools/WTSDataFactory.h"
#include "Includes/WTSSessionInfo.hpp"

static WTSBarStruct timed(uint32_t date, uint32_t time, double price) {
    auto b=bar(date,price); b.time=static_cast<uint64_t>(date-19900000)*10000+time; return b;
}
using Range = WTSDataFactory::BarIndexRange;
static void compareFactory(std::vector<WTSBarStruct> input, WTSKlinePeriod period, uint32_t times,
                           WTSSessionInfo* session, bool include, bool align,
                           const std::vector<Range>& expected) {
    auto* slice=WTSKlineSlice::create("SHFE.rb.HOT",period,1,input.data(),input.size());
    ASSERT_NE(slice,nullptr); WTSDataFactory f;
    auto* legacy=f.extractKlineData(slice,period,times,session,include,align);
    std::vector<Range> ranges;
    auto* traced=f.extractKlineDataWithRanges(slice,period,times,session,ranges,include,align);
    ASSERT_NE(legacy,nullptr); ASSERT_NE(traced,nullptr);
    ASSERT_EQ(ranges,expected); ASSERT_EQ(legacy->size(),traced->size()); ASSERT_EQ(traced->size(),ranges.size());
    EXPECT_EQ(legacy->isClosed(),traced->isClosed());
    for (size_t i=0;i<ranges.size();++i) {
        const auto& a=*legacy->at(i); const auto& b=*traced->at(i);
        EXPECT_EQ(a.date,b.date); EXPECT_EQ(a.time,b.time); EXPECT_DOUBLE_EQ(a.open,b.open); EXPECT_DOUBLE_EQ(a.high,b.high);
        EXPECT_DOUBLE_EQ(a.low,b.low); EXPECT_DOUBLE_EQ(a.close,b.close); EXPECT_DOUBLE_EQ(a.vol,b.vol);
        double vol=0; for (uint32_t k=ranges[i].first;k<ranges[i].second;++k) vol+=input[k].vol;
        EXPECT_DOUBLE_EQ(b.vol,vol); EXPECT_DOUBLE_EQ(b.open,input[ranges[i].first].open); EXPECT_DOUBLE_EQ(b.close,input[ranges[i].second-1].close);
    }
    traced->release(); legacy->release(); slice->release();
}
TEST(ResampledRanges, DayAndPartialDayPreserveLegacyGrouping) {
    std::vector<WTSBarStruct> bars={bar(20240102,1),bar(20240103,2),bar(20240104,3),bar(20240105,4),bar(20240108,5)};
    compareFactory(bars,KP_DAY,2,nullptr,true,false,{{0,2},{2,4},{4,5}});
    // Legacy daily extraction includes its partial last bar for both values.
    compareFactory(bars,KP_DAY,2,nullptr,false,false,{{0,2},{2,4},{4,5}});
}
TEST(ResampledRanges, Minute1AlignsBySectionAndDropsOpenBar) {
    auto* s=WTSSessionInfo::create("two","two"); s->addTradingSection(900,903); s->addTradingSection(1000,1004);
    std::vector<WTSBarStruct> bars={timed(20240102,901,1),timed(20240102,902,2),timed(20240102,903,3),timed(20240102,1001,4),timed(20240102,1002,5),timed(20240102,1003,6)};
    compareFactory(bars,KP_Minute1,2,s,true,true,{{0,2},{2,3},{3,4},{4,6}});
    compareFactory(bars,KP_Minute1,2,s,false,true,{{0,2},{2,3},{3,4}});
    s->release();
}
TEST(ResampledRanges, Minute5HourAndHalfUseRealGrouping) {
    auto* s=WTSSessionInfo::create("day","day"); s->addTradingSection(900,1130); s->addTradingSection(1300,1500);
    std::vector<WTSBarStruct> bars={timed(20240102,905,1),timed(20240102,910,2),timed(20240102,915,3),timed(20240102,930,4),timed(20240102,935,5)};
    compareFactory(bars,KP_Minute5,2,s,true,false,{{0,2},{2,3},{3,4},{4,5}});
    compareFactory(bars,KP_Minute5,2,s,true,true,{{0,2},{2,3},{3,4},{4,5}});
    compareFactory(bars,KP_Minute5,PERIOD_TIMES_HALF,s,true,false,{{0,4},{4,5}});
    compareFactory(bars,KP_Minute5,PERIOD_TIMES_HOUR,s,true,false,{{0,5}});
    compareFactory(bars,KP_Minute5,PERIOD_TIMES_HOUR,s,false,false,{});
    s->release();
}
TEST(ResampledRanges, OvernightMinuteGroupsAcrossMidnight) {
    auto* s=WTSSessionInfo::create("night","night",-1260); s->addTradingSection(2100,230); 
    std::vector<WTSBarStruct> bars={timed(20240102,2355,1),timed(20240103,0,2),timed(20240103,5,3)};
    // Use actual trading date independently of the calendar day.
    for (auto& b:bars) b.date=20240103;
    compareFactory(bars,KP_Minute5,PERIOD_TIMES_HALF,s,true,false,{{0,2},{2,3}});
    compareFactory(bars,KP_Minute5,PERIOD_TIMES_HOUR,s,true,false,{{0,2},{2,3}});
    s->release();
}
struct DerivedFactory : WTSDataFactory {
    WTSDataFactory inner; std::vector<BarIndexRange> nested;
    WTSKlineData* extractKlineData(WTSKlineSlice* slice,WTSKlinePeriod period,uint32_t times,WTSSessionInfo* session,bool include,bool align) override {
        auto* data=inner.extractKlineDataWithRanges(slice,period,times,session,nested,include,align); if(data) data->release();
        return WTSDataFactory::extractKlineData(slice,period,times,session,include,align);
    }
};
TEST(ResampledRanges, DerivedOverrideRemainsUnknownAndNestedCallIndependent) {
    std::vector<WTSBarStruct> bars={bar(20240102),bar(20240103),bar(20240104)};
    auto* slice=WTSKlineSlice::create("SHFE.rb.HOT",KP_DAY,1,bars.data(),bars.size());
    DerivedFactory factory; std::vector<Range> outer={{99,100}};
    auto* result=factory.extractKlineDataWithRanges(slice,KP_DAY,2,nullptr,outer,true,false);
    ASSERT_NE(result,nullptr); EXPECT_TRUE(outer.empty()); EXPECT_EQ(factory.nested,(std::vector<Range>{{0,2},{2,3}}));
    EXPECT_EQ(result->size(),2u); result->release(); slice->release();
}
TEST_F(BarFeeTest, SamePhysicalMinuteGroupsReachActualCtaFeeAcrossRoll) {
    loader.raw["SHFE.rb.2405"]={timed(20240102,901,100),timed(20240102,902,100)};
    loader.raw["SHFE.rb.2409"]={timed(20240103,901,100),timed(20240103,902,100)};
    ASSERT_TRUE(initialize(true)); CtaMocker m(&r,"bar-resampled",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT",2); sink.period="m";
    sink.reenter=true; loader.reenter=true; r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare());
    auto grouped=r._bars_cache.at("SHFE.rb.HOT#m#2"); ASSERT_EQ(grouped->_physical_sources,(std::vector<std::string>{"rb2405","rb2409"}));
    r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_SUCCEEDED); EXPECT_TRUE(r.fee_error().empty());
    ASSERT_EQ(sink.sources.size(),8u); for(size_t i=0;i<4;++i) EXPECT_EQ(sink.sources[i],"SHFE.rb2405");
    for(size_t i=4;i<8;++i) EXPECT_EQ(sink.sources[i],"SHFE.rb2409");
    EXPECT_DOUBLE_EQ(m._fund_info._total_fees,50); EXPECT_DOUBLE_EQ(m._pos_map["SHFE.rb.HOT"]._volume,2);
}
TEST_F(BarFeeTest, OneUnknownConstituentMakesOnlyItsAggregateUnknown) {
    loader.raw["SHFE.rb.2405"]={timed(20240102,901,100),timed(20240102,902,100)};
    loader.raw["SHFE.rb.2409"]={timed(20240103,901,100),timed(20240103,902,100)};
    ASSERT_TRUE(initialize(true)); auto* raw=r.get_kline_slice("SHFE.rb.HOT","m",10,1,false); if(raw) raw->release();
    auto base=r._bars_cache.at("SHFE.rb.HOT#m#1"); ASSERT_EQ(base->_physical_sources.size(),4u); base->_physical_sources[1].clear();
    CtaMocker m(&r,"bar-unknown-mix",0,false); ConsumerSink sink(&r,&m,"SHFE.rb.HOT",2); sink.period="m";
    r.register_sink(&sink,"test"); ASSERT_TRUE(r.prepare());
    auto grouped=r._bars_cache.at("SHFE.rb.HOT#m#2"); ASSERT_EQ(grouped->_physical_sources.size(),2u);
    EXPECT_TRUE(grouped->_physical_sources[0].empty()); EXPECT_EQ(grouped->_physical_sources[1],"rb2409");
    r.run(); EXPECT_EQ(r.replay_status().state,WT_REPLAY_FAILED); EXPECT_FALSE(r.fee_error().empty()); EXPECT_EQ(m._pos_map.count("SHFE.rb.HOT"),0u);
}
TEST_F(BarFeeTest, UnregisteredOrMisalignedSourceCannotEstablishIdentity) {
    loader.raw["SHFE.rb.2405"]={timed(20240102,901,100),timed(20240102,902,100)};
    loader.raw["SHFE.rb.2409"]={timed(20240103,901,100),timed(20240103,902,100)};
    ASSERT_TRUE(initialize(true)); auto* raw=r.get_kline_slice("SHFE.rb.HOT","m",10,1,false); if(raw) raw->release();
    auto base=r._bars_cache.at("SHFE.rb.HOT#m#1"); ASSERT_EQ(base->_physical_sources.size(),4u);
    base->_physical_sources[0]="rb9999"; base->_physical_sources[1]="rb9999";
    auto* grouped=r.get_kline_slice("SHFE.rb.HOT","m",10,2,false); if(grouped) grouped->release();
    auto output=r._bars_cache.at("SHFE.rb.HOT#m#2"); ASSERT_EQ(output->_physical_sources.size(),2u);
    EXPECT_TRUE(output->_physical_sources[0].empty()); EXPECT_EQ(output->_physical_sources[1],"rb2409");
    base->_physical_sources.pop_back();
    grouped=r.get_kline_slice("SHFE.rb.HOT","m",10,3,false); if(grouped) grouped->release();
    output=r._bars_cache.at("SHFE.rb.HOT#m#3"); EXPECT_TRUE(output->_physical_sources.empty());
}
