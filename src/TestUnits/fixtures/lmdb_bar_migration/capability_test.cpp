// Reuse the isolated-environment fixture without changing its historical cases.
#define main historical_migration_fixture_main
#include "migration_test.cpp"
#undef main

namespace {
void loadEvidence(Bindings& binding) {
 for (const auto& row : rows) {
  auto k=key(row); auto v=value(row);
  MDB_val mk{k.size(),k.data()}, mv{v.size(),v.data()};
  Evidence proof{row.ex,row.code,row.time,true};
  require(binding.putEvidence(mk,mv,proof),"load evidence");
 }
}
Result safeBatch(const Bindings& b,std::size_t size=2) {
 return stageBatch(b,LegacySourceHost::LittleEndian,LegacyValueLayout::Aligned64,size);
}
Result safeComplete(const Bindings& b) {
 Result r; do { r=safeBatch(b); } while(r.code==ResultCode::Incomplete); return r;
}
void revokeFirst(Bindings& b) {
 auto k=key(rows[0]); MDB_val mk{k.size(),k.data()};
 require(b.revokeEvidence(mk),"revoke evidence");
}
void waitFor(const std::atomic<bool>& flag) {
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(!flag.load() && std::chrono::steady_clock::now()<until) std::this_thread::yield();
 require(flag.load(),"timed out");
}
}
int main(int argc,char** argv) {
 try {
  require(argc==3,"usage mode directory"); const std::string mode=argv[1]; const fs::path base=argv[2];
  require(!fs::exists(base),"test directory must not exist"); fs::create_directories(base);
  auto source=base/"source",dest=base/"dest";
  if(mode.rfind("benchmark-",0)==0) {
   const auto count=static_cast<unsigned>(std::stoul(mode.substr(10)));
   require(count>0 && count<=250000,"benchmark count"); seedMany(source,count);
   Env s(source,true,256*1024*1024),d(dest,false,256*1024*1024); Bindings b; bind(s.p,d.p,b);
   for(unsigned start=0;start<count;start+=1024) {
    const auto n=std::min(1024u,count-start);
    std::vector<std::array<unsigned char,52>> keys; std::vector<std::vector<unsigned char>> values;
    std::vector<EvidenceInput> records; keys.reserve(n); values.reserve(n); records.reserve(n);
    for(unsigned i=0;i<n;++i) {
     const auto index=start+i; auto code=std::string("rb")+std::to_string(index);
     Row row{"SHFE",code.c_str(),0x100000000ULL+index}; keys.push_back(key(row)); values.push_back(value(row));
     records.push_back({{keys.back().size(),keys.back().data()},{values.back().size(),values.back().data()},
                        {"SHFE",code,row.time,true}});
    }
    require(b.putEvidenceBatch(records),"benchmark evidence load");
   }
   const auto begin=std::chrono::steady_clock::now(); Result r; unsigned batches=0;
   do { r=safeBatch(b,1024); ++batches; } while(r.code==ResultCode::Incomplete);
   const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
   require(r.code==ResultCode::Complete && r.committedRecords==count,"benchmark staging");
   std::cout<<"records="<<count<<" batch=1024 batches="<<batches<<" elapsed_ms="<<ms<<'\n'; return 0;
  }
  seed(source);
  if(mode=="crash") {
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); loadEvidence(b);
     require(safeBatch(b,1).code==ResultCode::Incomplete,"first batch"); }
   pid_t pid=fork(); require(pid>=0,"fork");
   if(pid==0) { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); loadEvidence(b);
    testExitAfterPut.store(1); (void)safeBatch(b,2); _exit(78); }
   int status=0; require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==77,"crash child");
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b);
   require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"partial batch committed");
   require(safeComplete(b).code==ResultCode::Complete,"crash retry with persisted evidence"); checkContent(d.p);
  } else {
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); loadEvidence(b);
   if(mode=="legacy") {
    unsigned calls=0; auto cb=[&](const MDB_val&,const MDB_val&,Evidence&) { ++calls; return true; };
    require(batch(b,cb,4).code==ResultCode::EvidenceCapabilityRequired && calls==0,"legacy callback accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"legacy wrote target");
   } else if(mode=="revoke") {
    require(safeBatch(b,1).code==ResultCode::Incomplete,"first batch"); revokeFirst(b);
    require(safeBatch(b,4).code==ResultCode::MissingEvidence,"revoked evidence accepted");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"revocation changed staging");
    loadEvidence(b); require(safeComplete(b).code==ResultCode::Complete,"restored evidence retry");
   } else if(mode=="tamper") {
    require(safeComplete(b).code==ResultCode::Complete,"initial complete"); corrupt(d.p,false);
    require(safeBatch(b).code==ResultCode::Conflict,"corruption accepted");
   } else if(mode=="evidence-tamper") {
    MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(d.p,nullptr,0,&txn));
    mcheck(mdb_dbi_open(txn,evidenceName,0,&db)); auto k=key(rows[0]); MDB_val mk{k.size(),k.data()}, raw{};
    mcheck(mdb_get(txn,db,&mk,&raw)); std::string altered(static_cast<char*>(raw.mv_data),raw.mv_size);
    altered[12]^=1; MDB_val replacement{altered.size(),altered.data()};
    mcheck(mdb_put(txn,db,&mk,&replacement,0)); mcheck(mdb_txn_commit(txn));
    require(safeBatch(b,4).code==ResultCode::MissingEvidence,"tampered source value accepted");
    require(entries(d.p,stageName)==0,"tampered evidence committed staging");
   } else if(mode=="batch-atomic") {
    revokeFirst(b); auto k=key(rows[0]); auto v=value(rows[0]);
    Evidence good{rows[0].ex,rows[0].code,rows[0].time,true}, bad=good; bad.independentlyVerified=false;
    std::vector<EvidenceInput> records{{{k.size(),k.data()},{v.size(),v.data()},good},
                                       {{k.size(),k.data()},{v.size(),v.data()},bad}};
    require(!b.putEvidenceBatch(records),"invalid evidence batch accepted");
    require(safeBatch(b,4).code==ResultCode::MissingEvidence,"part of invalid batch committed");
   } else if(mode=="concurrent-revoke") {
    testPauseAfterPut.store(true); std::atomic<bool> attempted{false},done{false}; Result staged;
    std::thread migrator([&]{staged=safeBatch(b,4);}); waitFor(testPaused);
    std::thread revoker([&]{attempted.store(true); revokeFirst(b); done.store(true);});
    waitFor(attempted); std::this_thread::sleep_for(std::chrono::milliseconds(30));
    bool prematurelyDone=done.load(); testResume.store(true); migrator.join(); revoker.join();
    require(!prematurelyDone && done.load(),"revocation was not serialized after commit");
    require(staged.code==ResultCode::Complete && manifestState(d.p)=='I',"revocation did not invalidate completed marker");
    require(safeBatch(b).code==ResultCode::MissingEvidence,"subsequent audit missed revocation");
    loadEvidence(b); require(safeComplete(b).code==ResultCode::Complete && manifestState(d.p)=='C',"revalidation after revoke");
   } else throw std::runtime_error("unknown mode");
  }
  std::cout << mode << " passed\n"; return 0;
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
