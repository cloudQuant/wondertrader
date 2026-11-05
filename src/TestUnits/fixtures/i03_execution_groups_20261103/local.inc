TEST_F(R7VwapV2LocalIntegration, GroupBatchScaleLimitShortAndExplicitNet)
{
 WTSSessionInfo* session=makeSession("I03_GROUP",{{900,1500}}); ASSERT_NE(session,nullptr);
 addInstrument("SHFE","rb","rb2510","SHFE.rb.2510","I03_GROUP",TM_Both,CC_Future,session);
 addInstrument("SHFE","au","au2212","SHFE.au.2212","I03_GROUP",TM_Both,CC_Future,session);
 WTSCommodityInfo* rbComm=_stub.get_comm_info("SHFE.rb.2510"); ASSERT_NE(rbComm,nullptr);
 LocalGroupPositionFact fact; auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_groups",&_data_manager);
 _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local); local->setStub(&_stub);
 auto cfg=i03GroupCfg(R"({"scale":0.5,"groups":[{"name":"G","items":{"SHFE.rb.2510":1,"SHFE.au.2212":-1}}],"group_limits":{"G":1.25},"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})"); ASSERT_TRUE(cfg && local->init(cfg.get()));
 const std::string rb="SHFE.rb.2510",au="SHFE.au.2212",group="G";
 auto gp=std::make_shared<LocalGroupPositionProbe>(),rp=std::make_shared<LocalGroupPositionProbe>(),ap=std::make_shared<LocalGroupPositionProbe>();
 WtLocalExecuterTestAccess::registerUnit(*local,group,new LocalGroupPositionUnit(gp),&fact); WtLocalExecuterTestAccess::registerUnit(*local,rb,new LocalGroupPositionUnit(rp),&fact); WtLocalExecuterTestAccess::registerUnit(*local,au,new LocalGroupPositionUnit(ap),&fact);
 wt_hashmap<std::string,double> targets; targets[rb]=2.5; targets[au]=-2.5; local->set_position(targets);
 EXPECT_EQ(gp->snapshot().size(),1u); if(!gp->snapshot().empty()) EXPECT_DOUBLE_EQ(gp->snapshot().back().second,1.25);
 EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,group),2.5); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,rb),0.0); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,au),0.0);
 targets[rb]=2.75; targets[au]=-2.75; local->set_position(targets); local->on_position_changed(group.c_str(),0.25); EXPECT_EQ(gp->snapshot().size(),1u); EXPECT_EQ(rp->snapshot().size(),1u); EXPECT_EQ(ap->snapshot().size(),1u);
 rbComm->setTradingMode(TM_Long); local->on_position_changed(group.c_str(),-5.0); EXPECT_EQ(gp->snapshot().size(),1u); EXPECT_EQ(rp->snapshot().size(),1u); EXPECT_EQ(ap->snapshot().size(),1u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,group),2.5);
 rbComm->setTradingMode(TM_Both); local->on_position_changed(group.c_str(),-5.0); EXPECT_EQ(gp->snapshot().size(),2u); if(gp->snapshot().size()==2) EXPECT_DOUBLE_EQ(gp->snapshot().back().second,-1.25);
 _stub.bind(rb.c_str(),nullptr,nullptr); targets.clear(); targets[group]=-2.0; local->set_position(targets); EXPECT_EQ(gp->snapshot().size(),2u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,group),-2.5);
 _stub.bind(rb.c_str(),rbComm,session); rbComm->setTradingMode(TM_Long); targets.clear(); targets[group]=2.0; targets[rb]=-1.0; targets[au]=1.0; local->set_position(targets);
 EXPECT_EQ(gp->snapshot().size(),3u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,group),2.0); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,rb),-1.0); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,au),1.0);
 WtLocalExecuterTestAccess::unregisterUnits(*local,{group,rb,au});
}
TEST_F(R7VwapV2LocalIntegration, InvalidLaterGroupDoesNotPublishOrStartPool)
{
 auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_bad_groups",&_data_manager); _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local);
 auto cfg=i03GroupCfg(R"({"scale":1,"poolsize":1,"groups":[{"name":"G","items":{"A":1}},{"name":"H","items":{"B":0}}]})"); ASSERT_TRUE(cfg); EXPECT_FALSE(local->init(cfg.get()));
 EXPECT_EQ(WtLocalExecuterTestAccess::groupCount(*local),0u); EXPECT_FALSE(WtLocalExecuterTestAccess::hasConfig(*local)); EXPECT_FALSE(WtLocalExecuterTestAccess::hasPool(*local));
}
namespace {
class I03LocalRaceStub : public IExecuterStub
{
public:
 WTSCommodityInfo* commodity=nullptr; std::function<void()> lookup;
 WTSCommodityInfo* get_comm_info(const char*) override { if(lookup) lookup(); return commodity; }
 WTSSessionInfo* get_sess_info(const char*) override { return nullptr; }
 IHotMgr* get_hot_mon() override { return nullptr; }
 uint32_t get_trading_day() override { return 20250101; }
 uint64_t get_real_time() override { return 20250101090000000ull; }
};
}
TEST_F(R7VwapV2LocalIntegration, MetadataReentryRejectsStaleTargetBatch)
{
 LocalGroupPositionFact fact; auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_group_race",&_data_manager); _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local);
 std::shared_ptr<WTSCommodityInfo> comm(r71::makeComm("SHFE","ag","I03",TM_Both),[](WTSCommodityInfo* p){if(p)p->release();}); I03LocalRaceStub stub; stub.commodity=comm.get(); local->setStub(&stub);
 auto cfg=i03GroupCfg(R"({"scale":1,"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})"); ASSERT_TRUE(cfg && local->init(cfg.get()));
 const std::string a="SHFE.ag.2512",b="SHFE.rb.2510"; auto ap=std::make_shared<LocalGroupPositionProbe>(),bp=std::make_shared<LocalGroupPositionProbe>(); WtLocalExecuterTestAccess::registerUnit(*local,a,new LocalGroupPositionUnit(ap),&fact); WtLocalExecuterTestAccess::registerUnit(*local,b,new LocalGroupPositionUnit(bp),&fact);
 bool entered=false; stub.lookup=[&]{if(!entered){entered=true; local->on_position_changed(b.c_str(),1.0);}};
 wt_hashmap<std::string,double> targets; targets[a]=-1.0; local->set_position(targets);
 EXPECT_TRUE(entered); EXPECT_TRUE(ap->snapshot().empty()); EXPECT_EQ(bp->snapshot().size(),1u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,a),0.0); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,b),1.0);
 WtLocalExecuterTestAccess::unregisterUnits(*local,{a,b});
}
TEST_F(R7VwapV2LocalIntegration, ZeroGroupCapAllowsOnlyZero)
{
 LocalGroupPositionFact fact; auto local=std::make_shared<CapturingLocalExecuter>(&_factory,"i03_local_zero_cap",&_data_manager); _manager.add_executer(std::static_pointer_cast<IExecCommand>(local)); _executors.push_back(local); local->setStub(&_stub);
 auto cfg=i03GroupCfg(R"({"scale":1,"groups":{"G":{"SHFE.rb.2510":1}},"group_limits":{"G":0},"policy":{"default":{"name":"LocalGroupPositionFact.LocalGroupPositionUnit"}}})"); ASSERT_TRUE(cfg && local->init(cfg.get())); auto gp=std::make_shared<LocalGroupPositionProbe>(); WtLocalExecuterTestAccess::registerUnit(*local,"G",new LocalGroupPositionUnit(gp),&fact);
 wt_hashmap<std::string,double> target; target["G"]=0.0; local->set_position(target); EXPECT_EQ(gp->snapshot().size(),1u); target["G"]=0.01; local->set_position(target); local->on_position_changed("G",0.01); EXPECT_EQ(gp->snapshot().size(),1u); EXPECT_DOUBLE_EQ(WtLocalExecuterTestAccess::target(*local,"G"),0.0);
 WtLocalExecuterTestAccess::unregisterUnits(*local,{"G"});
}
