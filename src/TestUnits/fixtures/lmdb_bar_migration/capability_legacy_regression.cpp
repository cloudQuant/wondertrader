// Keep migration_test.cpp unchanged as the historical callback-based oracle.
#define main historical_migration_fixture_main
#include "migration_test.cpp"
#undef main

namespace {
Result controlledBatch(const Bindings& binding, std::size_t size=2) {
 return stageBatch(binding,bigEndian?LegacySourceHost::BigEndian:LegacySourceHost::LittleEndian,
                   packedValue?LegacyValueLayout::Packed64:LegacyValueLayout::Aligned64,size);
}
Result controlledComplete(const Bindings& binding) {
 Result result;
 do { result=controlledBatch(binding); } while(result.code==ResultCode::Incomplete);
 return result;
}
void loadEvidence(Bindings& binding, int skip=-1, int wrongIdentity=-1, int wrongTime=-1) {
 for(unsigned i=0;i<sizeof(rows)/sizeof(rows[0]);++i) {
  if(static_cast<int>(i)==skip) continue;
  auto k=key(rows[i]); auto v=value(rows[i]);
  MDB_val mk{k.size(),k.data()},mv{v.size(),v.data()};
  Evidence e{rows[i].ex,rows[i].code,rows[i].time,true};
  if(static_cast<int>(i)==wrongIdentity) e.code="wrong";
  if(static_cast<int>(i)==wrongTime) ++e.verifiedTime;
  require(binding.putEvidence(mk,mv,e),"put evidence");
 }
}
void revoke(Bindings& binding, unsigned index) {
 auto k=key(rows[index]); MDB_val mk{k.size(),k.data()};
 require(binding.revokeEvidence(mk),"revoke evidence");
}
void loadManyEvidence(Bindings& binding, unsigned count) {
 std::vector<std::array<unsigned char,52>> keys; std::vector<std::vector<unsigned char>> values;
 std::vector<EvidenceInput> records; keys.reserve(count); values.reserve(count); records.reserve(count);
 for(unsigned i=0;i<count;++i) {
  const std::string code="rb"+std::to_string(i); Row row{"SHFE",code.c_str(),0x100000000ULL+i};
  keys.push_back(key(row)); values.push_back(value(row));
  records.push_back({{keys.back().size(),keys.back().data()},{values.back().size(),values.back().data()},
                     {"SHFE",code,row.time,true}});
 }
 require(binding.putEvidenceBatch(records),"load many evidence");
}
void waitFlag(const std::atomic<bool>& flag) {
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(8);
 while(!flag.load() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
 require(flag.load(),"test hook timeout");
}
void fillTarget(MDB_env* env) {
 MDB_txn* txn=nullptr; MDB_dbi filler;
 mcheck(mdb_txn_begin(env,nullptr,0,&txn)); mcheck(mdb_dbi_open(txn,"test.fill",MDB_CREATE,&filler));
 mcheck(mdb_txn_commit(txn));
 std::array<unsigned char,4096> payload{};
 for(std::size_t size : {payload.size(), std::size_t(1)}) {
  bool full=false;
  for(unsigned i=0;i<10000;++i) {
   mcheck(mdb_txn_begin(env,nullptr,0,&txn)); auto text=std::to_string(size)+":"+std::to_string(i);
   MDB_val keyArg{text.size(),text.data()}, valArg{size,payload.data()};
   int rc=mdb_put(txn,filler,&keyArg,&valArg,0);
   if(rc==MDB_MAP_FULL) { mdb_txn_abort(txn); full=true; break; }
   if(rc) { mdb_txn_abort(txn); mcheck(rc); }
   rc=mdb_txn_commit(txn);
   if(rc==MDB_MAP_FULL) { full=true; break; }
   mcheck(rc);
  }
  require(full,"target fill failed to reach MDB_MAP_FULL");
 }
}
}

int main(int argc,char** argv) {
 try {
  require(argc==3,"usage mode directory"); const std::string mode=argv[1]; const fs::path base=argv[2];
  bigEndian=mode=="big-packed"; packedValue=mode=="big-packed" || mode=="packed";
  if(mode=="write") { Env e(base,false); const Row extra{"CZCE","x1",111}; putRow(e.p,extra); return 0; }
  require(!fs::exists(base),"test directory must not exist"); fs::create_directories(base);
  const auto source=base/"source",dest=base/"dest";
  if(mode=="map-full" || mode=="map-full-partial") seedMany(source); else seed(source);
  if(mode=="badkey" || mode=="badvalue") { Env writable(source,false); putMalformed(writable.p,mode=="badkey"); }
  if(mode=="alias") {
   fs::create_directories(dest); fs::create_hard_link(source/"data.mdb",dest/"data.mdb");
   Env s(source,true),d(dest,false); Bindings b;
   require(prepareBindings(s.p,d.p,b).code==ResultCode::SameEnvironment,"backing-file alias accepted");
   require(entries(s.p,nullptr)==4,"alias changed source");
  } else if(mode=="unsafe") {
   Env s(source,false),d(dest,false); Bindings b;
   require(prepareBindings(s.p,d.p,b).code==ResultCode::UnsafeEnvironment && entries(d.p,stageName)==0,"writable source accepted");
  } else if(mode=="crash") {
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); loadEvidence(b);
    require(controlledBatch(b,1).code==ResultCode::Incomplete && manifestState(d.p)=='I',"first batch"); }
   pid_t pid=fork(); require(pid>=0,"fork");
   if(pid==0) { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); testExitAfterPut.store(1);
    (void)controlledBatch(b); _exit(78); }
   int status=0; require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==77,"child exit");
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b);
   require(entries(d.p,stageName)==1 && entries(d.p,metadataName)==4 && manifestState(d.p)=='I',"partial batch committed");
   require(controlledComplete(b).code==ResultCode::Complete,"crash retry"); checkContent(d.p);
  } else if(mode=="reopen") {
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); loadEvidence(b); require(controlledComplete(b).code==ResultCode::Complete,"first complete"); }
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b);
    auto r=controlledComplete(b); require(r.code==ResultCode::Complete && r.committedRecords==4 && manifestState(d.p)=='C',"persisted evidence retry"); checkContent(d.p); }
  } else if(mode=="concurrent-prepare") {
   Env s(source,true),d(dest,false); Bindings b; Barrier barrier; Result result[2];
   std::thread a([&]{barrier.wait(); result[0]=prepareBindings(s.p,d.p,b);});
   std::thread c([&]{barrier.wait(); result[1]=prepareBindings(s.p,d.p,b);}); a.join(); c.join();
   require((result[0].code==ResultCode::Prepared && result[1].code==ResultCode::InvalidArgument) ||
           (result[1].code==ResultCode::Prepared && result[0].code==ResultCode::InvalidArgument),"concurrent prepare");
   loadEvidence(b); require(controlledComplete(b).code==ResultCode::Complete,"binding unusable");
  } else {
   Env s(source,true),d(dest,false,(mode=="map-full" || mode=="map-full-partial")?1048576:8*1024*1024); Bindings b; bind(s.p,d.p,b);
   if(mode=="same") { Bindings same; require(prepareBindings(s.p,s.p,same).code==ResultCode::SameEnvironment,"same environment"); }
   else if(mode=="unprepared") { Bindings empty; require(controlledBatch(empty).code==ResultCode::UnpreparedBindings,"unprepared binding accepted"); }
   else if(mode=="prepare-twice") { require(prepareBindings(s.p,d.p,b).code==ResultCode::InvalidArgument,"binding reinitialized"); }
   else if(mode=="legacy") {
    unsigned calls=0; auto cb=[&](const MDB_val&,const MDB_val&,Evidence&) {++calls; return true;};
    require(batch(b,cb,4).code==ResultCode::EvidenceCapabilityRequired && calls==0,"legacy callback executed");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"legacy mutated staging");
   } else if(mode=="limit") {
    require(controlledBatch(b,0).code==ResultCode::InvalidArgument,"zero accepted");
    require(controlledBatch(b,maxBatchRecords+1).code==ResultCode::InvalidArgument,"oversize accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"limit mutated staging");
   } else if(mode=="badkey" || mode=="badvalue") {
    loadEvidence(b); auto k=key(rows[0]); auto v=value(rows[0]); k[0]='A';
    MDB_val mk{mode=="badkey"?k.size()-1:k.size(),k.data()}, mv{mode=="badvalue"?v.size()-1:v.size(),v.data()};
    Evidence proof{"AHFE","rb1",rows[0].time,true};
    require(!b.putEvidence(mk,mv,proof),"malformed evidence input accepted");
    require(controlledBatch(b).code==ResultCode::MissingEvidence,"malformed row accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"malformed row committed");
   } else if(mode=="missing" || mode=="identity" || mode=="time") {
    // DCE/m1 is the first source record in this fixture.
    loadEvidence(b,mode=="missing"?3:-1,mode=="identity"?3:-1,mode=="time"?3:-1);
    auto r=controlledComplete(b);
    require(r.code==(mode=="missing"?ResultCode::MissingEvidence:ResultCode::InvalidRecord),"invalid evidence accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"invalid evidence committed");
    loadEvidence(b); require(controlledComplete(b).code==ResultCode::Complete,"correct evidence retry");
   } else if(mode=="map-full" || mode=="map-full-partial") {
    loadManyEvidence(b,900); const bool partial=mode=="map-full-partial";
    if(partial) require(controlledBatch(b,1).code==ResultCode::Incomplete && manifestState(d.p)=='I',"first batch");
    fillTarget(d.p);
    auto r=controlledBatch(b,1000);
    if(r.code!=ResultCode::DestinationError || r.lmdbError!=MDB_MAP_FULL)
     std::cerr<<"map full result code="<<static_cast<int>(r.code)<<" lmdb="<<r.lmdbError<<'\n';
    require(r.code==ResultCode::DestinationError && r.lmdbError==MDB_MAP_FULL,"stage did not return map full");
    require(entries(d.p,stageName)==(partial?1u:0u) && entries(d.p,metadataName)==(partial?4u:0u),"map full committed partial data");
    if(partial) require(manifestState(d.p)=='I',"map full changed manifest");
    mcheck(mdb_env_set_mapsize(d.p,8*1024*1024));
    r=controlledBatch(b,1000);
    require(r.code==ResultCode::Complete && r.committedRecords==900 && entries(d.p,stageName)==900,"resize retry");
   } else if(mode=="stat-under" || mode=="stat-over") {
    loadEvidence(b); require(controlledBatch(b,1).code==ResultCode::Incomplete && manifestState(d.p)=='I',"first batch");
    testSourceCount.store(mode=="stat-under"?2:5); auto r=controlledBatch(b,mode=="stat-under"?1:10); testSourceCount.store(-1);
    require(r.code==ResultCode::SourceInconsistent,"count/cursor disagreement accepted");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"count mismatch changed staging");
    require(controlledComplete(b).code==ResultCode::Complete,"count mismatch retry");
   } else if(mode=="evidence-revoked-between-batches" || mode=="interrupted-batch") {
    loadEvidence(b); require(controlledBatch(b,1).code==ResultCode::Incomplete && manifestState(d.p)=='I',"first batch");
    // Revoke the first prefix for the former, and the third source record for the latter.
    revoke(b,mode=="evidence-revoked-between-batches"?3:1);
    auto r=controlledBatch(b,2); require(r.code==ResultCode::MissingEvidence,"revoked evidence accepted");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"failed batch changed previous commit");
    loadEvidence(b); require(controlledComplete(b).code==ResultCode::Complete,"recovery");
   } else if(mode=="source-after-batch") {
    loadEvidence(b); require(controlledBatch(b,1).code==ResultCode::Incomplete,"first batch");
    pid_t child=fork(); if(child==0) { execl(argv[0],argv[0],"write",source.c_str(),nullptr); _exit(127); }
    int status=0; require(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"source writer");
    require(controlledBatch(b).code==ResultCode::MetadataMismatch,"changed source resumed");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"source change changed staging");
   } else if(mode=="concurrent-same" || mode=="concurrent-different") {
    loadEvidence(b); std::unique_ptr<Env> second; Bindings b2;
    if(mode=="concurrent-different") { second=std::make_unique<Env>(base/"dest2",false); bind(s.p,second->p,b2); loadEvidence(b2); }
    testPauseAfterPut.store(true); Result first,other; std::atomic<bool> otherDone{false};
    std::thread t1([&]{first=controlledBatch(b,1);}); waitFlag(testPaused);
    std::thread t2([&]{other=controlledBatch(mode=="concurrent-same"?b:b2,1); otherDone.store(true);});
    if(mode=="concurrent-different") waitFlag(otherDone); else std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool beforeRelease=otherDone.load(); testResume.store(true); t1.join(); t2.join();
    require((mode=="concurrent-different") == beforeRelease,"incorrect binding serialization");
    require(first.code==ResultCode::Incomplete && other.code==ResultCode::Incomplete,"concurrent batch failure");
    require(controlledComplete(b).code==ResultCode::Complete,"first target retry");
    if(second) require(controlledComplete(b2).code==ResultCode::Complete,"second target retry");
   } else {
    loadEvidence(b); auto first=controlledBatch(b,1);
    require(first.code==ResultCode::Incomplete && first.committedRecords==1 && manifestState(d.p)=='I',"first batch");
    auto r=controlledComplete(b); require(r.code==ResultCode::Complete && r.committedRecords==4,"complete");
    require(entries(d.p,stageName)==4 && entries(d.p,metadataName)==4 && manifestState(d.p)=='C',"stage sizes"); checkContent(d.p);
    if(mode=="success" || mode=="packed" || mode=="big-packed") {
     require(controlledComplete(b).code==ResultCode::Complete,"idempotent retry");
    } else if(mode=="extra-stage") {
     addExtra(d.p); require(controlledComplete(b).code==ResultCode::MetadataMismatch && entries(d.p,stageName)==5,"extra stage accepted or modified");
    } else if(mode=="conflict" || mode=="missing-stage") {
     corrupt(d.p,mode=="missing-stage"); r=controlledComplete(b);
     require(r.code==(mode=="missing-stage"?ResultCode::MetadataMismatch:ResultCode::Conflict),"corrupt retry accepted");
     require(entries(d.p,stageName)==(mode=="missing-stage"?3u:4u),"retry changed stage");
     if(mode=="conflict") require(readMarkerByte(d.p)==99,"retry overwrote conflict");
    } else throw std::runtime_error("unknown mode");
   }
  }
  std::cout<<mode<<": PASS\n"; return 0;
 } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
