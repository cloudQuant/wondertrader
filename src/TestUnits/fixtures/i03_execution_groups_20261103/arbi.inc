namespace {
class I03ArbiGroupStub : public IExecuterStub
{
public:
 std::map<std::string,WTSCommodityInfo*> commodities;
 std::function<void()> lookup;
 WTSCommodityInfo* get_comm_info(const char* code) override { if(lookup) lookup(); auto it=commodities.find(code?code:""); return it==commodities.end()?nullptr:it->second; }
 WTSSessionInfo* get_sess_info(const char*) override { return nullptr; }
 IHotMgr* get_hot_mon() override { return nullptr; }
 uint32_t get_trading_day() override { return 20250101; }
 uint64_t get_real_time() override { return 20250101090000000ull; }
};
}
TEST(WtArbiExecuterSerialization, GroupBatchScaleLimitShortAndExplicitNet)
{
 ArbiProbeFact fact; auto gp=std::make_shared<ArbiProbe>(),rp=std::make_shared<ArbiProbe>(),ap=std::make_shared<ArbiProbe>();
 std::shared_ptr<WTSCommodityInfo> rbComm(r71::makeComm("SHFE","rb","I03",TM_Both),[](WTSCommodityInfo* p){if(p)p->release();});
 std::shared_ptr<WTSCommodityInfo> auComm(r71::makeComm("SHFE","au","I03",TM_Both),[](WTSCommodityInfo* p){if(p)p->release();}); I03ArbiGroupStub stub;
 const std::string rb="SHFE.rb.2510",au="SHFE.au.2212",group="G"; stub.commodities[rb]=rbComm.get(); stub.commodities[au]=auComm.get();
 WtArbiExecuter executer(NULL,"i03_arbi_groups",NULL); executer.setStub(&stub);
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":0.5,"groups":[{"name":"G","items":{"SHFE.rb.2510":1,"SHFE.au.2212":-1}}],"group_limits":{"G":1.25},"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg && executer.init(cfg.get()));
 wtp::WtArbiExecuterTestAccess::registerUnit(executer,group,gp,&fact,false); wtp::WtArbiExecuterTestAccess::registerUnit(executer,rb,rp,&fact,false); wtp::WtArbiExecuterTestAccess::registerUnit(executer,au,ap,&fact,false);
 wt_hashmap<std::string,double> targets; targets[rb]=2.5; targets[au]=-2.5; executer.set_position(targets);
 EXPECT_EQ(gp->positionSnapshot().size(),1u); if(!gp->positionSnapshot().empty()) EXPECT_DOUBLE_EQ(gp->positionSnapshot().back(),1.25); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,group),2.5);
 targets[rb]=2.75; targets[au]=-2.75; executer.set_position(targets); executer.on_position_changed(group.c_str(),0.25); EXPECT_EQ(gp->positionSnapshot().size(),1u); EXPECT_EQ(rp->positionSnapshot().size(),1u); EXPECT_EQ(ap->positionSnapshot().size(),1u);
 rbComm->setTradingMode(TM_Long); executer.on_position_changed(group.c_str(),-5.0); EXPECT_EQ(gp->positionSnapshot().size(),1u); EXPECT_EQ(rp->positionSnapshot().size(),1u); EXPECT_EQ(ap->positionSnapshot().size(),1u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,group),2.5);
 rbComm->setTradingMode(TM_Both); executer.on_position_changed(group.c_str(),-5.0); EXPECT_EQ(gp->positionSnapshot().size(),2u); if(gp->positionSnapshot().size()==2) EXPECT_DOUBLE_EQ(gp->positionSnapshot().back(),-1.25);
 stub.commodities.erase(rb); targets.clear(); targets[group]=-2.0; executer.set_position(targets); EXPECT_EQ(gp->positionSnapshot().size(),2u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,group),-2.5);
 stub.commodities[rb]=rbComm.get(); rbComm->setTradingMode(TM_Long); targets.clear(); targets[group]=2.0; targets[rb]=-1.0; targets[au]=1.0; executer.set_position(targets);
 EXPECT_EQ(gp->positionSnapshot().size(),3u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,group),2.0); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,rb),-1.0); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,au),1.0);
}
TEST(WtArbiExecuterSerialization, InvalidLaterGroupDoesNotPublishOrStartPool)
{
 WtArbiExecuter executer(NULL,"i03_arbi_bad_groups",NULL); std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":1,"poolsize":1,"groups":[{"name":"G","items":{"A":1}},{"name":"H","items":{"B":0}}]})"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg); EXPECT_FALSE(executer.init(cfg.get())); EXPECT_EQ(wtp::WtArbiExecuterTestAccess::groupCount(executer),0u); EXPECT_FALSE(wtp::WtArbiExecuterTestAccess::hasConfig(executer)); EXPECT_FALSE(wtp::WtArbiExecuterTestAccess::hasPool(executer));
}
TEST(WtArbiExecuterSerialization, MetadataReentryRejectsStaleTargetBatch)
{
 ArbiProbeFact fact; auto ap=std::make_shared<ArbiProbe>(),bp=std::make_shared<ArbiProbe>(); std::shared_ptr<WTSCommodityInfo> comm(r71::makeComm("SHFE","ag","I03",TM_Both),[](WTSCommodityInfo* p){if(p)p->release();});
 I03ArbiGroupStub stub; const std::string a="SHFE.ag.2512",b="SHFE.rb.2510"; stub.commodities[a]=comm.get(); WtArbiExecuter executer(NULL,"i03_arbi_group_race",NULL); executer.setStub(&stub);
 std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":1,"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg && executer.init(cfg.get()));
 wtp::WtArbiExecuterTestAccess::registerUnit(executer,a,ap,&fact,false); wtp::WtArbiExecuterTestAccess::registerUnit(executer,b,bp,&fact,false);
 bool entered=false; stub.lookup=[&]{if(!entered){entered=true; executer.on_position_changed(b.c_str(),1.0);}};
 wt_hashmap<std::string,double> targets; targets[a]=-1.0; executer.set_position(targets);
 EXPECT_TRUE(entered); EXPECT_TRUE(ap->positionSnapshot().empty()); EXPECT_EQ(bp->positionSnapshot().size(),1u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,a),0.0); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,b),1.0);
}
TEST(WtArbiExecuterSerialization, ZeroGroupCapAllowsOnlyZero)
{
 ArbiProbeFact fact; auto gp=std::make_shared<ArbiProbe>(); WtArbiExecuter executer(NULL,"i03_arbi_zero_cap",NULL); std::shared_ptr<WTSVariant> cfg(WTSCfgLoader::load_from_content(R"({"scale":1,"groups":{"G":{"SHFE.rb.2510":1}},"group_limits":{"G":0},"policy":{"default":{"name":"WtExeFact.WtMinImpactExeUnit"}}})"),[](WTSVariant* p){if(p)p->release();}); ASSERT_TRUE(cfg && executer.init(cfg.get()));
 wtp::WtArbiExecuterTestAccess::registerUnit(executer,"G",gp,&fact,false); wt_hashmap<std::string,double> target; target["G"]=0.0; executer.set_position(target); EXPECT_EQ(gp->positionSnapshot().size(),1u); target["G"]=0.01; executer.set_position(target); executer.on_position_changed("G",0.01); EXPECT_EQ(gp->positionSnapshot().size(),1u); EXPECT_DOUBLE_EQ(wtp::WtArbiExecuterTestAccess::target(executer,"G"),0.0);
}
