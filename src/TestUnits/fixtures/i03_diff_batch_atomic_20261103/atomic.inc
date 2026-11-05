#include <condition_variable>
#include <limits>

TEST_F(P02DiffExecuterTest, AtomicOverflowRejectsEntireBatchAndIncrementWithoutEffects)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_overflow.json"));
 const std::string bad="SHFE.ag.2212", good="SHFE.rb.2212"; const double max=std::numeric_limits<double>::max();
 P02DiffExecuterTestAccess::setSnapshot(_executer,bad,max,-max); const auto revision=P02DiffExecuterTestAccess::positionRevision(_executer);
 int saves=0; P02DiffExecuterTestAccess::setSaveHook(_executer,[&](bool){++saves;return true;});
 wt_hashmap<std::string,double> batch; batch[bad]=0.0; batch[good]=1.0; _executer.set_position(batch);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,bad),max); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,bad),-max);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,good),-999999.0); EXPECT_EQ(P02DiffExecuterTestAccess::positionRevision(_executer),revision);
 EXPECT_EQ(P02DiffExecuterTestAccess::unitStates(_executer),0u); EXPECT_EQ(P02DiffExecuterTestAccess::activeBatches(_executer),0u); EXPECT_EQ(saves,0); EXPECT_FALSE(fs::exists(exec_data_file("p02_diff_exe")));
 _executer.on_position_changed(bad.c_str(),max/2); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,bad),max); EXPECT_EQ(P02DiffExecuterTestAccess::positionRevision(_executer),revision); EXPECT_EQ(saves,0);
 P02DiffExecuterTestAccess::setSaveHook(_executer,{});
}

TEST_F(P02DiffExecuterTest, IncrementDuringBatchPreflightRetainsOwnership)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_increment.json"));
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::registerInitializing(_executer,code,new I03DiffUnit(calls),&fact); P02DiffExecuterTestAccess::finishInitializing(_executer,code);
 P02DiffExecuterTestAccess::setSnapshot(_executer,code,4.0,4.0);
 std::mutex mutex; std::condition_variable cv; bool entered=false, release=false;
 P02DiffExecuterTestAccess::setBatchHook(_executer,[&](uint64_t){std::unique_lock<std::mutex> lock(mutex); entered=true; cv.notify_all(); cv.wait_for(lock,std::chrono::seconds(5),[&]{return release;});});
 wt_hashmap<std::string,double> batch; batch[code]=10.0; std::thread worker([&]{_executer.set_position(batch);});
 {std::unique_lock<std::mutex> lock(mutex); EXPECT_TRUE(cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}));}
 _executer.on_position_changed(code.c_str(),1.0);
 {std::lock_guard<std::mutex> lock(mutex); release=true;} cv.notify_all(); worker.join(); P02DiffExecuterTestAccess::setBatchHook(_executer,{});
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),6.0); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),6.0); EXPECT_EQ(*calls,(std::vector<double>{6.0}));
 P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}

TEST_F(P02DiffExecuterTest, LaterBatchClearsCodeFirstIntroducedByEarlierInflightBatch)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_batches.json")); _bd_mgr.enableContract();
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::registerInitializing(_executer,code,new I03DiffUnit(calls),&fact); P02DiffExecuterTestAccess::finishInitializing(_executer,code);
 std::mutex mutex; std::condition_variable cv; bool firstEntered=false, secondEntered=false, firstFinished=false;
 P02DiffExecuterTestAccess::setBatchHook(_executer,[&](uint64_t ticket){std::unique_lock<std::mutex> lock(mutex); if(ticket==1){firstEntered=true;cv.notify_all();cv.wait_for(lock,std::chrono::seconds(5),[&]{return secondEntered;});}else{secondEntered=true;cv.notify_all();cv.wait_for(lock,std::chrono::seconds(5),[&]{return firstFinished;});}});
 wt_hashmap<std::string,double> first; first[code]=1.0; wt_hashmap<std::string,double> second;
 std::thread a([&]{_executer.set_position(first); {std::lock_guard<std::mutex> lock(mutex); firstFinished=true;} cv.notify_all();});
 {std::unique_lock<std::mutex> lock(mutex); EXPECT_TRUE(cv.wait_for(lock,std::chrono::seconds(5),[&]{return firstEntered;}));}
 std::thread b([&]{_executer.set_position(second);}); a.join(); b.join(); P02DiffExecuterTestAccess::setBatchHook(_executer,{});
 EXPECT_TRUE(secondEntered); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),0.0); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),0.0); EXPECT_EQ(*calls,(std::vector<double>{2.0,0.0})); EXPECT_EQ(P02DiffExecuterTestAccess::activeBatches(_executer),0u);
 P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}

TEST_F(P02DiffExecuterTest, FailedBatchSnapshotBlocksEveryAffectedUnit)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_failure.json"));
 I03DiffFact fact; auto first=std::make_shared<std::vector<double>>(), second=std::make_shared<std::vector<double>>(); const std::string a="SHFE.ag.2212",b="SHFE.rb.2212";
 P02DiffExecuterTestAccess::registerInitializing(_executer,a,new I03DiffUnit(first),&fact); P02DiffExecuterTestAccess::finishInitializing(_executer,a);
 P02DiffExecuterTestAccess::registerInitializing(_executer,b,new I03DiffUnit(second),&fact); P02DiffExecuterTestAccess::finishInitializing(_executer,b);
 int calls=0; P02DiffExecuterTestAccess::setSaveHook(_executer,[&](bool tombstones){++calls; return tombstones;});
 wt_hashmap<std::string,double> batch; batch[a]=1.0; batch[b]=-1.0; _executer.set_position(batch); P02DiffExecuterTestAccess::setSaveHook(_executer,{});
 EXPECT_EQ(calls,2); EXPECT_TRUE(first->empty()); EXPECT_TRUE(second->empty()); EXPECT_EQ(_executer.queryUnitInitStatus(a.c_str()).state,ExecUnitInitState::FAILED); EXPECT_EQ(_executer.queryUnitInitStatus(b.c_str()).state,ExecUnitInitState::FAILED);
 EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,a),2.0); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,b),-2.0);
 P02DiffExecuterTestAccess::unregisterUnit(_executer,a); P02DiffExecuterTestAccess::unregisterUnit(_executer,b);
}

TEST_F(P02DiffExecuterTest, InitializationReentryBeforeRegistrationDeliversLatestOnce)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_init.json"));
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212"; int entries=0; bool registered=false, inHook=false;
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,[&](const std::string& candidate){
  if(inHook) return; inHook=true; ++entries; P02DiffExecuterTestAccess::beginUnregisteredCreation(_executer,candidate);
  _executer.on_position_changed(candidate.c_str(),-2.0); EXPECT_TRUE(calls->empty());
  registered=P02DiffExecuterTestAccess::registerCreated(_executer,candidate,new I03DiffUnit(calls),&fact);
  if(registered) P02DiffExecuterTestAccess::finishInitializing(_executer,candidate); inHook=false;
 });
 wt_hashmap<std::string,double> batch; batch[code]=1.25; _executer.set_position(batch);
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,{}); EXPECT_EQ(entries,1); EXPECT_TRUE(registered); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),-1.5);
 EXPECT_EQ(*calls,(std::vector<double>{-1.5})); P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}

TEST_F(P02DiffExecuterTest, OmittedTargetWithoutReadyUnitRetainsPendingZeroNotification)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_omitted.json")); _bd_mgr.enableContract();
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::setSnapshot(_executer,code,4.0,4.0); P02DiffExecuterTestAccess::beginUnregisteredCreation(_executer,code);
 wt_hashmap<std::string,double> empty; _executer.set_position(empty);
 EXPECT_TRUE(calls->empty()); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),0.0);
 const bool registered=P02DiffExecuterTestAccess::registerCreated(_executer,code,new I03DiffUnit(calls),&fact); EXPECT_TRUE(registered);
 if(registered) P02DiffExecuterTestAccess::finishInitializing(_executer,code);
 EXPECT_EQ(*calls,(std::vector<double>{0.0})); P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}

TEST_F(P02DiffExecuterTest, EqualTargetStillCreatesUnitWithoutWritingOrNotifying)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_equal.json"));
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::setSnapshot(_executer,code,4.0,3.0); const auto revision=P02DiffExecuterTestAccess::positionRevision(_executer);
 int saves=0, lookups=0; P02DiffExecuterTestAccess::setSaveHook(_executer,[&](bool){++saves;return true;});
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,[&](const std::string& candidate){++lookups; P02DiffExecuterTestAccess::beginUnregisteredCreation(_executer,candidate); const bool registered=P02DiffExecuterTestAccess::registerCreated(_executer,candidate,new I03DiffUnit(calls),&fact); EXPECT_TRUE(registered); if(registered) P02DiffExecuterTestAccess::finishInitializing(_executer,candidate);});
 wt_hashmap<std::string,double> targets; targets[code]=2.0; _executer.set_position(targets); _executer.set_position(targets);
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,{}); P02DiffExecuterTestAccess::setSaveHook(_executer,{});
 EXPECT_EQ(lookups,1); EXPECT_EQ(saves,0); EXPECT_EQ(P02DiffExecuterTestAccess::positionRevision(_executer),revision); EXPECT_TRUE(calls->empty()); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::diff(_executer,code),3.0);
 P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}

TEST_F(P02DiffExecuterTest, EmptyCodeRejectsEntireBatchWithoutEffects)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_empty.json")); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::setSnapshot(_executer,code,3.0,3.0); const auto revision=P02DiffExecuterTestAccess::positionRevision(_executer);
 int saves=0, lookups=0; P02DiffExecuterTestAccess::setSaveHook(_executer,[&](bool){++saves;return true;}); P02DiffExecuterTestAccess::setTargetLookupHook(_executer,[&](const std::string&){++lookups;});
 wt_hashmap<std::string,double> targets; targets[code]=9.0; targets[""]=1.0; _executer.set_position(targets);
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,{}); P02DiffExecuterTestAccess::setSaveHook(_executer,{});
 EXPECT_EQ(saves,0); EXPECT_EQ(lookups,0); EXPECT_EQ(P02DiffExecuterTestAccess::unitStates(_executer),0u); EXPECT_EQ(P02DiffExecuterTestAccess::positionRevision(_executer),revision); EXPECT_DOUBLE_EQ(P02DiffExecuterTestAccess::target(_executer,code),3.0);
}

TEST_F(P02DiffExecuterTest, EqualTargetCreatesReadyUnitAndRecoversExactlyOnce)
{
 std::error_code ec; fs::remove(exec_data_file("p02_diff_exe"),ec); ASSERT_TRUE(init_with(kDiffCfg,"i03_atomic_equal_ready.json"));
 I03DiffFact fact; auto calls=std::make_shared<std::vector<double>>(); const std::string code="SHFE.ag.2212";
 P02DiffExecuterTestAccess::setSnapshot(_executer,code,4.0,3.0); const auto revision=P02DiffExecuterTestAccess::positionRevision(_executer); P02DiffExecuterTestAccess::setReadyForRegistration(_executer);
 int saves=0, lookups=0; P02DiffExecuterTestAccess::setSaveHook(_executer,[&](bool){++saves;return true;});
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,[&](const std::string& candidate){++lookups; P02DiffExecuterTestAccess::beginUnregisteredCreation(_executer,candidate); const bool registered=P02DiffExecuterTestAccess::registerCreated(_executer,candidate,new I03DiffUnit(calls),&fact); EXPECT_TRUE(registered); if(registered) P02DiffExecuterTestAccess::finishInitializing(_executer,candidate);});
 wt_hashmap<std::string,double> targets; targets[code]=2.0; _executer.set_position(targets); _executer.set_position(targets);
 P02DiffExecuterTestAccess::setTargetLookupHook(_executer,{}); P02DiffExecuterTestAccess::setSaveHook(_executer,{});
 EXPECT_EQ(lookups,1); EXPECT_EQ(saves,0); EXPECT_EQ(P02DiffExecuterTestAccess::positionRevision(_executer),revision); EXPECT_EQ(*calls,(std::vector<double>{3.0}));
 P02DiffExecuterTestAccess::unregisterUnit(_executer,code);
}
