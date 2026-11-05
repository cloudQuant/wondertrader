#define WT_AD_MIGRATION_TEST_HOOKS
#include "../../../WtDataStorageAD/LMDBBarMigration.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace wt_ad_bar_migration;
namespace wt_ad_bar_migration {
std::atomic<long long> testSourceCount{-1};
std::atomic<int> testExitAfterPut{0};
std::atomic<bool> testPauseAfterPut{false}, testPaused{false}, testResume{false};
void adjustSourceStatForTest(MDB_stat& stat) {
 auto count=testSourceCount.load(); if(count>=0) stat.ms_entries=static_cast<std::size_t>(count);
}
void afterStagedPutForTest() {
 if(testExitAfterPut.exchange(0)) _exit(77);
 if(testPauseAfterPut.exchange(false)) {
  testPaused.store(true);
  while(!testResume.load()) std::this_thread::yield();
 }
}
}
namespace {
void require(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
void mcheck(int rc) { if (rc) throw std::runtime_error(mdb_strerror(rc)); }
struct Env {
 MDB_env* p = nullptr;
 Env(const fs::path& path, bool ro, std::size_t mapSize=8*1024*1024) {
  fs::create_directories(path); mcheck(mdb_env_create(&p));
  mcheck(mdb_env_set_maxdbs(p, 4)); mcheck(mdb_env_set_mapsize(p, mapSize));
  mcheck(mdb_env_open(p,path.c_str(),ro ? MDB_RDONLY : 0,0600));
 }
 ~Env() { if (p) mdb_env_close(p); }
};
struct Row { const char* ex; const char* code; std::uint64_t time; };
bool bigEndian = false;
bool packedValue = false;
constexpr Row rows[] = {{"SHFE","rb1",0xfffffffeULL},{"SHFE","rb1",0x100000001ULL},
                        {"SHFE","rb2",0x100000001ULL},{"DCE","m1",0xffffffffULL}};
std::array<unsigned char,52> key(const Row& r) {
 std::array<unsigned char,52> k{}; std::memcpy(k.data(),r.ex,std::strlen(r.ex));
 std::memcpy(k.data()+16,r.code,std::strlen(r.code));
 for (unsigned i=0;i<4;++i) k[48+i]=static_cast<unsigned char>(r.time >> (bigEndian?i*8:24-i*8));
 return k;
}
std::vector<unsigned char> value(const Row& r) {
 std::vector<unsigned char> v(packedValue?84:88);
 for (unsigned i=0;i<8;++i) v[(packedValue?4:8)+i]=static_cast<unsigned char>(r.time >> (bigEndian?56-i*8:i*8));
 v[24]=42; return v;
}
void putRow(MDB_env* env, const Row& r) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,0,&txn));
 mcheck(mdb_dbi_open(txn,nullptr,0,&db)); auto k=key(r); auto v=value(r);
 MDB_val a{k.size(),k.data()},b{v.size(),v.data()}; mcheck(mdb_put(txn,db,&a,&b,0)); mcheck(mdb_txn_commit(txn));
}
void putMalformed(MDB_env* env, bool badKey) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,0,&txn));
 mcheck(mdb_dbi_open(txn,nullptr,0,&db)); auto k=key(rows[0]); auto v=value(rows[0]);
 k[0]='A'; MDB_val a{badKey ? k.size()-1:k.size(),k.data()},b{badKey?v.size():v.size()-1,v.data()};
 mcheck(mdb_put(txn,db,&a,&b,0)); mcheck(mdb_txn_commit(txn));
}
EvidenceLookup lookup() {
 return [](const MDB_val& keyArg,const MDB_val&,Evidence& e) {
  if(keyArg.mv_size>=1 && static_cast<const char*>(keyArg.mv_data)[0]=='A') {
   e.exchange="AHFE"; e.code="rb1"; e.verifiedTime=rows[0].time; e.independentlyVerified=true; return true;
  }
  for (const auto& r:rows) { auto k=key(r); if (keyArg.mv_size==k.size() && std::memcmp(keyArg.mv_data,k.data(),k.size())==0) {
   e.exchange=r.ex; e.code=r.code; e.verifiedTime=r.time; e.independentlyVerified=true; return true; } }
  return false;
 };
}
char manifestState(MDB_env* env) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,MDB_RDONLY,&txn));
 mcheck(mdb_dbi_open(txn,metadataName,0,&db)); MDB_val k{5,const_cast<char*>("state")},v{};
 mcheck(mdb_get(txn,db,&k,&v)); require(v.mv_size==1,"manifest state size");
 char c=*static_cast<const char*>(v.mv_data); mdb_txn_abort(txn); return c;
}
void checkContent(MDB_env* env) {
 MDB_txn* txn=nullptr; MDB_dbi db; MDB_cursor* cursor=nullptr;
 mcheck(mdb_txn_begin(env,nullptr,MDB_RDONLY,&txn)); mcheck(mdb_dbi_open(txn,stageName,0,&db));
 mcheck(mdb_cursor_open(txn,db,&cursor)); MDB_val k{},v{}; std::string last; unsigned found=0;
 int rc=mdb_cursor_get(cursor,&k,&v,MDB_FIRST);
 for(;rc==0;rc=mdb_cursor_get(cursor,&k,&v,MDB_NEXT)) {
  std::string current(static_cast<const char*>(k.mv_data),k.mv_size); require(last.empty() || last<current,"V2 order"); last=current;
  DecodedKey decoded; require(decode(k.mv_data,k.mv_size,decoded)==Status::Ok,"decode V2"); bool match=false;
  for(const auto& r:rows) if(decoded.exchange==r.ex && decoded.code==r.code && decoded.time==r.time) {
   auto expected=value(r); require(v.mv_size==expected.size() && std::memcmp(v.mv_data,expected.data(),v.mv_size)==0,"value mismatch"); match=true; break;
  }
  require(match,"unexpected V2 key"); ++found;
 }
 require(rc==MDB_NOTFOUND && found==4,"missing converted record"); mdb_cursor_close(cursor); mdb_txn_abort(txn);
}
std::size_t entries(MDB_env* env,const char* name) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,MDB_RDONLY,&txn));
 int rc=mdb_dbi_open(txn,name,0,&db); if(rc==MDB_NOTFOUND) { mdb_txn_abort(txn); return 0; }
 mcheck(rc); MDB_stat st{}; mcheck(mdb_stat(txn,db,&st)); mdb_txn_abort(txn); return st.ms_entries;
}
void corrupt(MDB_env* env,bool remove) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,0,&txn)); mcheck(mdb_dbi_open(txn,stageName,0,&db));
 Key k{}; require(encode(rows[0].ex,rows[0].code,rows[0].time,k)==Status::Ok,"encode");
 MDB_val a{k.size(),k.data()}; if(remove) mcheck(mdb_del(txn,db,&a,nullptr));
 else { auto v=value(rows[0]); v[40]=99; MDB_val b{v.size(),v.data()}; mcheck(mdb_put(txn,db,&a,&b,0)); }
 mcheck(mdb_txn_commit(txn));
}
void addExtra(MDB_env* env) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,0,&txn)); mcheck(mdb_dbi_open(txn,stageName,0,&db));
 Key keybuf{}; require(encode("EXTRA","bogus",1,keybuf)==Status::Ok,"encode");
 auto v=value(rows[0]); MDB_val k{keybuf.size(),keybuf.data()},val{v.size(),v.data()}; mcheck(mdb_put(txn,db,&k,&val,0));
 mcheck(mdb_txn_commit(txn));
}
struct Barrier {
 std::mutex mutex; std::condition_variable cv; int count=0;
 void wait() { std::unique_lock<std::mutex> lock(mutex); ++count; cv.notify_all();
  if(!cv.wait_for(lock,std::chrono::seconds(8),[&]{return count==2;})) throw std::runtime_error("barrier timeout"); }
};
void injectConcurrentDestinationWrite(const Bindings& b) {
 MDB_txn* txn=nullptr; mcheck(mdb_txn_begin(b.destination(),nullptr,0,&txn));
 Key k{}; require(encode("EXTRA","concurrent",7,k)==Status::Ok,"encode concurrent");
 auto v=value(rows[0]); MDB_val a{k.size(),k.data()},out{v.size(),v.data()};
 mcheck(mdb_put(txn,b.stageDbi(),&a,&out,0)); mcheck(mdb_txn_commit(txn));
}
unsigned readMarkerByte(MDB_env* env) {
 MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(env,nullptr,MDB_RDONLY,&txn)); mcheck(mdb_dbi_open(txn,stageName,0,&db));
 Key keybuf{}; require(encode(rows[0].ex,rows[0].code,rows[0].time,keybuf)==Status::Ok,"encode");
 MDB_val k{keybuf.size(),keybuf.data()},v{}; mcheck(mdb_get(txn,db,&k,&v)); require(v.mv_size>40,"value size");
 unsigned result=static_cast<const unsigned char*>(v.mv_data)[40]; mdb_txn_abort(txn); return result;
}
void bind(MDB_env* s,MDB_env* d,Bindings& b) {
 auto r=prepareBindings(s,d,b); require(r.code==ResultCode::Prepared,"binding initialization");
}
Result batch(const Bindings& b,const EvidenceLookup& cb,std::size_t size=2) {
 return stageBatch(b,bigEndian?LegacySourceHost::BigEndian:LegacySourceHost::LittleEndian,
                   packedValue?LegacyValueLayout::Packed64:LegacyValueLayout::Aligned64,cb,size);
}
Result stageIt(const Bindings& b,const EvidenceLookup& cb) {
 Result r; do { r=batch(b,cb); } while(r.code==ResultCode::Incomplete); return r;
}
void seed(const fs::path& path) { Env e(path,false); for(const Row& r:rows) putRow(e.p,r); }
void seedMany(const fs::path& path, unsigned count=900) {
 Env e(path,false,256*1024*1024); MDB_txn* txn=nullptr; MDB_dbi db; mcheck(mdb_txn_begin(e.p,nullptr,0,&txn)); mcheck(mdb_dbi_open(txn,nullptr,0,&db));
 for(unsigned i=0;i<count;++i) {
  std::string code="rb"+std::to_string(i); Row row{"SHFE",code.c_str(),0x100000000ULL+i};
  auto k=key(row); auto v=value(row); MDB_val a{k.size(),k.data()},b{v.size(),v.data()}; mcheck(mdb_put(txn,db,&a,&b,0));
 }
 mcheck(mdb_txn_commit(txn));
}
}

int main(int argc,char** argv) {
 try {
  require(argc==3,"usage mode directory"); const std::string mode=argv[1]; const fs::path base=argv[2];
  bigEndian=mode=="big-packed"; packedValue=mode=="big-packed" || mode=="packed";
  if(mode=="write") { Env e(base,false); const Row extra{"CZCE","x1",111}; putRow(e.p,extra); return 0; }
  require(!fs::exists(base),"test directory must not exist");
  fs::create_directories(base); const auto source=base/"source",dest=base/"dest";
  if(mode.rfind("benchmark-",0)==0) {
   const unsigned count=static_cast<unsigned>(std::stoul(mode.substr(10)));
   require(count>0 && count<=250000,"benchmark size"); seedMany(source,count);
   Env s(source,true,256*1024*1024),d(dest,false,256*1024*1024); Bindings b; bind(s.p,d.p,b);
   std::uint64_t lookups=0; auto evidence=[&](const MDB_val& k,const MDB_val&,Evidence& e) {
    ++lookups; if(k.mv_size!=52) return false; const auto* bytes=static_cast<const char*>(k.mv_data);
    std::string code(bytes+16,strnlen(bytes+16,32)); if(code.rfind("rb",0)!=0) return false;
    unsigned index=static_cast<unsigned>(std::stoul(code.substr(2))); if(index>=count) return false;
    e.exchange="SHFE"; e.code=code; e.verifiedTime=0x100000000ULL+index; e.independentlyVerified=true; return true;
   };
   auto begin=std::chrono::steady_clock::now(); Result r; unsigned batches=0;
   do { r=batch(b,evidence,1024); ++batches; } while(r.code==ResultCode::Incomplete);
   auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
   require(r.code==ResultCode::Complete && r.committedRecords==count && entries(d.p,stageName)==count,"benchmark failed");
   std::cout<<"records="<<count<<" batch=1024 batches="<<batches<<" evidence_lookups="<<lookups<<" elapsed_ms="<<elapsed<<"\n"; return 0;
  }
  if(mode=="map-full" || mode=="map-full-partial") seedMany(source); else seed(source);
  if(mode=="badkey" || mode=="badvalue") { Env writable(source,false); putMalformed(writable.p,mode=="badkey"); }
  if(mode=="crash") {
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); auto r=batch(b,lookup(),1);
    require(r.code==ResultCode::Incomplete && r.committedRecords==1 && manifestState(d.p)=='I',"first committed batch"); }
   pid_t pid=fork(); require(pid>=0,"fork");
   if(pid==0) { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b);
    testExitAfterPut.store(1); (void)batch(b,lookup()); _exit(78); }
   int status=0; require(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==77,"crash child");
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); require(entries(d.p,stageName)==1 && entries(d.p,metadataName)==4 && manifestState(d.p)=='I',"aborted batch published");
   auto r=stageIt(b,lookup()); require(r.code==ResultCode::Complete && r.committedRecords==4,"retry after crash");
  } else if(mode=="reopen") {
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); auto r=stageIt(b,lookup()); require(r.code==ResultCode::Complete,"initial complete"); }
   { Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b); auto r=stageIt(b,lookup());
    require(r.code==ResultCode::Complete && r.committedRecords==4 && manifestState(d.p)=='C',"reopen verify"); checkContent(d.p); }
  } else if(mode=="alias") {
   fs::create_directories(dest); fs::create_hard_link(source/"data.mdb",dest/"data.mdb");
   Env s(source,true),d(dest,false); Bindings b; auto r=prepareBindings(s.p,d.p,b);
   require(r.code==ResultCode::SameEnvironment,"backing-file alias accepted");
   require(entries(s.p,nullptr)==4,"alias altered source");
  } else if(mode=="map-full" || mode=="map-full-partial") {
   Env s(source,true),d(dest,false,mode=="map-full-partial"?131072:65536); Bindings b; bind(s.p,d.p,b);
   auto evidence=[](const MDB_val& k,const MDB_val&,Evidence& e) {
    if(k.mv_size!=52) return false; const auto* b=static_cast<const char*>(k.mv_data);
    std::string code(b+16,strnlen(b+16,32)); if(code.rfind("rb",0)!=0) return false;
    unsigned index=static_cast<unsigned>(std::stoul(code.substr(2))); if(index>=900) return false;
    e.exchange="SHFE"; e.code=code; e.verifiedTime=0x100000000ULL+index; e.independentlyVerified=true; return true;
   };
   std::size_t expectedBefore=0;
   if(mode=="map-full-partial") {
    auto first=batch(b,evidence,1); require(first.code==ResultCode::Incomplete && manifestState(d.p)=='I',"initial batch failed");
    expectedBefore=1;
   }
   auto r=batch(b,evidence,1000); require(r.code==ResultCode::DestinationError && r.lmdbError==MDB_MAP_FULL,"expected map full");
   require(entries(d.p,stageName)==expectedBefore && entries(d.p,metadataName)==(expectedBefore?4u:0u) && entries(s.p,nullptr)==900,"map-full caused partial commit");
   if(expectedBefore) require(manifestState(d.p)=='I',"map-full changed manifest");
   mcheck(mdb_env_set_mapsize(d.p,8*1024*1024)); r=batch(b,evidence,1000);
   require(r.code==ResultCode::Complete && r.committedRecords==900 && entries(d.p,stageName)==900,"map resize retry");
  } else if(mode=="concurrent-prepare") {
   Env s(source,true),d(dest,false); Bindings b; Barrier barrier; Result result[2];
   std::thread a([&]{barrier.wait(); result[0]=prepareBindings(s.p,d.p,b);});
   std::thread c([&]{barrier.wait(); result[1]=prepareBindings(s.p,d.p,b);}); a.join(); c.join();
   require((result[0].code==ResultCode::Prepared && result[1].code==ResultCode::InvalidArgument) ||
           (result[1].code==ResultCode::Prepared && result[0].code==ResultCode::InvalidArgument),"concurrent prepare");
   require(stageIt(b,lookup()).code==ResultCode::Complete,"prepared binding unusable");
  } else if(mode=="unsafe") {
   Env s(source,false),d(dest,false); Bindings b; auto r=prepareBindings(s.p,d.p,b);
   require(r.code==ResultCode::UnsafeEnvironment && entries(d.p,stageName)==0,"writable source accepted");
  } else {
   Env s(source,true),d(dest,false); Bindings b; bind(s.p,d.p,b);
   if(mode=="same") { Bindings same; auto r=prepareBindings(s.p,s.p,same); require(r.code==ResultCode::SameEnvironment,"same environment"); }
   else if(mode=="unprepared") { Bindings empty; require(batch(empty,lookup()).code==ResultCode::UnpreparedBindings,"unprepared binding accepted"); }
   else if(mode=="prepare-twice") { require(prepareBindings(s.p,d.p,b).code==ResultCode::InvalidArgument,"binding reinitialized"); }
   else if(mode=="concurrent-same" || mode=="concurrent-different") {
    std::unique_ptr<Env> second;
    Bindings secondBinding; const Bindings* b2=&b;
    if(mode=="concurrent-different") { second=std::make_unique<Env>(base/"dest2",false); bind(s.p,second->p,secondBinding); b2=&secondBinding; }
    Barrier barrier; Result results[2]; auto baseLookup=lookup();
    auto makeCallback=[&]() -> EvidenceLookup { return [&,first=true](const MDB_val& k,const MDB_val& v,Evidence& e) mutable {
      if(first) { first=false; barrier.wait(); } return baseLookup(k,v,e); }; };
    auto cb1=makeCallback(),cb2=makeCallback();
    std::thread t1([&]{results[0]=batch(b,cb1,1);}), t2([&]{results[1]=batch(*b2,cb2,1);});
    t1.join(); t2.join();
    if(mode=="concurrent-same") {
     require((results[0].code==ResultCode::Incomplete && results[1].code==ResultCode::DestinationChanged) ||
             (results[1].code==ResultCode::Incomplete && results[0].code==ResultCode::DestinationChanged),"same target race result");
     require(stageIt(b,lookup()).code==ResultCode::Complete,"same target retry");
    } else {
     require(results[0].code==ResultCode::Incomplete && results[1].code==ResultCode::Incomplete,"different target blocked");
     require(stageIt(b,lookup()).code==ResultCode::Complete && stageIt(*b2,lookup()).code==ResultCode::Complete,"different target retry");
    }
   }
   else if(mode=="reentrant") {
    auto baseLookup=lookup(); bool checked=false;
    auto cb=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(!checked) { checked=true; require(batch(b,baseLookup).code==ResultCode::ReentrantCall,"reentrant stage accepted"); }
     return baseLookup(k,v,e);
    };
    require(stageIt(b,cb).code==ResultCode::Complete && checked,"reentrant control failed");
   }
   else if(mode=="reentrant-thread") {
    auto baseLookup=lookup(); bool started=false; Result nested;
    auto cb=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(!started) { started=true; std::thread t([&]{nested=batch(b,baseLookup,1);}); t.join(); }
     return baseLookup(k,v,e);
    };
    auto outer=batch(b,cb,1);
    require(started && nested.code==ResultCode::Incomplete && outer.code==ResultCode::DestinationChanged,"thread reentry control");
    require(stageIt(b,baseLookup).code==ResultCode::Complete,"thread reentry recovery");
   }
   else if(mode=="destination-change") {
    auto baseLookup=lookup(); bool wrote=false;
    auto cb=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(!wrote) { wrote=true; std::thread writer([&]{injectConcurrentDestinationWrite(b);}); writer.join(); }
     return baseLookup(k,v,e);
    };
    auto r=batch(b,cb,1); require(r.code==ResultCode::DestinationChanged,"target snapshot change missed");
    require(entries(d.p,stageName)==1 && entries(d.p,metadataName)==0,"migration changed concurrent staging");
   }
   else if(mode=="stat-under" || mode=="stat-over") {
    auto first=batch(b,lookup(),1); require(first.code==ResultCode::Incomplete && manifestState(d.p)=='I',"first batch");
    testSourceCount.store(mode=="stat-under"?2:5);
    auto r=batch(b,lookup(),mode=="stat-under"?1:10); testSourceCount.store(-1);
    require(r.code==ResultCode::SourceInconsistent,"source count and cursor disagreement accepted");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"count mismatch changed staging");
    require(stageIt(b,lookup()).code==ResultCode::Complete,"count mismatch retry");
   }
   else if(mode=="evidence-revoked-between-batches") {
    auto original=lookup(); auto first=batch(b,original,1);
    require(first.code==ResultCode::Incomplete && first.committedRecords==1 && manifestState(d.p)=='I',"first batch");
    unsigned calls=0; auto revoked=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     ++calls; if(calls==1) return false; return original(k,v,e);
    };
    auto failed=batch(b,revoked,2);
    require(failed.code==ResultCode::MissingEvidence && calls==1,"revoked prior evidence accepted");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"revocation changed staging");
    auto recovered=stageIt(b,original); require(recovered.code==ResultCode::Complete && recovered.committedRecords==4,"valid evidence retry");
   }
   else if(mode=="evidence-revoked-during-batch") {
    auto original=lookup(); bool revoked=false; unsigned calls=0; std::string firstKey;
    auto changing=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     ++calls;
     if(calls==1) firstKey.assign(static_cast<const char*>(k.mv_data), k.mv_size);
     if(calls==2) revoked=true; // prior evidence becomes invalid after it was read
     if(revoked && firstKey.size()==k.mv_size &&
        std::memcmp(firstKey.data(),k.mv_data,k.mv_size)==0) return false;
     return original(k,v,e);
    };
    auto r=batch(b,changing,4);
    const auto staged=entries(d.p,stageName);
    const auto metadataEntries=entries(d.p,metadataName);
    const char status=metadataEntries ? manifestState(d.p) : '-';
    auto recheck=batch(b,changing,4);
    const auto afterRecheck=entries(d.p,stageName);
    const char statusAfter=entries(d.p,metadataName) ? manifestState(d.p) : '-';
    std::cerr << "revoked=" << revoked << " calls=" << calls << " code="
              << static_cast<int>(r.code) << " staged=" << staged << " state=" << status
              << " recheck=" << static_cast<int>(recheck.code)
              << " staged_after=" << afterRecheck << " state_after=" << statusAfter << '\n';
    // Desired contract: once evidence is revoked, the current batch cannot
    // publish. The current callback API has no mechanism to observe this.
    require(revoked && r.code!=ResultCode::Complete && staged==0,
            "changed evidence was accepted and committed");
   }
   else if(mode=="limit") {
    require(batch(b,lookup(),0).code==ResultCode::InvalidArgument,"zero batch accepted");
    require(batch(b,lookup(),maxBatchRecords+1).code==ResultCode::InvalidArgument,"oversize batch accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"invalid batch mutated dest");
   }
   else if(mode=="interrupted-batch") {
    auto r=batch(b,lookup(),1); require(r.code==ResultCode::Incomplete,"first batch");
    int n=0; auto real=lookup(); auto bad=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(++n==3) return false; return real(k,v,e); };
    r=batch(b,bad,2); require(r.code==ResultCode::MissingEvidence,"injected missing evidence");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"failed batch changed previous commit");
    r=stageIt(b,lookup()); require(r.code==ResultCode::Complete && r.committedRecords==4,"resume after failed batch");
   }
   else if(mode=="source-after-batch") {
    auto r=batch(b,lookup(),1); require(r.code==ResultCode::Incomplete,"first batch");
    pid_t child=fork(); if(child==0) { execl(argv[0],argv[0],"write",source.c_str(),nullptr); _exit(127); }
    int status=0; require(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"source writer");
    r=batch(b,lookup()); require(r.code==ResultCode::MetadataMismatch,"changed source resumed");
    require(entries(d.p,stageName)==1 && manifestState(d.p)=='I',"source change changed stage");
   }
   else if(mode=="badkey" || mode=="badvalue") {
    auto r=batch(b,lookup()); require(r.code==ResultCode::InvalidRecord &&
      r.codecStatus==(mode=="badkey"?Status::InvalidKeySize:Status::InvalidValueSize),"malformed accepted");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"malformed batch committed");
   }
   else if(mode=="missing" || mode=="identity" || mode=="time" || mode=="exception") {
    int n=0; auto real=lookup(); auto cb=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(++n==2) { if(mode=="exception") throw std::runtime_error("injected");
      if(mode=="missing") return false; bool ok=real(k,v,e); if(mode=="identity") e.code="wrong"; else ++e.verifiedTime; return ok; }
     return real(k,v,e); };
    auto r=stageIt(b,cb); require(r.code==(mode=="exception"?ResultCode::CallbackError:mode=="missing"?ResultCode::MissingEvidence:ResultCode::InvalidRecord),"failed input result");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"failed input committed");
    require(stageIt(b,lookup()).code==ResultCode::Complete,"retry after failure");
   } else if(mode=="source-change") {
    auto real=lookup(); bool wrote=false; auto cb=[&](const MDB_val& k,const MDB_val& v,Evidence& e) {
     if(!wrote) { wrote=true; pid_t child=fork(); if(child==0) { execl(argv[0],argv[0],"write",source.c_str(),nullptr); _exit(127); }
      int status=0; require(child>0 && waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"source writer"); }
     return real(k,v,e); };
    auto r=stageIt(b,cb); require(r.code==ResultCode::SourceChanged,"source change undetected");
    require(entries(d.p,stageName)==0 && entries(d.p,metadataName)==0,"source change committed");
   } else {
    auto first=batch(b,lookup(),1); require(first.code==ResultCode::Incomplete && first.committedRecords==1 && manifestState(d.p)=='I',"first batch");
    auto r=stageIt(b,lookup()); require(r.code==ResultCode::Complete && r.committedRecords==4,"first stage");
    require(entries(d.p,stageName)==4 && entries(d.p,metadataName)==4 && manifestState(d.p)=='C',"stage sizes");
    checkContent(d.p);
    if(mode=="success" || mode=="packed" || mode=="big-packed") { r=stageIt(b,lookup()); require(r.code==ResultCode::Complete && r.committedRecords==4,"idempotent retry"); }
    else if(mode=="extra-stage") { addExtra(d.p); r=stageIt(b,lookup());
     require(r.code==ResultCode::MetadataMismatch && entries(d.p,stageName)==5,"extra stage accepted or modified"); }
    else if(mode=="conflict" || mode=="missing-stage") { corrupt(d.p,mode=="missing-stage");
     r=stageIt(b,lookup());
     require(r.code==(mode=="missing-stage"?ResultCode::MetadataMismatch:ResultCode::Conflict),"corrupt retry accepted");
     require(entries(d.p,stageName)==(mode=="missing-stage"?3u:4u),"retry changed stage");
     if(mode=="conflict") require(readMarkerByte(d.p)==99,"retry overwrote conflict"); }
    else throw std::runtime_error("unknown mode");
   }
  }
  std::cout<<mode<<": PASS\n"; return 0;
 } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"\n"; return 1; }
}
